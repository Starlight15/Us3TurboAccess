#pragma once

#include <cstdint>
#include <string>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"
#include "client/src/core/upload/upload_coordinator.h"

namespace us3_turbo_access::client {

// MultipartUpload 内部使用的分片上传会话基类。
// UploadPart 的 RequestOptions 构建逻辑在此统一实现，
// 子类只需实现 DoPutObjectPart（传输层转发）+ Complete / Abort（协议层差异）。
class MultipartSessionBase : public IMultipartSession {
 public:
  // 上传单个 part：构建 RequestOptions → 调子类的 DoPutObjectPart
  Result<TransferOutcome> UploadPart(
      std::uint32_t part_number, std::uint64_t object_offset,
      const std::string& checksum_policy, ConstBufferView buffer) override;

  const std::string& upload_id() const noexcept override { return upload_id_; }
  std::size_t max_part_size() const noexcept override { return max_part_size_; }

 protected:
  MultipartSessionBase(ObjectId object, std::string upload_id,
                       std::size_t max_part_size, bool set_length);

  // 子类实现：将构建好的 RequestOptions 转发到对应 TransferPath::PutObjectPart
  virtual Result<TransferOutcome> DoPutObjectPart(
      const RequestOptions& request, ConstBufferView buffer,
      const std::string& upload_id, std::uint32_t part_number) = 0;

  ObjectId    object_;
  std::string upload_id_;
  std::size_t max_part_size_;
  bool        set_length_;  // GDS=false, HTTP/UCX=true
};

}  // namespace us3_turbo_access::client
