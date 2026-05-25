#include <cstdlib>
#include <string>

#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include "us3_turbo_access/gateway/options.h"
#include "us3_turbo_access/gateway/server.h"

DEFINE_int32(brpc_port, 8080, "Gateway brpc + HTTP port");
DEFINE_int32(rdma_port, 18515, "RDMA listener port (M2)");
DEFINE_int32(gds_rdma_port, 0, "GDS RDMA listener port; 0 = rdma_port + 1");
DEFINE_int32(idle_timeout_sec, -1, "brpc idle timeout in seconds");
DEFINE_int32(num_threads, 4, "brpc worker thread count");
DEFINE_int32(io_worker_threads, 32,
             "Background IO pool size handling chunk-level RDMA / backend IO");
DEFINE_string(bind_host, "0.0.0.0", "Bind host for the brpc listener");
DEFINE_string(public_host, "127.0.0.1", "Public host published to clients");
DEFINE_string(gateway_id, "us3-turbo-access-gateway", "Gateway identifier");
DEFINE_string(backend, "memory", "Backend kind: memory | null | composite");
DEFINE_uint64(backend_capacity, 4ULL * 1024ULL * 1024ULL * 1024ULL,
              "MemoryBackend capacity in bytes");
DEFINE_uint64(default_chunk_size, 8U * 1024U * 1024U,
              "Default chunk plan element size");
DEFINE_int32(session_ttl_sec, 600, "Session TTL in seconds");
DEFINE_int32(session_sweep_interval_sec, 30,
             "Background sweep interval for expired sessions");
DEFINE_uint64(multipart_max_part_size, 1ULL * 1024ULL * 1024ULL * 1024ULL,
              "Maximum multipart part size in bytes (advertised to clients)");
DEFINE_uint64(multipart_min_part_size, 5ULL * 1024ULL * 1024ULL,
              "Minimum part size in bytes (S3-style; 0 disables)");
DEFINE_bool(rdma_enable, false, "Enable native RDMA listener (M2)");
DEFINE_bool(gds_enable, true, "Enable GDS / cuObjServer data path");

namespace {

us3_turbo_access::gateway::BackendKind ParseBackend(const std::string& name) {
  if (name == "null") {
    return us3_turbo_access::gateway::BackendKind::kNull;
  }
  if (name == "composite") {
    return us3_turbo_access::gateway::BackendKind::kComposite;
  }
  return us3_turbo_access::gateway::BackendKind::kMemory;
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  us3_turbo_access::gateway::GatewayOptions options;
  options.bind_host = FLAGS_bind_host;
  options.public_host = FLAGS_public_host;
  options.brpc_port = FLAGS_brpc_port;
  options.rdma_port = FLAGS_rdma_port;
  options.gds_rdma_port = FLAGS_gds_rdma_port;
  options.idle_timeout_sec = FLAGS_idle_timeout_sec;
  options.num_threads = FLAGS_num_threads;
  options.io_worker_threads = FLAGS_io_worker_threads;
  options.gateway_id = FLAGS_gateway_id;
  options.backend_kind = ParseBackend(FLAGS_backend);
  options.backend_capacity = static_cast<std::size_t>(FLAGS_backend_capacity);
  options.default_chunk_size = static_cast<std::size_t>(FLAGS_default_chunk_size);
  options.session_ttl = std::chrono::seconds(FLAGS_session_ttl_sec);
  options.session_sweep_interval =
      std::chrono::seconds(FLAGS_session_sweep_interval_sec);
  options.multipart_max_part_size =
      static_cast<std::size_t>(FLAGS_multipart_max_part_size);
  options.multipart_min_part_size =
      static_cast<std::size_t>(FLAGS_multipart_min_part_size);
  options.rdma_enable = FLAGS_rdma_enable;
  options.gds_enable = FLAGS_gds_enable;

  us3_turbo_access::gateway::GatewayServer server(std::move(options));
  auto started = server.Start();
  if (!started.success()) {
    spdlog::error("gateway startup failed: {}", started.error().message);
    return EXIT_FAILURE;
  }
  server.RunUntilAskedToQuit();
  server.Shutdown();
  return EXIT_SUCCESS;
}
