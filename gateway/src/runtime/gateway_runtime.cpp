#include "runtime/gateway_runtime.h"

#include <utility>

#include <brpc/server.h>
#include <spdlog/logger.h>

#include "api/control_plane_service.h"
#include "api/http_frontend.h"
#include "api/rdma_data_plane_service.h"
#include "backend/composite_backend.h"
#include "backend/memory_data_store.h"
#include "backend/memory_index_store.h"
#include "backend/null_backend.h"
#include "backend/null_data_store.h"
#include "backend/null_index_store.h"
#include "core/metadata/metadata_service.h"
#include "core/multipart/multipart_coordinator.h"
#include "core/multipart/multipart_store.h"
#include "core/multipart/multipart_app_service.h"
#include "core/session_opener/session_opener.h"
#include "core/session/session_store.h"
#include "core/session/session_sweeper.h"
#include "data_path/gds/gds_executor.h"
#include "data_path/gds/gds_multipart_path_handler.h"
#include "data_path/ucx/ucx_executor.h"
#include "data_path/ucx/ucx_multipart_path_handler.h"
#include "runtime/io_worker_pool.h"
#include "data_path/http/http_executor.h"
#include "data_path/http/http_multipart_path_handler.h"
#include "common/error.h"
#include "common/log.h"

namespace us3_turbo_access::gateway::runtime {

namespace {

std::unique_ptr<backend::IBackend> MakeBackend(const GatewayOptions& opts) {
  switch (opts.backend_kind) {
    case BackendKind::kMemory:
      // 内存 backend 现在走 composite：MemoryIndex + MemoryData。
      return std::make_unique<backend::CompositeBackend>(
          std::make_unique<backend::MemoryIndexStore>(),
          std::make_unique<backend::MemoryDataStore>(opts.backend_capacity));
    case BackendKind::kNull:
      return std::make_unique<backend::NullBackend>();
    case BackendKind::kComposite:
      // 协议链路验证用：Null + Null，wire 通但字节是假的。
      return std::make_unique<backend::CompositeBackend>(
          std::make_unique<backend::NullIndexStore>(),
          std::make_unique<backend::NullDataStore>());
  }
  return std::make_unique<backend::CompositeBackend>(
      std::make_unique<backend::MemoryIndexStore>(),
      std::make_unique<backend::MemoryDataStore>(opts.backend_capacity));
}

}  // namespace

GatewayRuntime::GatewayRuntime(GatewayOptions options)
    : options_(std::move(options)) {}

GatewayRuntime::~GatewayRuntime() { Shutdown(); }

bool GatewayRuntime::initialized() const noexcept { return started_; }
const GatewayOptions& GatewayRuntime::options() const noexcept { return options_; }

// 按依赖顺序装配；任一步失败 fail-fast，未起资源由析构兜底。
Result<bool> GatewayRuntime::Initialize() {
  if (started_) {
    return Result<bool>::Success(true);
  }
  logger_ = common::EnsureLogger(options_.logger);
  options_.logger = logger_;


  // backend 是所有上层模块的依赖根。
  backend_ = MakeBackend(options_);
  sessions_ = std::make_unique<core::SessionStore>(
      options_.gateway_id, options_.session_ttl, logger_);
  metadata_ = std::make_unique<core::MetadataService>(*backend_);
  multipart_store_ = std::make_unique<core::multipart::MultipartStore>(
      options_.multipart_ttl);
  multipart_coordinator_ = std::make_unique<core::multipart::MultipartCoordinator>(
      *backend_, *multipart_store_, options_.multipart_max_part_size,
      options_.multipart_min_part_size, logger_);
  multipart_app_ = std::make_unique<core::multipart::MultipartAppService>(
      *multipart_coordinator_, logger_);
  http_executor_ = std::make_unique<data_path::http::HttpExecutor>(
      *backend_, logger_);
  http_multipart_handler_ = std::make_unique<data_path::http::HttpMultipartPathHandler>(
      *http_executor_, *multipart_coordinator_, logger_);
  io_pool_ = std::make_unique<runtime::IoWorkerPool>(
      static_cast<std::size_t>(std::max(1, options_.io_worker_threads)));

  // 必须先于 brpc.Start，避免 client OpenSession 时 GDS 未就绪。
  if (options_.gds_enable) {
    const std::string gds_bind =
        options_.bind_host == "0.0.0.0" ? options_.public_host : options_.bind_host;
    gds_executor_ = std::make_unique<data_path::gds::GdsExecutor>(
        options_.public_host, gds_bind, options_.gds, *backend_, *metadata_,
        logger_);
    auto started = gds_executor_->Start();
    if (!started.success()) {
      return Result<bool>::Failure(started.error());
    }
    gds_multipart_handler_ = std::make_unique<data_path::gds::GdsMultipartPathHandler>(
        *gds_executor_, *multipart_coordinator_, logger_);
  }

  // UCX 数据通路（kNativeRdma DataPath）。
  if (options_.ucx_enable) {
    const std::string ucx_bind =
        options_.bind_host == "0.0.0.0" ? options_.public_host : options_.bind_host;
    ucx_executor_ = std::make_unique<data_path::ucx::UcxExecutor>(
        options_.public_host, ucx_bind, options_.ucx, *backend_, *metadata_,
        logger_);
    auto started = ucx_executor_->Start();
    if (!started.success()) {
      return Result<bool>::Failure(started.error());
    }
    ucx_multipart_handler_ = std::make_unique<data_path::ucx::UcxMultipartPathHandler>(
        *ucx_executor_, *multipart_coordinator_, logger_);
  }

  session_opener_ = std::make_unique<core::SessionOpener>(
      *sessions_, http_executor_.get(), gds_executor_.get(),
      ucx_executor_.get(), logger_);

  control_plane_ = std::make_unique<api::ControlPlaneService>(
      *sessions_, *metadata_, *session_opener_, gds_executor_.get(),
      gds_multipart_handler_.get(),
      *multipart_app_, *io_pool_, logger_);
  http_frontend_ = std::make_unique<api::HttpFrontend>(
      options_.gateway_id, *metadata_, *http_executor_,
      *http_multipart_handler_, *multipart_app_, options_.http_max_put_bytes, logger_);

  brpc::ServerOptions server_options;
  server_options.idle_timeout_sec = options_.idle_timeout_sec;
  server_options.num_threads = options_.num_threads;
  server_options.max_concurrency = options_.max_concurrency;
  server_options.http_master_service = http_frontend_.get();

  if (server_.AddService(control_plane_.get(),
                         brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kInternal, "failed to register control-plane service"));
  }

