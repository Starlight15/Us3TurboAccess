#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "us3_turbo_access/gateway/result.h"
#include "us3_turbo_access/gateway/types.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 索引层：只管对象元数据与 multipart 协议状态，不存字节。
 * 真实实现可对接 etcd / FoundationDB / S3 metadata service 之类。
 */
class IIndexStore {
 public:
  virtual ~IIndexStore() = default;

  IIndexStore(const IIndexStore&) = delete;
  IIndexStore& operator=(const IIndexStore&) = delete;

  [[nodiscard]] virtual std::string_view kind() const noexcept = 0;

  // 查对象元数据；不存在返回 kNotFound。
  [[nodiscard]] virtual Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) = 0;

  // 登记/覆写对象索引（写完字节后调一次）。
  [[nodiscard]] virtual Result<bool>
    PutObjectIndex(std::string_view bucket, std::string_view key,
                   std::size_t content_length, std::string_view etag,
                   std::string_view version) = 0;

  [[nodiscard]] virtual Result<bool>
    DeleteObjectIndex(std::string_view bucket, std::string_view key) = 0;

  // multipart 协议状态归索引层（语义层信息，无字节）。
  [[nodiscard]] virtual Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) = 0;

  [[nodiscard]] virtual Result<bool>
    AbortMultipart(std::string_view upload_id) = 0;

  // 把 upload_id 终态登记到 (bucket, key) 对应的对象索引上。
  [[nodiscard]] virtual Result<ObjectMetadata>
    CommitMultipart(std::string_view upload_id, std::string_view bucket,
                    std::string_view key, std::size_t content_length,
                    std::string_view etag, std::string_view version) = 0;

 protected:
  IIndexStore() = default;
};

}  // namespace us3_turbo_access::gateway::backend
