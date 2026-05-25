#include "client/src/core/client/client_core.h"

#include "client/src/control/metadata_client.h"
#include "client/src/core/client/capability_probe.h"
#include "client/src/core/common/errors.h"
#include "client/src/core/gds/gds_context.h"
#include "client/src/core/gds/gds_transfer_path.h"
#include "client/src/core/rdma/rdma_transfer_path.h"
#include "client/src/core/routing/transfer_router.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"

namespace us3_turbo_access::client {

struct ClientCore::Impl {
  explicit Impl(ClientOptions opts)
      : options(std::move(opts)),
        caps(DetectPlatformCapabilities(options)),
        metadata_client(options),
        gds_data_client(options),
        gds_executor(caps, GdsContext{.options = options,
                                      .metadata_client = metadata_client,
                                      .data_client = gds_data_client,
                                      .memory_registry = gds_memory_registry,
                                      .cuobj_client = cuobject_client}),
        transfer_router(options.data_path, gds_executor, rdma_executor) {}

  ClientOptions options;
  PlatformCapabilities caps;
  MetadataClient metadata_client;
  GdsDataClient gds_data_client;
  GdsMemoryRegistry gds_memory_registry;
  CuObjectClient cuobject_client;
  GdsTransferPath gds_executor;
  RdmaTransferPath rdma_executor;
  TransferRouter transfer_router;
  bool initialized{false};
};

ClientCore::ClientCore(ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

ClientCore::~ClientCore() = default;

Result<bool> ClientCore::Initialize() {
  if (impl_->initialized) {
    return Result<bool>::Success(true);
  }
  if (impl_->options.endpoint.empty()) {
    return Result<bool>::Failure(MakeInvalidArgument("endpoint must not be empty"));
  }

  auto control_init = impl_->metadata_client.Initialize();
  if (!control_init.success()) {
    return control_init;
  }
  auto gds_init = impl_->gds_data_client.Initialize();
  if (!gds_init.success()) {
    return gds_init;
  }

  impl_->initialized = true;
  return Result<bool>::Success(true);
}

void ClientCore::Shutdown() {
  impl_->metadata_client.Shutdown();
  impl_->gds_data_client.Shutdown();
  impl_->initialized = false;
}

bool ClientCore::initialized() const { return impl_->initialized; }

const PlatformCapabilities& ClientCore::capabilities() const { return impl_->caps; }

const ClientOptions& ClientCore::options() const { return impl_->options; }

const MetadataClient& ClientCore::metadata_client() const { return impl_->metadata_client; }

const TransferRouter& ClientCore::transfer_router() const { return impl_->transfer_router; }

GdsDataClient& ClientCore::gds_data_client() { return impl_->gds_data_client; }
GdsMemoryRegistry& ClientCore::gds_memory_registry() { return impl_->gds_memory_registry; }
const CuObjectClient& ClientCore::cuobj_client() const { return impl_->cuobject_client; }

}  // namespace us3_turbo_access::client
