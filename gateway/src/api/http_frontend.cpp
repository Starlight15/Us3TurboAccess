#include "api/http_frontend.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <brpc/closure_guard.h>
#include <brpc/http_status_code.h>
#include <butil/endpoint.h>
#include <butil/iobuf.h>
#include <bvar/variable.h>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "api/conversions.h"
#include "common/range.h"
#include "core/metadata/metadata_service.h"
#include "core/multipart/multipart_app_service.h"
#include "data_path/http/http_executor.h"
#include "data_path/http/http_multipart_path_handler.h"
#include "common/error.h"
#include "common/metrics.h"

namespace us3_turbo_access::gateway::api {

namespace {

constexpr std::string_view kV1Prefix      = "/v1/objects/";
constexpr std::string_view kV0Prefix      = "/objects/";
constexpr std::string_view kUploadsPrefix = "/v1/uploads/";
constexpr std::string_view kCompleteSuffix = "/complete";

// Generate a simple request ID: timestamp + random hex
std::string GenerateRequestId() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  auto now = std::chrono::system_clock::now().time_since_epoch();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  auto rand = rng();

  std::ostringstream oss;
  oss << std::hex << ms << "-" << rand;
  return oss.str();
}

// RAII helper for access log (request end)
class ScopedAccessLog {
 public:
  ScopedAccessLog(brpc::Controller* cntl,
                  std::string_view method,
                  std::string_view path,
                  std::string_view request_id,
                  std::shared_ptr<spdlog::logger> logger)
      : cntl_(cntl),
        method_(method),
        path_(path),
        request_id_(request_id),
        logger_(std::move(logger)),
        start_(std::chrono::steady_clock::now()) {}

  ~ScopedAccessLog() {
    if (logger_ == nullptr) return;

    auto elapsed = std::chrono::steady_clock::now() - start_;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    auto status = cntl_->http_response().status_code();
    auto bytes = cntl_->response_attachment().size();

    logger_->info("http {} {} status={} bytes={} elapsed_ms={} request_id={}",
                  method_, path_, status, bytes, elapsed_ms, request_id_);
  }

  ScopedAccessLog(const ScopedAccessLog&) = delete;
  ScopedAccessLog& operator=(const ScopedAccessLog&) = delete;

