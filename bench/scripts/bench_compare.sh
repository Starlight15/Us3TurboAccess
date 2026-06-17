#!/usr/bin/env bash
# 跨通路 (rdma|gds|http) × 模式 (put|multipart) × N 轮 的性能对比 runner。
# 收集 6 个 bench × N 轮 = 6N 行 CSV，写到 --out FILE（默认 ./bench_results.csv）。
#
# 用法：
#   ./scripts/bench_compare.sh [选项]
#
# 主要选项（所有 bench 共用）：
#   --rounds N               每个 bench 跑 N 轮（默认 2）
#   --threads N              ClientExecutor worker 数（默认 4）
#   --object-size N          单对象字节（put 用；默认 4 MiB）
#   --put-count N            put bench 测量笔数（默认 64）
#   --put-warmup N           put bench warmup 笔数（默认 8）
#   --mp-object-size N       multipart 单对象总字节（默认 32 MiB）
#   --mp-part-size N         multipart part 字节（默认 8 MiB；>= 5 MiB）
#   --mp-count N             multipart 测量轮数（默认 4）
#   --mp-warmup N            multipart warmup 轮数（默认 1）
#   --out FILE               CSV 输出（默认 ./bench_results.csv）
#   --cpus CPULIST           bench 进程绑核（taskset -c CPULIST）
#   --paths LIST             逗号分隔，默认 "rdma,gds,http"
#   --modes LIST             逗号分隔，默认 "put,multipart"
set -u
set -o pipefail

US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"
GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"

# 默认参数
ROUNDS=2
THREADS=4
OBJECT_SIZE=$((4*1024*1024))
PUT_COUNT=64
PUT_WARMUP=8
MP_OBJECT_SIZE=$((32*1024*1024))
MP_PART_SIZE=$((8*1024*1024))
MP_COUNT=4
MP_WARMUP=1
OUT_CSV="${US3_REPO_ROOT}/bench/logs/bench_results.csv"
CPUS=""
PATHS="rdma,gds,http"
MODES="put,multipart"

# Gateway
BRPC_PORT="${BRPC_PORT:-18082}"
UCX_PORT="${UCX_PORT:-18520}"
GDS_RDMA_PORT="${GDS_RDMA_PORT:-18516}"
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
BACKEND_CAPACITY="${BACKEND_CAPACITY:-$((8*1024*1024*1024))}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rounds)         ROUNDS="$2"; shift 2 ;;
    --threads)        THREADS="$2"; shift 2 ;;
    --object-size)    OBJECT_SIZE="$2"; shift 2 ;;
    --put-count)      PUT_COUNT="$2"; shift 2 ;;
    --put-warmup)     PUT_WARMUP="$2"; shift 2 ;;
    --mp-object-size) MP_OBJECT_SIZE="$2"; shift 2 ;;
    --mp-part-size)   MP_PART_SIZE="$2"; shift 2 ;;
    --mp-count)       MP_COUNT="$2"; shift 2 ;;
    --mp-warmup)      MP_WARMUP="$2"; shift 2 ;;
    --out)            OUT_CSV="$2"; shift 2 ;;
    --cpus)           CPUS="$2"; shift 2 ;;
    --paths)          PATHS="$2"; shift 2 ;;
    --modes)          MODES="$2"; shift 2 ;;
    -h|--help)        sed -n '1,25p' "$0"; exit 0 ;;
    *)                echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# 二进制路径表
declare -A BENCH_BIN
BENCH_BIN[rdma:put]="${US3_BUILD_DIR}/bench/us3_turbo_access_rdma_put_bench"
BENCH_BIN[rdma:multipart]="${US3_BUILD_DIR}/bench/us3_turbo_access_rdma_multipart_bench"
BENCH_BIN[gds:put]="${US3_BUILD_DIR}/bench/us3_turbo_access_gds_put_bench"
BENCH_BIN[gds:multipart]="${US3_BUILD_DIR}/bench/us3_turbo_access_gds_multipart_bench"
BENCH_BIN[http:put]="${US3_BUILD_DIR}/bench/us3_turbo_access_http_put_bench"
BENCH_BIN[http:multipart]="${US3_BUILD_DIR}/bench/us3_turbo_access_http_multipart_bench"

GATEWAY_PID=""
READY_TIMEOUT_SEC=20
KILL_GRACE_SEC=3

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }

cleanup() {
  local rc=$?
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    log "stopping gateway pid=${GATEWAY_PID}"
    kill "${GATEWAY_PID}" 2>/dev/null || true
    for _ in $(seq 1 "${KILL_GRACE_SEC}"); do
      kill -0 "${GATEWAY_PID}" 2>/dev/null || break
      sleep 1
    done
    kill -0 "${GATEWAY_PID}" 2>/dev/null && kill -KILL "${GATEWAY_PID}" 2>/dev/null
  fi
  exit "${rc}"
}
trap cleanup EXIT INT TERM

kill_existing() {
  local pids
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  [[ -z "${pids}" ]] && return
  log "killing existing gateway pids: ${pids}"
  kill ${pids} 2>/dev/null || true
  sleep 2
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  [[ -n "${pids}" ]] && kill -KILL ${pids} 2>/dev/null
  sleep 3
}

