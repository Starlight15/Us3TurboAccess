#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

// 一次 multipart 上传的会话视角。Start 之后，descriptor / upload_id /
// max_part_size 全部由 session 内部保有，调用方只需要给 part 级变量。
// 每条链路（HTTP / RDMA / GDS）有自己的 session 实现，可以在内部保留链路
// 特有的上下文，而不必与其它链路共享一个统一参数形状。
class IMultipartSession {
 public:
  struct PartRef {
    std::uint32_t part_number{0};
    std::string   etag;
  };
  struct CompleteResult {
    std::string etag;
    std::string version;
    std::size_t content_length{0};
  };

  virtual ~IMultipartSession() = default;

  [[nodiscard]] virtual const std::string& upload_id() const noexcept = 0;
  [[nodiscard]] virtual std::size_t        max_part_size() const noexcept = 0;

  [[nodiscard]] virtual Result<TransferOutcome>
    UploadPart(std::uint32_t  part_number,
               std::uint64_t  object_offset,
               const std::string& checksum_policy,
               ConstBufferView    buffer) = 0;

  [[nodiscard]] virtual Result<CompleteResult>
    Complete(const std::vector<PartRef>& parts) = 0;

  [[nodiscard]] virtual Result<bool> Abort() = 0;
};

// Flow 退化为 session 工厂：只负责 Start，把 descriptor 一次性交给 session。
class IMultipartFlow {
 public:
  virtual ~IMultipartFlow() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<IMultipartSession>>
    Start(const ObjectDescriptor& desc) = 0;
};

}  // namespace us3_turbo_access::client
