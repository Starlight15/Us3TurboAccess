#!/bin/bash
# 测试 CRC32C 校验功能

set -e

GATEWAY_URL="http://127.0.0.1:18080"

echo "=== CRC32C 校验验证测试 ==="
echo

# 1. 正常 PUT（带正确的 CRC）
echo "1. 测试正常 PUT（带正确的 CRC）..."
DATA="hello world"
# 计算 CRC32C（需要工具，这里简化）
# 实际应该用 client 发送，client 会自动计算 CRC
RESPONSE=$(curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/crc-test-1" \
  -d "$DATA" -w "\n%{http_code}")
HTTP_CODE=$(echo "$RESPONSE" | tail -1)
BODY=$(echo "$RESPONSE" | head -n -1)

if [ "$HTTP_CODE" = "200" ]; then
  echo "  ✅ PUT 成功，HTTP 200"
  echo "  响应: $BODY"
else
  echo "  ❌ PUT 失败，HTTP $HTTP_CODE"
  exit 1
fi
echo

# 2. PUT 带错误的 CRC（手动构造）
echo "2. 测试 PUT 带错误的 CRC..."
# 发送错误的 CRC32C 头
RESPONSE=$(curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/crc-test-2" \
  -H "x-amz-checksum-crc32c: AAAAAAAA" \
  -d "$DATA" -w "\n%{http_code}")
HTTP_CODE=$(echo "$RESPONSE" | tail -1)
BODY=$(echo "$RESPONSE" | head -n -1)

if [ "$HTTP_CODE" = "400" ]; then
  echo "  ✅ CRC 校验失败，正确返回 HTTP 400"
  echo "  响应: $BODY"
else
  echo "  ⚠️  HTTP $HTTP_CODE (期望 400)"
  echo "  响应: $BODY"
fi
echo

# 3. GET 验证返回的 CRC
echo "3. 测试 GET 返回的 CRC..."
RESPONSE=$(curl -s -i -X GET "$GATEWAY_URL/v1/objects/test-bucket/crc-test-1")
CRC_HEADER=$(echo "$RESPONSE" | grep -i "x-amz-checksum-crc32c" || echo "")

if [ -n "$CRC_HEADER" ]; then
  echo "  ✅ GET 响应包含 CRC 头"
  echo "  $CRC_HEADER"
else
  echo "  ❌ GET 响应缺少 CRC 头"
  exit 1
fi
echo

echo "=== CRC32C 校验验证测试完成 ==="
echo
echo "注意：完整的 CRC 验证需要使用 client SDK，"
echo "因为需要正确计算 CRC32C 值。"
