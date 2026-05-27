// HTTP 通路双向数据校验 example：
//   每条传输都做 (client_crc, server_crc, memcmp) 三件套，作为后续性能测试
//   的"已知好状态"参照点。任一断言失败立即退出并打印定位信息。
//
// 覆盖：
//   1. 整对象 PUT（带 CRC）→ server 响应头 CRC == client 算的
//   2. HEAD 校验 size + etag
//   3. 单连接 GET 整对象 → memcmp + 重算 CRC
//   4. 并发 GET 整对象 → memcmp + 重算 CRC
//   5. Range GET 子串 → memcmp 子串
//   6. multipart 8 part 并发上传：每个 part 带 CRC + server 响应 CRC 校验
//      → Complete → GET 整对象 memcmp + 重算 CRC
//   7. HEAD 不存在 key → 拿到错误
//
// 用法：http_verify_example <endpoint> [bucket] [seed]

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "client/src/data/http_crc32c.h"   // 复用 client crc32c 算法
#include "us3_turbo_access/client/client.h"

namespace {

using namespace us3_turbo_access::client;

constexpr std::size_t kSingleSize    = 4ULL * 1024 * 1024;        // 4 MiB
// multipart part 大小必须 ≥ server min_part_size（默认 5 MiB），否则
// CompleteUpload 会拒（最后一个 part 可以小于）。用 8 MiB / part 留点余量。
constexpr std::size_t kMultipartSize = 32ULL * 1024 * 1024;       // 32 MiB
constexpr std::size_t kPartSize      = 8ULL * 1024 * 1024;        // 8 MiB / part
constexpr std::size_t kPartConcurr   = 4;
constexpr std::size_t kParallelGetChunks = 4;

#define EXPECT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL @" << __LINE__ << ": " << msg << std::endl;           \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define EXPECT_OK(result, msg)                                                 \
  do {                                                                         \
    if (!(result).success()) {                                                 \
      std::cerr << "FAIL @" << __LINE__ << ": " << msg << " err="              \
                << (result).error().message << std::endl;                      \
      return 1;                                                                \
    }                                                                          \
  } while (0)

// 用固定 seed + mt19937_64 填 buffer，保证 example 可重现。
void FillRandom(std::byte* p, std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const auto v = rng();
    std::memcpy(p + i, &v, 8);
  }
  if (i < n) {
    const auto v = rng();
    std::memcpy(p + i, &v, n - i);
  }
}

std::uint32_t ClientCrc(const std::vector<std::byte>& buf) {
  return Crc32c(std::span<const std::byte>(buf.data(), buf.size()));
}

