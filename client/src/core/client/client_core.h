#pragma once

#include <memory>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class MetadataClient;
class TransferRouter;

/**
 * @brief 内部装配体：拥有所有组件实例并管理生命周期。
 *
 * 仅供 Client 使用，对外不可见。
 */
class ClientCore {
 public:
  explicit ClientCore(ClientOptions options);
  ~ClientCore();

  ClientCore(const ClientCore&) = delete;
  ClientCore& operator=(const ClientCore&) = delete;
  ClientCore(ClientCore&&) = delete;
  ClientCore& operator=(ClientCore&&) = delete;

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;
  [[nodiscard]] const PlatformCapabilities& capabilities() const;
  [[nodiscard]] const ClientOptions& options() const;
  [[nodiscard]] const MetadataClient& metadata_client() const;
  [[nodiscard]] const TransferRouter& transfer_router() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace us3_turbo_access::client
