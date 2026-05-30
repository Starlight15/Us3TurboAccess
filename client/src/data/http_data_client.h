#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "client/src/transports/http/http_channel.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * 标准对象存储 HTTP 客户端。
 *
 * 接口分两类：
 *   整对象：HeadObject / GetObject / PutObject。
 *   Multipart：StartUpload / UploadPart / CompleteUpload / AbortUpload。
 * 公开方法都同步语义；内部 *Once 走单次 RPC，公开版本套 RetryIfRetryable
 * 做指数退避（CompleteUpload 例外，非幂等，见 .cpp）。
 *
 * 可选 end-to-end CRC32C：
 *   PUT / UploadPart 接受可选 client 端 CRC32C，写到 x-amz-checksum-crc32c
 *   请求头让 server 校验；成功返回时把 server 实算 CRC 解析放进 PutReport /
 *   PartEtag 的 server_crc32c 字段，给上层双向校验。
 *
 * 与 GDS / RDMA 通路完全隔离：内部独立 HttpChannel（protocol="http"），不复用
 * BrpcChannel（后者跑 baidu_std 协议）。URI 形态：
 *   整对象：  /v1/objects/{bucket}/{key:*}
 *   multipart init：    POST   /v1/uploads/{bucket}/{key:*}
 *   multipart part：    PUT    /v1/uploads/{upload_id}?partNumber=N
 *   multipart complete：POST   /v1/uploads/{upload_id}/complete
 *   multipart abort：   DELETE /v1/uploads/{upload_id}
 */
class HttpDataClient {
 public:
  struct GetReport {
    std::size_t    bytes{0};   // 实际写入 buffer 的字节数
    ObjectMetadata meta;        // ETag / Content-Length / x-fa-version + headers
    /** server 端实算 CRC32C：成功解出 x-amz-checksum-crc32c 响应头时填值，
     *  否则 nullopt。用于 client 端做 end-to-end 数据完整性校验。 */
    std::optional<std::uint32_t>   server_crc32c;
  };
  struct PutReport {
    std::size_t                    bytes{0};
    ObjectMetadata                 meta;
    /** server 端实算 CRC32C：成功解出 x-amz-checksum-crc32c 响应头时填值，
     *  否则 nullopt。给上层做 end-to-end 校验。 */
    std::optional<std::uint32_t>   server_crc32c;
  };

  // V2 multipart 类型
  struct StartUploadResp {
    std::string upload_id;
    std::size_t max_part_size{0};
  };
  /**
   * UploadPart 返回值。etag 用于 CompleteUpload 的 parts 列表；
   * server_crc32c 给上层做 end-to-end 校验，与 PutReport.server_crc32c 同源。
   */
  struct PartEtag {
    std::uint32_t                  part_number{0};
    std::string                    etag;
    std::optional<std::uint32_t>   server_crc32c;
  };
  struct CompleteUploadResp {
    std::string etag;
    std::string version;
    std::size_t content_length{0};
  };

  explicit HttpDataClient(const ClientOptions& options);

  [[nodiscard]] Result<bool> Initialize();
  void Shutdown();
  [[nodiscard]] bool initialized() const;

  /** HEAD /v1/objects/{bucket}/{key}：只取元信息（content_length / etag / version）。*/
  [[nodiscard]] Result<ObjectMetadata>
    HeadObject(const ObjectId& object) const;

  /**
   * GET /v1/objects/{bucket}/{key}。
   *   - length 缺省 → 拉整对象，不带 Range 头，期望 200 OK。
   *   - length 给定 → 带 Range: bytes=offset-(offset+length-1)，期望 206。
   * 写入 buffer.data 最多 buffer.size 字节；超出 buffer.size 视为参数错误。
   */
  [[nodiscard]] Result<GetReport>
    GetObject(const ObjectId& object, std::uint64_t offset,
              std::optional<std::uint64_t> length,
              MutableBufferView buffer) const;

  /**
   * PUT /v1/objects/{bucket}/{key}：buffer 全量作为 body。
   * crc32c 非空时附加 x-amz-checksum-crc32c 头，server 端做 end-to-end 校验。
   */
  [[nodiscard]] Result<PutReport>
    PutObject(const ObjectId& object, ConstBufferView buffer,
              std::optional<std::uint32_t> crc32c = std::nullopt) const;

  // ----- V2 multipart -----

  /** POST /v1/uploads/{bucket}/{key} → 创建 upload，返回 upload_id + max_part_size。*/
  [[nodiscard]] Result<StartUploadResp>
    StartUpload(const ObjectId& object,
                std::uint64_t expected_total_size,
                const std::string& idempotency_key) const;

  /** PUT /v1/uploads/{upload_id}?partNumber=N，body = buffer。crc32c 同 PutObject。*/
  [[nodiscard]] Result<PartEtag>
    UploadPart(const std::string& upload_id, std::uint32_t part_number,
               ConstBufferView buffer,
               std::optional<std::uint32_t> crc32c = std::nullopt) const;

  /** POST /v1/uploads/{upload_id}/complete，body = JSON parts list。*/
  [[nodiscard]] Result<CompleteUploadResp>
    CompleteUpload(const std::string& upload_id,
                   const std::vector<PartEtag>& parts) const;

  /** DELETE /v1/uploads/{upload_id}。*/
  [[nodiscard]] Result<bool>
    AbortUpload(const std::string& upload_id) const;

 private:
  // 实际发请求的"单次"实现，不做 retry；public 版本套 RetryIfRetryable。
  [[nodiscard]] Result<ObjectMetadata>
    HeadObjectOnce(const ObjectId& object) const;
  [[nodiscard]] Result<GetReport>
    GetObjectOnce(const ObjectId& object, std::uint64_t offset,
                  std::optional<std::uint64_t> length,
                  MutableBufferView buffer) const;
  [[nodiscard]] Result<PutReport>
    PutObjectOnce(const ObjectId& object, ConstBufferView buffer,
                  std::optional<std::uint32_t> crc32c) const;
  [[nodiscard]] Result<StartUploadResp>
    StartUploadOnce(const ObjectId& object,
                    std::uint64_t expected_total_size,
                    const std::string& idempotency_key) const;
  [[nodiscard]] Result<PartEtag>
    UploadPartOnce(const std::string& upload_id, std::uint32_t part_number,
                   ConstBufferView buffer,
                   std::optional<std::uint32_t> crc32c) const;
  [[nodiscard]] Result<CompleteUploadResp>
    CompleteUploadOnce(const std::string& upload_id,
                       const std::vector<PartEtag>& parts) const;
  [[nodiscard]] Result<bool>
    AbortUploadOnce(const std::string& upload_id) const;

  HttpChannel channel_;
};

}  // namespace us3_turbo_access::client
