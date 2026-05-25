#include "api/http_frontend.h"

#include <cstdint>
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

#include "common/range.h"
#include "core/metadata/metadata_service.h"
#include "data_path/http/http_executor.h"
#include "common/error.h"

namespace us3_turbo_access::gateway::api {

namespace {

constexpr std::string_view kV1Prefix = "/v1/objects/";
constexpr std::string_view kV0Prefix = "/objects/";

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
                           std::shared_ptr<spdlog::logger> logger)
    : gateway_id_(std::move(gateway_id)),
      metadata_(metadata),
      http_(http),
      logger_(std::move(logger)) {}

void HttpFrontend::default_method(google::protobuf::RpcController* cntl_base,
                                   const ::us3_turbo_access::gateway::GatewayHttpRequest*,
                                   ::us3_turbo_access::gateway::GatewayHttpResponse*,
                                   google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  const auto method = cntl->http_request().method();
  const auto path = cntl->http_request().uri().path();

  if (logger_ != nullptr) {
    logger_->info("http {} {} from {}", brpc::HttpMethod2Str(method), path,
                  butil::endpoint2str(cntl->remote_side()).c_str());
  }

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
      WriteError(cntl, gateway_id_,
                 common::MakeError(ErrorCode::kBadRequest, "method not allowed"));
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
  auto head = metadata_.Head(bucket, key);
  if (!head.success()) {
    WriteError(cntl, gateway_id_, head.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("Content-Length",
                                  std::to_string(head.value().content_length));
  cntl->http_response().SetHeader("ETag", head.value().etag);
  cntl->http_response().SetHeader("x-fa-version", head.value().version);
}

void HttpFrontend::HandleGet(brpc::Controller* cntl, const std::string& bucket,
                             const std::string& key) {
  auto head = metadata_.Head(bucket, key);
  if (!head.success()) {
    WriteError(cntl, gateway_id_, head.error());
    return;
  }
  const auto* range_header = cntl->http_request().GetHeader("Range");
  const auto range =
      common::ParseHttpRange(range_header, head.value().content_length);
  if (range.unsatisfiable) {
    WriteError(cntl, gateway_id_,
               common::MakeError(ErrorCode::kRangeNotSatisfiable,
                                "range not satisfiable"));
    return;
  }
  data_path::http::HttpResponseSink sink{cntl};
  auto report = http_.Get(bucket, key, range.offset, range.length, sink);
  if (!report.success()) {
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
  if (range.partial && report.value().bytes_transferred != 0U) {
    cntl->http_response().SetHeader(
        "Content-Range",
        "bytes " + std::to_string(range.offset) + "-" +
            std::to_string(range.offset + report.value().bytes_transferred - 1U) +
            "/" + std::to_string(head.value().content_length));
  }
}

void HttpFrontend::HandlePut(brpc::Controller* cntl, const std::string& bucket,
                             const std::string& key) {
  const auto payload = cntl->request_attachment().to_string();
  const std::span<const std::byte> body(
      reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  auto report = http_.Put(bucket, key, body);
  if (!report.success()) {
    WriteError(cntl, gateway_id_, report.error());
    return;
  }
  cntl->http_response().set_status_code(brpc::HTTP_STATUS_OK);
  SetCommonHeaders(cntl, gateway_id_, "completed");
  cntl->http_response().SetHeader("ETag", report.value().meta.etag);
  cntl->http_response().SetHeader("x-fa-version", report.value().meta.version);
  WriteJson(cntl, nlohmann::json{{"etag", report.value().meta.etag},
                                  {"version", report.value().meta.version},
                                  {"bytes_written", report.value().bytes_transferred}});
}

}  // namespace us3_turbo_access::gateway::api
