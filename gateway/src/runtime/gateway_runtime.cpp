#include "runtime/gateway_runtime.h"

#include <utility>

#include <brpc/server.h>
#include <spdlog/logger.h>

#include "api/control_plane_service.h"
#include "api/http_frontend.h"
#include "backend/memory_backend.h"
#include "backend/null_backend.h"
#include "core/negotiator/negotiator.h"
#include "core/session/session_store.h"
#include "core/transfer/transfer_engine.h"
#include "data_path/gds/gds_executor.h"
#include "common/error.h"
#include "common/log.h"

namespace us3_turbo_access::gateway::runtime {

namespace {

std::unique_ptr<backend::IBackend> MakeBackend(const GatewayOptions& opts) {
  switch (opts.backend_kind) {
    case BackendKind::kMemory:
      return std::make_unique<backend::MemoryBackend>(opts.backend_capacity);
    case BackendKind::kNull:
      return std::make_unique<backend::NullBackend>();
  }
  return std::make_unique<backend::MemoryBackend>(opts.backend_capacity);
}

std::string MakeRdmaEndpoint(const GatewayOptions& opts) {
  return opts.public_host + ":" + std::to_string(opts.rdma_port);
}

}  // namespace

GatewayRuntime::GatewayRuntime(GatewayOptions options)
    : options_(std::move(options)) {}

GatewayRuntime::~GatewayRuntime() { Shutdown(); }

bool GatewayRuntime::initialized() const noexcept { return started_; }
const GatewayOptions& GatewayRuntime::options() const noexcept { return options_; }

Result<bool> GatewayRuntime::Initialize() {
  if (started_) {
    return Result<bool>::Success(true);
  }
  logger_ = common::EnsureLogger(options_.logger);
  options_.logger = logger_;

  if (options_.rdma_enable) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kRdmaUnavailable,
        "native-rdma data path not enabled in M1; restart with --rdma_enable=false"));
  }

  if (options_.gds_rdma_port == 0) {
    options_.gds_rdma_port = options_.rdma_port + 1;
  }

  backend_ = MakeBackend(options_);
  sessions_ = std::make_unique<core::SessionStore>(
      options_.gateway_id, MakeRdmaEndpoint(options_),
      options_.default_chunk_size, options_.session_ttl, logger_);
  transfers_ = std::make_unique<core::TransferEngine>(*backend_, logger_);

  if (options_.gds_enable) {
    const std::string gds_bind =
        options_.bind_host == "0.0.0.0" ? options_.public_host : options_.bind_host;
    gds_executor_ = std::make_unique<data_path::gds::GdsExecutor>(
        options_.public_host, gds_bind, options_.gds_rdma_port, *backend_,
        logger_);
    auto started = gds_executor_->Start();
    if (!started.success()) {
      return Result<bool>::Failure(started.error());
    }
  }

  negotiator_ = std::make_unique<core::Negotiator>(
      options_.public_host, options_.gds_rdma_port, *sessions_, *transfers_,
      gds_executor_.get(), logger_);

  control_plane_ = std::make_unique<api::ControlPlaneService>(
      *sessions_, *transfers_, *negotiator_, gds_executor_.get(), logger_);
  http_frontend_ = std::make_unique<api::HttpFrontend>(
      options_.gateway_id, *sessions_, *transfers_, logger_);

  brpc::ServerOptions server_options;
  server_options.idle_timeout_sec = options_.idle_timeout_sec;
  server_options.num_threads = options_.num_threads;
  server_options.http_master_service = http_frontend_.get();

  if (server_.AddService(control_plane_.get(),
                         brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kInternal, "failed to register control-plane service"));
  }

  const std::string bind_endpoint =
      options_.bind_host + ":" + std::to_string(options_.brpc_port);
  if (server_.Start(bind_endpoint.c_str(), &server_options) != 0) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kInternal,
        "failed to start brpc server on " + bind_endpoint));
  }
  // brpc takes ownership of http_master_service once the server is started;
  // release the unique_ptr so we don't double-free during Shutdown.
  http_frontend_.release();
  started_ = true;
  logger_->info(
      "gateway ready id={} backend={} brpc_endpoint={} public_host={}",
      options_.gateway_id, std::string(backend_->kind()), bind_endpoint,
      options_.public_host);
  return Result<bool>::Success(true);
}

void GatewayRuntime::Run() {
  if (!started_) {
    return;
  }
  server_.RunUntilAskedToQuit();
}

void GatewayRuntime::Shutdown() {
  if (started_) {
    server_.Stop(0);
    server_.Join();
    started_ = false;
  }
  control_plane_.reset();
  http_frontend_.reset();  // typically already released to brpc; safe no-op.
  negotiator_.reset();
  if (gds_executor_ != nullptr) {
    gds_executor_->Stop();
  }
  gds_executor_.reset();
  transfers_.reset();
  sessions_.reset();
  backend_.reset();
}

}  // namespace us3_turbo_access::gateway::runtime
