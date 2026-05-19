#include "common/log.h"

#include <mutex>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace us3_turbo_access::gateway::common {

namespace {

std::shared_ptr<spdlog::logger> CreateDefault() {
  try {
    auto logger = spdlog::stdout_color_mt("gateway");
    logger->set_level(spdlog::level::info);
    return logger;
  } catch (const spdlog::spdlog_ex&) {
    return spdlog::get("gateway");
  }
}

}  // namespace

std::shared_ptr<spdlog::logger> EnsureLogger(
    std::shared_ptr<spdlog::logger> logger) {
  if (logger != nullptr) {
    return logger;
  }
  static std::once_flag once;
  static std::shared_ptr<spdlog::logger> fallback;
  std::call_once(once, [] { fallback = CreateDefault(); });
  return fallback;
}

}  // namespace us3_turbo_access::gateway::common
