#include "data_path/data_path_executor.h"

namespace us3_turbo_access::gateway::data_path {

Result<bool> IDataPathExecutor::OnSessionOpened(const core::Session&) {
  return Result<bool>::Success(true);
}

}  // namespace us3_turbo_access::gateway::data_path
