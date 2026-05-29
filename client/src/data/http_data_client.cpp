#include "client/src/data/http_data_client.h"

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <brpc/controller.h>
#include <brpc/http_status_code.h>
#include <butil/iobuf.h>
#include <nlohmann/json.hpp>

#include "client/src/core/common/brpc_channel.h"  // ApplyRequestHeaders / CheckRpcFailure
#include "client/src/core/common/errors.h"
#include "client/src/data/http_crc32c.h"
#include "client/src/data/http_retry.h"

namespace us3_turbo_access::client {

namespace {

constexpr DataPath kPath = DataPath::kHttpTcp;

// 把 key=value 拼到 uri 上，第一个加 '?'，后续加 '&'。uri 末尾扫一遍判断
// 是否已经带过 '?'，避免维护额外 first_q 状态。
void AppendQueryParam(std::string& uri, std::string_view key,
                       std::string_view value) {
  uri += (uri.find('?') == std::string::npos) ? '?' : '&';
  uri.append(key);
  uri += '=';
  uri.append(value);
}

RpcCallMetadata MakeContext(const ClientOptions& options) {
  return RpcCallMetadata{
      .client_id       = options.client_id,
      .bearer_token    = options.bearer_token,
      .default_headers = options.default_headers,
      .timeout         = options.default_timeout,
  };
}

ErrorCode HttpStatusToCode(int status) {
  // server 端已经通过 common::ToHttpStatus 把 ErrorCode 映射成了 HTTP 码；
  // 这里做反向兜底，且优先 trust x-fa-error-code 头（在调用方读）。
  switch (status) {
    case brpc::HTTP_STATUS_NOT_FOUND:                return ErrorCode::kNotFound;
    case brpc::HTTP_STATUS_BAD_REQUEST:              return ErrorCode::kBadRequest;
    case brpc::HTTP_STATUS_REQUEST_RANGE_NOT_SATISFIABLE:
                                                       return ErrorCode::kRangeNotSatisfiable;
    case brpc::HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE:  return ErrorCode::kPayloadTooLarge;
    case brpc::HTTP_STATUS_CONFLICT:                  return ErrorCode::kStaleState;
    case 507:  // HTTP_STATUS_INSUFFICIENT_STORAGE (brpc 未定义此常量)
                                                       return ErrorCode::kCapacityExceeded;
    case brpc::HTTP_STATUS_UNAUTHORIZED:              return ErrorCode::kTicketInvalid;
    case brpc::HTTP_STATUS_FORBIDDEN:                return ErrorCode::kControlPlaneError;
    case brpc::HTTP_STATUS_BAD_GATEWAY:               return ErrorCode::kBackendUnavailable;
    case brpc::HTTP_STATUS_INTERNAL_SERVER_ERROR:    return ErrorCode::kInternal;
    case brpc::HTTP_STATUS_SERVICE_UNAVAILABLE:
    case brpc::HTTP_STATUS_GATEWAY_TIMEOUT:          return ErrorCode::kBackendUnavailable;
    default:
      return (status >= 500) ? ErrorCode::kInternal : ErrorCode::kBadRequest;
  }
}

// 取 x-fa-error-code 头（server 写的实际是 HTTP status，但我们仍优先读它，
// 给将来 server 直接写 ErrorCode 数值留余地）。
std::optional<int> ParseErrorCodeHeader(const brpc::Controller& cntl) {
  const auto* h = cntl.http_response().GetHeader("x-fa-error-code");
  if (h == nullptr || h->empty()) return std::nullopt;
  try { return std::stoi(*h); } catch (...) { return std::nullopt; }
}

// 从 server 错误响应 body 抽 "error" 文案（gateway/src/api/http_frontend.cpp::WriteError 的格式）。
std::string ExtractErrorMessage(const brpc::Controller& cntl) {
  const auto body = cntl.response_attachment().to_string();
  if (body.empty()) return {};
  try {
    auto j = nlohmann::json::parse(body);
    if (j.contains("error") && j["error"].is_string()) {
      return j["error"].get<std::string>();
    }
  } catch (...) {}
  return body;  // 不是 JSON 就原样回显（短）
}

Error MapHttpFailure(const brpc::Controller& cntl, std::string_view fallback) {
  const int status = cntl.http_response().status_code();
  ErrorCode code = HttpStatusToCode(status);
  // x-fa-error-code 头：server 当前写的是 HTTP status code（与 status 同值），
  // 但优先读它意味着未来 server 切到写真正的 ErrorCode 数值时本端不需要改。
  if (auto raw = ParseErrorCodeHeader(cntl); raw.has_value()) {
    // 头里也是 HTTP code 时，HttpStatusToCode 已处理；否则按枚举值收。
    if (*raw >= 0 && *raw <= static_cast<int>(ErrorCode::kPayloadTooLarge)) {
      code = static_cast<ErrorCode>(*raw);
    }
  }
  std::string msg = ExtractErrorMessage(cntl);
  if (msg.empty()) {
    msg = std::string(fallback) + " (HTTP " + std::to_string(status) + ")";
  }
  const bool retryable = (status >= 500);
  return MakeError(code, std::move(msg), retryable);
}

// 从响应头取 server 算出的 CRC32C。解码失败 / 头缺失返回 nullopt（仍走成功路径
// 但 PutReport.server_crc32c 留空，上层无法做双向校验）。
std::optional<std::uint32_t> ExtractServerCrc32c(const brpc::Controller& cntl) {
  const auto* h = cntl.http_response().GetHeader("x-amz-checksum-crc32c");
  if (h == nullptr || h->empty()) return std::nullopt;
  return DecodeBase64Crc32cBigEndian(*h);
}

// 把 response 里的常用头抽到 ObjectMetadata，其它头原样收进 headers。
ObjectMetadata ExtractMeta(const brpc::Controller& cntl) {
  ObjectMetadata meta;
  if (const auto* h = cntl.http_response().GetHeader("Content-Length"); h != nullptr) {
    try { meta.content_length = static_cast<std::size_t>(std::stoull(*h)); }
    catch (...) {}
  }
  if (const auto* h = cntl.http_response().GetHeader("ETag"); h != nullptr) {
    meta.etag = *h;
  }
  if (const auto* h = cntl.http_response().GetHeader("x-fa-version"); h != nullptr) {
    meta.version = *h;
  }
  // 全量头收进 headers（便于上层做诊断）。
  for (auto it = cntl.http_response().HeaderBegin();
       it != cntl.http_response().HeaderEnd(); ++it) {
    meta.headers.emplace(it->first, it->second);
  }
  return meta;
}

}  // namespace

HttpDataClient::HttpDataClient(const ClientOptions& options) : channel_(options) {}

Result<bool> HttpDataClient::Initialize() { return channel_.Initialize(); }
void HttpDataClient::Shutdown() { channel_.Shutdown(); }
bool HttpDataClient::initialized() const { return channel_.ready(); }

Result<ObjectMetadata> HttpDataClient::HeadObjectOnce(const ObjectId& object) const {
  if (!initialized()) {
    return Result<ObjectMetadata>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (object.bucket.empty() || object.key.empty()) {
    return Result<ObjectMetadata>::Failure(MakeInvalidArgument(
        "HeadObject requires non-empty bucket and key"));
  }
  brpc::Controller cntl;
  cntl.http_request().uri() = BuildObjectUri(channel_.options().endpoint,
                                                object.bucket, object.key);
  cntl.http_request().set_method(brpc::HTTP_METHOD_HEAD);
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

  auto rpc = CheckRpcFailure(cntl, "HEAD object failed", kPath, /*request_id=*/"");
  if (!rpc.success()) return Result<ObjectMetadata>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<ObjectMetadata>::Failure(MapHttpFailure(cntl, "HEAD object"));
  }
  return Result<ObjectMetadata>::Success(ExtractMeta(cntl));
}

Result<HttpDataClient::GetReport> HttpDataClient::GetObjectOnce(
    const ObjectId& object, std::uint64_t offset,
    std::optional<std::uint64_t> length, MutableBufferView buffer) const {
  if (!initialized()) {
    return Result<GetReport>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (object.bucket.empty() || object.key.empty()) {
    return Result<GetReport>::Failure(MakeInvalidArgument(
        "GetObject requires non-empty bucket and key"));
  }
  if (buffer.data == nullptr || buffer.size == 0) {
    return Result<GetReport>::Failure(MakeInvalidArgument(
        "GetObject buffer is empty or null"));
  }
  // 显式给 length=0 ≠ 不给 length。length=0 表示"读 0 字节"，直接返回成功；
  // 不给 length 走整对象路径。
  if (length.has_value() && *length == 0) {
    GetReport empty;
    return Result<GetReport>::Success(std::move(empty));
  }

  brpc::Controller cntl;
  cntl.http_request().uri() = BuildObjectUri(channel_.options().endpoint,
                                                object.bucket, object.key);
  cntl.http_request().set_method(brpc::HTTP_METHOD_GET);
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  // Range：只在调用方显式给 length 时附加（即使 offset>0 单独 offset 也按整对象处理，
  // 上层路径 PutObject/GetObject 都按 length 来配 Range）。
  if (length.has_value() && *length > 0) {
    const std::uint64_t last = offset + *length - 1;
    cntl.http_request().SetHeader(
        "Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(last));
  } else if (offset > 0) {
    cntl.http_request().SetHeader(
        "Range", "bytes=" + std::to_string(offset) + "-");
  }
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

  auto rpc = CheckRpcFailure(cntl, "GET object failed", kPath, "");
  if (!rpc.success()) return Result<GetReport>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<GetReport>::Failure(MapHttpFailure(cntl, "GET object"));
  }

  // 把 attachment 拷进 buffer：用 IOBuf::copy_to 避免一次 to_string() 中转。
  const auto& att = cntl.response_attachment();
  const std::size_t n_total = att.size();
  if (n_total > buffer.size) {
    return Result<GetReport>::Failure(MakeError(
        ErrorCode::kInvalidArgument,
        "GET response exceeds buffer.size: got " + std::to_string(n_total) +
            " > buffer " + std::to_string(buffer.size)));
  }
  if (n_total > 0) {
    const std::size_t copied = att.copy_to(buffer.data, n_total, /*pos=*/0);
    if (copied != n_total) {
      return Result<GetReport>::Failure(MakeError(
          ErrorCode::kInternal,
          "IOBuf::copy_to short copy: " + std::to_string(copied) + "/" +
              std::to_string(n_total)));
    }
  }
  GetReport out;
  out.bytes = n_total;
  out.meta  = ExtractMeta(cntl);
  return Result<GetReport>::Success(std::move(out));
}

Result<HttpDataClient::PutReport> HttpDataClient::PutObjectOnce(
    const ObjectId& object, ConstBufferView buffer,
    std::optional<std::uint32_t> crc32c) const {
  if (!initialized()) {
    return Result<PutReport>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (object.bucket.empty() || object.key.empty()) {
    return Result<PutReport>::Failure(MakeInvalidArgument(
        "PutObject requires non-empty bucket and key"));
  }
  if (buffer.data == nullptr && buffer.size != 0) {
    return Result<PutReport>::Failure(MakeInvalidArgument("PutObject buffer.data is null"));
  }

  brpc::Controller cntl;
  cntl.http_request().uri() = BuildObjectUri(channel_.options().endpoint,
                                                object.bucket, object.key);
  cntl.http_request().set_method(brpc::HTTP_METHOD_PUT);
  cntl.http_request().set_content_type("application/octet-stream");
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  if (crc32c.has_value()) {
    cntl.http_request().SetHeader("x-amz-checksum-crc32c",
                                   Base64Crc32cBigEndian(*crc32c));
  }
  if (buffer.size > 0) {
    // 零拷贝：把用户 buffer 直接挂到 IOBuf 上（PutObjectOnce 是同步调用，
    // CallMethod 返回前 buffer 一定存活；brpc max_retry=0 也不会再次重发用同
    // 一份 buffer，所以 IOBuf 引用计数走到 0 时 deleter 是 no-op 即可）。
    cntl.request_attachment().append_user_data(
        const_cast<void*>(buffer.data), buffer.size, [](void*) {});
  }
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

  auto rpc = CheckRpcFailure(cntl, "PUT object failed", kPath, "");
  if (!rpc.success()) return Result<PutReport>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<PutReport>::Failure(MapHttpFailure(cntl, "PUT object"));
  }

  PutReport out;
  out.bytes         = buffer.size;
  out.meta          = ExtractMeta(cntl);
  out.server_crc32c = ExtractServerCrc32c(cntl);
  return Result<PutReport>::Success(std::move(out));
}

namespace {

// 构造 multipart URI：/v1/uploads/{bucket}/{key}（init）或 /v1/uploads/{upload_id}[suffix]。
// 沿用 BuildObjectUri 的 percent-encode 规则（key 保留 '/'）。
std::string BuildUploadInitUri(const std::string& endpoint,
                                 const std::string& bucket,
                                 const std::string& key) {
  // 复用 transports/http 里的 percent-encode 逻辑：BuildObjectUri 已经处理过同样的 bucket/key 路径形态，
  // 这里只是前缀不一样。
  std::string uri = BuildObjectUri(endpoint, bucket, key);
  // 把 "/v1/objects/" 换成 "/v1/uploads/"
  constexpr std::string_view kObj    = "/v1/objects/";
  constexpr std::string_view kUpload = "/v1/uploads/";
  const auto pos = uri.find(kObj);
  if (pos != std::string::npos) {
    uri.replace(pos, kObj.size(), kUpload);
  }
  return uri;
}

std::string BuildUploadIdUri(const std::string& endpoint,
                              const std::string& upload_id,
                              std::string_view suffix /* "" or "/complete" */) {
  // upload_id 是服务端生成的内部 id（mpu-{shard}-{hex}），不含特殊字符，直接拼。
  std::string uri = "http://";
  uri += endpoint;
  uri += "/v1/uploads/";
  uri += upload_id;
  uri += suffix;
  return uri;
}

}  // namespace

Result<HttpDataClient::StartUploadResp> HttpDataClient::StartUploadOnce(
    const ObjectId& object, std::uint64_t expected_total_size,
    const std::string& idempotency_key) const {
  if (!initialized()) {
    return Result<StartUploadResp>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (object.bucket.empty() || object.key.empty()) {
    return Result<StartUploadResp>::Failure(MakeInvalidArgument(
        "StartUpload requires non-empty bucket and key"));
  }
  brpc::Controller cntl;
  std::string uri = BuildUploadInitUri(channel_.options().endpoint,
                                         object.bucket, object.key);
  // query 串：expected_total_size + idempotency_key（按需）
  if (expected_total_size > 0) {
    AppendQueryParam(uri, "expected_total_size",
                      std::to_string(expected_total_size));
  }
  if (!idempotency_key.empty()) {
    AppendQueryParam(uri, "idempotency_key", idempotency_key);
  }
  cntl.http_request().uri() = uri;
  cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
  auto rpc = CheckRpcFailure(cntl, "StartUpload failed", kPath, "");
  if (!rpc.success()) return Result<StartUploadResp>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<StartUploadResp>::Failure(MapHttpFailure(cntl, "StartUpload"));
  }
  StartUploadResp out;
  try {
    const auto body = cntl.response_attachment().to_string();
    auto j = nlohmann::json::parse(body);
    out.upload_id     = j.at("upload_id").get<std::string>();
    out.max_part_size = j.value("max_part_size", 0ULL);
  } catch (const std::exception& e) {
    return Result<StartUploadResp>::Failure(MakeError(
        ErrorCode::kSerializationError,
        std::string("StartUpload body parse failed: ") + e.what()));
  }
  return Result<StartUploadResp>::Success(std::move(out));
}

Result<HttpDataClient::PartEtag> HttpDataClient::UploadPartOnce(
    const std::string& upload_id, std::uint32_t part_number,
    ConstBufferView buffer, std::optional<std::uint32_t> crc32c) const {
  if (!initialized()) {
    return Result<PartEtag>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (upload_id.empty()) {
    return Result<PartEtag>::Failure(MakeInvalidArgument(
        "UploadPart requires non-empty upload_id"));
  }
  if (part_number == 0) {
    return Result<PartEtag>::Failure(MakeInvalidArgument(
        "UploadPart requires part_number >= 1"));
  }
  if (buffer.data == nullptr && buffer.size != 0) {
    return Result<PartEtag>::Failure(MakeInvalidArgument("UploadPart buffer.data is null"));
  }
  brpc::Controller cntl;
  std::string uri = BuildUploadIdUri(channel_.options().endpoint, upload_id, "");
  uri += "?partNumber=";
  uri += std::to_string(part_number);
  cntl.http_request().uri() = uri;
  cntl.http_request().set_method(brpc::HTTP_METHOD_PUT);
  cntl.http_request().set_content_type("application/octet-stream");
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  if (crc32c.has_value()) {
    cntl.http_request().SetHeader("x-amz-checksum-crc32c",
                                   Base64Crc32cBigEndian(*crc32c));
  }
  if (buffer.size > 0) {
    // 同 PutObjectOnce：零拷贝挂 user buffer，调用同步等 CallMethod 返回。
    cntl.request_attachment().append_user_data(
        const_cast<void*>(buffer.data), buffer.size, [](void*) {});
  }
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
  auto rpc = CheckRpcFailure(cntl, "UploadPart failed", kPath, "");
  if (!rpc.success()) return Result<PartEtag>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<PartEtag>::Failure(MapHttpFailure(cntl, "UploadPart"));
  }
  PartEtag out;
  out.part_number   = part_number;
  out.server_crc32c = ExtractServerCrc32c(cntl);
  try {
    const auto body = cntl.response_attachment().to_string();
    auto j = nlohmann::json::parse(body);
    out.etag = j.at("part_etag").get<std::string>();
  } catch (const std::exception& e) {
    return Result<PartEtag>::Failure(MakeError(
        ErrorCode::kSerializationError,
        std::string("UploadPart body parse failed: ") + e.what()));
  }
  return Result<PartEtag>::Success(std::move(out));
}

Result<HttpDataClient::CompleteUploadResp> HttpDataClient::CompleteUploadOnce(
    const std::string& upload_id,
    const std::vector<PartEtag>& parts) const {
  if (!initialized()) {
    return Result<CompleteUploadResp>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (upload_id.empty()) {
    return Result<CompleteUploadResp>::Failure(MakeInvalidArgument(
        "CompleteUpload requires non-empty upload_id"));
  }
  if (parts.empty()) {
    return Result<CompleteUploadResp>::Failure(MakeInvalidArgument(
        "CompleteUpload requires at least one part"));
  }
  brpc::Controller cntl;
  cntl.http_request().uri() = BuildUploadIdUri(channel_.options().endpoint,
                                                  upload_id, "/complete");
  cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
  cntl.http_request().set_content_type("application/json");
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));

  nlohmann::json body;
  body["parts"] = nlohmann::json::array();
  for (const auto& p : parts) {
    body["parts"].push_back({
        {"part_number", p.part_number},
        {"etag",        p.etag},
    });
  }
  const auto body_str = body.dump();
  cntl.request_attachment().append(body_str.data(), body_str.size());
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
  auto rpc = CheckRpcFailure(cntl, "CompleteUpload failed", kPath, "");
  if (!rpc.success()) return Result<CompleteUploadResp>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<CompleteUploadResp>::Failure(MapHttpFailure(cntl, "CompleteUpload"));
  }
  CompleteUploadResp out;
  try {
    const auto resp_body = cntl.response_attachment().to_string();
    auto j = nlohmann::json::parse(resp_body);
    out.etag           = j.value("etag", std::string{});
    out.version        = j.value("version", std::string{});
    out.content_length = static_cast<std::size_t>(j.value("content_length", 0ULL));
  } catch (const std::exception& e) {
    return Result<CompleteUploadResp>::Failure(MakeError(
        ErrorCode::kSerializationError,
        std::string("CompleteUpload body parse failed: ") + e.what()));
  }
  return Result<CompleteUploadResp>::Success(std::move(out));
}

