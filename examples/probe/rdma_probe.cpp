// RDMA 客户端环境探针：
//   1) RdmaResources::Open 打开 RDMA 设备
//   2) PinnedBuffer::Allocate 申请 host pinned 内存
//   3) 在临时 PD 上注册 MR；HostMemoryRegistry 校验 buffer 类型
//   4) 反例：kHostRegular / kCudaDevice 一律拒绝
//
// 用途：确认硬件 / 驱动 / 内存通路就位；正式 PUT 流程的 PD 走 cm_id->verbs
// 由 RdmaCmConnection 现场建，这里只为 probe 临时申请。

#include <infiniband/verbs.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "client/src/transports/rdma/host_memory_registry.h"
#include "client/src/transports/rdma/rdma_resources.h"
#include "us3_turbo_access/client/options.h"
#include "us3_turbo_access/client/pinned_buffer.h"
#include "us3_turbo_access/client/types.h"

namespace {

ibv_context* OpenDeviceForProbe(const std::string& name) {
  int num = 0;
  ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) {
    if (list) ibv_free_device_list(list);
    return nullptr;
  }
  ibv_context* picked = nullptr;
  for (int i = 0; i < num; ++i) {
    if (!name.empty() && std::strcmp(ibv_get_device_name(list[i]), name.c_str()) != 0) {
      continue;
    }
    picked = ibv_open_device(list[i]);
    if (picked != nullptr) break;
  }
  ibv_free_device_list(list);
  return picked;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace us3_turbo_access::client;

  RdmaClientOptions opts;
  if (argc >= 2) {
    opts.device_name = argv[1];
  }
  const std::size_t kSize = 4 * 1024 * 1024;  // 4 MiB

  std::cout << "[1] 打开 RDMA 设备 device=\""
            << (opts.device_name.empty() ? "(auto)" : opts.device_name) << "\""
            << std::endl;
  auto res = RdmaResources::Open(opts);
  if (!res.success()) {
    std::cerr << "FAIL: " << res.error().message << std::endl;
    return 1;
  }
  std::cout << "    OK: device=" << res.value()->device_name() << std::endl;

  // probe 自建临时 PD，独立于 RdmaResources（生产路径里 PD 由 cm_id 现场建）。
  ibv_context* ctx = OpenDeviceForProbe(opts.device_name);
  if (ctx == nullptr) { std::cerr << "FAIL: open device failed" << std::endl; return 1; }
  ibv_pd* pd = ibv_alloc_pd(ctx);
  if (pd == nullptr) { std::cerr << "FAIL: ibv_alloc_pd" << std::endl; (void)ibv_close_device(ctx); return 1; }

  std::cout << "[2] 分配 PinnedBuffer " << kSize << " bytes" << std::endl;
  auto buf = PinnedBuffer::Allocate(kSize);
  if (!buf.success()) {
    std::cerr << "FAIL: " << buf.error().message << std::endl;
    (void)ibv_dealloc_pd(pd);
    (void)ibv_close_device(ctx);
    return 1;
  }
  std::cout << "    OK: ptr=" << buf.value().data()
            << " size=" << buf.value().size() << std::endl;

  HostMemoryRegistry registry(pd);

  std::cout << "[3] 注册 kHostPinned buffer 到 MR" << std::endl;
  auto mr_ok = registry.Register(buf.value().view());
  if (!mr_ok.success()) {
    std::cerr << "FAIL: " << mr_ok.error().message << std::endl;
    return 1;
  }
  std::cout << "    OK: addr=" << mr_ok.value().addr()
            << " size=" << mr_ok.value().size()
            << " lkey=" << mr_ok.value().lkey()
            << " rkey=" << mr_ok.value().rkey() << std::endl;

  std::cout << "[4] 反例：kHostRegular 必须被拒" << std::endl;
  std::vector<std::byte> heap(kSize);
  ConstBufferView reg{.data = heap.data(),
                      .size = heap.size(),
                      .type = BufferType::kHostRegular};
  auto mr_bad = registry.Register(reg);
  if (mr_bad.success()) {
    std::cerr << "FAIL: kHostRegular 不应被接受" << std::endl;
    return 1;
  }
  std::cout << "    OK: 拒绝 = " << mr_bad.error().message << std::endl;

  std::cout << "[5] 反例：kCudaDevice 必须被拒" << std::endl;
  ConstBufferView cuda{.data = reinterpret_cast<const void*>(0xDEADBEEFULL),
                       .size = kSize,
                       .type = BufferType::kCudaDevice};
  auto mr_cu = registry.Register(cuda);
  if (mr_cu.success()) {
    std::cerr << "FAIL: kCudaDevice 不应被接受" << std::endl;
    return 1;
  }
  std::cout << "    OK: 拒绝 = " << mr_cu.error().message << std::endl;

  // mr_ok 析构会先 dereg_mr，然后再清 PD。
  (void)mr_ok;
  // 不再 dealloc PD：MrLease 析构需要它仍存活；让进程退出回收即可。
  // 严格生产路径里资源生命周期跟 ConnectionEntry 绑定，不在这里管。

  std::cout << "ALL OK" << std::endl;
  return 0;
}
