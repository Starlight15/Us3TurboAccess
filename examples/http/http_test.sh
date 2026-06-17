#!/usr/bin/env bash
# 标准对象存储 HTTP 通路一键端到端测试。
# 启 gateway（默认 brpc_port 同时跑 baidu_std + HTTP）→ 跑 http_put_example。
set -u
set -o pipefail

# 默认从仓库根 build/ 取产物（do_make.sh 的输出位置）；可被环境变量覆盖。
US3_REPO_ROOT="${US3_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
US3_BUILD_DIR="${US3_BUILD_DIR:-${US3_REPO_ROOT}/build}"

GATEWAY_BIN="${GATEWAY_BIN:-${US3_BUILD_DIR}/gateway/us3_turbo_access_gateway}"
EXAMPLE_BIN="${EXAMPLE_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_http_put_example}"
MULTIPART_BIN="${MULTIPART_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_http_multipart_example}"
PARALLEL_BIN="${PARALLEL_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_http_parallel_get_example}"
VERIFY_BIN="${VERIFY_BIN:-${US3_BUILD_DIR}/examples/us3_turbo_access_http_verify_example}"

BRPC_PORT="${BRPC_PORT:-18082}"
PUBLIC_HOST="${PUBLIC_HOST:-127.0.0.1}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-${US3_REPO_ROOT}/examples/logs/gateway_http_${BRPC_PORT}.log}"

BYTES="${BYTES:-1048576}"
BUCKET="${BUCKET:-us3-test}"
KEY="${KEY:-claude/http-test-$(date +%s)}"
BACKEND_CAPACITY="${BACKEND_CAPACITY:-$((1*1024*1024*1024))}"

# V2 测试参数
MULTIPART_TOTAL="${MULTIPART_TOTAL:-$((32*1024*1024))}"
MULTIPART_PART="${MULTIPART_PART:-$((8*1024*1024))}"   # >= server min_part_size=5MB
MULTIPART_CONCURRENCY="${MULTIPART_CONCURRENCY:-2}"
PARALLEL_BYTES="${PARALLEL_BYTES:-$((100*1024*1024))}"
PARALLEL_CHUNKS="${PARALLEL_CHUNKS:-8}"

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

[[ -x "${GATEWAY_BIN}" ]] || { log "gateway not found"; exit 2; }
[[ -x "${EXAMPLE_BIN}" ]] || { log "http_put_example not found"; exit 2; }
[[ -x "${MULTIPART_BIN}" ]] || { log "http_multipart_example not found"; exit 2; }
[[ -x "${PARALLEL_BIN}" ]] || { log "http_parallel_get_example not found"; exit 2; }
[[ -x "${VERIFY_BIN}" ]] || { log "http_verify_example not found"; exit 2; }

kill_existing
: > "${GATEWAY_LOG}"
log "starting gateway (log: ${GATEWAY_LOG})"
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

log "running http_put_example: bytes=${BYTES} bucket=${BUCKET} key=${KEY}"
"${EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${BYTES}" "${BUCKET}" "${KEY}"
rc=$?
log "http_put_example exit code: ${rc}"
[[ ${rc} -ne 0 ]] && exit "${rc}"

MP_KEY="${KEY}.multipart"
log "running http_multipart_example: total=${MULTIPART_TOTAL} part=${MULTIPART_PART} conc=${MULTIPART_CONCURRENCY} key=${MP_KEY}"
"${MULTIPART_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${MULTIPART_TOTAL}" "${MULTIPART_PART}" "${BUCKET}" "${MP_KEY}" "${MULTIPART_CONCURRENCY}"
rc=$?
log "http_multipart_example exit code: ${rc}"
[[ ${rc} -ne 0 ]] && exit "${rc}"

PG_KEY="${KEY}.parallel"
log "running http_parallel_get_example: bytes=${PARALLEL_BYTES} chunks=${PARALLEL_CHUNKS} key=${PG_KEY}"
"${PARALLEL_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${PARALLEL_BYTES}" "${BUCKET}" "${PG_KEY}" "${PARALLEL_CHUNKS}"
rc=$?
log "http_parallel_get_example exit code: ${rc}"
[[ ${rc} -ne 0 ]] && exit "${rc}"

log "running http_verify_example: bucket=${BUCKET}"
"${VERIFY_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${BUCKET}"
rc=$?
log "http_verify_example exit code: ${rc}"
exit "${rc}"
