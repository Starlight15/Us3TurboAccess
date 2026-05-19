#pragma once

#include "client/src/control/metadata_client.h"
#include "client/src/data/gds_data_client.h"
#include "client/src/transports/gds/cuobject_client.h"
#include "client/src/transports/gds/gds_memory_registry.h"
#include "us3_turbo_access/client/options.h"

namespace us3_turbo_access::client {

/**
 * @brief 聚合 GDS 路径需要的所有组件引用，避免函数签名过长。
 *
 * 仅保存引用，生命周期由调用方（ClientCore）保证。
 */
struct GdsContext {
  const ClientOptions& options;
  const MetadataClient& metadata_client;
  const GdsDataClient& data_client;
  const GdsMemoryRegistry& memory_registry;
  const CuObjectClient& cuobj_client;
};

}  // namespace us3_turbo_access::client