Result<bool> HttpDataClient::AbortUploadOnce(const std::string& upload_id) const {
  if (!initialized()) {
    return Result<bool>::Failure(MakeNotInitialized("HTTP data client"));
  }
  if (upload_id.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument(
        "AbortUpload requires non-empty upload_id"));
  }
  brpc::Controller cntl;
  cntl.http_request().uri() = BuildUploadIdUri(channel_.options().endpoint,
                                                  upload_id, "");
  cntl.http_request().set_method(brpc::HTTP_METHOD_DELETE);
  ApplyRequestHeaders(cntl, MakeContext(channel_.options()));
  channel_.channel()->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
  auto rpc = CheckRpcFailure(cntl, "AbortUpload failed", kPath, "");
  if (!rpc.success()) return Result<bool>::Failure(rpc.error());

  const int status = cntl.http_response().status_code();
  if (status < 200 || status >= 300) {
    return Result<bool>::Failure(MapHttpFailure(cntl, "AbortUpload"));
  }
  return Result<bool>::Success(true);
}

// ---------------- public retry wrappers ----------------
// 幂等方法（HEAD/GET/PUT/StartUpload/UploadPart/AbortUpload）走 RetryIfRetryable；
// CompleteUpload 不重试（max_attempts=1）：part_number+etag 决定的"成功一次"
// 在中间结果不确定下重试可能造成"已 Complete 又 Complete"的歧义。
//
// 下面 7 处 [&]{ return XxxOnce(...); } 属于 docs/code-review-process.md
// §4.3 例外类别 1（STL 风格 nullary 谓词 + 单语句 + 仅本翻译单元内单点
// 使用），允许保留，避免给每个 Once 方法多套一层 binder struct。

