#pragma once

#include <atomic>
#include <functional>

#include "client/src/core/routing/transfer_path.h"
#include "client/src/data/http_data_client.h"
#include "us3_turbo_access/client/options.h"

namespace us3_turbo_access::client {

class ClientExecutor;

/**
 * 标准对象存储 HTTP 传输路径（kHostRegular host buffer ↔ gateway）。
 *
 * 与 GDS / RDMA 互不影响：内部只持 HttpDataClient 引用，不复用
 * BrpcChannel、不依赖任何 GDS / RDMA 代码；buffer 类型只接 kHostRegular。
 * available 状态由 ClientCore 在 Initialize 时按 HTTP channel 探活结果写入。
 */
class HttpTransferPath final : public TransferPath {
 public:
  /**
   * executor_provider 给 GetObject 走并发分片用：参数是 `() -> ClientExecutor*`
   * 的 lambda，让 HttpTransferPath 不直接持有线程池引用（线程池在 ClientCore
   * 里随 Initialize 延后初始化，HttpTransferPath 在构造时拿不到）。
   * 返回 nullptr 表示线程池未就绪，那一笔走单连接降级路径。
   */
  using ExecutorProvider = std::function<ClientExecutor*()>;

  HttpTransferPath(const ClientOptions& options, HttpDataClient& data_client,
                   ExecutorProvider executor_provider);

  /** ClientCore::Initialize 成功打开 HTTP channel 后调 true；Shutdown 时调 false。*/
  void SetAvailable(bool available) noexcept;

  [[nodiscard]] DataPath path() const override;
  [[nodiscard]] bool available() const override;
  [[nodiscard]] Result<TransferOutcome>
    GetObject(const RequestOptions& request,
              MutableBufferView buffer) const override;
  [[nodiscard]] Result<TransferOutcome>
    PutObject(const RequestOptions& request,
              ConstBufferView buffer) const override;

  /**
   * Multipart 单 part 上传：upload_id + part_number 来自 MultipartUpload。
   * 与 RdmaTransferPath::PutObjectPart 对称：上层 client.cpp 按 data_path
   * 分发到这里。返回的 TransferOutcome.etag 是 part_etag。
   */
  [[nodiscard]] Result<TransferOutcome>
    PutObjectPart(const RequestOptions& request, ConstBufferView buffer,
                  const std::string& upload_id,
                  std::uint32_t part_number) const;

  // Multipart convenience overload: builds RequestOptions from desc (sets length = buffer.size).
  [[nodiscard]] Result<TransferOutcome>
    PutObjectPart(const ObjectDescriptor& desc, ConstBufferView buffer,
                  const std::string& upload_id,
                  std::uint32_t part_number) const;

 private:
  // GetObject 单连接降级：当 length 小于阈值或 executor 不可用时调。
  [[nodiscard]] Result<TransferOutcome>
    GetObjectSingle(const RequestOptions& request,
                    MutableBufferView buffer) const;
  // GetObject 并发分片：把 [offset, offset+length) 拆 N 块并发拉。
  [[nodiscard]] Result<TransferOutcome>
    GetObjectParallel(const RequestOptions& request,
                      MutableBufferView buffer,
                      ClientExecutor& executor) const;

  const ClientOptions&   options_;
  HttpDataClient&        data_client_;
  ExecutorProvider       executor_provider_;
  std::atomic<bool>      available_{false};
};

}  // namespace us3_turbo_access::client
