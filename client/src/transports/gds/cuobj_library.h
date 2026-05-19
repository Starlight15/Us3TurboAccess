#pragma once

#include <cstddef>

#include <cufile.h>
#include <cuobjclient.h>

#include "us3_turbo_access/client/result.h"

namespace us3_turbo_access::client {

/**
 * @brief 通过 dlopen 解析得到的 libcuobjclient 函数指针表。
 */
struct CuObjApi {
  using Constructor = void (*)(void*, CUObjOps_t&, cuObjProto_t);
  using Destructor = void (*)(void*);
  using IsConnected = bool (*)(void*);
  using GetDescriptor = cuObjErr_t (*)(void*, void*, std::size_t);
  using GetMaxRequestCallbackSize = ssize_t (*)(void*, void*);
  using PutDescriptor = cuObjErr_t (*)(void*, void*);
  using GetCtx = void* (*)(const void*);
  using GetObject = ssize_t (*)(void*, void*, void*, std::size_t, loff_t, loff_t);
  using PutObject = ssize_t (*)(void*, void*, void*, std::size_t, loff_t, loff_t);

  Constructor constructor{nullptr};
  Destructor destructor{nullptr};
  IsConnected is_connected{nullptr};
  GetDescriptor get_descriptor{nullptr};
  GetMaxRequestCallbackSize get_max_request_callback_size{nullptr};
  PutDescriptor put_descriptor{nullptr};
  GetCtx get_ctx{nullptr};
  GetObject cuobj_get{nullptr};
  PutObject cuobj_put{nullptr};
};

/**
 * @brief libcuobjclient 的动态加载封装。
 *
 * 进程内只有一个全局实例，通过 Get() 懒初始化。Get() 失败时返回错误，
 * 不会抛出异常；调用方不应缓存指针，需要每次调用 Get() 以共享同一份 api。
 */
class CuObjLibrary {
 public:
  CuObjLibrary(const CuObjLibrary&) = delete;
  CuObjLibrary& operator=(const CuObjLibrary&) = delete;
  CuObjLibrary(CuObjLibrary&& other) noexcept;
  CuObjLibrary& operator=(CuObjLibrary&& other) noexcept;
  ~CuObjLibrary();

  [[nodiscard]] const CuObjApi& api() const { return api_; }

  [[nodiscard]] static Result<const CuObjLibrary*> Get();

  // Internal: only CuObjLibrary::Get() should construct an instance via Load().
  CuObjLibrary(void* handle, CuObjApi api);

 private:
  CuObjLibrary() = default;

  void Reset();

  void* handle_{nullptr};
  CuObjApi api_{};
};

}  // namespace us3_turbo_access::client
