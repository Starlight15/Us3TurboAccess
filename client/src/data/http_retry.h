#pragma once

// 向后兼容头：原 http_retry.h 已迁移到 client/src/core/routing/retry_policy.h，
// 三通路共享。本头保留为 thin shim，避免 HTTP 通路改 include。
#include "client/src/core/routing/retry_policy.h"