Result<ObjectMetadata> HttpDataClient::HeadObject(const ObjectId& object) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
                            [&] { return HeadObjectOnce(object); });
}

Result<HttpDataClient::GetReport> HttpDataClient::GetObject(
    const ObjectId& object, std::uint64_t offset,
    std::optional<std::uint64_t> length, MutableBufferView buffer) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
      [&] { return GetObjectOnce(object, offset, length, buffer); });
}

Result<HttpDataClient::PutReport> HttpDataClient::PutObject(
    const ObjectId& object, ConstBufferView buffer,
    std::optional<std::uint32_t> crc32c) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
      [&] { return PutObjectOnce(object, buffer, crc32c); });
}

Result<HttpDataClient::StartUploadResp> HttpDataClient::StartUpload(
    const ObjectId& object, std::uint64_t expected_total_size,
    const std::string& idempotency_key) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
      [&] { return StartUploadOnce(object, expected_total_size, idempotency_key); });
}

Result<HttpDataClient::PartEtag> HttpDataClient::UploadPart(
    const std::string& upload_id, std::uint32_t part_number,
    ConstBufferView buffer, std::optional<std::uint32_t> crc32c) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
      [&] { return UploadPartOnce(upload_id, part_number, buffer, crc32c); });
}

Result<HttpDataClient::CompleteUploadResp> HttpDataClient::CompleteUpload(
    const std::string& upload_id,
    const std::vector<PartEtag>& parts) const {
  // CompleteUpload 单独走 max_attempts=1：server 端 Complete 不是天然幂等
  // （已 Complete 的 upload_id 重做会撞 kStaleState）。retryable=true 的
  // 5xx 重试可能让 client 看到 "已成功但又失败" 的歧义状态，所以禁用重试，
  // 让上层（MultipartUpload）做更准确的失败处理（best-effort AbortUpload）。
  RetryPolicy p;
  p.max_attempts = 1;
  return RetryIfRetryable(p,
      [&] { return CompleteUploadOnce(upload_id, parts); });
}

Result<bool> HttpDataClient::AbortUpload(const std::string& upload_id) const {
  return RetryIfRetryable(MakeRetryPolicy(channel_.options().http),
      [&] { return AbortUploadOnce(upload_id); });
}

}  // namespace us3_turbo_access::client