wait_for_port() {
  local deadline=$(( $(date +%s) + READY_TIMEOUT_SEC ))
  while (( $(date +%s) < deadline )); do
    (echo > "/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1 && return 0
    sleep 0.3
  done
  return 1
}

start_gateway() {
  local rdma_enable="$1"
  local gds_enable="$2"
  local extra_flags=()
  if [[ "${rdma_enable}" == "true" ]]; then
    extra_flags+=( --ucx_enable=true --ucx_port="${UCX_PORT}" )
  fi
  extra_flags+=( --gds_enable="${gds_enable}" )
  if [[ "${gds_enable}" == "true" ]]; then
    extra_flags+=( --gds_rdma_port="${GDS_RDMA_PORT}" )
  fi

  local log_path="${US3_REPO_ROOT}/bench/logs/gateway_bench_${BRPC_PORT}.log"
  : > "${log_path}"
  log "starting gateway rdma=${rdma_enable} gds=${gds_enable} log=${log_path}"
  "${GATEWAY_BIN}" \
    --brpc_port="${BRPC_PORT}" \
    --public_host="${PUBLIC_HOST}" \
    --bind_host="${BIND_HOST}" \
    --backend=memory \
    --backend_capacity="${BACKEND_CAPACITY}" \
    "${extra_flags[@]}" \
    >"${log_path}" 2>&1 &
  GATEWAY_PID=$!
  if ! wait_for_port "${BRPC_PORT}"; then
    log "gateway not ready"
    tail -n 30 "${log_path}" >&2
    return 1
  fi
  log "gateway ready pid=${GATEWAY_PID}"
}

stop_gateway() {
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    kill "${GATEWAY_PID}" 2>/dev/null || true
    for _ in $(seq 1 "${KILL_GRACE_SEC}"); do
      kill -0 "${GATEWAY_PID}" 2>/dev/null || break
      sleep 1
    done
    kill -0 "${GATEWAY_PID}" 2>/dev/null && kill -KILL "${GATEWAY_PID}" 2>/dev/null
  fi
  GATEWAY_PID=""
  sleep 2
}

# 把 JSON 一行转 CSV 一行（取 path,mode,threads,object_size,count,throughput_mbps,
# lat_p50_ms,lat_p95_ms,lat_p99_ms,cpu_user_s,cpu_sys_s,cpu_pct,wall_s）。
# 不依赖 jq：用 python3。
json_to_csv_fields() {
  local round="$1"
  local json="$2"
  python3 - "${round}" "${json}" <<'PY'
import json, sys
round_v = sys.argv[1]
obj = json.loads(sys.argv[2])
fields = ["path", "mode", "threads", "object_size", "count",
          "throughput_mbps", "lat_p50_ms", "lat_p95_ms", "lat_p99_ms",
          "cpu_user_s", "cpu_sys_s", "cpu_pct", "wall_s"]
row = [round_v] + [str(obj.get(k, "")) for k in fields]
print(",".join(row))
PY
}

run_one() {
  local path="$1"; local mode="$2"; local round="$3"
  local bin="${BENCH_BIN[${path}:${mode}]}"
  [[ -x "${bin}" ]] || { log "bench not found: ${bin}"; return 2; }
  local args=( --endpoint="${PUBLIC_HOST}:${BRPC_PORT}"
               --threads="${THREADS}" )
  if [[ "${mode}" == "put" ]]; then
    args+=( --object-size="${OBJECT_SIZE}"
            --count="${PUT_COUNT}" --warmup="${PUT_WARMUP}" )
  else
    args+=( --object-size="${MP_OBJECT_SIZE}"
            --part-size="${MP_PART_SIZE}"
            --count="${MP_COUNT}" --warmup="${MP_WARMUP}" )
  fi
  log "run ${path}:${mode} round=${round}"
  local raw_out
  if [[ -n "${CPUS}" ]]; then
    raw_out=$(taskset -c "${CPUS}" "${bin}" "${args[@]}" 2>/dev/null)
  else
    raw_out=$("${bin}" "${args[@]}" 2>/dev/null)
  fi
  local rc=$?
  if [[ ${rc} -ne 0 ]]; then
    log "FAIL ${path}:${mode} round=${round} rc=${rc}"
    return ${rc}
  fi
  # bench 每次在 stdout 最后一行输出 JSON；UCX wakeup 噪音写 stderr，
  # 但某些环境下也可能混入 stdout。只取最后一行。
  local json_line
  json_line=$(echo "${raw_out}" | grep '^{' | tail -n1)
  echo "${json_line}"
  json_to_csv_fields "${round}" "${json_line}" >> "${OUT_CSV}"
}

# CSV header
mkdir -p "$(dirname "${OUT_CSV}")"
echo "round,path,mode,threads,object_size,count,throughput_mbps,lat_p50_ms,lat_p95_ms,lat_p99_ms,cpu_user_s,cpu_sys_s,cpu_pct,wall_s" > "${OUT_CSV}"
log "out CSV: ${OUT_CSV}"

IFS=',' read -ra PATH_ARR <<< "${PATHS}"
IFS=',' read -ra MODE_ARR <<< "${MODES}"

[[ -x "${GATEWAY_BIN}" ]] || { log "gateway not found: ${GATEWAY_BIN}"; exit 2; }
kill_existing

# 三个通路对 gateway 的需求不同：RDMA 必须 --rdma_enable=true；GDS 必须
# --gds_enable=true（其它通路也可一并打开，因为 server 上各 endpoint 互不干扰）。
# 简单起见：每条 path 单独起一遍 gateway，跑完该 path 全部模式 × 轮再 stop。
for path in "${PATH_ARR[@]}"; do
  stop_gateway || true
  case "${path}" in
    rdma) start_gateway true  false ;;
    gds)  start_gateway false true  ;;
    http) start_gateway false false ;;
    *)    log "unknown path: ${path}"; continue ;;
  esac

  for mode in "${MODE_ARR[@]}"; do
    for r in $(seq 1 "${ROUNDS}"); do
      run_one "${path}" "${mode}" "${r}"
    done
  done
done

stop_gateway

log "done. CSV: ${OUT_CSV}"
echo "===== CSV head ====="
head -n 1 "${OUT_CSV}"
echo "===== CSV body ====="
tail -n +2 "${OUT_CSV}"
