#include "core/session_opener/session_opener.h"

#include <utility>

#include "common/error.h"
#include "core/session/session_store.h"
#include "data_path/data_path_executor.h"

namespace us3_turbo_access::gateway::core {

SessionOpener::SessionOpener(SessionStore& sessions,
                             data_path::IDataPathExecutor* http_executor,
                             data_path::IDataPathExecutor* gds_executor,
                             data_path::IDataPathExecutor* rdma_executor,
                             std::shared_ptr<spdlog::logger> logger)
    : sessions_(sessions),
      http_executor_(http_executor),
      gds_executor_(gds_executor),
      rdma_executor_(rdma_executor),
      logger_(std::move(logger)) {}

data_path::IDataPathExecutor* SessionOpener::SelectExecutor(DataPath path) const {
  switch (path) {
    case DataPath::kHttpTcp:     return http_executor_;
    case DataPath::kGdsCuObject: return gds_executor_;
    case DataPath::kNativeRdma:  return rdma_executor_;
  }
  return nullptr;
}

// 编排 OpenSession：建 session → 选 executor → 调数据面 hook。
// 失败不回滚，依赖 sweeper 按 TTL 清理。
Result<OpenSessionResult> SessionOpener::Open(const OpenSessionParams& req) {
  auto created = sessions_.Create(req);
  if (!created.success()) {
    return Result<OpenSessionResult>::Failure(created.error());
  }
  const auto& session = *created.value();

  auto* executor = SelectExecutor(session.data_path);
  if (executor == nullptr) {
    return Result<OpenSessionResult>::Failure(common::MakeError(
        ErrorCode::kBadRequest,
        "no executor registered for data path: " +
            std::string(ToString(session.data_path))));
  }

  auto hook = executor->OnSessionOpened(session);
  if (!hook.success()) {
    return Result<OpenSessionResult>::Failure(hook.error());
  }

  OpenSessionResult outcome;
  outcome.session = created.value();
  return Result<OpenSessionResult>::Success(std::move(outcome));
}

}  // namespace us3_turbo_access::gateway::core
