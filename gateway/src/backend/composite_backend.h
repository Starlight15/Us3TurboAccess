#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "backend/backend.h"
#include "backend/data_store.h"
#include "backend/index_store.h"

namespace us3_turbo_access::gateway::backend {

/**
 * 把 IBackend 拆成 IIndexStore + IDataStore 的适配层。
 * 上层（GdsExecutor / MultipartCoordinator）继续看到统一的 IBackend，
 * 元数据与字节存储可以独立选型与替换。
 *
 * upload_id ↔ (bucket, key) 的映射在本类里短暂保存，
 * 用于在 CompleteMultipart 时把字节拼到正确的对象。
 */
class CompositeBackend final : public IBackend {
 public:
  CompositeBackend(std::unique_ptr<IIndexStore> index_store,
                   std::unique_ptr<IDataStore> data_store);

  [[nodiscard]] std::string_view kind() const noexcept override {
    return "composite";
  }

  [[nodiscard]] Result<ObjectMetadata>
    Head(std::string_view bucket, std::string_view key) override;

  [[nodiscard]] Result<std::size_t>
    Read(std::string_view bucket, std::string_view key,
         std::uint64_t offset, std::span<std::byte> dst) override;

  [[nodiscard]] Result<ObjectMetadata>
    Write(std::string_view bucket, std::string_view key,
          std::span<const std::byte> src) override;

  [[nodiscard]] Result<ObjectMetadata>
    WriteRange(std::string_view bucket, std::string_view key,
               std::uint64_t offset, std::span<const std::byte> src,
               std::optional<std::size_t> total_size) override;

  [[nodiscard]] Result<ObjectMetadata>
    Reserve(std::string_view bucket, std::string_view key,
            std::size_t total_size) override;

  [[nodiscard]] Result<std::string>
    StartMultipart(std::string_view bucket, std::string_view key,
                   std::optional<std::size_t> total_size_hint) override;

  [[nodiscard]] Result<std::string>
    WritePart(std::string_view upload_id, std::uint32_t part_number,
              std::uint64_t offset, std::span<const std::byte> src) override;

  [[nodiscard]] Result<ObjectMetadata>
    CompleteMultipart(std::string_view upload_id,
                      const std::vector<PartRecord>& parts) override;

  [[nodiscard]] Result<bool>
    AbortMultipart(std::string_view upload_id) override;

 private:
  static ObjectMetadata MakeMeta(std::size_t size);
  static std::string    MakePartEtag(std::uint32_t part_number, std::size_t size);

  std::unique_ptr<IIndexStore> index_;
  std::unique_ptr<IDataStore>  data_;

  // upload_id → (bucket, key) 映射，CompleteMultipart 时查回真正落盘目标。
  std::mutex                                                            mpu_mu_;
  std::unordered_map<std::string, std::pair<std::string, std::string>>  mpu_targets_;
};

}  // namespace us3_turbo_access::gateway::backend
