#!/usr/bin/env bash
# GDS PUT 一键端到端测试：启 gateway → 跑 gds_put_example → 自动清理。
set -u
set -o pipefail

US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"

GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"
EXAMPLE_BIN="${EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_gds_put_example}"

BRPC_PORT="${BRPC_PORT:-18082}"
RDMA_PORT="${RDMA_PORT:-18535}"
GDS_RDMA_PORT="${GDS_RDMA_PORT:-18536}"
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-${US3_REPO_ROOT}/examples/logs/gateway_${BRPC_PORT}.log}"

BYTES="${BYTES:-1048576}"
BUCKET="${BUCKET:-us3-test}"
KEY="${KEY:-claude/gds-put-test-$(date +%s)}"
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
  sleep 5
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
[[ -x "${EXAMPLE_BIN}" ]] || { log "gds_put_example not found: ${EXAMPLE_BIN}"; exit 2; }

kill_existing
: > "${GATEWAY_LOG}"
log "starting gateway (log: ${GATEWAY_LOG})"
"${GATEWAY_BIN}" \
  --brpc_port="${BRPC_PORT}" \
  --rdma_port="${RDMA_PORT}" \
  --gds_rdma_port="${GDS_RDMA_PORT}" \
  --public_host="${PUBLIC_HOST}" \
  --bind_host="${BIND_HOST}" \
  --backend=memory \
  --backend_capacity="${BACKEND_CAPACITY}" \
  >"${GATEWAY_LOG}" 2>&1 &
GATEWAY_PID=$!
log "gateway pid=${GATEWAY_PID}"

wait_for_port "${BRPC_PORT}" || { log "gateway not ready"; tail -n 30 "${GATEWAY_LOG}"; exit 3; }
log "gateway ready on :${BRPC_PORT}"

log "running gds_put_example: bytes=${BYTES} bucket=${BUCKET} key=${KEY}"
"${EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${BYTES}" "${BUCKET}" "${KEY}"
rc=$?
log "exit code: ${rc}"
exit "${rc}"
