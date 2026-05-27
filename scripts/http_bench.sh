#!/usr/bin/env bash
# HTTP 整对象 PUT 性能 bench 一键脚本。
#
# 用法：
#   ./scripts/http_bench.sh [--threads N] [--cpus CPULIST] [--object-size N]
#                           [--count N] [--warmup N]
#   --cpus CPULIST  通过 taskset -c CPULIST 限制 bench 进程能用的 CPU 核（例如 "0-3"）。
#                   gateway 不受影响（独立进程）。
#
# 行为：
#   1) kill 已有 gateway → 起新 gateway（gds_enable=false，只跑 HTTP）。
#   2) taskset 跑 http_put_bench → stderr 给人看；stdout 一行 JSON 给脚本串。
#   3) 退出时清理 gateway。
set -u
set -o pipefail

US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"

GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"
BENCH_BIN="${BENCH_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_http_put_bench}"

# 默认参数（可通过命令行覆盖）
THREADS=4
CPUS=""
OBJECT_SIZE=$((4*1024*1024))   # 4 MiB
COUNT=128
WARMUP=8
BUCKET="us3-bench"
KEY_PREFIX="bench/$(date +%s)/"

# Gateway 参数
BRPC_PORT="${BRPC_PORT:-18082}"
PUBLIC_HOST="${PUBLIC_HOST:-127.0.0.1}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-/tmp/us3_turbo_access_gateway_bench_${BRPC_PORT}.log}"
BACKEND_CAPACITY="${BACKEND_CAPACITY:-$((8*1024*1024*1024))}"  # 8 GiB

READY_TIMEOUT_SEC=15
KILL_GRACE_SEC=3
GATEWAY_PID=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --threads)     THREADS="$2"; shift 2 ;;
    --cpus)        CPUS="$2"; shift 2 ;;
    --object-size) OBJECT_SIZE="$2"; shift 2 ;;
    --count)       COUNT="$2"; shift 2 ;;
    --warmup)      WARMUP="$2"; shift 2 ;;
    --bucket)      BUCKET="$2"; shift 2 ;;
    --key-prefix)  KEY_PREFIX="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,15p' "$0"
      exit 0 ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2 ;;
  esac
done

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
  for _ in $(seq 1 "${KILL_GRACE_SEC}"); do
    pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
    [[ -z "${pids}" ]] && break
    sleep 1
  done
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  [[ -n "${pids}" ]] && kill -KILL ${pids} 2>/dev/null
  sleep 1
}

wait_for_port() {
  local deadline=$(( $(date +%s) + READY_TIMEOUT_SEC ))
  while (( $(date +%s) < deadline )); do
    (echo > "/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1 && return 0
    sleep 0.3
  done
  return 1
}

[[ -x "${GATEWAY_BIN}" ]] || { log "gateway not found: ${GATEWAY_BIN}"; exit 2; }
[[ -x "${BENCH_BIN}" ]]   || { log "bench not found: ${BENCH_BIN}"; exit 2; }
if [[ -n "${CPUS}" ]] && ! command -v taskset >/dev/null 2>&1; then
  log "taskset not found; --cpus 选项不可用"
  exit 2
fi

kill_existing
: > "${GATEWAY_LOG}"
log "starting gateway (gds_enable=false, log: ${GATEWAY_LOG})"
"${GATEWAY_BIN}" \
  --brpc_port="${BRPC_PORT}" \
  --public_host="${PUBLIC_HOST}" \
  --bind_host="${BIND_HOST}" \
  --backend=memory \
  --backend_capacity="${BACKEND_CAPACITY}" \
  --gds_enable=false \
  >"${GATEWAY_LOG}" 2>&1 &
GATEWAY_PID=$!
log "gateway pid=${GATEWAY_PID}"

wait_for_port "${BRPC_PORT}" || { log "gateway not ready"; tail -n 30 "${GATEWAY_LOG}"; exit 3; }
log "gateway ready on :${BRPC_PORT}"

log "running bench: threads=${THREADS} cpus=${CPUS:-(all)} object_size=${OBJECT_SIZE} count=${COUNT} warmup=${WARMUP}"

BENCH_CMD=( "${BENCH_BIN}"
  --endpoint="${PUBLIC_HOST}:${BRPC_PORT}"
  --threads="${THREADS}"
  --object-size="${OBJECT_SIZE}"
  --count="${COUNT}"
  --warmup="${WARMUP}"
  --bucket="${BUCKET}"
  --key-prefix="${KEY_PREFIX}"
)

# bench 进程被 taskset 限制 CPU；gateway 是独立进程，不受影响。
# stdout（JSON 一行）通过 tee 同时给终端和最终 RC 处理；stderr（人读）直通。
if [[ -n "${CPUS}" ]]; then
  taskset -c "${CPUS}" "${BENCH_CMD[@]}"
else
  "${BENCH_CMD[@]}"
fi
rc=$?
log "bench exit code: ${rc}"
exit "${rc}"
