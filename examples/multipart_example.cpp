// Multipart upload smoke / sizing test. Each part is uploaded via either
// the serial UploadPart loop (concurrency=1) or the parallel UploadParts
// helper (concurrency>1). Verifies via HEAD + per-part GET.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

namespace {

bool CheckCuda(cudaError_t status, const char* action) {
  if (status == cudaSuccess) return true;
  std::cerr << action << " failed: " << cudaGetErrorString(status) << std::endl;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc < 6 || argc > 8) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <total_bytes> <part_size> <bucket> <object_key>"
              << " [concurrency] [checksum_policy: none|md5]\n";
    return 1;
  }
  const std::string endpoint = argv[1];
  const std::size_t total_bytes = std::strtoull(argv[2], nullptr, 10);
  const std::size_t part_size = std::strtoull(argv[3], nullptr, 10);
  const std::string bucket = argv[4];
  const std::string object_key = argv[5];
  const std::size_t concurrency =
      argc >= 7 ? std::max<std::size_t>(1, std::strtoull(argv[6], nullptr, 10)) : 1;
  const std::string checksum_policy = argc >= 8 ? argv[7] : "none";

  if (total_bytes == 0 || part_size == 0) {
    std::cerr << "total_bytes and part_size must be > 0\n";
    return 1;
  }
  const std::size_t num_parts = (total_bytes + part_size - 1) / part_size;
  const std::size_t effective_concurrency =
      std::min(concurrency, num_parts);

  ClientOptions options;
  options.endpoint = endpoint;
  options.client_id = "us3-multipart-example";
  options.data_path = DataPath::kGdsCuObject;
  Client client(std::move(options));
  auto init = client.Initialize();
  if (!init.success()) {
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
            << " concurrency=" << effective_concurrency << std::endl;

  // Pre-stage all parts into per-part device buffers (so concurrent uploads
  // don't race on a shared staging buffer).
  std::vector<void*>          dev_parts(num_parts, nullptr);
  std::vector<std::size_t>    part_sizes(num_parts);
  std::vector<std::byte>      host_payload(part_size);

  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * part_size;
    const std::size_t sz = std::min(part_size, total_bytes - off);
    part_sizes[i] = sz;
    if (!CheckCuda(cudaMalloc(&dev_parts[i], sz), "cudaMalloc part")) {
      return 1;
    }
    for (std::size_t j = 0; j < sz; ++j) {
      host_payload[j] = static_cast<std::byte>((off + j) % 251U);
    }
    if (!CheckCuda(cudaMemcpy(dev_parts[i], host_payload.data(), sz,
                              cudaMemcpyHostToDevice),
                   "h2d")) {
      return 1;
    }
  }

  upload.set_checksum_policy(checksum_policy);

  std::vector<MultipartUpload::PartSpec> specs;
  specs.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    specs.push_back(MultipartUpload::PartSpec{
        .part_number = static_cast<std::uint32_t>(i + 1),
        .object_offset = i * part_size,
        .buffer = ConstBufferView{.data = dev_parts[i],
                                  .size = part_sizes[i],
                                  .type = BufferType::kCudaDevice},
    });
  }

  auto upload_result = upload.UploadParts(specs, effective_concurrency);
  if (!upload_result.success()) {
    std::cerr << "UploadParts failed: " << upload_result.error().message
              << std::endl;
    return 1;
  }
  for (std::size_t i = 0; i < upload_result.value().size(); ++i) {
    std::cout << "  part " << (i + 1)
              << " bytes=" << part_sizes[i]
              << " etag=" << upload_result.value()[i].etag << std::endl;
  }

  auto complete = upload.Complete();
  if (!complete.success()) {
    std::cerr << "Complete failed: " << complete.error().message << std::endl;
    return 1;
  }
  std::cout << "Complete etag=" << complete.value().etag
            << " size=" << complete.value().content_length << std::endl;

  // Idempotency check: a second Complete on the same handle should fail
  // (handle marked finished). For a gateway-side concurrent-Complete test
  // we would need two clients hitting the same upload_id; that's covered
  // by the kCompleting CAS but cannot be exercised from a single handle.
  auto complete2 = upload.Complete();
  if (complete2.success()) {
    std::cerr << "second Complete unexpectedly succeeded" << std::endl;
    return 1;
  }
  std::cout << "second Complete correctly rejected: "
            << complete2.error().message << std::endl;

  auto head = client.HeadObject(ObjectId{.bucket = bucket, .key = object_key});
  if (!head.success() || head.value().content_length != total_bytes) {
    std::cerr << "Head mismatch: " << head.value().content_length
              << " vs " << total_bytes << std::endl;
    return 1;
  }
  std::cout << "HEAD size=" << head.value().content_length << std::endl;

  // Re-use one of the device buffers for verification.
  std::vector<std::byte> host_verify(part_size);
  void* dev_dn = dev_parts[0];
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * part_size;
    const std::size_t sz = part_sizes[i];
    if (!CheckCuda(cudaMemset(dev_dn, 0, sz), "memset dn")) return 1;
    RequestOptions req;
    req.object = ObjectId{.bucket = bucket, .key = object_key};
    req.offset = off;
    req.length = sz;
    auto get = client.GetObject(req, MutableBufferView{.data = dev_dn,
                                                       .size = sz,
                                                       .type = BufferType::kCudaDevice});
    if (!get.success()) {
      std::cerr << "Get part " << i << " failed: " << get.error().message
                << std::endl;
      return 1;
    }
    if (!CheckCuda(cudaMemcpy(host_verify.data(), dev_dn, sz,
                              cudaMemcpyDeviceToHost),
                   "d2h")) {
      return 1;
    }
    for (std::size_t j = 0; j < sz; ++j) {
      const auto expected = static_cast<std::byte>((off + j) % 251U);
      if (host_verify[j] != expected) {
        std::cerr << "mismatch at off=" << (off + j) << std::endl;
        return 1;
      }
    }
  }
  std::cout << "same=true" << std::endl;

  for (void* p : dev_parts) {
    if (p != nullptr) cudaFree(p);
  }
  return 0;
}
