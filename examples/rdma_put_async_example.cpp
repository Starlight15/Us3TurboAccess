// Native RDMA 异步 PUT 并发测试：N 个 PinnedBuffer 同时 PutObjectAsync，
// 统一 wait + 校验，验证 *Async 接口 + 连接池在并发场景下功能正确。
// 用法：rdma_put_async_example <endpoint> <bytes> <concurrency> <bucket> <key_prefix> [rounds=2]
// rounds>=2 时跑多轮，第 2 轮起预期命中连接池（gateway 看到的 accept 次数 ≈ concurrency）。

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "us3_turbo_access/client/client.h"
#include "us3_turbo_access/client/pinned_buffer.h"

namespace {

void FillPattern(std::byte* p, std::size_t n, std::uint8_t seed) {
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::byte>((i + seed) % 251U);
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;
  if (argc < 6 || argc > 7) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> <bytes> <concurrency> <bucket> <key_prefix>"
                 " [rounds=2]" << std::endl;
    return 1;
  }
  const std::string endpoint    = argv[1];
  const std::size_t bytes       = std::strtoull(argv[2], nullptr, 10);
  const std::size_t concurrency = std::strtoull(argv[3], nullptr, 10);
  const std::string bucket      = argv[4];
  const std::string key_prefix  = argv[5];
  const std::size_t rounds      = argc == 7 ? std::strtoull(argv[6], nullptr, 10) : 2U;

  if (bytes == 0 || concurrency == 0 || rounds == 0) {
    std::cerr << "bytes/concurrency/rounds must be > 0" << std::endl;
    return 1;
  }

  ClientOptions options;
  options.endpoint   = endpoint;
  options.client_id  = "us3-rdma-async-example";
  options.data_path  = DataPath::kNativeRdma;
  options.async_worker_threads = concurrency;
  Client client(std::move(options));
  if (auto init = client.Initialize(); !init.success()) {
    std::cerr << "Initialize failed: " << init.error().message << std::endl;
    return 1;
  }

  // 每个并发持有自己的 pinned buffer + key，避免共享。
  std::vector<PinnedBuffer> buffers;
  std::vector<std::string>  keys;
  buffers.reserve(concurrency);
  keys.reserve(concurrency);
  for (std::size_t i = 0; i < concurrency; ++i) {
    auto buf = PinnedBuffer::Allocate(bytes);
    if (!buf.success()) {
      std::cerr << "PinnedBuffer::Allocate[" << i << "] failed: "
                << buf.error().message << std::endl;
      return 1;
    }
    FillPattern(static_cast<std::byte*>(buf.value().data()), bytes,
                static_cast<std::uint8_t>(i + 1));
    buffers.push_back(std::move(buf.value()));
    keys.push_back(key_prefix + "/obj-" + std::to_string(i));
  }

  // 多轮 PUT：第 1 轮建立连接池，后续轮预期命中复用。
  std::size_t total_ok = 0;
  for (std::size_t round = 0; round < rounds; ++round) {
    std::vector<std::future<Result<TransferOutcome>>> futs;
    futs.reserve(concurrency);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < concurrency; ++i) {
      const auto key = key_prefix + "/r" + std::to_string(round)
                       + "-obj-" + std::to_string(i);
      keys[i] = key;
      RequestOptions req;
      req.object = ObjectId{.bucket = bucket, .key = key};
      req.length = bytes;
      futs.push_back(client.PutObjectAsync(req, buffers[i].view()));
    }
    std::size_t ok = 0;
    for (std::size_t i = 0; i < concurrency; ++i) {
      auto r = futs[i].get();
      if (!r.success()) {
        std::cerr << "PutObjectAsync round=" << round << " i=" << i
                  << " failed: " << r.error().message << std::endl;
        continue;
      }
      ++ok;
      if (r.value().bytes_transferred != bytes) {
        std::cerr << "size mismatch on key " << keys[i] << std::endl;
        return 1;
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "round " << round << " PUT async: " << ok << "/" << concurrency
              << " ok, wall=" << ms << "ms" << std::endl;
    if (ok != concurrency) return 1;
    total_ok += ok;

    // HEAD round 校验。
    std::vector<std::future<Result<ObjectMetadata>>> head_futs;
    head_futs.reserve(concurrency);
    for (const auto& k : keys) {
      head_futs.push_back(
          client.HeadObjectAsync(ObjectId{.bucket = bucket, .key = k}));
    }
    for (std::size_t i = 0; i < concurrency; ++i) {
      auto r = head_futs[i].get();
      if (!r.success()) {
        std::cerr << "HeadObjectAsync round=" << round << " i=" << i
                  << " failed: " << r.error().message << std::endl;
        return 1;
      }
      if (r.value().content_length != bytes) {
        std::cerr << "HEAD size mismatch on key " << keys[i] << std::endl;
        return 1;
      }
    }
  }
  std::cout << "OK total_puts=" << total_ok
            << " (rounds=" << rounds << " concurrency=" << concurrency << ")"
            << std::endl;
  return 0;
}
