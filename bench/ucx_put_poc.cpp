// UCX PUT PoC：单文件 server+client，用 ucp_put_nbx 测 RDMA 单边写吞吐。
//
// 用途：验证 UCX 1.18 + Mellanox rc_mlx5 在我们的硬件上的实际性能上限，
// 作为 RDMA path 是否要从自管 verbs 切换到 UCX 的决策依据。
//
// 流程：
//   server: ucp_init → worker → listener → accept ep → register buffer →
//           AM send (raddr, rkey_packed) → wait client put 完成
//   client: ucp_init → worker → ep → AM recv (raddr, rkey) → ucp_ep_rkey_unpack →
//           N 次 ucp_put_nbx + ucp_ep_flush → 报告吞吐
//
// 与我们现有的 brpc baidu_std + verbs 实现不共享任何代码，避免互相影响。
// 用法：
//   server : ./us3_turbo_access_ucx_put_poc --server --port=19990
//   client : ./us3_turbo_access_ucx_put_poc --client 192.168.1.198 --port=19990
//            [--size=4194304] [--count=64] [--warmup=8]

#include <ucp/api/ucp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

// AM ID：自定义消息类型，server 用它把 (raddr, rkey) 发给 client。
constexpr unsigned kAmIdBufferInfo = 1;

struct PocArgs {
  bool        is_server   = false;
  bool        is_client   = false;
  std::string server_host;
  uint16_t    port        = 19990;
  std::size_t size        = 4 * 1024 * 1024;  // 单笔 PUT 字节
  std::size_t count       = 64;
  std::size_t warmup      = 8;
};

bool ParseArgs(int argc, char** argv, PocArgs& a) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto eat = [&](const std::string& prefix) -> std::string {
      if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
      return {};
    };
    if (arg == "--server") { a.is_server = true; continue; }
    if (arg == "--client") {
      a.is_client = true;
      if (i + 1 < argc) { a.server_host = argv[++i]; }
      continue;
    }
    if (auto v = eat("--port="); !v.empty()) { a.port = static_cast<uint16_t>(std::atoi(v.c_str())); continue; }
    if (auto v = eat("--size="); !v.empty()) { a.size = std::strtoull(v.c_str(), nullptr, 10); continue; }
    if (auto v = eat("--count="); !v.empty()) { a.count = std::strtoull(v.c_str(), nullptr, 10); continue; }
    if (auto v = eat("--warmup="); !v.empty()) { a.warmup = std::strtoull(v.c_str(), nullptr, 10); continue; }
  }
  if (a.is_server == a.is_client) {
    std::cerr << "must specify exactly one of --server / --client" << std::endl;
    return false;
  }
  if (a.is_client && a.server_host.empty()) {
    std::cerr << "client requires server host" << std::endl;
    return false;
  }
  return true;
}

// ---- Common: 进度循环 ---------------------------------------------

void ProgressUntil(ucp_worker_h worker, std::atomic<bool>& done) {
  while (!done.load(std::memory_order_acquire)) {
    ucp_worker_progress(worker);
  }
}

// PUT 完成回调：把状态写到 user_data 指向的 status，并 set flag。
struct PutCtx {
  std::atomic<bool>  done{false};
  ucs_status_t       status{UCS_OK};
};

void OnPutComplete(void* request, ucs_status_t status, void* user_data) {
  (void)request;
  auto* ctx = static_cast<PutCtx*>(user_data);
  ctx->status = status;
  ctx->done.store(true, std::memory_order_release);
}

// AM recv 回调（server 不接 AM，client 接 server 发的 buffer info）
struct AmRecvCtx {
  std::atomic<bool> got{false};
  std::vector<uint8_t> payload;  // 拷贝 buffer info（raddr + packed rkey）
};

ucs_status_t OnAmBufferInfo(void* arg, const void* header, size_t header_len,
                            void* data, size_t length,
                            const ucp_am_recv_param_t* param) {
  (void)header; (void)header_len; (void)param;
  auto* ctx = static_cast<AmRecvCtx*>(arg);
  ctx->payload.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + length);
  ctx->got.store(true, std::memory_order_release);
  return UCS_OK;
}

