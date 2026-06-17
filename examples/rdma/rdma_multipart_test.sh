#!/usr/bin/env bash
# RDMA Multipart 一键端到端测试：启 gateway (rdma_enable=true) → 跑 rdma_multipart_example → 自动清理。
set -u
set -o pipefail

US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"

GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"
EXAMPLE_BIN="${EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_rdma_multipart_example}"

BRPC_PORT="${BRPC_PORT:-18082}"
RDMA_PORT="${RDMA_PORT:-18515}"
GDS_RDMA_PORT="${GDS_RDMA_PORT:-18516}"
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-${US3_REPO_ROOT}/examples/logs/gateway_${BRPC_PORT}.log}"

TOTAL_BYTES="${TOTAL_BYTES:-$((32*1024*1024))}"
PART_SIZE="${PART_SIZE:-$((8*1024*1024))}"      # >= server min_part_size=5MB
CONCURRENCY="${CONCURRENCY:-2}"
BUCKET="${BUCKET:-us3-test}"
KEY="${KEY:-claude/rdma-mpu-test-$(date +%s)}"
BACKEND_CAPACITY="${BACKEND_CAPACITY:-$((1*1024*1024*1024))}"

READY_TIMEOUT_SEC=15
KILL_GRACE_SEC=3
GATEWAY_PID=""

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

cleanup() {
  local rc=$?
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    log "stopping gateway pid=${GATEWAY_PID}"
    kill "${GATEWAY_PID}" 2>/dev/null || true
    for _ in $(seq 1 "${KILL_GRACE_SEC}"); do
      kill -0 "${GATEWAY_PID}" 2>/dev/null || break; sleep 1
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
    [[ -z "${pids}" ]] && break; sleep 1
  done
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  [[ -n "${pids}" ]] && kill -KILL ${pids} 2>/dev/null
  sleep 5  # 等待 RDMA 端口释放
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
[[ -x "${EXAMPLE_BIN}" ]] || { log "rdma_multipart_example not found: ${EXAMPLE_BIN}"; exit 2; }

kill_existing
: > "${GATEWAY_LOG}"
log "starting gateway rdma_enable=true (log: ${GATEWAY_LOG})"
"${GATEWAY_BIN}" \
  --brpc_port="${BRPC_PORT}" \
  --rdma_port="${RDMA_PORT}" \
  --gds_rdma_port="${GDS_RDMA_PORT}" \
  --public_host="${PUBLIC_HOST}" \
  --bind_host="${BIND_HOST}" \
  --backend=memory \
  --backend_capacity="${BACKEND_CAPACITY}" \
  --rdma_enable=true \
  >"${GATEWAY_LOG}" 2>&1 &
GATEWAY_PID=$!
log "gateway pid=${GATEWAY_PID}"

wait_for_port "${BRPC_PORT}" || { log "gateway not ready"; tail -n 30 "${GATEWAY_LOG}"; exit 3; }
log "gateway ready on :${BRPC_PORT}"

log "running rdma_multipart_example: total=${TOTAL_BYTES} part=${PART_SIZE} concurrency=${CONCURRENCY} bucket=${BUCKET} key=${KEY}"
"${EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${TOTAL_BYTES}" "${PART_SIZE}" "${BUCKET}" "${KEY}" "${CONCURRENCY}"
rc=$?; log "exit code: ${rc}"
exit "${rc}"
