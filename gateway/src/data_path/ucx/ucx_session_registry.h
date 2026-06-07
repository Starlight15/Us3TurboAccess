#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ucp/api/ucp.h>

#include "data_path/ucx/ucx_buffer_pool.h"

namespace us3_turbo_access::gateway::data_path::ucx {

/**
 * 一次 UCX RMA WRITE session 的服务端状态。
 *
 * PrepareTransfer 时从 UcxBufferPool Acquire 一个已注册 MR 的 slot（含
 * gw_raddr/packed_rkey），直接下发给 client；client PUT 写入后发 AM_WRITE_DONE；
 * CommitObject 等 write_done 后 CRC + WriteRange 落盘，再 Return slot 到 pool。
 */
struct UcxSessionEntry {
  std::string              session_id;
  std::string              bucket;
  std::string              object_key;
  std::size_t              expected_bytes{0};

  // 已注册 MR 的 buffer slot（PrepareTransfer Acquire，CommitObject/Abort Return）
  RegisteredBuffer*        slot{nullptr};
  std::size_t              transfer_bytes{0};

  // write_done：client AM_WRITE_DONE 后置位，CommitObject 等待
  std::atomic<bool>        write_done{false};
  std::mutex               write_mu;
  std::condition_variable  write_cv;
};

class UcxSessionRegistry {
 public:
  UcxSessionRegistry() = default;
  ~UcxSessionRegistry() = default;

  [[nodiscard]] std::shared_ptr<UcxSessionEntry>
    Create(std::string session_id, std::string bucket,
           std::string object_key, std::size_t expected_bytes);

  [[nodiscard]] std::shared_ptr<UcxSessionEntry>
    Find(std::string_view session_id) const;

  /** 移除并返回 entry（调用方负责释放 buffer）。 */
  std::shared_ptr<UcxSessionEntry> Erase(std::string_view session_id);

 private:
  mutable std::mutex                                                   mu_;
  std::unordered_map<std::string, std::shared_ptr<UcxSessionEntry>>   entries_;
};

}  // namespace us3_turbo_access::gateway::data_path::ucx