// ---- Server ---------------------------------------------------

struct ServerState {
  ucp_listener_h listener = nullptr;
  ucp_ep_h       client_ep = nullptr;
  std::atomic<bool> got_ep{false};
};

void OnConnReq(ucp_conn_request_h conn_req, void* arg) {
  auto* s = static_cast<ServerState*>(arg);
  ucp_ep_params_t ep_params{};
  ep_params.field_mask   = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  ep_params.conn_request = conn_req;
  ucp_ep_h ep = nullptr;
  ucp_worker_h worker = nullptr;
  // worker 从 arg 拿不到，这里走 listener 的 worker 默认行为：
  // 实际我们用 ucp_listener_accept 模式，这里 ep_params 自带 worker 信息。
  // 简化：直接由 server 主循环里 accept。
  (void)worker; (void)ep;
  s->got_ep.store(true, std::memory_order_release);
  // 把 conn_req 暂存（用 atomic 不优雅，PoC 简化）
  // 这里直接构造 ep
  static thread_local ucp_conn_request_h pending = nullptr;
  pending = conn_req;
  // server 主循环会读 pending（PoC 简化）
  // 实际更优雅是把 conn_req 排进 queue
  // 这里不实现 queue，假定 client 串行连接
  s->listener = nullptr;  // 防止未使用
}

