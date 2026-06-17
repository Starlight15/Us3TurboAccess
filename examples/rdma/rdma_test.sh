#!/usr/bin/env bash
# Native RDMA 一键端到端测试。
# 启 gateway 开 --rdma_enable=true → 跑 rdma_put_example → 校验 HEAD size。
set -u
set -o pipefail

# 默认从仓库根 build/ 取产物（do_make.sh 的输出位置）；可被环境变量覆盖。
US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"

GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"
EXAMPLE_BIN="${EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_rdma_put_example}"
ASYNC_EXAMPLE_BIN="${ASYNC_EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_rdma_put_async_example}"
ASYNC_CONCURRENCY="${ASYNC_CONCURRENCY:-4}"
MULTIPART_EXAMPLE_BIN="${MULTIPART_EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_rdma_multipart_example}"
MULTIPART_TOTAL_BYTES="${MULTIPART_TOTAL_BYTES:-$((32*1024*1024))}"
MULTIPART_PART_SIZE="${MULTIPART_PART_SIZE:-$((8*1024*1024))}"  # >= server min_part_size=5MB
MULTIPART_CONCURRENCY="${MULTIPART_CONCURRENCY:-2}"

BRPC_PORT="${BRPC_PORT:-18082}"
RDMA_PORT="${RDMA_PORT:-18515}"
GDS_RDMA_PORT="${GDS_RDMA_PORT:-18516}"
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-${US3_REPO_ROOT}/examples/logs/gateway_${BRPC_PORT}.log}"

BYTES="${BYTES:-4194304}"
BUCKET="${BUCKET:-us3-test}"
KEY="${KEY:-claude/rdma-test-$(date +%s)}"
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
  # 给 RDMA / cuObjServer 端口释放一点时间
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

[[ -x "${GATEWAY_BIN}" ]] || { log "gateway not found"; exit 2; }
[[ -x "${EXAMPLE_BIN}" ]] || { log "rdma_put_example not found"; exit 2; }
[[ -x "${ASYNC_EXAMPLE_BIN}" ]] || { log "rdma_put_async_example not found"; exit 2; }
[[ -x "${MULTIPART_EXAMPLE_BIN}" ]] || { log "rdma_multipart_example not found"; exit 2; }

kill_existing
: > "${GATEWAY_LOG}"
log "starting gateway (rdma enabled, log: ${GATEWAY_LOG})"
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

log "running rdma_put_example: bytes=${BYTES} bucket=${BUCKET} key=${KEY}"
"${EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${BYTES}" "${BUCKET}" "${KEY}"
rc=$?
log "rdma_put_example exit code: ${rc}"
[[ ${rc} -ne 0 ]] && exit "${rc}"

ASYNC_KEY_PREFIX="${KEY}.async"
log "running rdma_put_async_example: bytes=${BYTES} concurrency=${ASYNC_CONCURRENCY} bucket=${BUCKET} key_prefix=${ASYNC_KEY_PREFIX}"
"${ASYNC_EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${BYTES}" "${ASYNC_CONCURRENCY}" "${BUCKET}" "${ASYNC_KEY_PREFIX}"
rc=$?
log "rdma_put_async_example exit code: ${rc}"
[[ ${rc} -ne 0 ]] && exit "${rc}"

MULTIPART_KEY="${KEY}.multipart"
log "running rdma_multipart_example: total=${MULTIPART_TOTAL_BYTES} part=${MULTIPART_PART_SIZE} concurrency=${MULTIPART_CONCURRENCY} bucket=${BUCKET} key=${MULTIPART_KEY}"
"${MULTIPART_EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${MULTIPART_TOTAL_BYTES}" "${MULTIPART_PART_SIZE}" "${BUCKET}" "${MULTIPART_KEY}" "${MULTIPART_CONCURRENCY}"
rc=$?
log "rdma_multipart_example exit code: ${rc}"
exit "${rc}"
