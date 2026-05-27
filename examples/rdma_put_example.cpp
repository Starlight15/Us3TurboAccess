// Native RDMA 端到端 PUT 测试：CPU host pinned buffer 上传到 gateway。
// 用法：rdma_put_example <endpoint> <bytes> <bucket> <object_key>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "us3_turbo_access/client/client.h"
#include "us3_turbo_access/client/pinned_buffer.h"

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc != 5) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <bucket> <object_key>" << std::endl;
    return 1;
  }
  const std::string endpoint = argv[1];
  const std::size_t bytes = std::strtoull(argv[2], nullptr, 10);
  const std::string bucket = argv[3];
  const std::string object_key = argv[4];

  // 申请 host pinned buffer + 填充。
  auto buf = PinnedBuffer::Allocate(bytes);
  if (!buf.success()) {
    std::cerr << "PinnedBuffer::Allocate failed: " << buf.error().message
              << std::endl;
    return 1;
  }
  auto* data = static_cast<std::byte*>(buf.value().data());
  for (std::size_t i = 0; i < bytes; ++i) {
    data[i] = static_cast<std::byte>(i % 251U);
  }

  ClientOptions options;
  options.endpoint = endpoint;
  options.client_id = "us3-rdma-example";
  options.data_path = DataPath::kNativeRdma;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  RequestOptions request;
  request.object = ObjectId{.bucket = bucket, .key = object_key};
  request.length = bytes;

  auto put = client.PutObject(request, buf.value().view());
  if (!put.success()) {
    std::cerr << "PutObject failed: " << put.error().message << std::endl;
    return 1;
  }
  std::cout << "PUT path=" << ToString(put.value().selected_path)
            << " bytes=" << put.value().bytes_transferred
            << " etag=" << put.value().etag << std::endl;

  auto head = client.HeadObject(request.object);
  if (!head.success()) {
    std::cerr << "HeadObject failed: " << head.error().message << std::endl;
    return 1;
  }
  std::cout << "HEAD size=" << head.value().content_length << std::endl;
  if (head.value().content_length != bytes) {
    std::cerr << "size mismatch" << std::endl;
    return 1;
  }
  std::cout << "OK" << std::endl;
  return 0;
}
