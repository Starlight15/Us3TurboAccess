// HTTP V2 multipart 端到端测试：N 个 part 并发 PUT → Complete → HEAD + GET 校验。
// 用法：http_multipart_example <endpoint> <total_bytes> <part_size>
//                              <bucket> <object_key> [concurrency=2]

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc < 6 || argc > 7) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <total_bytes> <part_size>"
                 " <bucket> <object_key> [concurrency=2]" << std::endl;
    return 1;
  }
  const std::string endpoint    = argv[1];
  const std::size_t total_bytes = std::strtoull(argv[2], nullptr, 10);
  const std::size_t part_size   = std::strtoull(argv[3], nullptr, 10);
  const std::string bucket      = argv[4];
  const std::string object_key  = argv[5];
  const std::size_t concurrency =
      argc == 7 ? std::max<std::size_t>(1, std::strtoull(argv[6], nullptr, 10)) : 2;

  if (total_bytes == 0 || part_size == 0) {
    std::cerr << "total_bytes/part_size must be > 0" << std::endl;
    return 1;
  }
  const std::size_t num_parts = (total_bytes + part_size - 1) / part_size;

  ClientOptions options;
  options.endpoint  = endpoint;
  options.client_id = "us3-http-multipart-example";
  options.data_path = DataPath::kHttpTcp;
  options.async_worker_threads = concurrency;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  auto start = client.StartUpload(ObjectId{.bucket = bucket, .key = object_key},
                                    total_bytes);
  if (!start.success()) {
    std::cerr << "StartUpload failed: " << start.error().message << std::endl;
    return 1;
  }
  auto& upload = start.value();
  std::cout << "StartUpload upload_id=" << upload.upload_id()
            << " max_part_size=" << upload.max_part_size()
            << " num_parts=" << num_parts
            << " concurrency=" << concurrency << std::endl;

  // 准备每个 part 的 buffer。HTTP 路径只接 kHostRegular，用普通 std::vector。
  std::vector<std::vector<std::byte>> payloads(num_parts);
  std::vector<std::size_t>            sizes(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * part_size;
    const std::size_t sz  = std::min(part_size, total_bytes - off);
    sizes[i] = sz;
    payloads[i].resize(sz);
    auto* p = payloads[i].data();
    for (std::size_t j = 0; j < sz; ++j) {
      p[j] = static_cast<std::byte>((off + j) % 251U);
    }
  }

  std::vector<MultipartUpload::PartSpec> specs;
  specs.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    specs.push_back(MultipartUpload::PartSpec{
        .part_number   = static_cast<std::uint32_t>(i + 1),
        .object_offset = i * part_size,
        .buffer = ConstBufferView{.data = payloads[i].data(),
                                  .size = sizes[i],
                                  .type = BufferType::kHostRegular},
    });
  }

  auto up = upload.UploadParts(specs, std::min(concurrency, num_parts));
  if (!up.success()) {
    std::cerr << "UploadParts failed: " << up.error().message << std::endl;
    return 1;
  }
  for (std::size_t i = 0; i < up.value().size(); ++i) {
    std::cout << "  part " << (i + 1)
              << " bytes=" << sizes[i]
              << " etag=" << up.value()[i].etag << std::endl;
  }

  auto complete = upload.Complete();
  if (!complete.success()) {
    std::cerr << "Complete failed: " << complete.error().message << std::endl;
    return 1;
  }
  std::cout << "Complete etag=" << complete.value().etag
            << " size=" << complete.value().content_length << std::endl;

  // HEAD 校验。HEAD 当前走 control plane（与 V1 一样的行为）；要全 HTTP 链路
  // 检查就直接 GET 整对象比对。
  std::vector<std::byte> out_full(total_bytes);
  RequestOptions get_req;
  get_req.object = ObjectId{.bucket = bucket, .key = object_key};
  MutableBufferView out_view{.data = out_full.data(),
                             .size = total_bytes,
                             .type = BufferType::kHostRegular};
  auto get_full = client.GetObject(get_req, out_view);
  if (!get_full.success()) {
    std::cerr << "GET full failed: " << get_full.error().message << std::endl;
    return 1;
  }
  if (get_full.value().bytes_transferred != total_bytes) {
    std::cerr << "GET size mismatch: got " << get_full.value().bytes_transferred
              << " vs " << total_bytes << std::endl;
    return 1;
  }
  // 重组 expected 全对象再 memcmp。
  std::vector<std::byte> expected(total_bytes);
  for (std::size_t i = 0; i < num_parts; ++i) {
    std::memcpy(expected.data() + i * part_size, payloads[i].data(), sizes[i]);
  }
  if (std::memcmp(out_full.data(), expected.data(), total_bytes) != 0) {
    std::cerr << "GET payload mismatch" << std::endl;
    return 1;
  }
  std::cout << "GET memcmp: same=true" << std::endl;

  std::cout << "OK" << std::endl;
  return 0;
}
