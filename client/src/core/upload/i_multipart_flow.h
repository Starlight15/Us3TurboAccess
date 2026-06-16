#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "us3_turbo_access/client/result.h"
#include "us3_turbo_access/client/types.h"

namespace us3_turbo_access::client {

class IMultipartFlow {
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

  virtual ~IMultipartFlow() = default;

  [[nodiscard]] virtual Result<StartUploadResult>
    StartMultipart(const ObjectDescriptor& desc) = 0;
  [[nodiscard]] virtual Result<TransferOutcome>
    UploadPart(const ObjectDescriptor& desc, const std::string& upload_id,
               std::uint32_t part_number, ConstBufferView buffer) = 0;
  [[nodiscard]] virtual Result<CompleteResult>
    CompleteMultipart(const std::string& upload_id,
                      const std::vector<PartRef>& parts) = 0;
  [[nodiscard]] virtual Result<bool>
    AbortMultipart(const std::string& upload_id) = 0;
};

}  // namespace us3_turbo_access::client
