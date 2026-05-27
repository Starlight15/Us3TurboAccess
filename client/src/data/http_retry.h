#pragma once

#include <chrono>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>

#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * 重试策略：指数退避 + 抖动。
 *   - max_attempts==1 等价于不重试（CompleteUpload 之类非幂等接口设这个）。
 *   - 仅对 error.retryable=true 的失败做重试；其他错误直接透传。
 *
 * 算法：第 n 次重试 sleep = min(max_backoff, initial * 2^(n-1)) * jitter，
 *      jitter ∈ [0.5, 1.5)，避免雷鸣。
 */
struct RetryPolicy {
  int                       max_attempts{3};
  std::chrono::milliseconds initial_backoff{std::chrono::milliseconds(100)};
  std::chrono::milliseconds max_backoff{std::chrono::milliseconds(2000)};
};

[[nodiscard]] inline RetryPolicy MakeRetryPolicy(const HttpClientOptions& opts) {
  RetryPolicy p;
  p.max_attempts     = opts.max_retry_attempts > 0 ? opts.max_retry_attempts : 1;
  p.initial_backoff  = opts.retry_initial_backoff;
  p.max_backoff      = opts.retry_max_backoff;
  return p;
}

/**
 * 通用 retry wrapper。fn() 必须返回 Result<T>。
 * 调用示例：
 *   auto r = RetryIfRetryable(policy, [&]{ return data_client_.PutObject(...); });
 */
template <typename Fn>
auto RetryIfRetryable(const RetryPolicy& policy, Fn&& fn)
    -> std::invoke_result_t<Fn> {
  using ResultT = std::invoke_result_t<Fn>;
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_real_distribution<double> jitter(0.5, 1.5);

  ResultT last = fn();
  for (int attempt = 1; attempt < policy.max_attempts; ++attempt) {
    if (last.success()) return last;
    if (!last.error().retryable) return last;
    auto backoff = std::chrono::milliseconds{
        static_cast<long long>(
            policy.initial_backoff.count() * (1LL << (attempt - 1)))};
    if (backoff > policy.max_backoff) backoff = policy.max_backoff;
    backoff = std::chrono::milliseconds{
        static_cast<long long>(backoff.count() * jitter(rng))};
    std::this_thread::sleep_for(backoff);
    last = fn();
  }
  return last;
}

}  // namespace us3_turbo_access::client
