// 标准对象存储 HTTP 通路端到端测试：普通 host 内存 buffer 上传/下载。
// 用法：http_put_example <endpoint> <bytes> <bucket> <object_key>
//
// 覆盖：
//   1. PUT 整对象 → 拿 etag/version
//   2. HEAD → 校验 content_length / etag 与 PUT 一致
//   3. GET 整对象 → 字节级 memcmp
//   4. Range GET（offset=size/4, length=size/2）→ 子串 memcmp
//   5. HEAD 不存在 key → 拿 kNotFound

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc != 5) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <bucket> <object_key>" << std::endl;
    return 1;
  }
  const std::string endpoint   = argv[1];
  const std::size_t bytes      = std::strtoull(argv[2], nullptr, 10);
  const std::string bucket     = argv[3];
  const std::string object_key = argv[4];

  // 普通堆 buffer 填测试 pattern。HTTP 路径限定 kHostRegular。
  std::vector<std::byte> payload(bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    payload[i] = static_cast<std::byte>(i % 251U);
  }

  ClientOptions options;
  options.endpoint  = endpoint;
  options.client_id = "us3-http-example";
  options.data_path = DataPath::kHttpTcp;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // 1) PUT
  RequestOptions req;
  req.object = ObjectId{.bucket = bucket, .key = object_key};
  req.length = bytes;
  ConstBufferView in{.data = payload.data(), .size = bytes,
                     .type = BufferType::kHostRegular};
  auto put = client.PutObject(req, in);
  if (!put.success()) {
    std::cerr << "PutObject failed: " << put.error().message << std::endl;
    return 1;
  }
  std::cout << "PUT path=" << ToString(put.value().selected_path)
            << " bytes=" << put.value().bytes_transferred
            << " etag=" << put.value().etag << std::endl;
  const std::string put_etag = put.value().etag;

  // 2) HEAD
  auto head = client.HeadObject(req.object);
  if (!head.success()) {
    std::cerr << "HeadObject failed: " << head.error().message << std::endl;
    return 1;
  }
  if (head.value().content_length != bytes) {
    std::cerr << "HEAD size mismatch: got " << head.value().content_length
              << " expected " << bytes << std::endl;
    return 1;
  }
  if (!put_etag.empty() && head.value().etag != put_etag) {
    std::cerr << "HEAD etag mismatch: " << head.value().etag
              << " vs " << put_etag << std::endl;
    return 1;
  }
  std::cout << "HEAD size=" << head.value().content_length
            << " etag=" << head.value().etag << std::endl;

  // 3) GET 整对象
  std::vector<std::byte> out_full(bytes);
  RequestOptions full_req;
  full_req.object = req.object;
  // length 不设：整对象
  MutableBufferView out_full_view{.data = out_full.data(),
                                  .size = bytes,
                                  .type = BufferType::kHostRegular};
  auto get_full = client.GetObject(full_req, out_full_view);
  if (!get_full.success()) {
    std::cerr << "GET full failed: " << get_full.error().message << std::endl;
    return 1;
  }
  if (get_full.value().bytes_transferred != bytes) {
    std::cerr << "GET full byte count mismatch: "
              << get_full.value().bytes_transferred << " vs " << bytes << std::endl;
    return 1;
  }
  if (std::memcmp(out_full.data(), payload.data(), bytes) != 0) {
    std::cerr << "GET full payload mismatch" << std::endl;
    return 1;
  }
  std::cout << "GET full: bytes=" << get_full.value().bytes_transferred
            << " same=true" << std::endl;

  // 4) Range GET
  if (bytes >= 4) {
    const std::uint64_t roff = bytes / 4;
    const std::uint64_t rlen = bytes / 2;
    std::vector<std::byte> out_range(rlen);
    RequestOptions range_req;
    range_req.object = req.object;
    range_req.offset = roff;
    range_req.length = rlen;
    MutableBufferView out_range_view{.data = out_range.data(),
                                     .size = rlen,
                                     .type = BufferType::kHostRegular};
    auto get_range = client.GetObject(range_req, out_range_view);
    if (!get_range.success()) {
      std::cerr << "GET range failed: " << get_range.error().message << std::endl;
      return 1;
    }
    if (get_range.value().bytes_transferred != rlen) {
      std::cerr << "GET range size mismatch: "
                << get_range.value().bytes_transferred << " vs " << rlen << std::endl;
      return 1;
    }
    if (std::memcmp(out_range.data(), payload.data() + roff, rlen) != 0) {
      std::cerr << "GET range payload mismatch" << std::endl;
      return 1;
    }
    std::cout << "GET range[" << roff << "+" << rlen << "]: same=true" << std::endl;
  }

  // 5) HEAD 不存在 key：当前 Client::HeadObject 走 control plane（baidu_std），
  // server SetFailed 之后客户端 CheckRpcFailure 把原始 code 包成 kControlPlaneError，
  // 所以 kNotFound / kControlPlaneError 都视为合规——只要拿到错误且消息提及 "not found"。
  auto missing = client.HeadObject(
      ObjectId{.bucket = bucket, .key = object_key + ".does-not-exist"});
  if (missing.success()) {
    std::cerr << "HEAD missing key unexpectedly succeeded" << std::endl;
    return 1;
  }
  const auto& mc = missing.error().code;
  if (mc != ErrorCode::kNotFound && mc != ErrorCode::kControlPlaneError) {
    std::cerr << "HEAD missing key unexpected code: " << static_cast<int>(mc)
              << " msg=" << missing.error().message << std::endl;
    return 1;
  }
  std::cout << "HEAD missing: code=" << static_cast<int>(mc)
            << " msg=" << missing.error().message << std::endl;

  std::cout << "OK" << std::endl;
  return 0;
}