// 简化版 server：主循环 accept 一个 ep，做 PUT 目标 + AM 通知 client。
int RunServer(const PocArgs& a) {
  // 1. context
  ucp_params_t ucp_params{};
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ucp_params.features   = UCP_FEATURE_RMA | UCP_FEATURE_AM;
  ucp_context_h ctx = nullptr;
  if (ucp_init(&ucp_params, nullptr, &ctx) != UCS_OK) {
    std::cerr << "ucp_init failed" << std::endl; return 1;
  }

  // 2. worker（多线程模式让进度可在另一线程跑；PoC 用 single）
  ucp_worker_params_t wp{};
  wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wp.thread_mode = UCS_THREAD_MODE_SINGLE;
  ucp_worker_h worker = nullptr;
  if (ucp_worker_create(ctx, &wp, &worker) != UCS_OK) {
    std::cerr << "ucp_worker_create failed" << std::endl; return 1;
  }

  // 3. listener（保存 conn_req 到一个 pending 指针）
  static ucp_conn_request_h pending_conn = nullptr;
  static std::atomic<bool>   pending_set{false};
  auto on_conn = +[](ucp_conn_request_h cr, void*) {
    pending_conn = cr;
    pending_set.store(true, std::memory_order_release);
  };

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons(a.port);

  ucp_listener_params_t lp{};
  lp.field_mask                 = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                    UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  lp.sockaddr.addr              = reinterpret_cast<const sockaddr*>(&addr);
  lp.sockaddr.addrlen           = sizeof(addr);
  lp.conn_handler.cb            = on_conn;
  lp.conn_handler.arg           = nullptr;

  ucp_listener_h listener = nullptr;
  if (ucp_listener_create(worker, &lp, &listener) != UCS_OK) {
    std::cerr << "ucp_listener_create failed" << std::endl; return 1;
  }
  std::cerr << "[server] listening on 0.0.0.0:" << a.port << std::endl;

  // 4. 等 client 连接
  while (!pending_set.load(std::memory_order_acquire)) {
    ucp_worker_progress(worker);
  }

  // 5. accept ep
  ucp_ep_params_t ep_params{};
  ep_params.field_mask   = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  ep_params.conn_request = pending_conn;
  ucp_ep_h ep = nullptr;
  if (ucp_ep_create(worker, &ep_params, &ep) != UCS_OK) {
    std::cerr << "ucp_ep_create (accept) failed" << std::endl; return 1;
  }
  std::cerr << "[server] accepted client" << std::endl;

  // 6. 注册 buffer（page-aligned + mlocked 模拟 production 路径）
  void* buf = std::aligned_alloc(4096, a.size);
  if (buf == nullptr) { std::cerr << "aligned_alloc failed" << std::endl; return 1; }
  if (::mlock(buf, a.size) != 0) {
    std::cerr << "mlock failed (check ulimit -l)" << std::endl;
  }
  ucp_mem_h memh = nullptr;
  ucp_mem_map_params_t mp{};
  mp.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mp.address    = buf;
  mp.length     = a.size;
  if (ucp_mem_map(ctx, &mp, &memh) != UCS_OK) {
    std::cerr << "ucp_mem_map failed" << std::endl; return 1;
  }

  // 7. pack rkey
  void* rkey_buf = nullptr;
  size_t rkey_len = 0;
  if (ucp_rkey_pack(ctx, memh, &rkey_buf, &rkey_len) != UCS_OK) {
    std::cerr << "ucp_rkey_pack failed" << std::endl; return 1;
  }
  std::cerr << "[server] buffer addr=" << buf << " rkey_len=" << rkey_len << std::endl;

  // 8. 把 (raddr, rkey) 通过 AM 发给 client
  //    payload 布局：[uint64 raddr][uint32 rkey_len][rkey_bytes...]
  std::vector<uint8_t> payload;
  payload.resize(sizeof(uint64_t) + sizeof(uint32_t) + rkey_len);
  uint64_t raddr = reinterpret_cast<uint64_t>(buf);
  std::memcpy(payload.data(), &raddr, sizeof(raddr));
  uint32_t rkey_len32 = static_cast<uint32_t>(rkey_len);
  std::memcpy(payload.data() + sizeof(raddr), &rkey_len32, sizeof(rkey_len32));
  std::memcpy(payload.data() + sizeof(raddr) + sizeof(rkey_len32),
              rkey_buf, rkey_len);

  ucp_request_param_t sp{};
  sp.op_attr_mask  = UCP_OP_ATTR_FIELD_FLAGS;
  sp.flags         = UCP_AM_SEND_FLAG_EAGER;  // 立即发，不等
  void* sreq = ucp_am_send_nbx(ep, kAmIdBufferInfo, nullptr, 0,
                                 payload.data(), payload.size(), &sp);
  if (UCS_PTR_IS_ERR(sreq)) {
    std::cerr << "ucp_am_send_nbx failed: "
              << ucs_status_string(UCS_PTR_STATUS(sreq)) << std::endl;
    return 1;
  }
  if (UCS_PTR_IS_PTR(sreq)) {
    while (ucp_request_check_status(sreq) == UCS_INPROGRESS) {
      ucp_worker_progress(worker);
    }
    ucp_request_free(sreq);
  }
  std::cerr << "[server] sent buffer info, waiting for client (Ctrl-C to quit)" << std::endl;

  // 9. 持续 progress 让 client 的 PUT 能完成（server 不主动收，UCX 内部处理）
  //    简化：跑足够久（10 秒）然后退出
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    ucp_worker_progress(worker);
  }

  // 10. 清理
  ucp_rkey_buffer_release(rkey_buf);
  ucp_mem_unmap(ctx, memh);
  ::munlock(buf, a.size);
  std::free(buf);
  ucp_listener_destroy(listener);
  // ep close：使用 force flag，PoC 不在乎残留
  void* close_req = ucp_ep_close_nbx(ep, nullptr);
  if (UCS_PTR_IS_PTR(close_req)) {
    while (ucp_request_check_status(close_req) == UCS_INPROGRESS) {
      ucp_worker_progress(worker);
    }
    ucp_request_free(close_req);
  }
  ucp_worker_destroy(worker);
  ucp_cleanup(ctx);
  return 0;
}

// ---- Client ---------------------------------------------------

