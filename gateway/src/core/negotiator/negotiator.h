#pragma once

#include <memory>
#include <string>

#include <spdlog/logger.h>

#include "core/session/rdma_parameters.h"
#include "core/session/session.h"
#include "us3_turbo_access/gateway/result.h"

namespace us3_turbo_access::gateway::core {
class SessionStore;
class TransferEngine;
}  // namespace us3_turbo_access::gateway::core

namespace us3_turbo_access::gateway::data_path::gds {
class GdsExecutor;
}  // namespace us3_turbo_access::gateway::data_path::gds

namespace us3_turbo_access::gateway::core {

/**
 * @brief Output of a successful session negotiation.
 *
 * Carries the persisted session plus the effective transport endpoint and
 * parameters the client must use — these may differ from the session's
 * default fields when GDS (or another path-specific override) is selected.
 */
struct NegotiationOutcome {
  std::shared_ptr<Session>   session;
  std::string                gateway_endpoint;
  RdmaParameters             rdma_parameters;
};

/**
 * @brief Owns the session-negotiation business logic.
 *
 * Pulls together the @ref SessionStore (creation + idempotency), the
 * @ref TransferEngine (backend reservations for chunked PUT), and the GDS
 * executor (availability checks, endpoint resolution). The control-plane
 * service hands a fully-parsed `NegotiateRequest` over and translates the
 * @ref NegotiationOutcome back into protobuf — no business logic lives in
 * the adapter.
 */
class Negotiator {
 public:
  Negotiator(std::string public_host, int gds_rdma_port,
             SessionStore& sessions, TransferEngine& transfers,
             data_path::gds::GdsExecutor* gds_executor,
             std::shared_ptr<spdlog::logger> logger);

  Negotiator(const Negotiator&) = delete;
  Negotiator& operator=(const Negotiator&) = delete;

  [[nodiscard]] Result<NegotiationOutcome> Negotiate(const NegotiateRequest& req);

 private:
  std::string                     public_host_;
  int                              gds_rdma_port_{0};
  SessionStore&                    sessions_;
  TransferEngine&                  transfers_;
  data_path::gds::GdsExecutor*     gds_executor_{nullptr};
  std::shared_ptr<spdlog::logger>  logger_;
};

}  // namespace us3_turbo_access::gateway::core
