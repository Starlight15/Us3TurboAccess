#include "data_path/ucx/ucx_buffer_pool.h"

#include <cstdlib>
#include <sys/mman.h>

namespace us3_turbo_access::gateway::data_flow::ucx {

RegisteredBuffer* UcxBufferPool::Acquire(std::size_t requested_size) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = pool_.begin(); it != pool_.end(); ++it) {
    if ((*it)->capacity >= requested_size) {
      RegisteredBuffer* slot = *it;
      pool_.erase(it);
      return slot;
    }
  }
  return nullptr;  // 调用方自行分配+注册
}

void UcxBufferPool::Return(RegisteredBuffer* slot) noexcept {
  if (!slot) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (pool_.size() < max_idle_) {
      pool_.push_back(slot);
      return;
    }
  }
  // 超出 max_idle：unmap + free
  if (slot->memh) ucp_mem_unmap(ctx_, slot->memh);
  if (slot->buf)  { ::munlock(slot->buf, slot->capacity); std::free(slot->buf); }
  delete slot;
}

void UcxBufferPool::Drain() noexcept {
  std::lock_guard<std::mutex> lk(mu_);
  for (RegisteredBuffer* slot : pool_) {
    if (slot->memh) ucp_mem_unmap(ctx_, slot->memh);
    if (slot->buf)  { ::munlock(slot->buf, slot->capacity); std::free(slot->buf); }
    delete slot;
  }
  pool_.clear();
}

}  // namespace us3_turbo_access::gateway::data_flow::ucx
