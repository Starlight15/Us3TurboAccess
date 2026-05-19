#include "data_path/gds/gds_executor.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <cuobjserver.h>
#include <infiniband/verbs.h>

#include "common/error.h"

namespace us3_turbo_access::gateway::data_path::gds {

namespace {

constexpr std::size_t kMaxCuObjTransferBytes =
    1ULL * 1024ULL * 1024ULL * 1024ULL;  // cuObjServer 1 GiB hard limit

[[nodiscard]] Error MakeGdsError(ErrorCode code, std::string message,
                                 bool retryable = true) {
  Error err;
  err.code = code;
  err.message = std::move(message);
  err.retryable = retryable;
  return err;
}

[[nodiscard]] std::string BuildObjectId(const core::Session& session) {
  return session.bucket + "/" + session.object_key;
}

[[nodiscard]] std::uint64_t ParseRemoteBufferAddress(const std::string& token) {
  const auto colon = token.find(':');
  const std::string hex =
      colon == std::string::npos ? token : token.substr(0, colon);
  if (hex.empty()) {
    return 0;
  }
  return std::strtoull(hex.c_str(), nullptr, 16);
}

[[nodiscard]] std::string DescribeStatus(ibv_wc_status status) {
  const char* description = ibv_wc_status_str(status);
  if (description == nullptr) {
    return std::to_string(static_cast<int>(status));
  }
  return description;
}

class RegistrationGuard {
 public:
  RegistrationGuard(cuObjServer& server, rdma_buffer* buffer)
      : server_(server), buffer_(buffer) {}
  ~RegistrationGuard() {
    if (buffer_ != nullptr) {
      server_.deRegisterBuffer(buffer_);
    }
  }
  RegistrationGuard(const RegistrationGuard&) = delete;
  RegistrationGuard& operator=(const RegistrationGuard&) = delete;

 private:
  cuObjServer& server_;
  rdma_buffer* buffer_;
};

class ChannelGuard {
 public:
  ChannelGuard(cuObjServer& server, std::uint16_t channel)
      : server_(server), channel_(channel) {}
  ~ChannelGuard() {
    if (channel_ != INVALID_CHANNEL_ID) {
      server_.freeChannelId(channel_);
    }
  }
  ChannelGuard(const ChannelGuard&) = delete;
  ChannelGuard& operator=(const ChannelGuard&) = delete;

 private:
  cuObjServer&  server_;
  std::uint16_t channel_;
};

class HostBuffer {
 public:
  HostBuffer(cuObjServer& server, std::size_t size) : size_(size) {
    data_ = server.allocHostBuffer(size_);
  }
  ~HostBuffer() {
    if (data_ != nullptr) {
      std::free(data_);
    }
  }
  HostBuffer(const HostBuffer&) = delete;
  HostBuffer& operator=(const HostBuffer&) = delete;
  [[nodiscard]] void* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  void*       data_{nullptr};
  std::size_t size_{0};
};

}  // namespace

GdsExecutor::GdsExecutor(std::string public_host, std::string bind_host,
                         int port, backend::IBackend& backend,
                         std::shared_ptr<spdlog::logger> logger)
    : public_host_(std::move(public_host)),
      bind_host_(std::move(bind_host)),
      port_(port),
      backend_(backend),
      logger_(std::move(logger)) {}

GdsExecutor::~GdsExecutor() { Stop(); }

std::string GdsExecutor::endpoint() const {
  return public_host_ + ":" + std::to_string(port_);
}

Result<bool> GdsExecutor::Start() {
  std::scoped_lock lock(mu_);
  if (server_ != nullptr && server_->isConnected()) {
    return Result<bool>::Success(true);
  }
  auto server = std::make_shared<cuObjServer>(
      bind_host_.c_str(), static_cast<unsigned short>(port_),
      CUOBJ_PROTO_RDMA_DC_V1);
  if (!server->isConnected()) {
    return Result<bool>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable,
        "cuObjServer init failed on " + bind_host_ + ":" +
            std::to_string(port_)));
  }
  server_ = std::move(server);
  if (logger_ != nullptr) {
    logger_->info("gds: cuObjServer listening on {}:{}", bind_host_, port_);
  }
  return Result<bool>::Success(true);
}

void GdsExecutor::Stop() {
  std::scoped_lock lock(mu_);
  server_.reset();
}

bool GdsExecutor::available() const {
  std::scoped_lock lock(mu_);
  return server_ != nullptr && server_->isConnected();
}

Result<std::shared_ptr<cuObjServer>> GdsExecutor::GetServer() const {
  std::scoped_lock lock(mu_);
  if (server_ == nullptr || !server_->isConnected()) {
    return Result<std::shared_ptr<cuObjServer>>::Failure(
        MakeGdsError(ErrorCode::kRdmaUnavailable,
                     "cuObjServer not available"));
  }
  return Result<std::shared_ptr<cuObjServer>>::Success(server_);
}