int RunClient(const PocArgs& a) {
  // 1. context（也启用 RMA + AM）
  ucp_params_t ucp_params{};
  ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
  ucp_params.features   = UCP_FEATURE_RMA | UCP_FEATURE_AM;
  ucp_context_h ctx = nullptr;
  if (ucp_init(&ucp_params, nullptr, &ctx) != UCS_OK) {
    std::cerr << "ucp_init failed" << std::endl; return 1;
  }

  ucp_worker_params_t wp{};
  wp.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  wp.thread_mode = UCS_THREAD_MODE_SINGLE;
  ucp_worker_h worker = nullptr;
  if (ucp_worker_create(ctx, &wp, &worker) != UCS_OK) {
    std::cerr << "ucp_worker_create failed" << std::endl; return 1;
  }

  // 2. 注册 AM handler（接收 server 的 buffer info）
  AmRecvCtx am_ctx;
  ucp_am_handler_param_t ap{};
  ap.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                    UCP_AM_HANDLER_PARAM_FIELD_CB |
                    UCP_AM_HANDLER_PARAM_FIELD_ARG;
  ap.id  = kAmIdBufferInfo;
  ap.cb  = OnAmBufferInfo;
  ap.arg = &am_ctx;
  if (ucp_worker_set_am_recv_handler(worker, &ap) != UCS_OK) {
    std::cerr << "set_am_recv_handler failed" << std::endl; return 1;
  }

  // 3. 连 server
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(a.port);
  if (inet_pton(AF_INET, a.server_host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "bad host" << std::endl; return 1;
  }
  ucp_ep_params_t ep_params{};
  ep_params.field_mask     = UCP_EP_PARAM_FIELD_FLAGS |
                              UCP_EP_PARAM_FIELD_SOCK_ADDR;
  ep_params.flags          = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
  ep_params.sockaddr.addr  = reinterpret_cast<const sockaddr*>(&addr);
  ep_params.sockaddr.addrlen = sizeof(addr);
  ucp_ep_h ep = nullptr;
  if (ucp_ep_create(worker, &ep_params, &ep) != UCS_OK) {
    std::cerr << "ucp_ep_create failed" << std::endl; return 1;
  }
  std::cerr << "[client] connecting to " << a.server_host << ":" << a.port << std::endl;

  // 4. 等 server 把 buffer info 发过来
  while (!am_ctx.got.load(std::memory_order_acquire)) {
    ucp_worker_progress(worker);
  }
  uint64_t raddr = 0;
  uint32_t rkey_len = 0;
  std::memcpy(&raddr, am_ctx.payload.data(), sizeof(raddr));
  std::memcpy(&rkey_len, am_ctx.payload.data() + sizeof(raddr), sizeof(rkey_len));
  const uint8_t* rkey_bytes = am_ctx.payload.data() + sizeof(raddr) + sizeof(rkey_len);
  std::cerr << "[client] got server buffer raddr=0x" << std::hex << raddr
            << " rkey_len=" << std::dec << rkey_len << std::endl;

  // 5. 解 rkey
  ucp_rkey_h rkey = nullptr;
  if (ucp_ep_rkey_unpack(ep, rkey_bytes, &rkey) != UCS_OK) {
    std::cerr << "ucp_ep_rkey_unpack failed" << std::endl; return 1;
  }

  // 6. 注册本地 buffer
  void* buf = std::aligned_alloc(4096, a.size);
  if (buf == nullptr) { std::cerr << "aligned_alloc failed" << std::endl; return 1; }
  // 填随机数据（避免内核压缩 zero page）
  std::memset(buf, 0xAB, a.size);
  if (::mlock(buf, a.size) != 0) {
    std::cerr << "mlock failed (check ulimit -l)" << std::endl;
  }
  ucp_mem_h memh = nullptr;
  ucp_mem_map_params_t mp{};
  mp.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH;
  mp.address    = buf;
  mp.length     = a.size;
  if (ucp_mem_map(ctx, &mp, &memh) != UCS_OK) {
    std::cerr << "ucp_mem_map (client) failed" << std::endl; return 1;
  }

  // 7. 一笔 PUT 闭包
  auto do_one_put = [&](void* local_buf, std::size_t len, uint64_t target) {
    PutCtx pc;
    ucp_request_param_t pp{};
    pp.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                        UCP_OP_ATTR_FIELD_USER_DATA;
    pp.cb.send      = OnPutComplete;
    pp.user_data    = &pc;
    void* req = ucp_put_nbx(ep, local_buf, len, target, rkey, &pp);
    if (req == nullptr) {
      // 立即完成（inline）：cb 不会调
      return UCS_OK;
    }
    if (UCS_PTR_IS_ERR(req)) {
      return UCS_PTR_STATUS(req);
    }
    while (!pc.done.load(std::memory_order_acquire)) {
      ucp_worker_progress(worker);
    }
    ucp_request_free(req);
    return pc.status;
  };

  // 8. ucp_ep_flush 闭包（保证 RDMA WRITE 真完成到对端）
  auto do_flush = [&]() {
    ucp_request_param_t fp{};
    void* req = ucp_ep_flush_nbx(ep, &fp);
    if (req == nullptr) return UCS_OK;
    if (UCS_PTR_IS_ERR(req)) return UCS_PTR_STATUS(req);
    while (ucp_request_check_status(req) == UCS_INPROGRESS) {
      ucp_worker_progress(worker);
    }
    auto st = ucp_request_check_status(req);
    ucp_request_free(req);
    return st;
  };

  // 9. warmup
  std::cerr << "[client] warmup " << a.warmup << " iters" << std::endl;
  for (std::size_t i = 0; i < a.warmup; ++i) {
    auto st = do_one_put(buf, a.size, raddr);
    if (st != UCS_OK) {
      std::cerr << "warmup put failed: " << ucs_status_string(st) << std::endl;
      return 1;
    }
  }
  do_flush();

  // 10. 测量：count 次 put + 最后 flush
  std::cerr << "[client] measure " << a.count << " iters, size="
            << a.size << " bytes" << std::endl;
  std::vector<double> per_op_us(a.count, 0.0);
  auto t_begin = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < a.count; ++i) {
    auto t0 = std::chrono::steady_clock::now();
    auto st = do_one_put(buf, a.size, raddr);
    if (st != UCS_OK) {
      std::cerr << "put failed at i=" << i << ": " << ucs_status_string(st) << std::endl;
      return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    per_op_us[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
  }
  do_flush();
  auto t_end = std::chrono::steady_clock::now();

  double wall_s = std::chrono::duration<double>(t_end - t_begin).count();
  double total_bytes = static_cast<double>(a.size) * a.count;
  double mibs = total_bytes / wall_s / (1024.0 * 1024.0);

  // 简单分位计算
  std::sort(per_op_us.begin(), per_op_us.end());
  auto pct = [&](double p) -> double {
    auto idx = static_cast<std::size_t>(p * (per_op_us.size() - 1));
    return per_op_us[idx];
  };

  std::cout << "==== UCX put_nbx PoC ====" << std::endl;
  std::cout << "  size=" << a.size << " count=" << a.count
            << " warmup=" << a.warmup << std::endl;
  std::cout << "  wall=" << wall_s << " s throughput=" << mibs << " MiB/s" << std::endl;
  std::cout << "  latency_us: p50=" << pct(0.5)
            << " p95=" << pct(0.95)
            << " p99=" << pct(0.99) << std::endl;

  // 11. 清理
  ucp_rkey_destroy(rkey);
  ucp_mem_unmap(ctx, memh);
  ::munlock(buf, a.size);
  std::free(buf);
  void* close_req = ucp_ep_close_nbx(ep, nullptr);
  if (UCS_PTR_IS_PTR(close_req)) {
    while (ucp_request_check_status(close_req) == UCS_INPROGRESS) {
      ucp_worker_progress(worker);
    }
    ucp_request_free(close_req);
  }
  ucp_worker_destroy(worker);
  ucp_cleanup(ctx);
  return 0;
}

}  // namespace

#include <algorithm>  // std::sort

int main(int argc, char** argv) {
  PocArgs a;
  if (!ParseArgs(argc, argv, a)) {
    std::cerr << "Usage:\n"
              << "  server: " << argv[0] << " --server [--port=N]\n"
              << "  client: " << argv[0] << " --client <host> [--port=N] "
                 "[--size=N] [--count=N] [--warmup=N]\n";
    return 2;
  }
  return a.is_server ? RunServer(a) : RunClient(a);
}
