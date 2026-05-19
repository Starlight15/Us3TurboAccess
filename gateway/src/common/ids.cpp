#include "common/ids.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
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

std::vector<ChunkPlanItem> BuildChunkPlan(std::uint64_t offset,
                                          std::uint64_t total_size,
                                          std::size_t chunk_size_hint) {
  std::vector<ChunkPlanItem> plan;
  if (total_size == 0U) {
    return plan;
  }
  const std::uint64_t chunk = chunk_size_hint == 0U
                                  ? total_size
                                  : static_cast<std::uint64_t>(chunk_size_hint);
  for (std::uint64_t cursor = 0; cursor < total_size; cursor += chunk) {
    const auto remaining = total_size - cursor;
    ChunkPlanItem item;
    item.offset = offset + cursor;
    item.size = std::min<std::uint64_t>(chunk, remaining);
    plan.push_back(item);
  }
  return plan;
}

}  // namespace us3_turbo_access::gateway::common