 private:
  brpc::Controller* cntl_;
  std::string method_;
  std::string path_;
  std::string request_id_;
  std::shared_ptr<spdlog::logger> logger_;
  std::chrono::steady_clock::time_point start_;
};

std::string_view StripPrefix(std::string_view value, std::string_view prefix) {
  if (value.substr(0, prefix.size()) == prefix) {
    value.remove_prefix(prefix.size());
  }
  return value;
}

bool ParseObjectPath(std::string_view path, std::string* bucket,
                     std::string* key) {
  if (path.starts_with(kV1Prefix)) {
    path = StripPrefix(path, kV1Prefix);
  } else if (path.starts_with(kV0Prefix)) {
    path = StripPrefix(path, kV0Prefix);
  } else {
    return false;
  }
  const auto slash = path.find('/');
  if (slash == std::string_view::npos || slash == 0U ||
      slash + 1U >= path.size()) {
    return false;
  }
  *bucket = std::string(path.substr(0, slash));
  *key = std::string(path.substr(slash + 1U));
  return true;
}

// /v1/uploads/{bucket}/{key:*}   — POST 创建 upload
bool ParseUploadInitPath(std::string_view path, std::string* bucket,
                         std::string* key) {
  if (!path.starts_with(kUploadsPrefix)) return false;
  path.remove_prefix(kUploadsPrefix.size());
  const auto slash = path.find('/');
  if (slash == std::string_view::npos || slash == 0U ||
      slash + 1U >= path.size()) {
    return false;
  }
  *bucket = std::string(path.substr(0, slash));
  *key = std::string(path.substr(slash + 1U));
  return true;
}

// /v1/uploads/{upload_id}              — PUT (part) / DELETE (abort)
// /v1/uploads/{upload_id}/complete     — POST (complete)
// 第一段（去前缀后）就是 upload_id，没有 '/'（上层产生的 mpu-xx-xxxxxxx 形态）。
// upload_id 不可能含 '/'，所以 bucket/key 路径与 upload_id 路径用是否含 '/' 区分。
bool ParseUploadIdPath(std::string_view path, std::string* upload_id,
                       bool* has_complete_suffix) {
  if (!path.starts_with(kUploadsPrefix)) return false;
  path.remove_prefix(kUploadsPrefix.size());
  *has_complete_suffix = false;
  if (path.ends_with(kCompleteSuffix)) {
    *has_complete_suffix = true;
    path.remove_suffix(kCompleteSuffix.size());
  }
  if (path.empty() || path.find('/') != std::string_view::npos) {
    // 不是单段 upload_id（含 '/' 说明是 InitPath 的 bucket/key 形态）
    return false;
  }
  *upload_id = std::string(path);
  return true;
}

// 从 query 取 partNumber；返回 0 表示缺/非法。
std::uint32_t ParsePartNumber(const brpc::Controller* cntl) {
  const std::string* q = cntl->http_request().uri().GetQuery("partNumber");
  if (q == nullptr || q->empty()) return 0;
  try {
    long v = std::stol(*q);
    if (v < 0 || v > 0xFFFFFFFFL) return 0;
    return static_cast<std::uint32_t>(v);
  } catch (...) { return 0; }
}

// x-amz-checksum-crc32c 头（S3 兼容：base64 of big-endian uint32）。
std::optional<std::uint32_t> ParseCrc32cHeader(const brpc::Controller* cntl) {
  const auto* h = cntl->http_request().GetHeader("x-amz-checksum-crc32c");
  if (h == nullptr || h->empty()) return std::nullopt;
  // base64 解码（不依赖外部库；只接受标准 alphabet + '=' padding）。
  auto idx_of = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  std::uint32_t buf = 0;
  int bits = 0;
  for (char c : *h) {
    if (c == '=') break;
    int v = idx_of(c);
    if (v < 0) return std::nullopt;
    buf = (buf << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFFu));
    }
  }
  if (out.size() < 4) return std::nullopt;
  return (static_cast<std::uint32_t>(out[0]) << 24) |
         (static_cast<std::uint32_t>(out[1]) << 16) |
         (static_cast<std::uint32_t>(out[2]) << 8) |
         static_cast<std::uint32_t>(out[3]);
}

// 把 server 算出的 CRC32C 按 S3 约定 base64(big-endian u32) 写回响应头。
std::string EncodeCrc32cBase64(std::uint32_t crc) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const std::uint8_t b[4] = {
      static_cast<std::uint8_t>((crc >> 24) & 0xFF),
      static_cast<std::uint8_t>((crc >> 16) & 0xFF),
      static_cast<std::uint8_t>((crc >> 8) & 0xFF),
      static_cast<std::uint8_t>(crc & 0xFF),
  };
  std::string s;
  s.reserve(8);
  s.push_back(kAlphabet[b[0] >> 2]);
  s.push_back(kAlphabet[((b[0] & 0x03) << 4) | (b[1] >> 4)]);
  s.push_back(kAlphabet[((b[1] & 0x0F) << 2) | (b[2] >> 6)]);
  s.push_back(kAlphabet[b[2] & 0x3F]);
  s.push_back(kAlphabet[b[3] >> 2]);
  s.push_back(kAlphabet[(b[3] & 0x03) << 4]);
  s += "==";
  return s;
}

// 从 Content-Length 头解出客户端声明的 body 大小；解析失败或没头返回 nullopt。
// 实际收到的字节数由 cntl->request_attachment().size() 反映。
std::optional<std::uint64_t> ParseContentLengthHeader(const brpc::Controller* cntl) {
  const auto* h = cntl->http_request().GetHeader("Content-Length");
  if (h == nullptr || h->empty()) return std::nullopt;
  try {
    return static_cast<std::uint64_t>(std::stoull(*h));
  } catch (...) {
    return std::nullopt;
  }
}

void SetCommonHeaders(brpc::Controller* cntl, std::string_view gateway_id,
                      std::string_view transfer_status) {
  cntl->http_response().SetHeader("x-fa-gateway-id", std::string(gateway_id));
  cntl->http_response().SetHeader("x-fa-selected-gateway",
                                  std::string(gateway_id));
  cntl->http_response().SetHeader("x-fa-transfer-status",
                                  std::string(transfer_status));
}

