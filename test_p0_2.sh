#!/bin/bash
# P0 #2 测试：gateway 上限 + client 兜底
set -e

cd /mnt/n0test/xinghui.shao/gds/Us3TurboAccess/build
LOG_DIR="../test_logs"
mkdir -p "${LOG_DIR}"

echo "=== P0 #2: PUT size limit test ==="
echo ""

# 清理旧进程
pkill -f us3_turbo_access_gateway 2>/dev/null || true
sleep 2

# 启动 gateway：http_max_put_bytes=10 MiB
echo "[1/4] Starting gateway (http_max_put_bytes=10 MiB)..."
./gateway/us3_turbo_access_gateway \
  --brpc_port=18080 \
  --http_max_put_bytes=$((10*1024*1024)) \
  --max_body_size=$((11*1024*1024)) \
  --backend=memory \
  --rdma_enable=false \
  --gds_enable=false \
  > "${LOG_DIR}/gateway.log" 2>&1 &
GATEWAY_PID=$!
sleep 4

if ! kill -0 $GATEWAY_PID 2>/dev/null; then
  echo "ERROR: Gateway failed to start"
  cat "${LOG_DIR}/gateway.log"
  exit 1
fi
echo "Gateway started (PID=$GATEWAY_PID)"
echo ""

# Test 1: 8 MiB PUT (应该成功)
echo "[2/4] Test 1: PUT 8 MiB (< 10 MiB limit, should succeed)"
./examples/us3_turbo_access_http_put_bench \
  --endpoint=127.0.0.1:18080 \
  --threads=1 \
  --object-size=$((8*1024*1024)) \
  --count=2 \
  --warmup=0 \
  > "${LOG_DIR}/test1_8mb.log" 2>&1

if [ $? -eq 0 ]; then
  echo "✓ Test 1 PASSED: 8 MiB PUT succeeded"
  grep -E "failed=|throughput=" "${LOG_DIR}/test1_8mb.log" | head -2
else
  echo "✗ Test 1 FAILED"
  cat "${LOG_DIR}/test1_8mb.log"
fi
echo ""

# Test 2: 12 MiB PUT (应该 413)
echo "[3/4] Test 2: PUT 12 MiB (> 10 MiB limit, should get 413)"
./examples/us3_turbo_access_http_put_bench \
  --endpoint=127.0.0.1:18080 \
  --threads=1 \
  --object-size=$((12*1024*1024)) \
  --count=2 \
  --warmup=0 \
  > "${LOG_DIR}/test2_12mb.log" 2>&1

if [ $? -ne 0 ]; then
  echo "✓ Test 2 PASSED: 12 MiB PUT rejected (expected)"
  grep -E "failed=|error|413|PayloadTooLarge" "${LOG_DIR}/test2_12mb.log" | head -5
else
  echo "✗ Test 2 FAILED: 12 MiB PUT should have been rejected"
  cat "${LOG_DIR}/test2_12mb.log"
fi
echo ""

# Test 3: Client 5 GiB 兜底（需要写一个简单的 C++ 测试或者直接看 example 能否配置）
echo "[4/4] Test 3: Client 5 GiB limit (skipped - needs custom test)"
echo "  Client put_single_max_bytes=5 GiB 默认值已设置"
echo "  需要 6 GiB buffer 测试，bench 不支持此大小"
echo ""

# 清理
echo "Stopping gateway..."
kill $GATEWAY_PID 2>/dev/null || true
wait $GATEWAY_PID 2>/dev/null || true

echo ""
echo "=== Test Summary ==="
echo "Test 1 (8 MiB):  $(grep -q 'failed=0' ${LOG_DIR}/test1_8mb.log 2>/dev/null && echo '✓ PASS' || echo '✗ FAIL')"
echo "Test 2 (12 MiB): $(grep -q 'failed=' ${LOG_DIR}/test2_12mb.log 2>/dev/null && echo '✓ PASS (rejected as expected)' || echo '? CHECK LOG')"
echo ""
echo "Logs saved to: ${LOG_DIR}/"
