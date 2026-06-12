#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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
 * PrepareTransfer 时从 UcxBufferPool Acquire 一个已注册 MR 的 slot；
 * client ucp_put_nbx 写入后发 AM_WRITE_DONE；OnAmWriteDone 触发时：
 *   - 若 CommitObject/CommitPart 已到达且注册了 pending_commit：直接执行（零等待）
 *   - 否则置 write_done，CommitObject/CommitPart 到达时检查后立即执行
 */
struct UcxSessionEntry {
  std::string              session_id;
  std::string              bucket;
  std::string              object_key;
  std::size_t              expected_bytes{0};

  // 已注册 MR 的 buffer slot（PrepareTransfer Acquire，CommitObject/Abort Return）
  RegisteredBuffer*        slot{nullptr};
  std::size_t              transfer_bytes{0};

  // 异步 commit 协议：write_done 和 pending_commit 共用同一把锁
  // write_done=true 表示 AM_WRITE_DONE 已收到，数据已在 buffer
  // pending_commit 非空表示 CommitObject/Part RPC 已到达但数据尚未就绪，等 AM 触发
  std::atomic<bool>        write_done{false};
  std::mutex               commit_mu;
  std::function<void()>    pending_commit;  // 由 OnAmWriteDone 在 ProgressLoop 线程调用
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
