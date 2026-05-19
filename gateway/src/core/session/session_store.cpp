#include "core/session/session_store.h"

#include <utility>

#include "common/ids.h"
#include "common/error.h"

namespace us3_turbo_access::gateway::core {

SessionStore::SessionStore(std::string gateway_id, std::string gateway_endpoint,
                           std::size_t default_chunk_size,
                           std::chrono::seconds session_ttl,
                           std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)),
      gateway_id_(std::move(gateway_id)),
      gateway_endpoint_(std::move(gateway_endpoint)),
      default_chunk_size_(default_chunk_size),
      session_ttl_(session_ttl) {}

namespace {

std::pair<std::string, std::string> SplitHostPort(std::string_view endpoint) {
  const auto colon = endpoint.rfind(':');
  if (colon == std::string_view::npos) {
    return {std::string(endpoint), {}};
  }
  return {std::string(endpoint.substr(0, colon)),
          std::string(endpoint.substr(colon + 1))};
}

}  // namespace

Result<std::shared_ptr<Session>> SessionStore::Create(const NegotiateRequest& req) {
  {
    std::scoped_lock lock(mu_);
    if (!req.idempotency_key.empty()) {
      auto it = by_idempotency_.find(req.idempotency_key);
      if (it != by_idempotency_.end()) {
        return Result<std::shared_ptr<Session>>::Success(it->second);
      }
    }
  }

  auto session = std::make_shared<Session>();
  session->session_id =
      req.session_id.empty() ? common::MakeRandomId("ses-") : req.session_id;
  session->request_id =
      req.request_id.empty() ? common::MakeRandomId("req-") : req.request_id;
  session->ticket = common::MakeRandomId("ticket-");
  session->gateway_id = gateway_id_;
  session->gateway_endpoint = gateway_endpoint_;
  session->channel_id = req.channel_id.empty() ? "channel-0" : req.channel_id;
  session->async_handle = common::MakeRandomId("async-");
  session->expire_at = common::MakeExpireAt(session_ttl_);
  session->bucket = req.bucket;
  session->object_key = req.object_key;
  session->op = req.op;
  session->data_path = req.data_path;
  session->buffer_type = req.buffer_type.empty() ? "host-regular" : req.buffer_type;
  session->offset = req.offset;
  session->expected_size = req.expected_size;
  session->chunk_plan =
      common::BuildChunkPlan(session->offset, session->expected_size, default_chunk_size_);
  session->idempotency_key = req.idempotency_key;
  session->state.store(SessionState::kNegotiated, std::memory_order_release);
  session->expire_deadline =
      std::chrono::steady_clock::now() + session_ttl_;

  const auto [host, port] = SplitHostPort(gateway_endpoint_);
  session->rdma_parameters.host = host;
  session->rdma_parameters.port = port;

  {
    std::scoped_lock lock(mu_);
    by_id_[session->session_id] = session;
    by_ticket_[session->ticket] = session->session_id;
    if (!session->idempotency_key.empty()) {
      by_idempotency_[session->idempotency_key] = session;
    }
  }
  if (logger_ != nullptr) {
    logger_->info("session.create id={} ticket={} op={} data_path={} size={}",
                  session->session_id, session->ticket,
                  std::string(ToString(session->op)),
                  std::string(ToString(session->data_path)),
                  session->expected_size);
  }
  return Result<std::shared_ptr<Session>>::Success(session);
}

Result<std::shared_ptr<Session>> SessionStore::Find(
    std::string_view session_id) const {
  std::scoped_lock lock(mu_);
  auto it = by_id_.find(std::string(session_id));
  if (it == by_id_.end()) {
    return Result<std::shared_ptr<Session>>::Failure(
        common::MakeError(ErrorCode::kSessionNotFound, "session not found"));
  }
  return Result<std::shared_ptr<Session>>::Success(it->second);
}

Result<std::shared_ptr<Session>> SessionStore::FindByTicket(
    std::string_view ticket) const {
  std::scoped_lock lock(mu_);
  auto it = by_ticket_.find(std::string(ticket));
  if (it == by_ticket_.end()) {
    return Result<std::shared_ptr<Session>>::Failure(
        common::MakeError(ErrorCode::kTicketInvalid, "ticket not found"));
  }
  auto session_it = by_id_.find(it->second);
  if (session_it == by_id_.end()) {
    return Result<std::shared_ptr<Session>>::Failure(
        common::MakeError(ErrorCode::kSessionNotFound, "session not found"));
  }
  return Result<std::shared_ptr<Session>>::Success(session_it->second);
}

Result<std::shared_ptr<Session>> SessionStore::Claim(std::string_view ticket) {
  auto lookup = FindByTicket(ticket);
  if (!lookup.success()) {
    return lookup;
  }
  auto session = lookup.value();
  SessionState expected = SessionState::kNegotiated;
  if (!session->state.compare_exchange_strong(expected, SessionState::kClaimed,
                                              std::memory_order_acq_rel)) {
    return Result<std::shared_ptr<Session>>::Failure(common::MakeError(
        ErrorCode::kStaleState, "session not in claimable state"));
  }
  return Result<std::shared_ptr<Session>>::Success(session);
}

Result<std::shared_ptr<Session>> SessionStore::Transition(
    std::string_view session_id, SessionState expected, SessionState desired,
    ErrorCode failure_code) {
  auto lookup = Find(session_id);
  if (!lookup.success()) {
    return lookup;
  }
  auto session = lookup.value();
  SessionState current = expected;
  if (!session->state.compare_exchange_strong(current, desired,
                                              std::memory_order_acq_rel)) {
    return Result<std::shared_ptr<Session>>::Failure(common::MakeError(
        failure_code, "session state transition rejected"));
  }
  return Result<std::shared_ptr<Session>>::Success(session);
}

Result<std::shared_ptr<Session>> SessionStore::MarkActive(
    std::string_view session_id) {
  return Transition(session_id, SessionState::kClaimed, SessionState::kActive,
                    ErrorCode::kStaleState);
}

Result<std::shared_ptr<Session>> SessionStore::MarkCompleted(
    std::string_view session_id) {
  auto lookup = Find(session_id);
  if (!lookup.success()) {
    return lookup;
  }
  lookup.value()->state.store(SessionState::kCompleted,
                              std::memory_order_release);
  return lookup;
}

Result<std::shared_ptr<Session>> SessionStore::MarkFailed(
    std::string_view session_id) {
  auto lookup = Find(session_id);
  if (!lookup.success()) {
    return lookup;
  }
  lookup.value()->state.store(SessionState::kFailed,
                              std::memory_order_release);
  return lookup;
}

void SessionStore::UpdateRdmaParameters(std::string_view session_id,
                                        const RdmaParameters& parameters) {
  std::scoped_lock lock(mu_);
  auto it = by_id_.find(std::string(session_id));
  if (it == by_id_.end()) {
    return;
  }
  it->second->rdma_parameters = parameters;
}

std::size_t SessionStore::SweepExpired(
    std::chrono::steady_clock::time_point now) {
  std::size_t evicted = 0;
  std::scoped_lock lock(mu_);
  for (auto it = by_id_.begin(); it != by_id_.end();) {
    if (it->second->expire_deadline <= now) {
      it->second->state.store(SessionState::kExpired, std::memory_order_release);
      by_ticket_.erase(it->second->ticket);
      if (!it->second->idempotency_key.empty()) {
        by_idempotency_.erase(it->second->idempotency_key);
      }
      it = by_id_.erase(it);
      ++evicted;
    } else {
      ++it;
    }
  }
  return evicted;
}

}  // namespace us3_turbo_access::gateway::core
