#pragma once

#include <string_view>

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

class TransferPath {
 public:
  virtual ~TransferPath() = default;

  [[nodiscard]] virtual DataPath path() const = 0;
  [[nodiscard]] virtual bool available() const = 0;
  [[nodiscard]] virtual Result<TransferOutcome> GetObject(const RequestOptions& request,
                                                          MutableBufferView buffer) const = 0;
  [[nodiscard]] virtual Result<TransferOutcome> PutObject(const RequestOptions& request,
                                                          ConstBufferView buffer) const = 0;

  /**
   * 三通路共享的入口检查：路径可用 + buffer.type 匹配。失败返回 Result<bool>
   * 失败结果，调用方一行透传给上层。public 是因为各通路 .cpp 中的 anon-namespace
   * 自由函数也要调它（HTTP::Preflight / RDMA::PreflightRdma / GDS::PreflightGds）。
   *
   *   path_for_error  : 用于错误码 / 日志；DataPath::kHttpTcp 等
   *   path_name       : 人类可读字符串，进错误消息
   *   required        : 该通路要求的 buffer.type（HTTP=kHostRegular,
   *                     RDMA=kHostPinned, GDS=kCudaDevice）
   *   actual          : 来自 buffer.type
   *
   * 实现见 transfer_path.cpp；HTTP 与 RDMA 原来的私有 Preflight/PreflightRdma
   * 现在改为调本函数（B.5）。GDS 之前散落的 if(!available()) 也统一进来。
   */
  [[nodiscard]] static Result<bool>
    CommonPreflight(DataPath path_for_error, std::string_view path_name,
                    bool available, BufferType actual, BufferType required);
};

}  // namespace us3_turbo_access::client
