#!/usr/bin/env bash
# 重启 gateway 并跑一次 GDS example 闭环（新 client + 新 gateway）。
# 顺序：kill 现有 gateway → 前台拉起 gateway → 等 RPC 端口可用 → 跑 example → 结束后 kill gateway。
# 脚本退出时（无论成功失败）gateway 都会被停掉。

set -u
set -o pipefail

GATEWAY_BIN="${GATEWAY_BIN:-/mnt/n0test/xinghui.shao/gds/Us3TurboAccess/build-local/gateway/us3_turbo_access_gateway}"
EXAMPLE_BIN="${EXAMPLE_BIN:-${US3_REPO_ROOT:-../..}/build/examples/us3_turbo_access_gds_example}"

BRPC_PORT="${BRPC_PORT:-${HTTP_PORT:-18082}}"
RDMA_PORT="${RDMA_PORT:-18535}"
GDS_RDMA_PORT="${GDS_RDMA_PORT:-18536}"
PUBLIC_HOST="${PUBLIC_HOST:-192.168.1.198}"
BIND_HOST="${BIND_HOST:-0.0.0.0}"
GATEWAY_LOG="${GATEWAY_LOG:-${US3_REPO_ROOT:-../..}/examples/logs/gateway_${BRPC_PORT}.log}"

EXAMPLE_BYTES="${EXAMPLE_BYTES:-1048576}"
EXAMPLE_BUCKET="${EXAMPLE_BUCKET:-us3-test}"
EXAMPLE_KEY="${EXAMPLE_KEY:-claude/restart-test-$(date +%s)}"

READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-15}"
KILL_GRACE_SEC="${KILL_GRACE_SEC:-3}"

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
    if kill -0 "${GATEWAY_PID}" 2>/dev/null; then
      log "gateway did not exit on SIGTERM, sending SIGKILL"
      kill -KILL "${GATEWAY_PID}" 2>/dev/null || true
    fi
  fi
  exit "${rc}"
}
trap cleanup EXIT INT TERM

kill_existing_gateway() {
  local pids
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  if [[ -z "${pids}" ]]; then
    log "no existing gateway on port ${BRPC_PORT}"
    return
  fi
  log "killing existing gateway pids: ${pids}"
  # shellcheck disable=SC2086
  kill ${pids} 2>/dev/null || true
  for _ in $(seq 1 "${KILL_GRACE_SEC}"); do
    pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
    [[ -z "${pids}" ]] && break
    sleep 1
  done
  pids=$(pgrep -f "us3_turbo_access_gateway --brpc_port=${BRPC_PORT}" || true)
  if [[ -n "${pids}" ]]; then
    log "force-killing pids: ${pids}"
    # shellcheck disable=SC2086
    kill -KILL ${pids} 2>/dev/null || true
  fi
}

wait_for_port() {
  local port="$1"
  local deadline=$(( $(date +%s) + READY_TIMEOUT_SEC ))
  while (( $(date +%s) < deadline )); do
    if (echo > "/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.3
  done
  return 1
}

main() {
  [[ -x "${GATEWAY_BIN}" ]] || { log "gateway binary not found: ${GATEWAY_BIN}"; exit 2; }
  [[ -x "${EXAMPLE_BIN}" ]] || { log "example binary not found: ${EXAMPLE_BIN}"; exit 2; }

  kill_existing_gateway

  : > "${GATEWAY_LOG}"
  log "starting gateway (log: ${GATEWAY_LOG})"
  "${GATEWAY_BIN}" \
    --brpc_port="${BRPC_PORT}" \
    --rdma_port="${RDMA_PORT}" \
    --gds_rdma_port="${GDS_RDMA_PORT}" \
    --public_host="${PUBLIC_HOST}" \
    --bind_host="${BIND_HOST}" \
    --backend=memory \
    >"${GATEWAY_LOG}" 2>&1 &
  GATEWAY_PID=$!
  log "gateway pid=${GATEWAY_PID}"

  if ! wait_for_port "${BRPC_PORT}"; then
    log "gateway port ${BRPC_PORT} did not become ready within ${READY_TIMEOUT_SEC}s"
    tail -n 30 "${GATEWAY_LOG}" || true
    exit 3
  fi
  log "gateway ready on :${BRPC_PORT}"

  log "running example: bytes=${EXAMPLE_BYTES} bucket=${EXAMPLE_BUCKET} key=${EXAMPLE_KEY}"
  "${EXAMPLE_BIN}" "${PUBLIC_HOST}:${BRPC_PORT}" "${EXAMPLE_BYTES}" "${EXAMPLE_BUCKET}" "${EXAMPLE_KEY}"
  local rc=$?
  log "example exit code: ${rc}"
  exit "${rc}"
}

main "$@"
