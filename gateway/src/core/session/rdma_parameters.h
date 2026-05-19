#pragma once

#include <string>
#include <unordered_map>

namespace us3_turbo_access::gateway::core {

/**
 * @brief Strongly-typed view of the RDMA / GDS connection parameters that
 *        flow from the gateway to the client during session negotiation.
 *
 * The wire-level representation is a `map<string, string>` for forward
 * compatibility; this struct calls out the well-known fields (host / port)
 * so the rest of the codebase stops sprinkling string literals around, while
 * the @ref extras map keeps room for future ad-hoc fields.
 */
struct RdmaParameters {
  std::string                                  host;
  std::string                                  port;
  std::unordered_map<std::string, std::string> extras;

  /** @brief Serialise to the wire map used by the control-plane proto. */
  [[nodiscard]] std::unordered_map<std::string, std::string> ToMap() const;

  /** @brief Parse a wire map (well-known keys go to fields; rest to extras). */
  static RdmaParameters FromMap(
      const std::unordered_map<std::string, std::string>& map);
};

}  // namespace us3_turbo_access::gateway::core