void WriteJson(brpc::Controller* cntl, const nlohmann::json& body) {
  cntl->http_response().set_content_type("application/json");
  cntl->response_attachment().append(body.dump());
}

void WriteError(brpc::Controller* cntl, std::string_view gateway_id,
                const Error& err) {
  const int status = common::ToHttpStatus(err.code);
  cntl->http_response().set_status_code(status);
  SetCommonHeaders(cntl, gateway_id, "failed");
  cntl->http_response().SetHeader("x-fa-error-code", std::to_string(status));
  WriteJson(cntl, nlohmann::json{{"error", err.message},
                                 {"code", static_cast<int>(err.code)},
                                 {"retryable", err.retryable}});
}

}  // namespace

HttpFrontend::HttpFrontend(std::string gateway_id,
                           core::MetadataService& metadata,
                           data_path::http::HttpExecutor& http,
                           data_path::http::HttpMultipartPathHandler& multipart_handler,
                           core::multipart::MultipartAppService& multipart,
                           std::size_t max_put_bytes,
                           std::shared_ptr<spdlog::logger> logger)
    : gateway_id_(std::move(gateway_id)),
      metadata_(metadata),
      http_(http),
      multipart_handler_(multipart_handler),
      multipart_(multipart),
      max_put_bytes_(max_put_bytes),
      logger_(std::move(logger)) {}

void HttpFrontend::default_method(google::protobuf::RpcController* cntl_base,
                                   const ::us3_turbo_access::gateway::GatewayHttpRequest*,
                                   ::us3_turbo_access::gateway::GatewayHttpResponse*,
                                   google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  const auto method = cntl->http_request().method();
  const auto path = cntl->http_request().uri().path();

  // Generate request_id for tracing
  const auto request_id = GenerateRequestId();
  cntl->http_response().SetHeader("x-fa-request-id", request_id);

  // Access log: log at request start and end
  if (logger_ != nullptr) {
    logger_->info("http {} {} from {} request_id={}",
                  brpc::HttpMethod2Str(method), path,
                  butil::endpoint2str(cntl->remote_side()).c_str(),
                  request_id);
  }

  // RAII: log request end (status/bytes/elapsed) in destructor
  ScopedAccessLog access_log(cntl, brpc::HttpMethod2Str(method), path, request_id, logger_);

  if (method == brpc::HTTP_METHOD_GET &&
      (path == "/healthz" || path == "/v1/healthz")) {
    HandleHealth(cntl);
    return;
  }
  if (method == brpc::HTTP_METHOD_GET &&
      (path == "/vars" || path.starts_with("/vars/"))) {
    HandleVars(cntl, std::string(path));
    return;
  }

  // multipart 路由（V2）：/v1/uploads/...
  if (path.starts_with(kUploadsPrefix)) {
    // 1) POST /v1/uploads/{bucket}/{key}        → StartUpload
    if (method == brpc::HTTP_METHOD_POST) {
      std::string upload_id;
      bool has_complete = false;
      std::string bucket, key;
      if (ParseUploadIdPath(path, &upload_id, &has_complete) && has_complete) {
        HandleCompleteUpload(cntl, upload_id);
        return;
      }
      if (ParseUploadInitPath(path, &bucket, &key)) {
        HandleStartUpload(cntl, bucket, key);
        return;
      }
    }
    // 2) PUT /v1/uploads/{upload_id}?partNumber=N  → UploadPart
    if (method == brpc::HTTP_METHOD_PUT) {
      std::string upload_id;
      bool has_complete = false;
      if (ParseUploadIdPath(path, &upload_id, &has_complete) && !has_complete) {
        const auto pn = ParsePartNumber(cntl);
        if (pn == 0) {
          WriteError(cntl, gateway_id_,
                     common::MakeError(ErrorCode::kBadRequest,
                                      "PUT upload missing partNumber query"));
          return;
        }
        HandleUploadPart(cntl, upload_id, pn);
        return;
      }
    }
    // 3) DELETE /v1/uploads/{upload_id}            → AbortUpload
    if (method == brpc::HTTP_METHOD_DELETE) {
      std::string upload_id;
      bool has_complete = false;
      if (ParseUploadIdPath(path, &upload_id, &has_complete) && !has_complete) {
        HandleAbortUpload(cntl, upload_id);
        return;
      }
    }
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kBadRequest,
                                 "unsupported uploads route: " +
                                     std::string(brpc::HttpMethod2Str(method)) +
                                     " " + std::string(path)));
    return;
  }

  // 整对象路由：/v1/objects/{bucket}/{key:*}
  std::string bucket;
  std::string key;
  if (!ParseObjectPath(path, &bucket, &key)) {
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kNotFound,
                                "unsupported path: " + std::string(path)));
    return;
  }

  switch (method) {
    case brpc::HTTP_METHOD_HEAD:
      HandleHead(cntl, bucket, key);
      return;
    case brpc::HTTP_METHOD_GET:
      HandleGet(cntl, bucket, key);
      return;
    case brpc::HTTP_METHOD_PUT:
      HandlePut(cntl, bucket, key);
      return;
    default:
      cntl->http_response().SetHeader("Allow", "HEAD, GET, PUT");
      WriteError(cntl, gateway_id_,
                 common::MakeError(ErrorCode::kMethodNotAllowed, "method not allowed"));
      return;
  }
}

