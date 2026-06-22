#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/status.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

// IMultipartSession —— 一次 multipart 上传的“链路会话”视角。
//
// 由对应链路的 IMultipartFlow::CreateSession 创建，并由 MultipartUpload
// 独占持有。会话生命周期内绑定以下上下文，调用方无需再传：
//   - 目标 ObjectId
//   - upload_id（链路打开 multipart 后由 gateway 分配）
//   - max_part_size（gateway 强制上限）
//   - 各链路特有的内部资源（如 RDMA endpoint 池 / GDS context 等）
//
// 调用方在 UploadPart 时只需要给 part 级变量：
//   - part_number
//   - object_offset
//   - checksum_policy（运行期可被 set_checksum_policy 调整）
//   - buffer
//
// 这个接口不再尝试给三条链路提供“统一的 multipart 上传形状”——
// 三条链路各自实现 session，可以在内部自由演化协议细节。
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

// IMultipartFlow —— “链路 multipart session 工厂”。
//
// 不再承担公共 multipart 流程实现，唯一职责就是接收一个 ObjectDescriptor，
// 打开链路上的 multipart 会话（control plane RPC + 可能的端点准备），
// 然后把后续所有 part 级操作交给 IMultipartSession。
//
// UploadCoordinator 按 DataFlow 选出对应 flow；flow 的生命周期与 Client 绑定。
class IMultipartFlow {
 public:
  virtual ~IMultipartFlow() = default;

  // 成功时把新创建的 session 写入 *out；失败时 *out 不动，返回带 Error 的 Status。
  // out 必须非 nullptr，否则返回 kInvalidArgument。
  [[nodiscard]] virtual Status
    CreateSession(const ObjectDescriptor& desc,
                  std::unique_ptr<IMultipartSession>* out) = 0;
};

}  // namespace us3_turbo_access::client
