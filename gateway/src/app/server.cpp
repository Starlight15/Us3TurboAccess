#include "us3_turbo_access/gateway/server.h"

#include <utility>

#include "runtime/gateway_runtime.h"

namespace us3_turbo_access::gateway {

GatewayServer::GatewayServer(GatewayOptions options)
    : runtime_(std::make_unique<runtime::GatewayRuntime>(std::move(options))) {}

GatewayServer::~GatewayServer() = default;

Result<bool> GatewayServer::Start() {
  return runtime_->Initialize();
}

void GatewayServer::Shutdown() {
  runtime_->Shutdown();
}

void GatewayServer::RunUntilAskedToQuit() {
  runtime_->Run();
}

}  // namespace us3_turbo_access::gateway
