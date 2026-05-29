#!/bin/bash
set -e

cd /mnt/n0test/xinghui.shao/gds/Us3TurboAccess

# 清理旧进程
pkill -f us3_turbo_access_gateway 2>/dev/null || true
sleep 1

# 启动 gateway：http_max_put_bytes=1024, brpc max_body_size=2048
./gateway/us3_turbo_access_gateway \
  --brpc_port=18080 \
  --http_max_put_bytes=1024 \
  --max_body_size=2048 \
  --backend=null \
  > /dev/null 2>&1 &

GATEWAY_PID=$!
echo "Gateway started (PID=$GATEWAY_PID), waiting 3s..."
sleep 3

# 测试 1: 1023 字节 PUT → 应该 200
echo "Test 1: PUT 1023 bytes (should succeed)"
dd if=/dev/zero bs=1023 count=1 2>/dev/null | \
  curl -s -w "\nHTTP_CODE=%{http_code}\n" \
    -X PUT \
    -H "Content-Type: application/octet-stream" \
    --data-binary @- \
    http://127.0.0.1:18080/v1/objects/test-bucket/test-key-1023

echo ""

# 测试 2: 1025 字节 PUT → 应该 413
echo "Test 2: PUT 1025 bytes (should get 413)"
dd if=/dev/zero bs=1025 count=1 2>/dev/null | \
  curl -s -w "\nHTTP_CODE=%{http_code}\n" \
    -X PUT \
    -H "Content-Type: application/octet-stream" \
    --data-binary @- \
    http://127.0.0.1:18080/v1/objects/test-bucket/test-key-1025

echo ""
echo "Stopping gateway..."
kill $GATEWAY_PID 2>/dev/null || true
wait $GATEWAY_PID 2>/dev/null || true
echo "Done."
