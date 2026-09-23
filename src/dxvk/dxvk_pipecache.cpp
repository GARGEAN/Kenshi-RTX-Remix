#include "dxvk_pipecache.h"
#include "../util/util_kenshi_telemetry.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk {

  // DX11_V298_PERSISTENT_PIPELINE_CACHE: the serialized driver blob lives with
  // the other per-game caches (raw DXBC + pipeline-state map). Together the
  // three files let a later launch recreate every previously seen pipeline
  // from cached binaries before the game's first frame.
  static std::filesystem::path pipelineCacheFilePath() {
    return std::filesystem::path(env::getExePath()).parent_path()
      / "rtx-remix" / "cache" / (env::getExeBaseName() + ".vk-pipeline-cache");
  }

  DxvkPipelineCache::DxvkPipelineCache(
    const Rc<vk::DeviceFn>& vkd)
  : m_vkd(vkd) {
    // Seed the cache with the previous session's serialized data when
    // present. The driver validates the blob header (vendor/device/cache
    // UUID) itself and simply ignores data from another driver version,
    // so a stale file degrades to an empty cache instead of an error.
    std::vector<char> initialData;
    {
      std::error_code fileError;
      const std::filesystem::path path = pipelineCacheFilePath();
      const uintmax_t fileSize = std::filesystem::file_size(path, fileError);
      if (!fileError && fileSize > 32u && fileSize <= (512ull << 20)) {
        initialData.resize(static_cast<size_t>(fileSize));
        std::ifstream input(path, std::ios::in | std::ios::binary);
        input.read(initialData.data(),
          static_cast<std::streamsize>(initialData.size()));
        if (!input || static_cast<size_t>(input.gcount()) != initialData.size())
          initialData.clear();
      }
    }

    VkPipelineCacheCreateInfo info;
    info.sType            = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.pNext            = nullptr;
    info.flags            = 0;
    info.initialDataSize  = initialData.size();
    info.pInitialData     = initialData.empty() ? nullptr : initialData.data();

    VkResult result = m_vkd->vkCreatePipelineCache(m_vkd->device(),
      &info, nullptr, &m_handle);

    if (result != VK_SUCCESS && !initialData.empty()) {
      // A corrupt blob may fail creation outright on some drivers;
      // fall back to an empty cache rather than failing device creation.
      info.initialDataSize = 0;
      info.pInitialData    = nullptr;
      initialData.clear();
      result = m_vkd->vkCreatePipelineCache(m_vkd->device(),
        &info, nullptr, &m_handle);
    }

    if (result != VK_SUCCESS)
      throw DxvkError("DxvkPipelineCache: Failed to create cache");

    KENSHI_DIAGNOSTIC_INFO(str::format("[pipeline-cache-v664] loadedBytes=", initialData.size(),
      " path=", pipelineCacheFilePath().string()));
    // Size is only a hint for comparing contents, never proof of cache identity.
    m_lastSavedSize = initialData.size();
  }


  DxvkPipelineCache::~DxvkPipelineCache() {
    save();
    m_vkd->vkDestroyPipelineCache(
      m_vkd->device(), m_handle, nullptr);
  }


  void DxvkPipelineCache::save() {
    std::lock_guard<dxvk::mutex> lock(m_saveMutex);

    size_t dataSize = 0;
    if (m_vkd->vkGetPipelineCacheData(m_vkd->device(),
          m_handle, &dataSize, nullptr) != VK_SUCCESS
     || dataSize == 0)
      return;

    std::vector<char> data(dataSize);
    VkResult result = VK_INCOMPLETE;
    for (uint32_t attempt = 0; attempt < 3 && result == VK_INCOMPLETE; ++attempt) {
      if (attempt != 0) {
        if (m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &dataSize, nullptr) != VK_SUCCESS)
          return;
        data.resize(dataSize);
      }
      result = m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &dataSize, data.data());
    }
    if (result != VK_SUCCESS) return;
    data.resize(dataSize);

    const std::filesystem::path path = pipelineCacheFilePath();
    // Driver caches can change without growing. Compare the actual bytes before
    // skipping a same-size save, including after loading an earlier session.
    if (dataSize == m_lastSavedSize) {
      std::ifstream existing(path, std::ios::binary);
      std::vector<char> chunk(64u << 10);
      size_t offset = 0;
      while (existing && offset < data.size()) {
        const size_t bytes = std::min(chunk.size(), data.size() - offset);
        existing.read(chunk.data(), static_cast<std::streamsize>(bytes));
        if (static_cast<size_t>(existing.gcount()) != bytes ||
            std::memcmp(chunk.data(), data.data() + offset, bytes) != 0)
          break;
        offset += bytes;
      }
      if (offset == data.size() && existing.peek() == std::char_traits<char>::eof())
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    // Atomic publish: write a temporary sibling, then rename over the target
    // so a crash mid-write can never leave a truncated cache behind.
    std::filesystem::path temporary = path;
    temporary += str::format(".tmp.", ::GetCurrentProcessId());
    {
      std::ofstream output(temporary,
        std::ios::out | std::ios::binary | std::ios::trunc);
      output.write(data.data(), static_cast<std::streamsize>(data.size()));
      output.flush();
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, error);
        static bool s_saveFailureLogged = false;
        if (!s_saveFailureLogged) {
          s_saveFailureLogged = true;
          Logger::err(str::format(
            "DxvkPipelineCache: failed to write ", temporary.string()));
        }
        return;
      }
    }

    // Replace the old cache atomically; a failed rename must not delete it.
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      Logger::warn(str::format("[pipeline-cache-v664] replace failed error=", ::GetLastError()));
      std::error_code cleanupError;
      std::filesystem::remove(temporary, cleanupError);
      return;
    }

    m_lastSavedSize = dataSize;
    KENSHI_DIAGNOSTIC_INFO(str::format("[pipeline-cache-v664] savedBytes=", dataSize));
  }

}
