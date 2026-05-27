#pragma once

#include <cstdint>

/*
 * Wire-format definition shared between the RDMA gateway and SDK.
 *
 * The struct below travels through librdmacm's ACCEPT private_data field
 * during the connection establishment handshake. Because both client and
 * gateway compile units must agree on the byte layout, the header lives in
 * the shared include/ tree and is consumed by both sides.
 *
 * Forward compatibility: future field additions bump the version. An older
 * client receiving an unrecognised version must refuse the connection
 * rather than misparse the credentials.
 */

namespace us3_turbo_access::common {

/** ASCII tag 'U3TR' used to identify our private_data payload. */
inline constexpr std::uint32_t kRdmaCredentialsMagic   = 0x52543355u;  // 'U3TR'
/** Wire-layout version. Bump on every breaking change. */
inline constexpr std::uint32_t kRdmaCredentialsVersion = 1;

/**
 * @brief Connection credentials sent by the gateway on rdma_accept.
 *
 * The client reads these from the ESTABLISHED event. @c conn_token
 * identifies a connection entry inside the gateway's
 * RdmaConnectionRegistry.
 */
struct RdmaConnectCredentials {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint64_t conn_token;
};
static_assert(sizeof(RdmaConnectCredentials) == 16,
              "wire layout must stay 16 bytes for forward compat");

}  // namespace us3_turbo_access::common