void HttpFrontend::HandleHealth(brpc::Controller* cntl) {
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "ok");
  WriteJson(cntl, nlohmann::json{{"status", "ok"},
                                  {"gateway_id", gateway_id_}});
}

void HttpFrontend::HandleVars(brpc::Controller* cntl, const std::string& path) {
  std::string filter;
  if (path.size() > 6 && path[5] == '/') {
    filter = path.substr(6);  // strip "/vars/"
  }

  std::vector<std::string> names;
  bvar::Variable::list_exposed(&names);
  std::ostringstream body;
  for (const auto& name : names) {
    if (!filter.empty() && name.find(filter) == std::string::npos) {
      continue;
    }
    std::ostringstream value;
    if (bvar::Variable::describe_exposed(name, value) != 0) {
      continue;
    }
    body << name << " : " << value.str() << "\n";
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  cntl->http_response().set_content_type("text/plain");
  SetCommonHeaders(cntl, gateway_id_, "ok");
  cntl->response_attachment().append(body.str());
}

void HttpFrontend::HandleHead(brpc::Controller* cntl, const std::string& bucket,
                              const std::string& key) {
  common::ScopedLatency latency(common::metrics().http_head_latency_us);
  common::ScopedHttpInflight inflight(common::metrics().http_head_inflight,
                                       common::metrics().http_head_inflight_aborted_total);
  auto head = metadata_.Head(bucket, key);
  if (!head.success()) {
    common::metrics().http_head_fail_total << 1;
    inflight.MarkAborted();
    WriteError(cntl, gateway_id_, head.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("Content-Length",
                                  std::to_string(head.value().content_length));
  cntl->http_response().SetHeader("ETag", head.value().etag);
  cntl->http_response().SetHeader("x-fa-version", head.value().version);
  common::metrics().http_head_total << 1;
}

void HttpFrontend::HandleGet(brpc::Controller* cntl, const std::string& bucket,
                             const std::string& key) {
  common::ScopedLatency latency(common::metrics().http_get_latency_us);
  common::ScopedHttpInflight inflight(common::metrics().http_get_inflight,
                                       common::metrics().http_get_inflight_aborted_total);
  auto head = metadata_.Head(bucket, key);
  if (!head.success()) {
    common::metrics().http_get_fail_total << 1;
    inflight.MarkAborted();
    WriteError(cntl, gateway_id_, head.error());
    return;
  }
  const auto* range_header = cntl->http_request().GetHeader("Range");
  const auto range =
      common::ParseHttpRange(range_header, head.value().content_length);
  if (range.unsatisfiable) {
    common::metrics().http_get_fail_total << 1;
    inflight.MarkAborted();
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kRangeNotSatisfiable,
                                "range not satisfiable"));
    return;
  }
  data_path::http::HttpResponseSink sink{cntl};
  auto report = http_.Get(bucket, key, range.offset, range.length, sink);
  if (!report.success()) {
    common::metrics().http_get_fail_total << 1;
    inflight.MarkAborted();
    WriteError(cntl, gateway_id_, report.error());
    return;
  }
  cntl->http_response().set_status_code(range.partial ? brpc::HTTP_STATUS_PARTIAL_CONTENT
                                                       : brpc::HTTP_STATUS_OK);
  cntl->http_response().set_content_type("application/octet-stream");
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader(
      "Content-Length", std::to_string(report.value().bytes_transferred));
  cntl->http_response().SetHeader("ETag", head.value().etag);
  cntl->http_response().SetHeader("x-fa-version", head.value().version);
  // 写 server 算出的 CRC32C 让 client 做 end-to-end 校验（与 PUT 路径对称）。
  if (report.value().has_crc32c) {
    cntl->http_response().SetHeader(
        "x-amz-checksum-crc32c", EncodeCrc32cBase64(report.value().crc32c));
  }
  if (range.partial && report.value().bytes_transferred != 0U) {
    cntl->http_response().SetHeader(
        "Content-Range",
        "bytes " + std::to_string(range.offset) + "-" +
            std::to_string(range.offset + report.value().bytes_transferred - 1U) +
            "/" + std::to_string(head.value().content_length));
  }
  common::metrics().http_get_total << 1;
  common::metrics().http_get_bytes
      << static_cast<std::int64_t>(report.value().bytes_transferred);
}

void HttpFrontend::HandlePut(brpc::Controller* cntl, const std::string& bucket,
                             const std::string& key) {
  common::ScopedLatency latency(common::metrics().http_put_latency_us);
  common::ScopedHttpInflight inflight(common::metrics().http_put_inflight,
                                       common::metrics().http_put_inflight_aborted_total);

  // 413 上限：先看 Content-Length 声明（若有），再以实际收到的 attachment.size()
  // 兜底。两者任一超过 max_put_bytes_ 即拒。max_put_bytes_=0 表示不限。
  const auto declared = ParseContentLengthHeader(cntl);
  const std::uint64_t actual = cntl->request_attachment().size();
  if (max_put_bytes_ != 0 &&
      ((declared.has_value() && *declared > max_put_bytes_) ||
       actual > max_put_bytes_)) {
    common::metrics().http_put_fail_total << 1;
    inflight.MarkAborted();
    const std::uint64_t shown = declared.value_or(actual);
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kPayloadTooLarge,
                                 "PUT body " + std::to_string(shown) +
                                     " exceeds http_max_put_bytes " +
                                     std::to_string(max_put_bytes_) +
                                     "; use multipart upload"));
    return;
  }

  // 零拷贝：直接传递 IOBuf，避免 to_string() 内存拷贝
  const auto& body = cntl->request_attachment();
  auto expected_crc = ParseCrc32cHeader(cntl);
  auto report = http_.Put(bucket, key, body, expected_crc);
  if (!report.success()) {
    common::metrics().http_put_fail_total << 1;
    inflight.MarkAborted();
    WriteError(cntl, gateway_id_, report.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("ETag", report.value().meta.etag);
  cntl->http_response().SetHeader("x-fa-version", report.value().meta.version);
  if (report.value().has_crc32c) {
    cntl->http_response().SetHeader(
        "x-amz-checksum-crc32c", EncodeCrc32cBase64(report.value().crc32c));
  }
  nlohmann::json resp;
  resp["etag"] = report.value().meta.etag;
  resp["version"] = report.value().meta.version;
  resp["bytes_written"] = report.value().bytes_transferred;
  WriteJson(cntl, resp);
  common::metrics().http_put_total << 1;
  common::metrics().http_put_bytes
      << static_cast<std::int64_t>(report.value().bytes_transferred);
}

// POST /v1/uploads/{bucket}/{key}  → 创建 multipart upload。
void HttpFrontend::HandleStartUpload(brpc::Controller* cntl,
                                       const std::string& bucket,
                                       const std::string& key) {
  // 通过 MultipartAppService 发起上传，与控制面 baidu_std multipart 共用同一后端状态。
  core::multipart::StartUploadParams params;
  params.bucket = bucket;
  params.object_key = key;
  params.data_path = "http-tcp";
  // expected_total_size / idempotency_key 走 query string（可选，本期不强求）。
  if (const auto* q = cntl->http_request().uri().GetQuery("expected_total_size");
      q != nullptr && !q->empty()) {
    try { params.expected_total_size = std::stoull(*q); } catch (...) {}
  }
  if (const auto* q = cntl->http_request().uri().GetQuery("idempotency_key");
      q != nullptr) {
    params.idempotency_key = *q;
  }

  auto out = multipart_.StartUpload(params);
  if (!out.success()) {
    WriteError(cntl, gateway_id_, out.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  nlohmann::json resp;
  resp["upload_id"] = out.value().upload_id;
  resp["max_part_size"] = out.value().max_part_size;
  WriteJson(cntl, resp);
}

// PUT /v1/uploads/{upload_id}?partNumber=N
void HttpFrontend::HandleUploadPart(brpc::Controller* cntl,
                                     const std::string& upload_id,
                                     std::uint32_t part_number) {
  // 413 上限：multipart part 与单对象 PUT 共用 http_max_put_bytes_
  const auto declared = ParseContentLengthHeader(cntl);
  const std::uint64_t actual = cntl->request_attachment().size();
  if (max_put_bytes_ != 0 &&
      ((declared.has_value() && *declared > max_put_bytes_) ||
       actual > max_put_bytes_)) {
    const std::uint64_t shown = declared.value_or(actual);
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kPayloadTooLarge,
                                 "UploadPart body " + std::to_string(shown) +
                                     " exceeds http_max_put_bytes " +
                                     std::to_string(max_put_bytes_)));
    return;
  }

  // 零拷贝：直接传递 IOBuf
  const auto& body = cntl->request_attachment();
  auto expected_crc = ParseCrc32cHeader(cntl);
  auto report = multipart_handler_.UploadPart(upload_id, part_number, body, expected_crc);
  if (!report.success()) {
    WriteError(cntl, gateway_id_, report.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("ETag", report.value().meta.etag);
  if (report.value().has_crc32c) {
    cntl->http_response().SetHeader(
        "x-amz-checksum-crc32c", EncodeCrc32cBase64(report.value().crc32c));
  }
  nlohmann::json resp;
  resp["part_etag"] = report.value().meta.etag;
  resp["bytes_written"] = report.value().bytes_transferred;
  WriteJson(cntl, resp);
}

// POST /v1/uploads/{upload_id}/complete   body = {"parts":[{"part_number":N,"etag":"..."}, ...]}
void HttpFrontend::HandleCompleteUpload(brpc::Controller* cntl,
                                         const std::string& upload_id) {
  const auto raw = cntl->request_attachment().to_string();
  std::vector<backend::PartRecord> parts;
  try {
    auto j = nlohmann::json::parse(raw);
    parts = ToPartRecords(j.at("parts"));
  } catch (const std::exception& e) {
    WriteError(cntl, gateway_id_, common::MakeError(
        ErrorCode::kBadRequest,
        std::string("CompleteUpload body parse failed: ") + e.what()));
    return;
  }

  auto meta = multipart_.CompleteUpload(upload_id, parts, "http-tcp");
  if (!meta.success()) {
    WriteError(cntl, gateway_id_, meta.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("ETag", meta.value().etag);
  cntl->http_response().SetHeader("x-fa-version", meta.value().version);
  nlohmann::json resp;
  resp["etag"]           = meta.value().etag;
  resp["version"]        = meta.value().version;
  resp["content_length"] = meta.value().content_length;
  WriteJson(cntl, resp);
}

// DELETE /v1/uploads/{upload_id}
void HttpFrontend::HandleAbortUpload(brpc::Controller* cntl,
                                      const std::string& upload_id) {
  auto out = multipart_.AbortUpload(upload_id, "http-tcp");
  if (!out.success()) {
    WriteError(cntl, gateway_id_, out.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  WriteJson(cntl, nlohmann::json{{"aborted", true}});
}

}  // namespace us3_turbo_access::gateway::api
