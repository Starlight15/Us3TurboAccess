#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace us3_turbo_access::gateway::data_path::ucx {

struct UcxSessionEntry;

/**
 * RAII guard for UcxSessionEntry buffer lifecycle.
 * Ensures release function is called on scope exit (success/failure/exception).
 */
class UcxSessionEntryGuard {
 public:
  UcxSessionEntryGuard(std::shared_ptr<UcxSessionEntry> entry,
                        std::function<void(std::shared_ptr<UcxSessionEntry>)> release_fn)
      : entry_(std::move(entry)), release_fn_(std::move(release_fn)) {}

  ~UcxSessionEntryGuard() {
    if (entry_ && release_fn_) {
      release_fn_(entry_);
    }
  }

  UcxSessionEntryGuard(const UcxSessionEntryGuard&) = delete;
  UcxSessionEntryGuard& operator=(const UcxSessionEntryGuard&) = delete;
  UcxSessionEntryGuard(UcxSessionEntryGuard&&) = default;
  UcxSessionEntryGuard& operator=(UcxSessionEntryGuard&&) = default;

  [[nodiscard]] const std::shared_ptr<UcxSessionEntry>& get() const { return entry_; }

  /** 放弃 guard（不触发 release，调用方自行负责）。 */
  void release() { entry_.reset(); }

 private:
  std::shared_ptr<UcxSessionEntry>                              entry_;
  std::function<void(std::shared_ptr<UcxSessionEntry>)>         release_fn_;
};

}  // namespace us3_turbo_access::gateway::data_path::ucx
