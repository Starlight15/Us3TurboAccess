#pragma once

#include <memory>

#include <spdlog/logger.h>

#include "core/session/session.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {
class SessionStore;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_flow {
class IDataPathExecutor;
}  // namespace us3_turbo_access::gateway::data_flow

namespace us3_turbo_access::gateway::core {

/**
 * @brief Output of a successful session open.
 */
struct OpenSessionResult {
  std::shared_ptr<Session>   session;
};

/**
 * @brief 创建 session 并调用对应 data path executor 的 OnSessionOpened 钩子。
 *
 * SessionOpener 本身只持有 SessionStore 与各 data path executor 的引用，
 * 不再了解 backend 或 GDS 的细节 —— 这些逻辑下沉到 executor 自己实现的
 * OnSessionOpened 里。
 */
class SessionOpener {
 public:
  SessionOpener(SessionStore& sessions,
                data_flow::IDataPathExecutor* gds_executor,
                data_flow::IDataPathExecutor* rdma_executor,
                std::shared_ptr<spdlog::logger> logger);

  SessionOpener(const SessionOpener&) = delete;
  SessionOpener& operator=(const SessionOpener&) = delete;

  [[nodiscard]] Result<OpenSessionResult> Open(const OpenSessionParams& req);

 private:
  [[nodiscard]] data_flow::IDataPathExecutor* SelectExecutor(DataFlow path) const;

  SessionStore&                       sessions_;
  data_flow::IDataPathExecutor*       gds_executor_{nullptr};
  data_flow::IDataPathExecutor*       rdma_executor_{nullptr};
  std::shared_ptr<spdlog::logger>     logger_;
};

}  // namespace us3_turbo_access::gateway::core
