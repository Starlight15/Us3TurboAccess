#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::core {

/**
 * @brief Lifecycle phase of a transfer session.
 *
 * kOpened/kActive/kCompleted/kFailed/kExpired are wired today.
 * kClaimed is reserved for the native-RDMA path (M2): peer claims the ticket
 * before activating its QP.
 */
enum class SessionState : std::uint8_t {
  kOpened,
  kClaimed,    // reserved: native-RDMA peer has claimed the ticket
  kActive,
  kCompleted,
  kFailed,
  kExpired,
};

/**
 * @brief Parameters supplied when building a new session.
 */
struct OpenSessionParams {
  std::string   session_id;
  std::string   request_id;
  std::string   bucket;
  std::string   object_key;
  OperationType op{OperationType::kGet};
  DataPath      data_path{DataPath::kHttpTcp};
  std::string   buffer_type{"host-regular"};
  std::uint64_t offset{0};
  std::uint64_t expected_size{0};
  std::string   idempotency_key;
  // multipart upload 的单个 part：data path 据此跳过整对象 Reserve；
  // expected_size 解释为本 part 字节数。
  bool          is_multipart_part{false};
};

/**
 * @brief Snapshot of a resolved transfer session.
 *
 * Owned by `SessionStore`; consumers receive `std::shared_ptr<Session>` and
 * must treat state writes via store-provided helpers, not by reaching in.
 */
struct Session {
  std::string                          session_id;
  std::string                          request_id;
  std::string                          ticket;
  std::string                          gateway_id;
  std::string                          expire_at;
  std::string                          bucket;
  std::string                          object_key;
  OperationType                        op{OperationType::kGet};
  DataPath                             data_path{DataPath::kHttpTcp};
  std::string                          buffer_type;
  std::uint64_t                        offset{0};
  std::uint64_t                        expected_size{0};
  std::string                          idempotency_key;
  bool                                 is_multipart_part{false};

  std::atomic<SessionState>            state{SessionState::kOpened};
  std::chrono::steady_clock::time_point expire_deadline{};
};

}  // namespace us3_turbo_access::gateway::core
