#include "core/negotiator/negotiator.h"

#include <utility>

#include "common/error.h"
#include "core/session/session_store.h"
#include "core/transfer/transfer_engine.h"
#include "data_path/gds/gds_executor.h"

namespace us3_turbo_access::gateway::core {

Negotiator::Negotiator(std::string public_host, int gds_rdma_port,
                       SessionStore& sessions, TransferEngine& transfers,
                       data_path::gds::GdsExecutor* gds_executor,
                       std::shared_ptr<spdlog::logger> logger)
    : public_host_(std::move(public_host)),
      gds_rdma_port_(gds_rdma_port),
      sessions_(sessions),
      transfers_(transfers),
      gds_executor_(gds_executor),
      logger_(std::move(logger)) {}

Result<NegotiationOutcome> Negotiator::Negotiate(const NegotiateRequest& req) {
  auto created = sessions_.Create(req);
  if (!created.success()) {
    return Result<NegotiationOutcome>::Failure(created.error());
  }
  const auto& session = *created.value();

  NegotiationOutcome outcome;
  outcome.session = created.value();
  outcome.gateway_endpoint = session.gateway_endpoint;
  outcome.rdma_parameters = session.rdma_parameters;

  switch (session.data_path) {
    case DataPath::kHttpTcp:
      return Result<NegotiationOutcome>::Success(std::move(outcome));

    case DataPath::kGdsCuObject: {
      if (gds_executor_ == nullptr || !gds_executor_->available()) {
        return Result<NegotiationOutcome>::Failure(common::MakeError(
            ErrorCode::kRdmaUnavailable,
            "gds-cuobject service is not available on gateway"));
      }
      outcome.gateway_endpoint = gds_executor_->endpoint();
      outcome.rdma_parameters.host = public_host_;
      outcome.rdma_parameters.port = std::to_string(gds_rdma_port_);
      if (session.op == OperationType::kPut && session.expected_size != 0U) {
        auto reserved = transfers_.Reserve(
            session.bucket, session.object_key,
            static_cast<std::size_t>(session.expected_size));
        if (!reserved.success()) {
          return Result<NegotiationOutcome>::Failure(reserved.error());
        }
      }
      sessions_.UpdateRdmaParameters(session.session_id,
                                     outcome.rdma_parameters);
      return Result<NegotiationOutcome>::Success(std::move(outcome));
    }

    case DataPath::kNativeRdma:
      return Result<NegotiationOutcome>::Failure(common::MakeError(
          ErrorCode::kRdmaUnavailable,
          "native-rdma data path not enabled in M1"));
  }

  return Result<NegotiationOutcome>::Failure(common::MakeError(
      ErrorCode::kBadRequest, "unsupported data path"));
}

}  // namespace us3_turbo_access::gateway::core
