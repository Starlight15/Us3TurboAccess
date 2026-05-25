#include "data_path/gds/cuobj_resources.h"

#include <cstdlib>

namespace us3_turbo_access::gateway::data_path::gds {

HostBuffer::~HostBuffer() {
  if (data_ != nullptr) {
    std::free(data_);
  }
}

}  // namespace us3_turbo_access::gateway::data_path::gds
