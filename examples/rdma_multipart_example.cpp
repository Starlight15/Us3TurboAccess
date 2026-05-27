// Native RDMA multipart 上传端到端测试：
//   1. StartUpload（data_path=native-rdma）
//   2. UploadParts 并行推 N 个 part（PinnedBuffer + RDMA WRITE）
//   3. Complete 拼对象 → HEAD 校验 size
// 用法：rdma_multipart_example <endpoint> <total_bytes> <part_size>
//                              <bucket> <object_key> [concurrency=2]

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"
#include "us3_turbo_access/client/pinned_buffer.h"

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
  options.client_id = "us3-rdma-multipart-example";
  options.data_path = DataPath::kNativeRdma;
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

  // 每个 part 独立 pinned buffer，内容是 (offset+j)%251 模式，跟 multipart_example 一致。
  std::vector<PinnedBuffer> buffers;
  std::vector<std::size_t>  part_sizes(num_parts);
  buffers.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * part_size;
    const std::size_t sz  = std::min(part_size, total_bytes - off);
    part_sizes[i] = sz;
    auto buf = PinnedBuffer::Allocate(sz);
    if (!buf.success()) {
      std::cerr << "PinnedBuffer::Allocate[" << i << "] failed: "
                << buf.error().message << std::endl;
      return 1;
    }
    auto* p = static_cast<std::byte*>(buf.value().data());
    for (std::size_t j = 0; j < sz; ++j) {
      p[j] = static_cast<std::byte>((off + j) % 251U);
    }
    buffers.push_back(std::move(buf.value()));
  }

  std::vector<MultipartUpload::PartSpec> specs;
  specs.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    specs.push_back(MultipartUpload::PartSpec{
        .part_number   = static_cast<std::uint32_t>(i + 1),
        .object_offset = i * part_size,
        .buffer        = buffers[i].view(),
    });
  }

  auto up = upload.UploadParts(specs, std::min(concurrency, num_parts));
  if (!up.success()) {
    std::cerr << "UploadParts failed: " << up.error().message << std::endl;
    return 1;
  }
  for (std::size_t i = 0; i < up.value().size(); ++i) {
    std::cout << "  part " << (i + 1)
              << " bytes=" << part_sizes[i]
              << " etag=" << up.value()[i].etag << std::endl;
  }

  auto complete = upload.Complete();
  if (!complete.success()) {
    std::cerr << "Complete failed: " << complete.error().message << std::endl;
    return 1;
  }
  std::cout << "Complete etag=" << complete.value().etag
            << " size=" << complete.value().content_length << std::endl;

  auto head = client.HeadObject(ObjectId{.bucket = bucket, .key = object_key});
  if (!head.success() || head.value().content_length != total_bytes) {
    std::cerr << "HEAD mismatch: got "
              << (head.success() ? head.value().content_length : 0)
              << " expected " << total_bytes << std::endl;
    return 1;
  }
  std::cout << "HEAD size=" << head.value().content_length << std::endl;
  std::cout << "OK" << std::endl;
  return 0;
}
