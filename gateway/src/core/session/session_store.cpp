#include "core/session/session_store.h"

#include <functional>
#include <utility>

#include "common/ids.h"
#include "common/error.h"

namespace us3_turbo_access::gateway::core {

SessionStore::SessionStore(std::string gateway_id,
                           std::chrono::seconds session_ttl,
                           std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)),
      gateway_id_(std::move(gateway_id)),
      session_ttl_(session_ttl) {}

SessionStore::Shard& SessionStore::ShardFor(std::string_view key) const {
  const auto h = std::hash<std::string_view>{}(key);
  return shards_[h % kShardCount];
}

// 新建 session 并落到三索引，或幂等命中时复用并校验一致。
Result<std::shared_ptr<Session>> SessionStore::Create(const OpenSessionParams& req) {
  // 幂等命中：bucket/object/op 必须一致，否则视为 key 误用。
  if (!req.idempotency_key.empty()) {
    auto& shard = ShardFor(req.idempotency_key);
    std::scoped_lock lock(shard.mu);
    auto it = shard.by_idempotency.find(req.idempotency_key);
    if (it != shard.by_idempotency.end()) {
      const auto& existing = *it->second;
      if (existing.bucket != req.bucket ||
          existing.object_key != req.object_key ||
          existing.op != req.op) {
        return Result<std::shared_ptr<Session>>::Failure(common::MakeError(
            ErrorCode::kBadRequest,
            "idempotency_key collides with a different bucket/object/op"));
      }
      return Result<std::shared_ptr<Session>>::Success(it->second);
    }
  }

  auto session = std::make_shared<Session>();
  session->session_id =
      req.session_id.empty() ? common::MakeRandomId("ses-") : req.session_id;
  session->request_id =
      req.request_id.empty() ? common::MakeRandomId("req-") : req.request_id;
  session->ticket = common::MakeRandomId("ticket-");
  session->gateway_id = gateway_id_;
  session->expire_at = common::MakeExpireAt(session_ttl_);
  session->bucket = req.bucket;
  session->object_key = req.object_key;
  session->op = req.op;
  session->data_flow = req.data_flow;
  session->buffer_type = req.buffer_type.empty() ? "host-regular" : req.buffer_type;
  session->offset = req.offset;
  session->expected_size = req.expected_size;
  session->idempotency_key = req.idempotency_key;
  session->is_multipart_part = req.is_multipart_part;
  session->state.store(SessionState::kOpened, std::memory_order_release);
  session->expire_deadline =
      std::chrono::steady_clock::now() + session_ttl_;

  // 三索引落到各自 shard；shared_ptr 兜底引用，sweep 时两阶段清理。
  {
    auto& shard = ShardFor(session->session_id);
    std::scoped_lock lock(shard.mu);
    shard.by_id[session->session_id] = session;
  }
  {
    auto& shard = ShardFor(session->ticket);
    std::scoped_lock lock(shard.mu);
    shard.by_ticket[session->ticket] = session;
  }
  if (!session->idempotency_key.empty()) {
    auto& shard = ShardFor(session->idempotency_key);
    std::scoped_lock lock(shard.mu);
    shard.by_idempotency[session->idempotency_key] = session;
  }

  if (logger_ != nullptr) {
    logger_->info("session.create id={} ticket={} op={} data_flow={} size={}",
                  session->session_id, session->ticket,
                  std::string(ToString(session->op)),
                  std::string(ToString(session->data_flow)),
                  session->expected_size);
  }
  return Result<std::shared_ptr<Session>>::Success(session);
}

Result<std::shared_ptr<Session>> SessionStore::Find(
    std::string_view session_id) const {
  auto& shard = ShardFor(session_id);
  std::scoped_lock lock(shard.mu);
  auto it = shard.by_id.find(std::string(session_id));
  if (it == shard.by_id.end()) {
    return Result<std::shared_ptr<Session>>::Failure(
        common::MakeError(ErrorCode::kSessionNotFound, "session not found"));
  }
  return Result<std::shared_ptr<Session>>::Success(it->second);
}

Result<std::shared_ptr<Session>> SessionStore::FindByTicket(
    std::string_view ticket) const {
  auto& shard = ShardFor(ticket);
  std::scoped_lock lock(shard.mu);
  auto it = shard.by_ticket.find(std::string(ticket));
  if (it == shard.by_ticket.end()) {
    return Result<std::shared_ptr<Session>>::Failure(
        common::MakeError(ErrorCode::kTicketInvalid, "ticket not found"));
  }
  return Result<std::shared_ptr<Session>>::Success(it->second);
}

Result<std::shared_ptr<Session>> SessionStore::Claim(std::string_view ticket) {
  auto lookup = FindByTicket(ticket);
  if (!lookup.success()) {
    return lookup;
  }
  auto session = lookup.value();
  SessionState expected = SessionState::kOpened;
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

Result<std::shared_ptr<Session>> SessionStore::BumpActive(
    std::string_view session_id) {
  auto lookup = Find(session_id);
  if (!lookup.success()) {
    return lookup;
  }
  auto session = lookup.value();
  SessionState current = SessionState::kOpened;
  // CAS kOpened→kActive; if already past kOpened (kActive/kCompleted/...) no-op.
  session->state.compare_exchange_strong(current, SessionState::kActive,
                                          std::memory_order_acq_rel);
  return lookup;
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

std::size_t SessionStore::SweepExpired(
    std::chrono::steady_clock::time_point now) {
  std::size_t evicted = 0;

  // 第一步：每个 shard 自己扫一遍 by_id，把过期的拉出来
  std::vector<std::shared_ptr<Session>> expired;
  for (auto& shard : shards_) {
    std::scoped_lock lock(shard.mu);
    for (auto it = shard.by_id.begin(); it != shard.by_id.end();) {
      const auto current_state =
          it->second->state.load(std::memory_order_acquire);
      const bool ttl_expired = it->second->expire_deadline <= now;
      const bool terminal = current_state == SessionState::kFailed ||
                            current_state == SessionState::kCompleted;
      if (ttl_expired || terminal) {
        if (ttl_expired && !terminal) {
          it->second->state.store(SessionState::kExpired,
                                  std::memory_order_release);
        }
        expired.push_back(it->second);
        it = shard.by_id.erase(it);
        ++evicted;
      } else {
        ++it;
      }
    }
  }

  // 第二步：把 ticket / idempotency 索引也清掉（可能落在不同 shard）
  for (const auto& session : expired) {
    auto& tshard = ShardFor(session->ticket);
    {
      std::scoped_lock lock(tshard.mu);
      tshard.by_ticket.erase(session->ticket);
    }
    if (!session->idempotency_key.empty()) {
      auto& ishard = ShardFor(session->idempotency_key);
      std::scoped_lock lock(ishard.mu);
      ishard.by_idempotency.erase(session->idempotency_key);
    }
  }
  return evicted;
}

}  // namespace us3_turbo_access::gateway::core