Result<std::string> GdsExecutor::GetChunk(const core::Session& session,
                                          const std::string& rdma_token,
                                          std::uint64_t object_offset,
                                          std::uint64_t length) {
  if (length > kMaxCuObjTransferBytes) {
    return Result<std::string>::Failure(MakeGdsError(
        ErrorCode::kBadRequest,
        "GDS GET chunk exceeds 1 GiB cuObjServer limit", false));
  }
  if (length == 0U) {
    return Result<std::string>::Success("gds-cuobject-rdma-write-empty");
  }

  auto server_lookup = GetServer();
  if (!server_lookup.success()) {
    return Result<std::string>::Failure(server_lookup.error());
  }

  std::vector<std::byte> staging(static_cast<std::size_t>(length));
  auto read = backend_.Read(session.bucket, session.object_key, object_offset,
                            std::span<std::byte>(staging.data(), staging.size()));
  if (!read.success()) {
    return Result<std::string>::Failure(read.error());
  }
  if (read.value() == 0U) {
    return Result<std::string>::Success("gds-cuobject-rdma-write-empty");
  }
  staging.resize(read.value());

  auto server = server_lookup.value();
  auto* rdma_buf = server->registerBuffer(staging.data(), staging.size());
  if (rdma_buf == nullptr) {
    return Result<std::string>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable, "cuObjServer registerBuffer failed"));
  }
  RegistrationGuard reg_guard(*server, rdma_buf);

  const auto channel = server->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    return Result<std::string>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocateChannelId failed"));
  }
  ChannelGuard chan_guard(*server, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server->handleGetObject(
      BuildObjectId(session), rdma_buf, remote_buf_start, staging.size(),
      rdma_token, channel, 0, &status, nullptr);
  if (logger_ != nullptr) {
    logger_->info("gds.get object={} offset={} length={} transferred={} status={}",
                  BuildObjectId(session), object_offset, length, transferred,
                  DescribeStatus(status));
  }
  if (transferred < 0) {
    return Result<std::string>::Failure(MakeGdsError(
        ErrorCode::kRpcError,
        "cuObjServer handleGetObject failed: " + DescribeStatus(status)));
  }
  if (static_cast<std::size_t>(transferred) != staging.size()) {
    return Result<std::string>::Failure(MakeGdsError(
        ErrorCode::kRpcError,
        "cuObjServer handleGetObject short transfer"));
  }
  return Result<std::string>::Success("gds-cuobject-rdma-write");
}

Result<ObjectMetadata> GdsExecutor::PutChunk(const core::Session& session,
                                             const std::string& rdma_token,
                                             std::uint64_t object_offset,
                                             std::uint64_t length) {
  if (length > kMaxCuObjTransferBytes) {
    return Result<ObjectMetadata>::Failure(MakeGdsError(
        ErrorCode::kBadRequest,
        "GDS PUT chunk exceeds 1 GiB cuObjServer limit", false));
  }
  const std::optional<std::size_t> total_size =
      session.expected_size != 0U
          ? std::optional<std::size_t>(
                static_cast<std::size_t>(session.expected_size))
          : std::nullopt;
  if (length == 0U) {
    return backend_.WriteRange(session.bucket, session.object_key,
                               object_offset, {}, total_size);
  }

  auto server_lookup = GetServer();
  if (!server_lookup.success()) {
    return Result<ObjectMetadata>::Failure(server_lookup.error());
  }
  auto server = server_lookup.value();

  HostBuffer staging(*server, static_cast<std::size_t>(length));
  if (staging.data() == nullptr) {
    return Result<ObjectMetadata>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocHostBuffer failed"));
  }
  std::memset(staging.data(), 0, staging.size());

  auto* rdma_buf = server->registerBuffer(staging.data(), staging.size());
  if (rdma_buf == nullptr) {
    return Result<ObjectMetadata>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable, "cuObjServer registerBuffer failed"));
  }
  RegistrationGuard reg_guard(*server, rdma_buf);

  const auto channel = server->allocateChannelId();
  if (channel == INVALID_CHANNEL_ID) {
    return Result<ObjectMetadata>::Failure(MakeGdsError(
        ErrorCode::kRdmaUnavailable, "cuObjServer allocateChannelId failed"));
  }
  ChannelGuard chan_guard(*server, channel);

  const auto remote_buf_start = ParseRemoteBufferAddress(rdma_token);
  ibv_wc_status status = IBV_WC_SUCCESS;
  const auto transferred = server->handlePutObject(
      BuildObjectId(session), rdma_buf, remote_buf_start, staging.size(),
      rdma_token, channel, 0, &status, nullptr);
  if (logger_ != nullptr) {
    logger_->info("gds.put object={} offset={} length={} transferred={} status={}",
                  BuildObjectId(session), object_offset, length, transferred,
                  DescribeStatus(status));
  }
  if (transferred < 0) {
    return Result<ObjectMetadata>::Failure(MakeGdsError(
        ErrorCode::kRpcError,
        "cuObjServer handlePutObject failed: " + DescribeStatus(status)));
  }

  const auto bytes = static_cast<std::size_t>(transferred);
  std::span<const std::byte> view(
      static_cast<const std::byte*>(staging.data()), bytes);
  auto write = backend_.WriteRange(session.bucket, session.object_key,
                                   object_offset, view, total_size);
  if (write.success() && logger_ != nullptr) {
    logger_->info("gds.put.persist object={} size={} etag={}",
                  BuildObjectId(session), write.value().content_length,
                  write.value().etag);
  }
  return write;
}

}  // namespace us3_turbo_access::gateway::data_path::gds
