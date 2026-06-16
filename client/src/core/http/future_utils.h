#pragma once

#include <future>
#include <utility>
#include <vector>

namespace us3_turbo_access::client {

// 并发等待所有 future，返回结果向量。
// 任务已在 executor 中并发执行，get() 按提交顺序阻塞等待完成。
template <typename T>
std::vector<T> when_all(std::vector<std::future<T>>& futs) {
  std::vector<T> results;
  results.reserve(futs.size());
  for (auto& f : futs) {
    results.push_back(f.get());
  }
  return results;
}

}  // namespace us3_turbo_access::client