  // UCX 数据面 brpc service。
  if (ucx_executor_ != nullptr) {
    rdma_data_plane_ = std::make_unique<api::RdmaDataPlaneService>(
        *ucx_executor_, *ucx_multipart_handler_, logger_);
    if (server_.AddService(rdma_data_plane_.get(),
                            brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
      return Result<bool>::Failure(common::MakeError(
          ErrorCode::kInternal, "failed to register rdma-data-plane service"));
    }
  }

  const std::string bind_endpoint =
      options_.bind_host + ":" + std::to_string(options_.brpc_port);
  if (server_.Start(bind_endpoint.c_str(), &server_options) != 0) {
    return Result<bool>::Failure(common::MakeError(
        ErrorCode::kInternal,
        "failed to start brpc server on " + bind_endpoint));
  }
  // brpc 接管 http_master_service 所有权，release 防止双 free。
  http_frontend_.release();

  // 所有依赖就绪后再启动 sweeper。
  session_sweeper_ = std::make_unique<core::SessionSweeper>(
      *sessions_, options_.session_sweep_interval, logger_);
  session_sweeper_->SetMultipart(multipart_store_.get(),
                                  multipart_coordinator_.get());
  session_sweeper_->Start();

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
    if (session_sweeper_ != nullptr) {
      session_sweeper_->Stop();
    }
    server_.Stop(0);
    server_.Join();
    if (io_pool_ != nullptr) {
      io_pool_->Stop();
    }
    started_ = false;
  }
  session_sweeper_.reset();
  control_plane_.reset();
  rdma_data_plane_.reset();
  http_frontend_.reset();  // typically already released to brpc; safe no-op.
  session_opener_.reset();
  gds_multipart_handler_.reset();
  ucx_multipart_handler_.reset();
  if (gds_executor_ != nullptr) {
    gds_executor_->Stop();
  }
  gds_executor_.reset();
  if (ucx_executor_ != nullptr) {
    ucx_executor_->Stop();
  }
  ucx_executor_.reset();
  io_pool_.reset();
  http_multipart_handler_.reset();
  http_executor_.reset();
  multipart_app_.reset();
  multipart_coordinator_.reset();
  multipart_store_.reset();
  metadata_.reset();
  sessions_.reset();
  backend_.reset();
}

}  // namespace us3_turbo_access::gateway::runtime
