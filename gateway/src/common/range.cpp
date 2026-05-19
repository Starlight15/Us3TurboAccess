#include "common/range.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace us3_turbo_access::gateway::common {

namespace {

constexpr std::string_view kBytesPrefix = "bytes=";

}  // namespace

HttpRange ParseHttpRange(const std::string* header_value,
                         std::uint64_t object_size) {
  HttpRange out;
  if (header_value == nullptr || header_value->empty()) {
    out.length = object_size;
    return out;
  }
  out.partial = true;
  std::string_view value(*header_value);
  if (value.starts_with(kBytesPrefix)) {
    value.remove_prefix(kBytesPrefix.size());
  }
  const auto dash = value.find('-');
  if (dash == std::string_view::npos) {
    out.length = object_size;
    return out;
  }
  std::uint64_t start = 0;
  std::uint64_t end = object_size == 0U ? 0U : object_size - 1U;
  std::from_chars(value.data(), value.data() + static_cast<std::ptrdiff_t>(dash),
                  start);
  if (dash + 1U < value.size()) {
    std::from_chars(value.data() + static_cast<std::ptrdiff_t>(dash + 1U),
                    value.data() + static_cast<std::ptrdiff_t>(value.size()),
                    end);
  }
  if (object_size == 0U || start >= object_size) {
    out.offset = object_size;
    out.length = 0;
    out.unsatisfiable = true;
    return out;
  }
  end = std::min(end, object_size - 1U);
  out.offset = start;
  out.length = end >= start ? (end - start + 1U) : 0U;
  return out;
}

}  // namespace us3_turbo_access::gateway::common
