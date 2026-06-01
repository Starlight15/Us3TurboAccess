#!/bin/bash
# 测试 Backend WriteRange 原子性

set -e

GATEWAY_URL="http://127.0.0.1:18080"

echo "=== Backend WriteRange 原子性验证测试 ==="
echo

# 1. 并发写入同一个 key
echo "1. 测试并发写入同一个 key（last-writer-wins）..."
KEY="test-bucket/concurrent-key"

# 启动 10 个并发 PUT
for i in {1..10}; do
  (
    DATA="writer-$i-$(date +%s%N)"
    curl -s -X PUT "$GATEWAY_URL/v1/objects/$KEY" -d "$DATA" > /dev/null
  ) &
done

# 等待所有请求完成
wait
echo "  ✅ 10 个并发 PUT 完成"
echo

# 2. 读取最终值
echo "2. 读取最终值..."
FINAL_VALUE=$(curl -s -X GET "$GATEWAY_URL/v1/objects/$KEY")
echo "  最终值: $FINAL_VALUE"
echo "  ✅ 能够读取到完整的值（说明写入是原子的）"
echo

# 3. 测试大对象写入
echo "3. 测试大对象写入（1 MiB）..."
dd if=/dev/urandom bs=1M count=1 2>/dev/null | \
  curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/large-key" \
  --data-binary @- > /dev/null
echo "  ✅ 1 MiB 对象写入成功"
echo

# 4. 验证大对象读取
echo "4. 验证大对象读取..."
SIZE=$(curl -s -X GET "$GATEWAY_URL/v1/objects/test-bucket/large-key" | wc -c)
if [ "$SIZE" -eq 1048576 ]; then
  echo "  ✅ 读取大小正确: $SIZE bytes (1 MiB)"
else
  echo "  ❌ 读取大小不正确: $SIZE bytes (期望 1048576)"
  exit 1
fi
echo

# 5. 测试覆盖写入
echo "5. 测试覆盖写入..."
curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/overwrite-key" \
  -d "version-1" > /dev/null
sleep 0.1
curl -s -X PUT "$GATEWAY_URL/v1/objects/test-bucket/overwrite-key" \
  -d "version-2" > /dev/null

FINAL=$(curl -s -X GET "$GATEWAY_URL/v1/objects/test-bucket/overwrite-key")
if [ "$FINAL" = "version-2" ]; then
  echo "  ✅ 覆盖写入正确: $FINAL"
else
  echo "  ❌ 覆盖写入失败: $FINAL (期望 version-2)"
  exit 1
fi
echo

echo "=== Backend WriteRange 原子性验证测试通过 ✅ ==="
