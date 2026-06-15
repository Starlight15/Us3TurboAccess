#!/usr/bin/env bash
# 持续性能测试：每 cell 跑 5 round，每 round wall ≈ 30-40s。CSV 累积
# 一行 / round，带 round_idx，用于看 round-to-round 抖动。
#
# 用法： ./scripts/sustained_bench.sh --out FILE [--rounds N]

set -u
set -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
GATEWAY_BIN="${BUILD_DIR}/gateway/us3_turbo_access_gateway"
ROUNDS=5
OUT_CSV="${REPO_ROOT}/bench/logs/sustained_results.csv"

BRPC_PORT=18082
UCX_PORT=18520
GDS_RDMA_PORT=18516
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="0.0.0.0"
BACKEND_CAPACITY=$((128*1024*1024*1024))  # 128 GiB（足够装 5 轮）

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)    OUT_CSV="$2"; shift 2 ;;
    --rounds) ROUNDS="$2";  shift 2 ;;
    *)        echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# 各 cell 配置：path mode count threads object_size part_size warmup
# (part_size 只对 multipart 有效，put 时被忽略)
CELLS=(
  "rdma put       8192   4 4194304   0       16"
  "gds  put       32768  4 4194304   0       32"
  "http put       8192   4 4194304   0       16"
  "rdma multipart 512    4 33554432  8388608 2"
  "gds  multipart 512    4 33554432  8388608 2"
  "http multipart 384    4 33554432  8388608 2"
)

declare -A BIN
BIN[rdma:put]="${BUILD_DIR}/bench/us3_turbo_access_rdma_put_bench"
BIN[gds:put]="${BUILD_DIR}/bench/us3_turbo_access_gds_put_bench_v2"
BIN[http:put]="${BUILD_DIR}/bench/us3_turbo_access_http_put_bench"
BIN[rdma:multipart]="${BUILD_DIR}/bench/us3_turbo_access_rdma_multipart_bench"
BIN[gds:multipart]="${BUILD_DIR}/bench/us3_turbo_access_gds_multipart_bench"
BIN[http:multipart]="${BUILD_DIR}/bench/us3_turbo_access_http_multipart_bench"

GATEWAY_PID=""
log() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*" >&2; }

cleanup() {
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    kill "${GATEWAY_PID}" 2>/dev/null || true
    sleep 2
    kill -0 "${GATEWAY_PID}" 2>/dev/null && kill -KILL "${GATEWAY_PID}" 2>/dev/null
  fi
}
trap cleanup EXIT INT TERM

wait_port() {
  local deadline=$(( $(date +%s) + 20 ))
  while (( $(date +%s) < deadline )); do
    (echo > "/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1 && return 0
    sleep 0.3
  done
  return 1
}

start_gw() {
  local rdma="$1" gds="$2"
  pkill -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" 2>/dev/null || true
  sleep 2
  local flags=( --brpc_port="${BRPC_PORT}" --public_host="${PUBLIC_HOST}"
                --bind_host="${BIND_HOST}" --backend=memory
                --backend_capacity="${BACKEND_CAPACITY}" )
  if [[ "${rdma}" == true ]]; then
    flags+=( --ucx_enable=true --ucx_port="${UCX_PORT}" )
  fi
  flags+=( --gds_enable="${gds}" )
  if [[ "${gds}" == true ]]; then
    flags+=( --gds_rdma_port="${GDS_RDMA_PORT}" )
  fi
  log "starting gateway rdma=${rdma} gds=${gds}"
  "${GATEWAY_BIN}" "${flags[@]}" >${REPO_ROOT}/bench/logs/sustained_gw.log 2>&1 &
  GATEWAY_PID=$!
  if ! wait_port "${BRPC_PORT}"; then
    log "gateway not ready"; tail -n 30 ${REPO_ROOT}/bench/logs/sustained_gw.log >&2; return 1
  fi
  log "gateway pid=${GATEWAY_PID}"
}

stop_gw() {
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    kill "${GATEWAY_PID}" 2>/dev/null || true
    for _ in 1 2 3; do kill -0 "${GATEWAY_PID}" 2>/dev/null || break; sleep 1; done
    kill -0 "${GATEWAY_PID}" 2>/dev/null && kill -KILL "${GATEWAY_PID}" 2>/dev/null
  fi
  GATEWAY_PID=""
  sleep 2
}

extract() {
  python3 - "$@" <<'PY'
import json, sys
obj = json.loads(sys.argv[1])
fields = ["path","mode","threads","object_size","count",
          "throughput_mbps","lat_avg_ms","lat_p50_ms","lat_p95_ms","lat_p99_ms",
          "cpu_user_s","cpu_sys_s","cpu_pct","wall_s","failed"]
print(",".join(str(obj.get(k,"")) for k in fields))
PY
}

echo "round,path,mode,threads,object_size,count,throughput_mbps,lat_avg_ms,lat_p50_ms,lat_p95_ms,lat_p99_ms,cpu_user_s,cpu_sys_s,cpu_pct,wall_s,failed" > "${OUT_CSV}"
log "out CSV: ${OUT_CSV}"

run_cell() {
  local path="$1" mode="$2" count="$3" threads="$4" obj="$5" part="$6" warm="$7"
  local key="${path}:${mode}"
  local bin="${BIN[${key}]}"
  [[ -x "${bin}" ]] || { log "missing bin: ${bin}"; return 2; }

  # key-modulo 限定后端实际存储对象数：put 256 keys × 4 MiB = 1 GiB；
  # multipart 256 keys × 32 MiB = 8 GiB；与 128 GiB capacity 留充足余量。
  local args=( --endpoint="${PUBLIC_HOST}:${BRPC_PORT}"
               --threads="${threads}" --object-size="${obj}"
               --count="${count}" --warmup="${warm}"
               --key-modulo=256 )
  if [[ "${mode}" == multipart ]]; then
    args+=( --part-size="${part}" )
  fi

  for r in $(seq 1 "${ROUNDS}"); do
    log "  round=${r}/${ROUNDS} ${path}:${mode} count=${count}"
    local raw=$("${bin}" "${args[@]}" 2>/dev/null)
    local rc=$?
    if [[ ${rc} -ne 0 ]]; then
      log "  FAIL rc=${rc}"
      continue
    fi
    local json=$(echo "${raw}" | grep '^{' | tail -n1)
    local row=$(extract "${json}")
    echo "${r},${row}" >> "${OUT_CSV}"
  done
}

declare -A NEEDS_RDMA NEEDS_GDS
NEEDS_RDMA[rdma]=true; NEEDS_RDMA[gds]=false; NEEDS_RDMA[http]=false
NEEDS_GDS[rdma]=false; NEEDS_GDS[gds]=true;  NEEDS_GDS[http]=false

LAST_PATH=""
for cell in "${CELLS[@]}"; do
  read -r path mode count threads obj part warm <<<"${cell}"
  if [[ "${path}" != "${LAST_PATH}" ]]; then
    stop_gw
    start_gw "${NEEDS_RDMA[${path}]}" "${NEEDS_GDS[${path}]}" || exit 3
    LAST_PATH="${path}"
  fi
  run_cell "${path}" "${mode}" "${count}" "${threads}" "${obj}" "${part}" "${warm}"
done
stop_gw

log "done: ${OUT_CSV}"
echo "===== rows ====="
cat "${OUT_CSV}"
