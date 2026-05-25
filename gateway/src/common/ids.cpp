#include "common/ids.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace us3_turbo_access::gateway::common {

namespace {

std::mt19937_64& Engine() {
  thread_local std::mt19937_64 engine{std::random_device{}()};
  return engine;
}

}  // namespace

std::string MakeRandomId(std::string_view prefix) {
  auto& engine = Engine();
  const std::uint64_t hi = engine();
  const std::uint64_t lo = engine();
  std::ostringstream out;
  out << prefix << std::hex << std::setfill('0')
      << std::setw(16) << hi << std::setw(16) << lo;
  return out.str();
}

std::string MakeExpireAt(std::chrono::seconds ttl) {
  const auto now = std::chrono::system_clock::now() + ttl;
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  const std::time_t tt = static_cast<std::time_t>(secs);
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &tt);
#else
  gmtime_r(&tt, &tm_utc);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

}  // namespace us3_turbo_access::gateway::common