// 比较两段 byte，发现差异打印前 8 字节上下文，方便定位。
int CompareOrDump(const std::byte* got, const std::byte* want, std::size_t n,
                  const char* tag) {
  for (std::size_t i = 0; i < n; ++i) {
    if (got[i] != want[i]) {
      std::cerr << "FAIL " << tag << ": mismatch at offset=" << i
                << " got=0x" << std::hex
                << static_cast<unsigned>(static_cast<std::uint8_t>(got[i]))
                << " want=0x"
                << static_cast<unsigned>(static_cast<std::uint8_t>(want[i]))
                << std::dec << std::endl;
      return 1;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: " << argv[0]
              << " <endpoint> [bucket=us3-test] [seed=0xC0FFEE]" << std::endl;
    return 1;
  }
  const std::string endpoint = argv[1];
  const std::string bucket   = argc >= 3 ? argv[2] : "us3-test";
  const std::uint64_t seed   =
      argc >= 4 ? std::strtoull(argv[3], nullptr, 0) : 0xC0FFEEULL;

  const std::string ts = std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count());

  ClientOptions options;
  options.endpoint  = endpoint;
  options.client_id = "us3-http-verify";
  options.data_path = DataPath::kHttpTcp;
  options.async_worker_threads = kPartConcurr;
  options.http.parallel_get_chunks    = kParallelGetChunks;
  // 不强制小阈值——让默认 16 MiB 起作用，但 single GET 测试用 length 不设
  // 会自然走 single 路径；并发 GET 测试单独把 length 设到 kSingleSize.
  options.http.parallel_get_threshold = 0;  // 任何 length>0 走并发
  Client client(std::move(options));
  EXPECT_OK(client.Initialize(), "Client::Initialize");

  // ============================================================
  // Section 1: 整对象 PUT + 双向 CRC
  // ============================================================
  std::cout << "[1] single PUT (" << kSingleSize << " bytes)" << std::endl;
  std::vector<std::byte> payload(kSingleSize);
  FillRandom(payload.data(), payload.size(), seed);
  const std::uint32_t expect_crc = ClientCrc(payload);

  const ObjectId obj_single{bucket, "verify/single-" + ts};
  RequestOptions put_req;
  put_req.object = obj_single;
  put_req.length = kSingleSize;
  ConstBufferView in{.data = payload.data(), .size = kSingleSize,
                     .type = BufferType::kHostRegular};
  const auto t_put0 = std::chrono::steady_clock::now();
  auto put = client.PutObject(put_req, in);
  const auto t_put1 = std::chrono::steady_clock::now();
  EXPECT_OK(put, "PutObject");
  EXPECT_TRUE(put.value().server_crc32c.has_value(),
              "PUT response missing server_crc32c");
  EXPECT_TRUE(*put.value().server_crc32c == expect_crc,
              "PUT crc mismatch client=" + std::to_string(expect_crc) +
                  " server=" + std::to_string(*put.value().server_crc32c));
  const auto put_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t_put1 - t_put0)
          .count();
  std::cout << "    OK: bytes=" << put.value().bytes_transferred
            << " server_crc=0x" << std::hex << *put.value().server_crc32c
            << std::dec << " wall=" << put_ms << "ms" << std::endl;
  const std::string put_etag = put.value().etag;

  // ============================================================
  // Section 2: HEAD
  // ============================================================
  std::cout << "[2] HEAD" << std::endl;
  auto head = client.HeadObject(obj_single);
  EXPECT_OK(head, "HeadObject");
  EXPECT_TRUE(head.value().content_length == kSingleSize,
              "HEAD size mismatch: got " +
                  std::to_string(head.value().content_length));
  EXPECT_TRUE(head.value().etag == put_etag,
              "HEAD etag mismatch: " + head.value().etag + " vs " + put_etag);
  std::cout << "    OK: size=" << head.value().content_length
            << " etag=" << head.value().etag << std::endl;

  // ============================================================
  // Section 3: 单连接 GET 整对象 + memcmp + 重算 CRC
  // ============================================================
  std::cout << "[3] single GET (length 不设 → 走 single 路径)" << std::endl;
  std::vector<std::byte> out_single(kSingleSize);
  RequestOptions sg;
  sg.object = obj_single;       // length 不设 → GetObjectSingle
  MutableBufferView sg_view{out_single.data(), out_single.size(),
                             BufferType::kHostRegular};
  const auto t_g0 = std::chrono::steady_clock::now();
  auto get_s = client.GetObject(sg, sg_view);
  const auto t_g1 = std::chrono::steady_clock::now();
  EXPECT_OK(get_s, "single GetObject");
  EXPECT_TRUE(get_s.value().bytes_transferred == kSingleSize,
              "single GET size mismatch");
  if (CompareOrDump(out_single.data(), payload.data(), kSingleSize,
                    "single GET")) return 1;
  EXPECT_TRUE(ClientCrc(out_single) == expect_crc,
              "single GET crc mismatch after readback");
  const auto g_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t_g1 - t_g0)
          .count();
  std::cout << "    OK: memcmp + crc consistent wall=" << g_ms << "ms"
            << std::endl;

  // ============================================================
  // Section 4: 并发 GET 整对象 + memcmp + 重算 CRC
  // ============================================================
  std::cout << "[4] parallel GET (chunks=" << kParallelGetChunks << ")"
            << std::endl;
  std::vector<std::byte> out_par(kSingleSize);
  RequestOptions pg;
  pg.object = obj_single;
  pg.length = kSingleSize;       // 触发 parallel 路径
  MutableBufferView pg_view{out_par.data(), out_par.size(),
                             BufferType::kHostRegular};
  const auto t_p0 = std::chrono::steady_clock::now();
  auto get_p = client.GetObject(pg, pg_view);
  const auto t_p1 = std::chrono::steady_clock::now();
  EXPECT_OK(get_p, "parallel GetObject");
  EXPECT_TRUE(get_p.value().bytes_transferred == kSingleSize,
              "parallel GET size mismatch");
  if (CompareOrDump(out_par.data(), payload.data(), kSingleSize,
                    "parallel GET")) return 1;
  EXPECT_TRUE(ClientCrc(out_par) == expect_crc,
              "parallel GET crc mismatch after readback");
  const auto pg_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t_p1 - t_p0)
          .count();
  std::cout << "    OK: memcmp + crc consistent wall=" << pg_ms << "ms"
            << std::endl;

  // ============================================================
  // Section 5: Range GET 子串
  // ============================================================
  const std::uint64_t roff = kSingleSize / 4;
  const std::uint64_t rlen = kSingleSize / 2;
  std::cout << "[5] Range GET [" << roff << "+" << rlen << "]" << std::endl;
  std::vector<std::byte> out_range(rlen);
  RequestOptions rg;
  rg.object = obj_single;
  rg.offset = roff;
  rg.length = rlen;
  MutableBufferView rg_view{out_range.data(), out_range.size(),
                             BufferType::kHostRegular};
  auto get_r = client.GetObject(rg, rg_view);
  EXPECT_OK(get_r, "range GetObject");
  EXPECT_TRUE(get_r.value().bytes_transferred == rlen,
              "range GET size mismatch");
  if (CompareOrDump(out_range.data(), payload.data() + roff, rlen,
                    "range GET")) return 1;
  std::cout << "    OK: substring memcmp" << std::endl;

  // ============================================================
  // Section 6: multipart 8 part + 双向 CRC + Complete + GET memcmp
  // ============================================================
  std::cout << "[6] multipart (" << kMultipartSize << " bytes, "
            << kPartSize << " per part)" << std::endl;
  const ObjectId obj_mp{bucket, "verify/mp-" + ts};
  const std::size_t num_parts =
      (kMultipartSize + kPartSize - 1) / kPartSize;

  std::vector<std::vector<std::byte>> part_payloads(num_parts);
  std::vector<std::uint32_t>          expected_part_crcs(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    const std::size_t off = i * kPartSize;
    const std::size_t sz  = std::min(kPartSize, kMultipartSize - off);
    part_payloads[i].resize(sz);
    FillRandom(part_payloads[i].data(), sz, seed + i + 1);
    expected_part_crcs[i] = Crc32c(std::span<const std::byte>(
        part_payloads[i].data(), part_payloads[i].size()));
  }

  auto start = client.StartUpload(obj_mp, kMultipartSize);
  EXPECT_OK(start, "StartUpload");
  auto& upload = start.value();
  std::cout << "    StartUpload upload_id=" << upload.upload_id()
            << " num_parts=" << num_parts << std::endl;

  std::vector<MultipartUpload::PartSpec> specs;
  specs.reserve(num_parts);
  for (std::size_t i = 0; i < num_parts; ++i) {
    specs.push_back(MultipartUpload::PartSpec{
        .part_number = static_cast<std::uint32_t>(i + 1),
        .object_offset = i * kPartSize,
        .buffer = ConstBufferView{part_payloads[i].data(),
                                   part_payloads[i].size(),
                                   BufferType::kHostRegular},
    });
  }
  auto up = upload.UploadParts(specs, kPartConcurr);
  EXPECT_OK(up, "UploadParts");
  EXPECT_TRUE(up.value().size() == num_parts, "UploadParts wrong count");

  // 校验每个 part：server CRC 与 expected 一致。
  for (std::size_t i = 0; i < num_parts; ++i) {
    const auto& outcome = up.value()[i];
    EXPECT_TRUE(outcome.server_crc32c.has_value(),
                "part " + std::to_string(i + 1) + " missing server CRC");
    EXPECT_TRUE(*outcome.server_crc32c == expected_part_crcs[i],
                "part " + std::to_string(i + 1) + " CRC mismatch");
  }
  std::cout << "    OK: all " << num_parts << " parts CRC consistent" << std::endl;

  auto cmp = upload.Complete();
  EXPECT_OK(cmp, "Complete");
  EXPECT_TRUE(cmp.value().content_length == kMultipartSize,
              "Complete content_length mismatch");
  std::cout << "    Complete etag=" << cmp.value().etag
            << " size=" << cmp.value().content_length << std::endl;

  // 重组整对象 expected + GET 校验
  std::vector<std::byte> mp_expected(kMultipartSize);
  for (std::size_t i = 0; i < num_parts; ++i) {
    std::memcpy(mp_expected.data() + i * kPartSize,
                part_payloads[i].data(),
                part_payloads[i].size());
  }
  std::vector<std::byte> mp_got(kMultipartSize);
  RequestOptions mp_get;
  mp_get.object = obj_mp;
  MutableBufferView mp_view{mp_got.data(), mp_got.size(),
                             BufferType::kHostRegular};
  auto mp_get_r = client.GetObject(mp_get, mp_view);
  EXPECT_OK(mp_get_r, "multipart GET full");
  if (CompareOrDump(mp_got.data(), mp_expected.data(), kMultipartSize,
                    "multipart GET full")) return 1;
  EXPECT_TRUE(Crc32c(std::span<const std::byte>(mp_got.data(), mp_got.size()))
                  == Crc32c(std::span<const std::byte>(mp_expected.data(),
                                                          mp_expected.size())),
              "multipart full crc mismatch");
  std::cout << "    OK: multipart full memcmp + crc consistent" << std::endl;

  // ============================================================
  // Section 7: HEAD 不存在 key
  // ============================================================
  std::cout << "[7] HEAD missing key" << std::endl;
  auto missing = client.HeadObject(
      ObjectId{bucket, "verify/does-not-exist-" + ts});
  EXPECT_TRUE(!missing.success(), "HEAD missing should fail");
  const auto& mc = missing.error().code;
  EXPECT_TRUE(mc == ErrorCode::kNotFound || mc == ErrorCode::kControlPlaneError,
              "HEAD missing wrong code: " + std::to_string(static_cast<int>(mc)));
  std::cout << "    OK: rejected with code=" << static_cast<int>(mc) << std::endl;

  std::cout << "ALL OK" << std::endl;
  return 0;
}
