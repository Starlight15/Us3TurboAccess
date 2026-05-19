#pragma once

#include <memory>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class ClientCore;

/**
 * @brief Public entry point of the US3 turbo-access client SDK.
 *
 * Create one client per endpoint/configuration pair, call Initialize() before use,
 * and call Shutdown() when the client is no longer needed.
 */
class Client {
 public:
  /**
   * @brief Creates a client with the provided options.
   */
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  /**
   * @brief Initializes transport and control-plane dependencies.
   */
  [[nodiscard]] Result<bool> Initialize();

  /**
   * @brief Releases resources owned by the client core.
   */
  void Shutdown();

  /**
   * @brief Returns whether Initialize() has completed successfully.
   */
  [[nodiscard]] bool initialized() const;

  /**
   * @brief Returns the capabilities detected for the current environment.
   */
  [[nodiscard]] const PlatformCapabilities& capabilities() const;

  /**
   * @brief Fetches metadata for an object.
   */
  [[nodiscard]] Result<ObjectMetadata> HeadObject(const ObjectId& object) const;

  /**
   * @brief Reads object data into the provided buffer.
   */
  [[nodiscard]] Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                  MutableBufferView buffer) const;

  /**
   * @brief Writes object data from the provided buffer.
   */
  [[nodiscard]] Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                  ConstBufferView buffer) const;

 private:
  std::unique_ptr<ClientCore> core_;
};

}  // namespace us3_turbo_access::client
