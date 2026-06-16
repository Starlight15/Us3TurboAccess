#include "core/session/session_app_service.h"

#include <memory>
#include <string_view>
#include <utility>

#include "core/session/session.h"
#include "core/session/session_store.h"

namespace us3_turbo_access::gateway::core {

SessionAppService::SessionAppService(SessionStore& sessions,
                                     std::shared_ptr<spdlog::logger> logger)
    : sessions_(sessions),
      logger_(std::move(logger)) {}

std::shared_ptr<Session>
SessionAppService::ResolveForGdsChunk(std::string_view session_id,
                                       std::string_view transfer_ticket) const {
  if (!session_id.empty()) {
    auto lookup = sessions_.Find(session_id);
    if (lookup.success()) {
      return lookup.value();
    }
  }
  if (!transfer_ticket.empty()) {
    auto lookup = sessions_.FindByTicket(transfer_ticket);
    if (lookup.success()) {
      return lookup.value();
    }
  }
  return nullptr;
}

void SessionAppService::BumpActive(std::string_view session_id) const {
  (void)sessions_.BumpActive(session_id);
}

void SessionAppService::MarkFailed(const Session& session) const {
  (void)sessions_.MarkFailed(session.session_id);
}

bool SessionAppService::MarkFailedById(std::string_view session_id) const {
  auto marked = sessions_.MarkFailed(session_id);
  return marked.success() && marked.value() != nullptr;
}

}  // namespace us3_turbo_access::gateway::core
