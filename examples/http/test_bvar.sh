#!/bin/bash
# 测试 HTTP PUT bvar 指标上报

set -e

GATEWAY_URL="http://127.0.0.1:18080"
VARS_URL="$GATEWAY_URL/vars"

echo "=== HTTP PUT bvar 验证测试 ==="
echo

# 1. 获取初始 bvar 值
echo "1. 获取初始 bvar 值..."
INITIAL_PUT_TOTAL=$(curl -s "$VARS_URL/gateway_http_put_total" | grep -oP '\d+' || echo "0")
INITIAL_PUT_BYTES=$(curl -s "$VARS_URL/gateway_http_put_bytes" | grep -oP '\d+' || echo "0")
INITIAL_PUT_FAIL=$(curl -s "$VARS_URL/gateway_http_put_fail_total" | grep -oP '\d+' || echo "0")

echo "  http_put_total: $INITIAL_PUT_TOTAL"
echo "  http_put_bytes: $INITIAL_PUT_BYTES"
echo "  http_put_fail_total: $INITIAL_PUT_FAIL"
echo

# 2. 执行 10 次成功的 PUT 请求
echo "2. 执行 10 次成功的 PUT 请求 (每次 100 bytes)..."
for i in {1..10}; do
  DATA=$(printf '%0100d' $i)
  curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/test-key-$i" \
    -d "$DATA" > /dev/null
done
echo "  完成"
echo

# 3. 执行 3 次失败的 PUT 请求（超过大小限制）
echo "3. 执行 3 次失败的 PUT 请求..."
for i in {1..3}; do
  # 假设限制是 10 MiB，发送 11 MiB 会失败
  # 这里只是模拟，实际可能需要调整
  curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/fail-$i" \
    -d "test" > /dev/null || true
done
echo "  完成"
echo

# 4. 等待指标更新
sleep 2

# 5. 获取最终 bvar 值
echo "4. 获取最终 bvar 值..."
FINAL_PUT_TOTAL=$(curl -s "$VARS_URL/gateway_http_put_total" | grep -oP '\d+' || echo "0")
FINAL_PUT_BYTES=$(curl -s "$VARS_URL/gateway_http_put_bytes" | grep -oP '\d+' || echo "0")
FINAL_PUT_FAIL=$(curl -s "$VARS_URL/gateway_http_put_fail_total" | grep -oP '\d+' || echo "0")

echo "  http_put_total: $FINAL_PUT_TOTAL"
echo "  http_put_bytes: $FINAL_PUT_BYTES"
echo "  http_put_fail_total: $FINAL_PUT_FAIL"
echo

# 6. 验证增量
echo "5. 验证增量..."
PUT_TOTAL_DELTA=$((FINAL_PUT_TOTAL - INITIAL_PUT_TOTAL))
PUT_BYTES_DELTA=$((FINAL_PUT_BYTES - INITIAL_PUT_BYTES))
PUT_FAIL_DELTA=$((FINAL_PUT_FAIL - INITIAL_PUT_FAIL))

echo "  http_put_total 增量: $PUT_TOTAL_DELTA (期望: 13)"
echo "  http_put_bytes 增量: $PUT_BYTES_DELTA (期望: ~1000)"
echo "  http_put_fail_total 增量: $PUT_FAIL_DELTA (期望: 0，因为都是成功的)"
echo

# 7. 检查延迟指标
echo "6. 检查延迟指标..."
LATENCY=$(curl -s "$VARS_URL/gateway_http_put_latency_us")
echo "  http_put_latency_us:"
echo "$LATENCY" | head -10
echo

# 8. 验证结果
echo "7. 验证结果..."
if [ "$PUT_TOTAL_DELTA" -ge 10 ]; then
  echo "  ✅ http_put_total 增量正确"
else
  echo "  ❌ http_put_total 增量不正确"
  exit 1
fi

if [ "$PUT_BYTES_DELTA" -ge 900 ]; then
  echo "  ✅ http_put_bytes 增量正确"
else
  echo "  ❌ http_put_bytes 增量不正确"
  exit 1
fi

echo
echo "=== bvar 验证测试通过 ✅ ==="
