#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "client/src/core/upload/i_multipart_flow.h"
#include "us3_turbo_access/client/client.h"

namespace us3_turbo_access::client {

// MultipartUpload::Impl 是 client-internal 实现细节，定义在单独头文件以便
// client.cpp（StartUpload 创建 Impl）和 multipart_upload.cpp 共同可见。
struct MultipartUpload::Impl {
  std::unique_ptr<IMultipartSession> session;
  std::string        checksum_policy{"none"};
  DataPath           data_path{DataPath::kGdsCuObject};

  std::mutex                              mu;
  std::vector<IMultipartSession::PartRef> parts;
  std::size_t                             bytes_committed{0};
  bool                                    finished{false};
};

}  // namespace us3_turbo_access::client
