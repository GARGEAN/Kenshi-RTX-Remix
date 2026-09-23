#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_opacity_micromap_manager.h"
#include "rtx_omm_cache.h"
#include "rtx_omm_retention.h"
#include "rtx_omm_maintenance.h"
#include "../../util/util_env.h"
#include <chrono>
#include "../../util/util_kenshi_fault.h"

#include "rtx.h"
#include "rtx_context.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_options.h"
#include "rtx_hash_collision_detection.h"
#include "rtx_texture_manager.h"

#include "rtx_imgui.h"

#include "../util/util_global_time.h"

#include "rtx/pass/common_binding_indices.h"

// #define VALIDATION_MODE

#ifdef VALIDATION_MODE
#define omm_validation_assert(x) assert(x)
#else
#define omm_validation_assert(x)
#endif

const VkDeviceSize kBufferAlignment = 16;
const VkDeviceSize kBufferInBlasUsageAlignment = 256;

namespace dxvk {
  struct OmmReuseData {
    ommcache::Layout layout;
    ommcache::Key key {};
    std::shared_ptr<ommcache::Record> record;
    std::vector<uint16_t> texelCosts;
    Rc<DxvkBuffer> texcoords;
    bool enabled = false;
    bool cacheKeyReady = false;
    bool cacheRequested = false;
    bool cacheLoaded = false;
    bool saveRequested = false;
  };

  struct OmmReuseStats {
    uint64_t layouts = 0, sourceTriangles = 0, uniqueTriangles = 0;
    uint64_t uploads = 0, coldBakes = 0, unsupported = 0;
    uint64_t textureHashes = 0, textureHashUnavailable = 0, readyBindings = 0;
    uint64_t gpuShares = 0, sharedWaits = 0, ioWaits = 0, memoryWaits = 0;
    uint64_t uploadBytes = 0, completedBuilds = 0, fallbackBakes = 0;
    uint64_t textureWaits = 0, fingerprintWaits = 0, uploadWaits = 0, targetedBuckets = 0, coldMicroTriangles = 0;
  };
  static OmmReuseStats s_ommReuseStats;

  struct OmmRetentionMetadata {
    uint64_t detachedSinceMs = 0;
    uint64_t generation = 0;
    bool diskBacked = false;
  };
  struct OmmRetentionState {
    std::map<uint64_t, OmmRetentionMetadata> entries;
    std::map<std::pair<uint64_t,uint64_t>,uint64_t> abandoned;
    void forget(uint64_t hash) {
      auto it = entries.find(hash);
      if (it != entries.end()) {
        abandoned.erase({it->second.detachedSinceMs, hash});
        entries.erase(it);
      }
    }
    ommretention::Sweep sweep;
    uint64_t generation = 0;
    bool trimming = false;
  };
  // Non-owning metadata, bounded by live cache entries. It must not keep any
  // GPU resource alive. Separate manager keys also handle manager recreation.
  static std::unordered_map<const OpacityMicromapManager*, OmmRetentionState> s_ommRetention;
  static uint64_t ommRetentionTimeMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  // A repeatedly rejected job needs its peak reservation once, not the sum of
  // every probe/retry. A job larger than the current budget cannot be admitted
  // by evicting other maps. Leave it pending for a possible future budget rise.
  static VkDeviceSize ommPendingAllocation(VkDeviceSize previous, VkDeviceSize requested, VkDeviceSize budget) {
    return std::max(previous, requested <= budget ? requested : VkDeviceSize(0));
  }

  static VkDeviceSize ommReclaimTarget(VkDeviceSize requested, VkDeviceSize used, VkDeviceSize budget) {
    requested = requested <= budget ? requested : 0;
    // Include oversubscription after a budget contraction. Pending releases
    // are compared against this target exactly once by the eviction loop.
    return used > budget ? used - budget + requested : requested - std::min(requested, budget - used);
  }


  static ommcache::Store& portableOmmStore() {
    static ommcache::Store store(std::filesystem::u8path(env::getExePath()).parent_path() / "rtx-remix/cache/omm");
    return store;
  }

  static bool supportsCompactOpacity(const RtInstance& instance) {
    if (!instance.getBlas() || instance.getMaterialType() != MaterialDataType::Opaque || instance.isAnimated())
      return false;
    const auto& surface = instance.surface;
    if (surface.alphaSource != D3D11ColorSource::Texture || surface.modulateVertexAlpha ||
        !surface.alphaState.isBlendingDisabled || surface.alphaState.isParticle || surface.alphaState.isDecal)
      return false;
    const auto& geometry = instance.getBlas()->input.getGeometryData();
    if (geometry.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || geometry.indexRangeUnvalidated)
      return false;
    if (geometry.opacityMicromapLayout) return true;
    const GeometryBufferData data(geometry);
    return data.texcoordData && (!geometry.usesIndices() || data.indexData);
  }

  void OpacityMicromapManager::prepareReuseData(const RtInstance& instance, OpacityMicromapCacheItem& item) {
    if (item.reuseData) return;
    item.reuseData = std::make_shared<OmmReuseData>();
    auto& reuse = *item.reuseData;
    ++s_ommReuseStats.unsupported;
    if (!supportsCompactOpacity(instance) || usesSplitBillboardOpacityMicromap(instance)) return;
    const auto start = kenshi_telemetry::enabled() ? std::chrono::steady_clock::now()
      : std::chrono::steady_clock::time_point {};
    const auto& geometry = instance.getBlas()->input.getGeometryData();
    const uint32_t count = geometry.calculatePrimitiveCount();
    if (!count || count > (1u << 20) || count != item.numTriangles ||
        count != instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount()) return;
    if (geometry.opacityMicromapLayout) {
      if (geometry.opacityMicromapLayout->indices.size() != count || geometry.opacityMicromapLayout->triangles.empty()) return;
      reuse.layout = *geometry.opacityMicromapLayout;
    } else {
      const GeometryBufferData data(geometry);
      const uint64_t uvOffset = geometry.texcoordBuffer.offsetFromSlice();
      if (uvOffset > geometry.texcoordBuffer.length()) return;
      const bool indexed = geometry.usesIndices();
      const auto type = geometry.indexBuffer.indexType();
      const uint32_t width = !indexed ? 0 : type == VK_INDEX_TYPE_UINT16 ? 2 : type == VK_INDEX_TYPE_UINT32 ? 4 : 0;
      if (indexed && !width) return;
      if (!ommcache::readLayout(data.texcoordData, geometry.texcoordBuffer.length() - uvOffset, geometry.texcoordBuffer.stride(),
          data.indexData, geometry.indexBuffer.length(), geometry.indexBuffer.stride(), width, geometry.vertexCount, count, reuse.layout)) return;
    }
    reuse.enabled = true;
    --s_ommReuseStats.unsupported;
    ++s_ommReuseStats.layouts;
    s_ommReuseStats.sourceTriangles += count;
    s_ommReuseStats.uniqueTriangles += reuse.layout.triangles.size();
    if (kenshi_telemetry::enabled() && s_ommReuseStats.layouts <= 32) {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
      KENSHI_DIAGNOSTIC_INFO(str::format("[OMM V749] compact sourceTriangles=", count, " uniqueTriangles=", reuse.layout.triangles.size(), " prepareUs=", us));
    }
  }

  void OpacityMicromapManager::saveBakedArray(Rc<DxvkContext> ctx, OpacityMicromapCacheItem& item, VkDeviceSize bytes) {
    auto& reuse = *item.reuseData;
    if (reuse.saveRequested || reuse.cacheLoaded || !reuse.cacheRequested) return;
    reuse.saveRequested = true;
    if (!bytes || bytes > ommcache::kMaxBytes || m_ommReadbackBytes + m_textureFingerprintBytes + bytes > ommcache::kMaxBytes) return;
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
    info.size = bytes;
    auto buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM portable readback");
    if (buffer == nullptr) return;
    ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ctx->copyBuffer(buffer, 0, item.ommArrayBuffer, 0, bytes);
    ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(buffer);
    m_ommReadbacks.push_back({ buffer, reuse.key, bytes, m_device->getCurrentFrameId() });
    m_ommReadbackBytes += bytes;
  }

  void OpacityMicromapManager::collectBakedArrays() {
    const uint32_t frame = m_device->getCurrentFrameId();
    for (size_t i = 0; i < m_ommReadbacks.size();) {
      auto& readback = m_ommReadbacks[i];
      if (readback.frame != frame && !readback.buffer->isInUse()) {
        const auto* data = static_cast<const uint8_t*>(readback.buffer->mapPtr(0));
        if (data) portableOmmStore().publish(readback.key, std::vector<uint8_t>(data, data + readback.bytes));
        m_ommReadbackBytes -= readback.bytes;
        if (i + 1 != m_ommReadbacks.size()) readback = std::move(m_ommReadbacks.back());
        m_ommReadbacks.pop_back();
      } else { ++i; }
    }
    if ((s_ommReuseStats.layouts || !m_ommCache.empty()) && frame % 300 == 0) {
      auto& store = portableOmmStore();
      KENSHI_DIAGNOSTIC_INFO(str::format("[OMM V749] reuse sourceTriangles=", s_ommReuseStats.sourceTriangles,
        " uniqueTriangles=", s_ommReuseStats.uniqueTriangles, " cacheUploads=", s_ommReuseStats.uploads,
        " coldBakes=", s_ommReuseStats.coldBakes, " diskHits=", store.diskHits.load(),
        " diskMisses=", store.diskMisses.load(), " saved=", store.writes.load(),
        " saveErrors=", store.writeFailures.load(), " unsupported=", s_ommReuseStats.unsupported,
        " textureHashes=", s_ommReuseStats.textureHashes, " hashPending=", m_textureFingerprintCount,
        " hashUnavailable=", s_ommReuseStats.textureHashUnavailable, " readyBindings=", s_ommReuseStats.readyBindings));
      KENSHI_DIAGNOSTIC_INFO(str::format("[OMM V770] speed gpuShares=", s_ommReuseStats.gpuShares,
        " sharedWaits=", s_ommReuseStats.sharedWaits, " ioWaits=", s_ommReuseStats.ioWaits,
        " memoryWaits=", s_ommReuseStats.memoryWaits, " uploadMiB=", s_ommReuseStats.uploadBytes / (1024 * 1024),
        " builds=", s_ommReuseStats.completedBuilds, " fallbackBakes=", s_ommReuseStats.fallbackBakes,
        " textureWaits=", s_ommReuseStats.textureWaits, " fingerprintWaits=", s_ommReuseStats.fingerprintWaits,
        " uploadWaits=", s_ommReuseStats.uploadWaits, " targetedBuckets=", s_ommReuseStats.targetedBuckets,
        " coldMicroTriangles=", s_ommReuseStats.coldMicroTriangles,
        " queued=", m_unprocessedList.size(), " baked=", m_bakedList.size(),
        " cached=", m_ommCache.size(), " usedMiB=", m_memoryManager.getUsed() / (1024 * 1024),
        " budgetMiB=", m_memoryManager.getBudget() / (1024 * 1024)));
    }
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::getTextureFingerprint(
      Rc<DxvkContext> ctx, const TextureRef& texture, XXH64_hash_t& hash) {
    hash = texture.getImageHash();
    if (hash != kEmptyHash) return OmmResult::Success;
    const auto* view = texture.getImageView();
    if (!view) return OmmResult::DependenciesUnavailable;
    const auto cookie = view->cookie();
    auto found = m_textureFingerprints.find(cookie);
    if (found != m_textureFingerprints.end()) {
      hash = found->second.hash;
      return hash ? OmmResult::Success : found->second.unavailable ? OmmResult::Failure : OmmResult::DependenciesUnavailable;
    }
    // Bound one-time transfers, even when hundreds of new batches share a view.
    if (m_textureFingerprintCount >= 4) return OmmResult::DependenciesUnavailable;
    if (m_textureFingerprints.size() >= 512) {
      for (auto it = m_textureFingerprints.begin(); it != m_textureFingerprints.end(); ++it) {
        if (it->second.readback == nullptr) { m_textureFingerprints.erase(it); break; }
      }
    }
    const auto& image = view->image();
    const auto& info = image->info();
    const auto& subview = view->info();
    TextureFingerprint record;
    if (!(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || info.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
        subview.type != VK_IMAGE_VIEW_TYPE_2D || subview.numLayers != 1 || subview.aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
      record.unavailable = true;
    } else {
      const auto extent = util::computeMipLevelExtent(info.extent, subview.minLevel);
      const auto* format = image->formatInfo();
      const auto blocks = util::computeBlockCount(extent, format->blockSize);
      record.bytes = VkDeviceSize(blocks.width) * blocks.height * blocks.depth * format->elementSize;
      if (!record.bytes || record.bytes > ommcache::kMaxBytes) record.unavailable = true;
      else {
        if (m_ommReadbackBytes + m_textureFingerprintBytes + record.bytes > ommcache::kMaxBytes)
          return OmmResult::DependenciesUnavailable;
        DxvkBufferCreateInfo bufferInfo;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        bufferInfo.size = record.bytes;
        record.readback = m_device->createBuffer(bufferInfo,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM texture fingerprint");
        if (record.readback == nullptr) return OmmResult::OutOfMemory;
        const VkImageSubresourceLayers layer { VK_IMAGE_ASPECT_COLOR_BIT, subview.minLevel, subview.minLayer, 1 };
        ctx->copyImageToBuffer(record.readback, 0, 1, 0, image, layer, VkOffset3D { 0, 0, 0 }, extent);
        ctx->emitMemoryBarrier(0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        record.frame = m_device->getCurrentFrameId();
        m_textureFingerprintBytes += record.bytes;
        ++m_textureFingerprintCount;
      }
    }
    if (record.unavailable) ++s_ommReuseStats.textureHashUnavailable;
    const bool unavailable = record.unavailable;
    m_textureFingerprints.emplace(cookie, std::move(record));
    return unavailable ? OmmResult::Failure : OmmResult::DependenciesUnavailable;
  }

  void OpacityMicromapManager::collectTextureFingerprints() {
    const uint32_t frame = m_device->getCurrentFrameId();
    for (auto& entry : m_textureFingerprints) {
      auto& record = entry.second;
      if (record.readback == nullptr || record.frame == frame || record.readback->isInUse()) continue;
      const void* data = record.readback->mapPtr(0);
      if (data) {
        // Full packed base mip, including compressed blocks, with no row padding.
        // View/format/extent enter the OMM key separately. Do not change Remix's
        // global material/texture identities just to enable this private cache.
        record.hash = XXH3_64bits_withSeed(data, size_t(record.bytes), 0x4f4d4d4650523735ull);
        if (!record.hash) record.hash = 1;
        ++s_ommReuseStats.textureHashes;
      } else {
        record.unavailable = true;
        ++s_ommReuseStats.textureHashUnavailable;
      }
      m_textureFingerprintBytes -= record.bytes;
      --m_textureFingerprintCount;
      record.readback = nullptr;
    }
  }

  DxvkOpacityMicromap::DxvkOpacityMicromap(DxvkDevice& device) : m_vkd(device.vkd()) { }

  DxvkOpacityMicromap::~DxvkOpacityMicromap() {
    if (opacityMicromap != VK_NULL_HANDLE && sharedMicromapOwner == nullptr) {
      m_vkd->vkDestroyMicromapEXT(m_vkd->device(), opacityMicromap, nullptr);
      opacityMicromap = VK_NULL_HANDLE;
    }

    opacityMicromapTriangleIndexBuffer = nullptr;
    opacityMicromapBuffer = nullptr;
  }

  OpacityMicromapCacheItem::OpacityMicromapCacheItem() {
    // Default constructor is needed for [] access into OMM cache, but it must not be called
    // for a case when the cache item is not already present in the cache
    assert(0 && "Invalid state. Default constructor for OpacityMicromapCacheItem should never be called.");
    Logger::err("[RTX Opacity Micromap] Encountered inconsistent state. Default constructor for OpacityMicromapCacheItem should never be called.");
  }

  OpacityMicromapCacheItem::OpacityMicromapCacheItem(DxvkDevice& device,
                                                     OpacityMicromapCacheState _cacheState,
                                                     const uint32_t inputSubdivisionLevel,
                                                     const bool enableVertexAndTextureOperations,
                                                     uint32_t currentFrameIndex,
                                                     std::list<XXH64_hash_t>::iterator _leastRecentlyUsedListIter,
                                                     std::list<XXH64_hash_t>::iterator _cacheStateListIter,
                                                     const OmmRequest& ommRequest)
    : cacheState(_cacheState)
    , lastUseFrameIndex(currentFrameIndex)
    , leastRecentlyUsedListIter(_leastRecentlyUsedListIter)
    , cacheStateListIter(_cacheStateListIter)
    , isUnprocessedCacheStateListIterValid(true)
    , numTriangles(ommRequest.numTriangles)
    , ommFormat(ommRequest.ommFormat) {
    useVertexAndTextureOperations = enableVertexAndTextureOperations;
    const uint32_t maxSubdivisionLevel =
      ommFormat == VkOpacityMicromapFormatEXT::VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT
      ? device.properties().extOpacityMicromapProperties.maxOpacity2StateSubdivisionLevel
      : device.properties().extOpacityMicromapProperties.maxOpacity4StateSubdivisionLevel;
    subdivisionLevel = std::min(inputSubdivisionLevel, maxSubdivisionLevel);
  }

  bool OpacityMicromapCacheItem::isCompatibleWithOmmRequest(const OmmRequest& ommRequest) {
    return ommRequest.ommFormat == ommFormat &&
           ommRequest.numTriangles == numTriangles;
  }

  VkDeviceSize OpacityMicromapCacheItem::getDeviceSize() const {
    return blasOmmBuffersDeviceSize + arrayBufferDeviceSize;
  }

  OpacityMicromapMemoryManager::OpacityMicromapMemoryManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_memoryProperties(device->adapter()->memoryProperties()) {
    // +1 to account for OMMs used in a previous TLAS
    const uint32_t kMaxFramesOMMResourcesAreUsed = kMaxFramesInFlight + (RtxOptions::enablePreviousTLAS() ? 1u : 0u);

    for (uint32_t i = 0; i < kMaxFramesOMMResourcesAreUsed; i++) {
      m_pendingReleaseSize.push_front(0);
    }
  }

  void OpacityMicromapMemoryManager::onFrameStart() {
    VkDeviceSize sizeToRelease = std::min(m_pendingReleaseSize.back(), m_used);
    m_pendingReleaseSize.pop_back();
    m_pendingReleaseSize.push_front(0);

    m_used -= sizeToRelease;
  }

  void OpacityMicromapMemoryManager::registerVidmemFreeSize() {
    // Gather runtime vidmem stats
    VkDeviceSize vidmemSize = 0;
    VkDeviceSize vidmemUsedSize = 0;

    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    DxvkMemoryAllocator& memoryManager = m_device->getCommon()->memoryManager();
    const VkPhysicalDeviceMemoryProperties& memoryProperties = memoryManager.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
      if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vidmemSize += memHeapInfo.heaps[i].memoryBudget;
        vidmemUsedSize += memHeapInfo.heaps[i].memoryAllocated;
      }
    }

    m_vidmemFreeSize = vidmemSize - std::min(vidmemUsedSize, vidmemSize);
  }

  void OpacityMicromapMemoryManager::updateMemoryBudget(Rc<DxvkContext> ctx) {

    // Gather runtime vidmem stats
    VkDeviceSize vidmemSize = 0;
    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    DxvkMemoryAllocator& memoryManager = m_device->getCommon()->memoryManager();
    const VkPhysicalDeviceMemoryProperties& memoryProperties = memoryManager.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
      if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vidmemSize += memHeapInfo.heaps[i].memoryBudget;
      }
    }

    // Consider the smalleer free VidMem size reported now and at the end of last frame.
    // End of last mem stats often account for more intra frame allocations, but updateMemoryBudget()
    // is called at the start of the frame to adjust OMM budget before any OMM allocs happen in the frame
    const VkDeviceSize prevEndOfFrameVidmemFreeSize = m_vidmemFreeSize;
    registerVidmemFreeSize();
    if (prevEndOfFrameVidmemFreeSize != kInvalidDeviceSize) {
      m_vidmemFreeSize = std::min(m_vidmemFreeSize, prevEndOfFrameVidmemFreeSize);
    }

    double maxVidmemSizePercentage = static_cast<double>(OpacityMicromapOptions::Cache::maxVidmemSizePercentage());

    // Halve the max budget when using a low mem GPU
    if (RtxOptions::lowMemoryGpu()) {
      maxVidmemSizePercentage /= 2;
    }

    // Calculate a new budget given the runtime vidmem stats

    VkDeviceSize maxBudget =
      std::min(static_cast<VkDeviceSize>(maxVidmemSizePercentage * vidmemSize),
              static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::maxBudgetSizeMB()) * 1024 * 1024);

    const VkDeviceSize hardMinFreeVidmemToNotAllocate = static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::minFreeVidmemMBToNotAllocate()) * 1024 * 1024;
    const VkDeviceSize softMinFreeVidmemToNotAllocate = hardMinFreeVidmemToNotAllocate + static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::freeVidmemMBBudgetBuffer()) * 1024 * 1024;
    
    m_prevBudget = m_budget;

    // Recalculate budget if free memory dropped below the hard limit or is over the soft limit
    if (m_vidmemFreeSize < hardMinFreeVidmemToNotAllocate || m_vidmemFreeSize > softMinFreeVidmemToNotAllocate) {
      m_budget = std::min(m_vidmemFreeSize - std::min(softMinFreeVidmemToNotAllocate, m_vidmemFreeSize) + static_cast<VkDeviceSize>(m_used), maxBudget);
    }

    if (m_budget < static_cast<VkDeviceSize>(OpacityMicromapOptions::Cache::minBudgetSizeMB()) * 1024 * 1024) {
      m_budget = 0;
    }
  
    if (m_budget != m_prevBudget && m_budget == 0) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Opacity Micromap] Free Vidmem dropped below a limit. Setting budget to 0."));
    }

    // Invalidate m_vidmemFreeSize to make sure we use it only when it was set at the end of the frame again
    m_vidmemFreeSize = kInvalidDeviceSize;
  }

  bool OpacityMicromapMemoryManager::allocate(VkDeviceSize size) {
    if (size > getAvailable()) {
      ONCE(KENSHI_DIAGNOSTIC_INFO(str::format("[RTX Opacity Micromap] Out of memory budget. Requested: ", size, " bytes. Free: ", getAvailable(), " bytes, Budget: ", getBudget(), " bytes")));
      return false;
    }

    m_used += size;

    return true;
  }

  VkDeviceSize OpacityMicromapMemoryManager::getAvailable() const {
    return m_budget - std::min(m_used, m_budget);
  }

  void OpacityMicromapMemoryManager::release(VkDeviceSize size) {
    m_pendingReleaseSize.back() += size;
  }

  void OpacityMicromapMemoryManager::releaseAll() {
    release(m_used);
  }

  VkDeviceSize OpacityMicromapMemoryManager::getPrevBudget() const {
    return m_prevBudget;
  }

  float OpacityMicromapMemoryManager::calculateUsageRatio() const {
    return m_used / static_cast<float>(m_budget);
  }

  VkDeviceSize OpacityMicromapMemoryManager::calculatePendingAvailableSize() const {
    return std::min(getAvailable() + calculatePendingReleasedSize(), m_budget);
  }

  VkDeviceSize OpacityMicromapMemoryManager::calculatePendingReleasedSize() const {
    VkDeviceSize totalSizeToRelease = 0;
    for (auto& sizeToRelease : m_pendingReleaseSize)
      totalSizeToRelease += sizeToRelease;

    return totalSizeToRelease;
  }

  VkDeviceSize OpacityMicromapMemoryManager::getNextPendingReleasedSize() const {
    return m_pendingReleaseSize.back();
  }

  Rc<DxvkBuffer> OpacityMicromapManager::getScratchMemory(const size_t requiredScratchAllocSize) {
    if (m_scratchBuffer == nullptr || m_scratchBuffer->info().size < requiredScratchAllocSize) {
      DxvkBufferCreateInfo bufferCreateInfo {};
      bufferCreateInfo.size = requiredScratchAllocSize;
      bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // Assigning the result directly would destroy a perfectly good existing
      // buffer when a grow fails, turning a transient shortage into a permanent
      // null. Micromap builds reference this by DEVICE ADDRESS, so a null here
      // is not a skipped build - it is a GPU write to address 0.
      Rc<DxvkBuffer> grownScratch = device()->createBuffer(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "OMM Scratch");

      if (grownScratch == nullptr) {
        ONCE(Logger::err(str::format(
          "OpacityMicromapManager: scratch allocation of ", requiredScratchAllocSize,
          " bytes failed; keeping the previous buffer rather than binding a null scratch address.")));
        return nullptr;
      }

      m_scratchBuffer = std::move(grownScratch);
    }

    return m_scratchBuffer;
  }

  OpacityMicromapManager::OpacityMicromapManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_memoryManager(device)
    , m_ommGeneration(s_nextOmmGeneration++) {
  }

  OpacityMicromapManager::~OpacityMicromapManager() { 
    s_ommRetention.erase(this);
#ifdef VALIDATION_MODE
    // Delink instances so that the assert on cache data destruction doesn't trigger
    for (auto& sourceData : m_cachedSourceData) {
      sourceData.second.setInstance(nullptr, m_instanceOmmRequests, *this);
    }
#endif
  }

  void OpacityMicromapManager::onDestroy() {
  }

  OmmRequest::OmmRequest(const RtInstance& _instance, const InstanceManager& instanceManager, uint32_t _quadSliceIndex)
    : instance(_instance)
    , quadSliceIndex(_quadSliceIndex) {

    OpacityMicromapHashSourceData hashSourceData;

    // Fill material properties
    {
      hashSourceData.materialHash = instance.getMaterialHash();
      hashSourceData.alphaState = instance.surface.alphaState;
      hashSourceData.colorSource = instance.surface.colorSource;
      hashSourceData.alphaSource = instance.surface.alphaSource;
      hashSourceData.modulateVertexColor = instance.surface.modulateVertexColor;
      hashSourceData.modulateVertexAlpha = instance.surface.modulateVertexAlpha;
      hashSourceData.opacityTextureChannel = instance.surface.opacityTextureChannel;
      // Quantized to the same 10 bits the surface carries, so the key changes
      // exactly when the baked opacity would.
      hashSourceData.blendConstantAlpha = static_cast<uint16_t>(
        std::min(1.0f, std::max(0.0f, instance.surface.blendConstant.w)) * 1023.0f + 0.5f);
      hashSourceData.textureTransform = instance.surface.textureTransform;
    }

    if (isBillboardOmmRequest()) {
      hashSourceData.numTriangles = 2;

      const IntersectionBillboard& billboard = instanceManager.getBillboards()[instance.getFirstBillboardIndex() + quadSliceIndex];
      hashSourceData.texCoordHash = billboard.texCoordHash;
      hashSourceData.vertexOpacityHash = billboard.vertexOpacityHash;

      // Index hash is not explicitly included for billboards as it's already part of texcoordHash,
      // which is generated using actual triangle order in a billboard quad
    }
    else {
      hashSourceData.numTriangles = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();
      hashSourceData.texCoordHash = instance.getTexcoordHash();
      hashSourceData.indexHash = instance.getIndexHash();
      // ToDo add vertexOpacityHash
    }

    // Select OmmFormat for the OMM request
    {
      hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;

      auto& alphaState = instance.surface.alphaState;

      if (OpacityMicromapOptions::Building::allow2StateOpacityMicromaps() && (
        isBillboardOmmRequest() ||
        (!alphaState.isFullyOpaque && (alphaState.isParticle || alphaState.isDecal)) || alphaState.emissiveBlend))
        hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;

      if (OpacityMicromapOptions::Building::force2StateOpacityMicromaps())
        hashSourceData.ommFormat = VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    }

    if (OpacityMicromapOptions::Cache::hashInstanceIndexOnly()) {
      ommSrcHash = instance.getId();
    }
    else { // Generate a hash from the gathered source data
      ommSrcHash = XXH3_64bits(&hashSourceData, sizeof(hashSourceData));

      HashCollisionDetection::registerHashedSourceData(ommSrcHash, static_cast<void*>(&hashSourceData), HashSourceDataCategory::OpacityMicromap);
    }

    numTriangles = hashSourceData.numTriangles;
    ommFormat = hashSourceData.ommFormat;
  }

  OpacityMicromapManager::CachedSourceData::~CachedSourceData() {
    omm_validation_assert(!instance && "Instance has not been unlinked");
  }

  void OpacityMicromapManager::CachedSourceData::initialize(const OmmRequest& ommRequest, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager) {
    setInstance(&ommRequest.instance, instanceOmmRequests, ommManager);

    numTriangles = ommRequest.numTriangles;

    if (ommRequest.isBillboardOmmRequest()) {
      // ToDo: add compiler check support to ensure the right values are specified here
      triangleOffset = 2 * ommRequest.quadSliceIndex;
    } else {
      triangleOffset = 0;
    }
  }

  void OpacityMicromapManager::CachedSourceData::setInstance(const RtInstance* newInstance, fast_unordered_cache<InstanceOmmRequests>& instanceOmmRequests, OpacityMicromapManager& ommManager, bool deleteParentInstanceIfEmpty) {
    omm_validation_assert(instance != newInstance && "Redundant call setting the same instance twice.");

    if (instance && newInstance) {
      setInstance(nullptr, instanceOmmRequests, ommManager, deleteParentInstanceIfEmpty);
    }

    if (newInstance) {
      OpacityMicromapInstanceData& newOmmInstanceData = getOmmInstanceData(*newInstance);

      instanceOmmRequests[newOmmInstanceData.ommSrcHash].numActiveRequests += 1;

      // Request numTexelsPerMicroTriangle to be calculated.
      // Note: this may get set to true even after the data was calculated,
      //   but that is OK as the data will not be calculated twice 
      //   since it's checked for being available first then
      newOmmInstanceData.needsToCalculateNumTexelsPerMicroTriangle = true;
    }
    // instance should always be valid at this point, but let's check on previous instance being actually valid before unlinking it
    else if (instance) {
      auto instanceOmmRequestsIter = instanceOmmRequests.find(getOpacityMicromapHash(*instance));
      if (instanceOmmRequestsIter != instanceOmmRequests.end()) {
        omm_validation_assert(instanceOmmRequestsIter->second.numActiveRequests > 0);
        if (instanceOmmRequestsIter->second.numActiveRequests > 0) {
          instanceOmmRequestsIter->second.numActiveRequests -= 1;
        }
        if (deleteParentInstanceIfEmpty && instanceOmmRequestsIter->second.numActiveRequests == 0) {
          instanceOmmRequests.erase(instanceOmmRequestsIter);
        }
      } else {
        omm_validation_assert(0 && "OMM source data parent request container was already removed");
      }

      ommManager.onInstanceUnlinked(*instance);
    }

    instance = newInstance;
  }

  void OpacityMicromapManager::onInstanceUnlinked(const RtInstance& instance) {
    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);

    // Make sure to set the request to false, since the calculations are throttled 
    // and it's possible the calculation doesn't complete prior to instance being unlinked 
    // (i.e. due to linked OMM cache items getting destroyed)
    ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;

    // Delete staging numTexelsPerMicroTriangle data associated with the instance
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      m_numTexelsPerMicroTriangleStaging.erase(&instance);
    } else {
      omm_validation_assert(m_numTexelsPerMicroTriangleStaging.find(&instance) == m_numTexelsPerMicroTriangleStaging.end());
    }
  }

  void OpacityMicromapManager::destroyOmmData(OpacityMicromapCache::iterator ommCacheItemIter, bool destroyParentInstanceOmmRequestContainer) {
    const XXH64_hash_t ommSrcHash = ommCacheItemIter->first;
    s_ommRetention[this].forget(ommSrcHash);
    OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
    const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;
    if (ommCacheItem.hasSharedKey) {
      auto shared = m_sharedOmmSources.find(ommCacheItem.sharedKey);
      if (shared != m_sharedOmmSources.end() && shared->second == ommSrcHash)
        m_sharedOmmSources.erase(shared);
    }

#ifdef VALIDATION_MODE
    Logger::warn(str::format("[RTX Opacity Micromap] Destroying ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

    switch (ommCacheState) {
    case OpacityMicromapCacheState::eStep0_Unprocessed:
    case OpacityMicromapCacheState::eStep1_Baking:
      // Note the iterator may be invalid if the cache state list element was
      // already destroyed when source data was unlinked
      if (ommCacheItem.isUnprocessedCacheStateListIterValid) {
        m_unprocessedList.erase(ommCacheItem.cacheStateListIter);
        ommCacheItem.isUnprocessedCacheStateListIterValid = false;
      }
      m_numTexelsPerMicroTriangle.erase(ommSrcHash);
      break;
    case OpacityMicromapCacheState::eStep2_Baked:
      m_bakedList.erase(ommCacheItem.cacheStateListIter);
      break;
    case OpacityMicromapCacheState::eStep3_Built:
      m_builtList.erase(ommCacheItem.cacheStateListIter);
      break;
    case OpacityMicromapCacheState::eStep4_Ready:
      break;
    default:
      omm_validation_assert(0);
      break;
    }

    if (ommCacheState <= OpacityMicromapCacheState::eStep2_Baked)
      deleteCachedSourceData(ommSrcHash, ommCacheState, destroyParentInstanceOmmRequestContainer);

    m_leastRecentlyUsedList.erase(ommCacheItemIter->second.leastRecentlyUsedListIter);
    releaseOmmMemory(ommCacheItemIter->second);
    m_ommCache.erase(ommCacheItemIter);
  }

  void OpacityMicromapManager::releaseOmmMemory(OpacityMicromapCacheItem& item) {
    // Cache removal does not release resources still owned by cached BLASes or
    // submitted commands. Keep that memory charged until the last owner leaves.
    m_memoryManager.release(item.arrayBufferDeviceSize);
    if (item.blasOmmBuffers != nullptr && item.blasOmmBuffers->refCount() > 1) {
      m_retiredOmms.push_back({ item.blasOmmBuffers, item.blasOmmBuffersDeviceSize });
      m_needsBlasRebuild = true;
    } else {
      m_memoryManager.release(item.blasOmmBuffersDeviceSize);
    }
  }

  void OpacityMicromapManager::releaseRetiredOmms() {
    for (size_t i = 0; i < m_retiredOmms.size();) {
      if (m_retiredOmms[i].resource->refCount() == 1) {
        m_memoryManager.release(m_retiredOmms[i].size);
        if (i + 1 != m_retiredOmms.size())
          m_retiredOmms[i] = std::move(m_retiredOmms.back());
        m_retiredOmms.pop_back();
      } else {
        ++i;
      }
    }
  }

  void OpacityMicromapManager::destroyOmmData(XXH64_hash_t ommSrcHash) {
    destroyOmmData(m_ommCache.find(ommSrcHash));
  }

  void OpacityMicromapManager::destroyInstance(const RtInstance& instance, bool forceDestroy) {
    // Don't destroy the container as it's being used to iterate through below
    const bool destroyParentInstanceOmmRequestContainer = false;

    // Reset retain-mode state so this instance can be re-registered if recreated
    getOmmInstanceData(instance).ommBuildRequested = false;
    m_ommCandidates.erase(const_cast<RtInstance*>(&instance));

    auto destroyCachedData = [&](XXH64_hash_t ommSrcHash) {
      auto ommCacheIterator = m_ommCache.find(ommSrcHash);

      // Unknown element, ignore it
      if (ommCacheIterator == m_ommCache.end())
        return;

      OpacityMicromapCacheItem& ommCacheItem = ommCacheIterator->second;
      const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;

      if (!forceDestroy) {
        switch (ommCacheState) {
          // Continue with destruction of unbaked items
        case OpacityMicromapCacheState::eStep0_Unprocessed:
          break;

          // If the OMM data has been at least partially baked keep it in the cache
        case OpacityMicromapCacheState::eStep1_Baking:
          // Remove partially baked OMM items from to be baked list until a new instance is linked with it again
          if (ommCacheItem.isUnprocessedCacheStateListIterValid) {
            m_unprocessedList.erase(ommCacheItem.cacheStateListIter);
            ommCacheItem.isUnprocessedCacheStateListIterValid = false;
            deleteCachedSourceData(ommSrcHash, ommCacheState, destroyParentInstanceOmmRequestContainer);
            auto& retention = s_ommRetention[this];
            auto& metadata = retention.entries[ommSrcHash];
            retention.abandoned.erase({metadata.detachedSinceMs, ommSrcHash});
            metadata.detachedSinceMs = ommRetentionTimeMs();
            retention.abandoned.emplace(std::make_pair(metadata.detachedSinceMs, ommSrcHash), metadata.generation);
          }
          return;
        case OpacityMicromapCacheState::eStep2_Baked:
        case OpacityMicromapCacheState::eStep3_Built:
        case OpacityMicromapCacheState::eStep4_Ready:
          return;

        default:
          // Continue with destruction
          omm_validation_assert(0);
          break;
        }
      }

      // Note: invalidates the omm cache iterator
      destroyOmmData(ommCacheIterator, destroyParentInstanceOmmRequestContainer);
    };

    m_numTexelsPerMicroTriangleStaging.erase(&instance);

    // Destroy all OMM requests associated with the instance
    XXH64_hash_t ommSrcHash = getOpacityMicromapHash(instance);
    if (ommSrcHash != kEmptyHash) {
      auto instanceOmmRequestsIter = m_instanceOmmRequests.find(ommSrcHash);

      if (instanceOmmRequestsIter != m_instanceOmmRequests.end()) {
        static_assert(destroyParentInstanceOmmRequestContainer == false);
        for (auto& ommRequest : instanceOmmRequestsIter->second.ommRequests)
          destroyCachedData(ommRequest.ommSrcHash);

        m_instanceOmmRequests.erase(instanceOmmRequestsIter);
      }
    }
  }

  uint32_t OpacityMicromapManager::calculateNumMicroTriangles(uint16_t subdivisionLevel) {
    return static_cast<uint32_t>(round(pow(4, subdivisionLevel)));
  }

  void OpacityMicromapManager::clear() {
    s_ommRetention[this].entries.clear();
    s_ommRetention[this].abandoned.clear();
    s_ommRetention[this].sweep = {};
    s_ommRetention[this].trimming = false;
    kenshi_fault::add(kenshi_fault::OmmReset,m_device->getCurrentFrameId(),2,m_ommCache.size());
    m_unprocessedList.clear();
    m_bakedList.clear();
    m_builtList.clear();

    m_leastRecentlyUsedList.clear();
    for (auto& entry : m_ommCache)
      releaseOmmMemory(entry.second);
    m_ommCache.clear();
    m_sharedOmmSources.clear();
    m_newlyBuiltSources.clear();
    m_newlyReadySources.clear();
    m_cacheScanStart = kEmptyHash;

#ifdef VALIDATION_MODE
    // Delink instances so that the assert on cache data destruction doesn't trigger
    for (auto& sourceData : m_cachedSourceData) {
      sourceData.second.setInstance(nullptr, m_instanceOmmRequests, *this);
    }
#endif
    m_cachedSourceData.clear();
    m_ommBuildRequestStatistics.clear();

    m_numTexelsPerMicroTriangleStaging.clear();
    m_numTexelsPerMicroTriangle.clear();

    m_instanceOmmRequests.clear();

    // Note: m_ommCandidates is intentionally NOT cleared here. It tracks
    // instances that need a retry, while processOmmCandidates() also scans
    // the instance table for stale generation data after this generation bump.

    // Allocate from the same sequence as construction: a local increment can
    // collide with the next manager after an option reset followed by recreation.
    m_ommGeneration = s_nextOmmGeneration++;

    m_needsBlasRebuild = true;
    m_amountOfMemoryMissing = 0;

    // There's no need to clear m_blackListedList
  }

  void OpacityMicromapManager::onCameraCutSceneClear() {
    // Scene instances and bindless indices are about to disappear. Completed
    // OMMs can be rebound by source hash. Fully baked arrays have already
    // unlinked their instance and also survive; only unfinished bakes still
    // depend on old surface/bindless indices.
    uint32_t discarded = 0;
    for (auto it = m_ommCache.begin(); it != m_ommCache.end();) {
      if (it->second.cacheState < OpacityMicromapCacheState::eStep2_Baked) {
        auto old = it++;
        destroyOmmData(old);
        ++discarded;
      } else { ++it; }
    }
    m_ommCandidates.clear();
    m_instancesToDestroy.clear();
    m_instanceOmmRequests.clear();
    m_ommBuildRequestStatistics.clear();
    m_numTexelsPerMicroTriangleStaging.clear();
    m_numTexelsPerMicroTriangle.clear();
    m_ommGeneration = s_nextOmmGeneration++;
    m_needsBlasRebuild = true;
    m_amountOfMemoryMissing = 0;
    KENSHI_DIAGNOSTIC_INFO(str::format("[OMM V770] camera-cut retainedBuilt=", m_ommCache.size() - m_bakedList.size(),
      " retainedBaked=", m_bakedList.size(), " discardedIncomplete=", discarded));
  }

  void OpacityMicromapManager::showImguiSettings() const {

    const static ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;

#define ADVANCED(x) if (OpacityMicromapOptions::showAdvancedOptions()) x

    RemixGui::Checkbox("Show Advanced Settings", &OpacityMicromapOptions::showAdvancedOptionsObject());
    RemixGui::Checkbox("Enable Binding", &OpacityMicromapOptions::enableBindingObject());
    ADVANCED(RemixGui::Checkbox("Enable Baking Arrays", &OpacityMicromapOptions::enableBakingArraysObject()));
    ADVANCED(RemixGui::Checkbox("Enable Building", &OpacityMicromapOptions::enableBuildingObject()));

    RemixGui::Checkbox("Reset Every Frame", &OpacityMicromapOptions::enableResetEveryFrameObject());

    // Stats
    if (RemixGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      ImGui::Text("# Bound/Requested OMMs: %d/%d", m_numBoundOMMs, m_numRequestedOMMBindings);
      ADVANCED(ImGui::Text("# Staged Requested Items: %d", m_ommBuildRequestStatistics.size()));
      ADVANCED(ImGui::Text("# Unprocessed Items: %d", m_unprocessedList.size()));
      ADVANCED(ImGui::Text("# Baked Items: %d", m_bakedList.size()));
      ADVANCED(ImGui::Text("# Built Items: %d", m_builtList.size()));
      ADVANCED(ImGui::Text("# Cache Items: %d", m_ommCache.size()));
      ADVANCED(ImGui::Text("# Black Listed Items: %d", m_blackListedList.size()));
      ImGui::Text("VRAM usage/budget [MB]: %d/%d", m_memoryManager.getUsed() / (1024 * 1024), m_memoryManager.getBudget() / (1024 * 1024));

      ADVANCED(ImGui::Text("# Baked uTriagles [million]: %.1f", m_numMicroTrianglesBaked / 1e6));

      ADVANCED(ImGui::Text("# Built uTriagles [million]: %.1f", m_numMicroTrianglesBuilt / 1e6));
      ImGui::Unindent();
    }

    ADVANCED(
      if (RemixGui::CollapsingHeader("Scene")) {
        ImGui::Indent();
        ImGui::Unindent();
      });

    if (RemixGui::CollapsingHeader("Cache")) {
      ImGui::Indent();
      RemixGui::DragFloat("Budget: Max Vidmem Size %", &OpacityMicromapOptions::Cache::maxVidmemSizePercentageObject(), 0.001f, 0.0f, 1.f, "%.3f", sliderFlags);
      ADVANCED(RemixGui::DragInt("Budget: Min Required Size [MB]", &OpacityMicromapOptions::Cache::minBudgetSizeMBObject(), 8.f, 0, 256 * 1024, "%d", sliderFlags));
      RemixGui::DragInt("Budget: Max Allowed Size [MB]", &OpacityMicromapOptions::Cache::maxBudgetSizeMBObject(), 8.f, 0, 256 * 1024, "%d", sliderFlags);
      RemixGui::DragInt("Budget: Min Vidmem Free To Not Allocate [MB]", &OpacityMicromapOptions::Cache::minFreeVidmemMBToNotAllocateObject(), 16.f, 0, 256 * 1024, "%d", sliderFlags);
      ADVANCED(RemixGui::DragInt("Min Usage Frame Age Before Eviction", &OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEvictionObject(), 1.f, 0, 60 * 3600, "%d", sliderFlags));
      ADVANCED(RemixGui::Checkbox("Hash Instance Index Only", &OpacityMicromapOptions::Cache::hashInstanceIndexOnlyObject()));
      ImGui::Unindent();
    }


    if (RemixGui::CollapsingHeader("Requests Filter")) {
      ImGui::Indent();
      RemixGui::Checkbox("Enable Filtering", &OpacityMicromapOptions::BuildRequests::filteringObject());
      RemixGui::Checkbox("Animated Instances", &OpacityMicromapOptions::BuildRequests::enableAnimatedInstancesObject());
      RemixGui::Checkbox("Particles", &OpacityMicromapOptions::BuildRequests::enableParticlesObject());
      ADVANCED(RemixGui::Checkbox("Custom Filters for Billboards", &OpacityMicromapOptions::BuildRequests::customFiltersForBillboardsObject()));

      ADVANCED(RemixGui::DragInt("Max Staged Requests", &OpacityMicromapOptions::BuildRequests::maxRequestsObject(), 1.f, 1, 1000 * 1000, "%d", sliderFlags));
      // ToDo: we don't support setting this to 0 at the moment, should revisit later
      ADVANCED(RemixGui::DragInt("Min Instance Frame Age", &OpacityMicromapOptions::BuildRequests::minInstanceFrameAgeObject(), 1.f, 0, 200, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Min Num Frames Requested", &OpacityMicromapOptions::BuildRequests::minNumFramesRequestedObject(), 1.f, 0, 200, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Max Request Frame Age", &OpacityMicromapOptions::BuildRequests::maxRequestFrameAgeObject(), 1.f, 0, 60 * 3600, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Min Num Requests", &OpacityMicromapOptions::BuildRequests::minNumRequestsObject(), 1.f, 1, 1000, "%d", sliderFlags));
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Building")) {
      ImGui::Indent();

      RemixGui::Checkbox("Split Billboard Geometry", &OpacityMicromapOptions::Building::splitBillboardGeometryObject());
      RemixGui::DragInt("Max Allowed Billboards Per Instance To Split", &OpacityMicromapOptions::Building::maxAllowedBillboardsPerInstanceToSplitObject(), 1.f, 0, 4096, "%d", sliderFlags);

      // Note: 2 is minimum to ensure # micro triangle size is a multiple of 1 byte to ensure cross triangle alignment requirement
      RemixGui::DragInt("Subdivision Level", &OpacityMicromapOptions::Building::subdivisionLevelObject(), 1.f, 2, 11, "%d", sliderFlags);
      ADVANCED(RemixGui::Checkbox("Vertex, Texture Ops & Emissive Blending", &OpacityMicromapOptions::Building::enableVertexAndTextureOperationsObject()));
      ADVANCED(RemixGui::Checkbox("Allow 2 State Opacity Micromaps", &OpacityMicromapOptions::Building::allow2StateOpacityMicromapsObject()));
      ADVANCED(RemixGui::Checkbox("Force 2 State Opacity Micromaps", &OpacityMicromapOptions::Building::force2StateOpacityMicromapsObject()));

      ADVANCED(RemixGui::DragFloat("Decals: Min Resolve Transparency Threshold", &OpacityMicromapOptions::Building::decalsMinResolveTransparencyThresholdObject(), 0.001f, 0.0f, 1.f, "%.3f", sliderFlags));

      ADVANCED(RemixGui::DragInt("Max # of uTriangles to Bake [Million per Second]", &OpacityMicromapOptions::Building::maxMicroTrianglesToBakeMillionPerSecondObject(), 1.f, 1, 65536, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("Max # of uTriangles to Build [Million per Second]", &OpacityMicromapOptions::Building::maxMicroTrianglesToBuildMillionPerSecondObject(), 1.f, 1, 65536, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("# Frames with High Workload Multiplier at Start", &OpacityMicromapOptions::Building::numFramesAtStartToBuildWithHighWorkloadObject(), 1.f, 0, 100000, "%d", sliderFlags));
      ADVANCED(RemixGui::DragInt("High Workload Multiplier", &OpacityMicromapOptions::Building::highWorkloadMultiplierObject(), 1.f, 1, 1000, "%d", sliderFlags));

      if (RemixGui::CollapsingHeader("Conservative Estimation")) {
        ImGui::Indent();
        RemixGui::Checkbox("Enable", &OpacityMicromapOptions::Building::ConservativeEstimation::enableObject());
        ADVANCED(RemixGui::DragInt("Max Texel Taps Per uTriangle", &OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangleObject(), 16.f, 1, 256 * 256, "%d", sliderFlags);
        ImGui::Unindent());
      }

      ImGui::Unindent();
    }
  }

  void OpacityMicromapManager::logStatistics() const {
    KENSHI_DIAGNOSTIC_INFO(str::format(
      "[RTX Opacity Micromap] Statistics:\n",
      "\t# Bound/Requested OMMs: ", m_numBoundOMMs, "/", m_numRequestedOMMBindings, "\n",
      "\t# Staged Requested Items: ", m_ommBuildRequestStatistics.size(), "\n",
      "\t# Unprocessed Items: ", m_unprocessedList.size(), "\n",
      "\t# Baked Items: ", m_bakedList.size(), "\n",
      "\t# Built Items: ", m_builtList.size(), "\n",
      "\t# Cache Items: ", m_ommCache.size(), "\n",
      "\t# Black Listed Items: ", m_blackListedList.size(), "\n",
      "\tVRAM usage/budget [MB]: ", m_memoryManager.getUsed() / (1024 * 1024), "/", m_memoryManager.getBudget() / (1024 * 1024)));
  }

  bool OpacityMicromapManager::checkIsOpacityMicromapSupported(DxvkDevice& device) {
    bool isOpacityMicromapSupported = device.extensions().khrSynchronization2 &&
                                      device.extensions().extOpacityMicromap;

    if (RtxOptions::areValidationLayersEnabled() && isOpacityMicromapSupported) {
      Logger::warn(str::format("[RTX] Opacity Micromap vendor extension is not compatible with VK Validation Layers. Disabling Opacity Micromap extension."));
      isOpacityMicromapSupported = false;
    }

    return isOpacityMicromapSupported;
  }


  InstanceEventHandler OpacityMicromapManager::getInstanceEventHandler() {
    InstanceEventHandler instanceEvents(this);
    instanceEvents.onInstanceAddedCallback = [this](const RtInstance& instance) { onInstanceAdded(instance); };
    instanceEvents.onInstanceUpdatedCallback = [this](const RtInstance& instance, const DrawCallState& drawCall, const MaterialData& material, bool hasTransformChanged, bool hasVerticesChanged, bool isFirstUpdateThisFrame) { onInstanceUpdated(instance, drawCall, material, hasTransformChanged, hasVerticesChanged, isFirstUpdateThisFrame); };
    instanceEvents.onInstanceDestroyedCallback = [this](const RtInstance& instance) { onInstanceDestroyed(instance); };
    return instanceEvents;
  }

  void OpacityMicromapManager::onInstanceAdded(const RtInstance& instance) {
    // Defer insertion until processOmmCandidates() can collect instances
    // that have been through at least one full BLAS build cycle.
    // Direct insertion here was causing GPU device lost because instances
    // haven't had fillGeometryInfoFromBlasEntry or uploadSurfaceData run yet.
  }

  void OpacityMicromapManager::seedCandidates(const std::vector<RtInstance*>& instances) {
    for (RtInstance* inst : instances) {
      if (inst && !inst->isMarkedForGC()) {
        m_ommCandidates.insert(inst);
      }
    }
  }

  // Requires processOmmCandidates() to be called prior to this in a frame
  bool OpacityMicromapManager::usesOpacityMicromap(const RtInstance& instance) {
    const OpacityMicromapInstanceData& ommInstanceData = instance.getOpacityMicromapInstanceData();

    return ommInstanceData.usesOMM;
  }

  bool OpacityMicromapManager::usesSplitBillboardOpacityMicromap(const RtInstance& instance) {
    return
      OpacityMicromapOptions::Building::splitBillboardGeometry() &&
      // ToDo: this should be "> 1" since it is wasteful to split 1 billboard geos 
      // but doing so prevents OMM getting applied to a particle for portal gun diode on top,
      // so leaving it at "> 0" for now
      instance.getBillboardCount() > 0 &&
      instance.getBillboardCount() <= OpacityMicromapOptions::Building::maxAllowedBillboardsPerInstanceToSplit();
  }

  bool OpacityMicromapManager::useStagingNumTexelsPerMicroTriangleObject(const RtInstance& instance) {
    return instance.getFrameAge() == 0 && OpacityMicromapManager::usesSplitBillboardOpacityMicromap(instance);
  }

  XXH64_hash_t OpacityMicromapManager::getOpacityMicromapHash(const RtInstance& instance) {
    const OpacityMicromapInstanceData& ommInstanceData = instance.getOpacityMicromapInstanceData();
    return ommInstanceData.ommSrcHash;
  }

  OpacityMicromapInstanceData::OpacityMicromapInstanceData()
    : usesOMM(false)
    , needsToCalculateNumTexelsPerMicroTriangle(false)
    , ommBuildRequested(false) { }

  OpacityMicromapInstanceData& OpacityMicromapManager::getOmmInstanceData(const RtInstance& instance) {
    // OMM Instance Data is managed by OMM manager but stored in an instance to avoid indirect lookups. 
    // RtInstance is generally passed via const reference into OMM as often nothing else needs to be modified,
    // but OMM manager still needs to be able to modify the OMM instance data. So we remove the constness here
    return const_cast<OpacityMicromapInstanceData&>(instance.getOpacityMicromapInstanceData());
  }

  void OpacityMicromapManager::onInstanceUpdated(const RtInstance& instance,
                                                 const DrawCallState& /*drawCall*/,
                                                 const MaterialData& /*material*/,
                                                 const bool hasTransformChanged,
                                                 const bool hasVerticesChanged,
                                                 const bool isFirstUpdateThisFrame) {
    ScopedCpuProfileZone();

    // Skip calculating data needed for new OMMs if there's not enough memory to build any OMM request
    if (!m_hasEnoughMemoryToPotentiallyGenerateAnOmm) {
      return;
    }

    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);

    // OMMs for billboards are built on a first frame they are seen if OMM budget permits 
    // and since such instances often have 1 frame lifetime, the buffers need to be available in that first frame
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = true;
    }

    const auto item = m_ommCache.find(getOpacityMicromapHash(instance));
    if (item != m_ommCache.end() && item->second.reuseData && item->second.reuseData->enabled) {
      ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;
      return;
    }

    // Calculate num texels per micro triangle if requested.
    // This is calculated inline on a draw call submission timeline since
    // a draw call may try to block on an access to a buffer 
    // that was also used in an earlier draw call
    // but if the earlier draw call places a ref on the buffer for OMM to keep it around for latter use, 
    // it will block the draw call submission thread until that ref is lifted.
    // Calculating the data inline here avoids that.
    if (ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle) {
      calculateNumTexelsPerMicroTriangle(instance);
    }
  }
    
  // Calculates number of texels that cover a micro triangle in a triangle.
  // This matches the texcoord span done for conservative opacity estimation during OMM triangle array baking.
  // Returns UINT32_MAX if number of texels exceeds the maximum allowed value
  uint32_t OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(
    Vector2 triangleTexcoords[3],
    float rcpNumMicroTrianglesAlongEdge,
    Vector2 textureResolution) {

    // For the sake of simplicity, we only calculate number of texels needed for a first micro triangle in the triangle. 
    // Even though the micro triangles have the same UV area, the number of texels covering it may be different 
    // between them depending on how their texcoords fit into texel bounds cutoffs, but the variability should be 
    // small enough for OMM's purposes of estimating number of texels needed in a micro triangle when calculating baking costs.

    // Calculate micro triangle texcoords
    Vector2 texcoords[3];
    texcoords[0] = triangleTexcoords[0];
    texcoords[1] = triangleTexcoords[0] + rcpNumMicroTrianglesAlongEdge * (triangleTexcoords[1] - triangleTexcoords[0]);
    texcoords[2] = triangleTexcoords[0] + rcpNumMicroTrianglesAlongEdge * (triangleTexcoords[2] - triangleTexcoords[0]);

    // Find texcoord bbox for the micro triangle
    Vector2 texcoordsMin(FLT_MAX, FLT_MAX);
    Vector2 texcoordsMax(-FLT_MAX, -FLT_MAX);
    for (uint32_t i = 0; i < 3; i++) {
      texcoordsMin = min(texcoords[i], texcoordsMin);
      texcoordsMax = max(texcoords[i], texcoordsMax);
    }

    // Find the sampling index bbox for the micro triangle.
    // Align the bbox to actual texel centers that fully cover the bbox.
    // Align with a top left texel relative to the bbox min.
    // Add epsilon to avoid host underestimating sampling footprint due to float precision errors. 
    // 0.001 should generally be large enough.
    // Should the underestimation still occur, the shader will fall back to a conservative value for a micro triangle.
    const float kEpsilon = 0.001f;
    const float kHalfTexelOffset = 0.5f + kEpsilon;
    const Vector2 texcoordsIndexMin = doFloor(texcoordsMin * textureResolution - Vector2{ kHalfTexelOffset });
    // Align with a bottom right pixel relative to the bbox max
    const Vector2 texcoordsIndexMax = doFloor(texcoordsMax * textureResolution + Vector2{ kHalfTexelOffset });

    // Calculate number of texels in the given texcoord bbox.
    // +1: include the end point of the bbox
    const Vector2 texelSampleDims = texcoordsIndexMax - texcoordsIndexMin + Vector2{ 1.0f };
    const uint32_t numTexelsPerMicroTriangle =
      static_cast<uint32_t>(std::min<float>(round(texelSampleDims.x * texelSampleDims.y), static_cast<float>(UINT32_MAX)));

    return numTexelsPerMicroTriangle;
  }

  void OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(
    NumTexelsPerMicroTriangleCalculationData& numTexelsPerMicroTriangle,
    const RtInstance& instance,
    const uint32_t numTriangles) {

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();

    if (geometryData.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Opacity Micromap] Instance has non triangle list topology. This is only partially supported. Falling back to a conservative max value for estimated numTexelsPerMicroTriangle instead."));
      numTexelsPerMicroTriangle.result.resize(numTriangles, OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle());
      numTexelsPerMicroTriangle.status = OmmResult::Success;
      return;
    }


    if (!OpacityMicromapOptions::Building::ConservativeEstimation::enable()) {
      numTexelsPerMicroTriangle.result.resize(numTriangles, 1);
      numTexelsPerMicroTriangle.status = OmmResult::Success;
      return;
    }

    const GeometryBufferData bufferData(geometryData);
    const bool hasNonIdentityTextureTransform = instance.surface.textureTransform != Matrix4();
    const bool usesIndices = geometryData.usesIndices();
    const bool has16bitIndices = usesIndices ? geometryData.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16 : false;
    const uint32_t subdivisionLevel = OpacityMicromapOptions::Building::subdivisionLevel();

    // Retrieve opacity texture's resolution
    const RtxTextureManager& textureManager = m_device->getCommon()->getTextureManager();
    const TextureRef& opacityTexture = textureManager.getTextureTable()[instance.getOmmOpacityTextureIndex()];

    // Opacity texture is not available, this can happen when DLSS is turned off.
    if (!opacityTexture.getImageView()) {
      return;
    }

    const VkExtent3D& opacityTextureExtent = opacityTexture.getImageView()->imageInfo().extent;
    Vector2 opacityTextureResolution(
      static_cast<float>(opacityTextureExtent.width),
      static_cast<float>(opacityTextureExtent.height));

    // Calculate number of texel footprint per micro triangle for all triangles
    {
      const uint32_t kNumIndicesPerTriangle = 3;
      const float rcpNumMicroTrianglesPerEdge = 1.f / (1 << subdivisionLevel);
      const uint32_t kMaxTexelTapsPerMicroTriangle =
        static_cast<uint32_t>(
          std::min<int32_t>(
            OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle(),
            static_cast<int32_t>(UINT16_MAX)));

      // A defined GPU buffer does not guarantee a CPU mapping. Keep GPU OMM
      // baking available by using the existing conservative cost estimate.
      auto conservativeFallback = [&]() {
        numTexelsPerMicroTriangle.result.assign(numTriangles,
          OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle());
        numTexelsPerMicroTriangle.status = OmmResult::Success;
      };
      if (!bufferData.texcoordData || (usesIndices && !bufferData.indexData)) {
        conservativeFallback();
        return;
      }
      const uint64_t indexWidth = has16bitIndices ? 2u : 4u;
      const uint64_t indexStride = geometryData.indexBuffer.stride();
      const uint64_t indexCount = uint64_t(numTriangles) * kNumIndicesPerTriangle;
      const uint64_t uvStride = geometryData.texcoordBuffer.stride();
      const uint64_t uvOffset = geometryData.texcoordBuffer.offsetFromSlice();
      if ((usesIndices && (indexStride < indexWidth
            || (indexCount && (indexCount - 1) * indexStride + indexWidth > geometryData.indexBuffer.length())))
          || uvStride < sizeof(Vector2)) {
        conservativeFallback();
        return;
      }
      
      // Resize the vector to the target size when processing the data for the instance for the first time
      if (numTexelsPerMicroTriangle.numTrianglesCalculated == 0) {
        numTexelsPerMicroTriangle.result.resize(numTriangles);
      }

      // Go over all triangles calculating texel footprint per micro triangle
      // Note: don't issue "break" from the for loop as the logic depends on the for loop's increment statement executing for every iteration
      for (uint32_t& iTriangle = numTexelsPerMicroTriangle.numTrianglesCalculated; 
           iTriangle < numTriangles && m_numTrianglesToCalculateForNumTexelsPerMicroTriangle > 0;
           iTriangle++, m_numTrianglesToCalculateForNumTexelsPerMicroTriangle--) {
        Vector2 texcoords[3];
        uint32_t indexOffset = iTriangle * kNumIndicesPerTriangle;

        // Get triangle's texcoords
        for (uint32_t i = 0; i < kNumIndicesPerTriangle; i++) {
          uint32_t index = i + indexOffset;
          if (usesIndices) {
            // Read the complete 32-bit value, not getIndex32's widened uint16.
            index = 0;
            std::memcpy(&index, reinterpret_cast<const uint8_t*>(bufferData.indexData)
              + uint64_t(i + indexOffset) * indexStride, size_t(indexWidth));
          }
          if (index >= geometryData.vertexCount
              || uvOffset + uint64_t(index) * uvStride + sizeof(Vector2) > geometryData.texcoordBuffer.length()) {
            conservativeFallback();
            return;
          }

          texcoords[i] = bufferData.getTexCoord(index);

          if (hasNonIdentityTextureTransform) {
            texcoords[i] = (instance.surface.textureTransform * Vector4(texcoords[i].x, texcoords[i].y, 0.f, 1.f)).xy();
          }
        }

        uint32_t iNumTexelsPerMicroTriangle = calculateNumTexelsPerMicroTriangle(texcoords, rcpNumMicroTrianglesPerEdge, opacityTextureResolution);

        if (iNumTexelsPerMicroTriangle > kMaxTexelTapsPerMicroTriangle) {
          iNumTexelsPerMicroTriangle = 0;
        }

        numTexelsPerMicroTriangle.result[iTriangle] = static_cast<uint16_t>(iNumTexelsPerMicroTriangle);

        numTexelsPerMicroTriangle.numTrianglesWithinTexelBudget += iNumTexelsPerMicroTriangle != 0;
      }
    }

    // Not all triangles got calculated yet
    if (numTexelsPerMicroTriangle.numTrianglesCalculated != numTexelsPerMicroTriangle.result.size()) {
      return;
    }

    // Check the ratio of how many triangles benefit from OMM triangle arrays
    {
      const float percentageOfTrianglesWithinTexelBudget =
        numTexelsPerMicroTriangle.numTrianglesWithinTexelBudget / static_cast<float>(numTexelsPerMicroTriangle.numTrianglesCalculated);

      if (percentageOfTrianglesWithinTexelBudget >= OpacityMicromapOptions::Building::ConservativeEstimation::minValidOMMTrianglesInMeshPercentage()) {
        numTexelsPerMicroTriangle.status = OmmResult::Success;
      } else {
        ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Opacity Micromap] Instance requires more texel taps to resolve opacity than allowed."));
        numTexelsPerMicroTriangle.status = OmmResult::Rejected;
      }
    }
  }

  void OpacityMicromapManager::calculateNumTexelsPerMicroTriangle(const RtInstance& instance) {
    ScopedCpuProfileZone();

    if (m_numTrianglesToCalculateForNumTexelsPerMicroTriangle == 0) {
      return;
    }

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();
    const uint32_t numTriangles = geometryData.calculatePrimitiveCount();
    const uint32_t numTrianglesModifiedGeometry = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();

    if (numTriangles != numTrianglesModifiedGeometry || numTriangles == 0) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Opacity Micromap] Found unsupported instance type. Input and mofified geometry have different or 0 primitive counts."));
      return;
    }

    // Technically, we could generate OMMs without opacity texture present, but it's not currently supported
    if (instance.getOmmOpacityTextureIndex() == kSurfaceMaterialInvalidTextureIndex) {
      return;
    }

    const XXH64_hash_t ommSrcHash = getOpacityMicromapHash(instance);

    // Create an object to store the result.
    // Ultimately the result should be stored per Omm hash, but if the hash not been calculated yet
    // it is stored in the staging unordered map per instance
    bool hasInsertedNewObject;
    NumTexelsPerMicroTriangleCalculationData* numTexelsPerMicroTriangle;
    if (ommSrcHash != kEmptyHash) {
      // Using piecewise_construct to construct in-place with an empty constructor for the object
      auto elementIter = m_numTexelsPerMicroTriangle.emplace(
        std::piecewise_construct, std::make_tuple(ommSrcHash), std::make_tuple());
      hasInsertedNewObject = elementIter.second;
      numTexelsPerMicroTriangle = &elementIter.first->second;
    } else {
      // Using piecewise_construct to construct in-place with an empty constructor for the object
      auto elementIter = m_numTexelsPerMicroTriangleStaging.emplace(
        std::piecewise_construct, std::make_tuple(&instance), std::make_tuple());
      hasInsertedNewObject = elementIter.second;
      numTexelsPerMicroTriangle = &elementIter.first->second;
      omm_validation_assert(hasInsertedNewObject &&
                            "Invalid state. This should not be scheduled to be calculated for an instance that already has the result.");
    }

    // The result has been already calculated for this instance
    if (numTexelsPerMicroTriangle->status != OmmResult::DependenciesUnavailable) {
      return;
    }

    calculateNumTexelsPerMicroTriangle(*numTexelsPerMicroTriangle, instance, numTriangles);

    // The calculation is complete
    if (numTexelsPerMicroTriangle->status != OmmResult::DependenciesUnavailable) {
      OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
      ommInstanceData.needsToCalculateNumTexelsPerMicroTriangle = false;
    }
  }

  void OpacityMicromapManager::onInstanceDestroyed(const RtInstance& instance) {
    destroyInstance(instance);
  }

  bool OpacityMicromapManager::calculateInstanceUsesOpacityMicromap(const RtInstance& instance) {
    // A separate alpha input alone is exact for non-blended cutouts, whose
    // opacity calculation ignores RGB. Other contracts must not bake an
    // approximation using the opacity texture's RGB as the material colour.
    if (instance.usesSecondaryCutoutOpacity() &&
        (!instance.surface.alphaState.isBlendingDisabled ||
         instance.surface.opacityTextureChannel < 3 ||
         !OpacityMicromapOptions::Building::enableVertexAndTextureOperations())) {
      return false;
    }

    // Texcoord data is required
    if (instance.getTexcoordHash() == kEmptyHash ||
        // Texgen mode check excludes baked terrain as well
        instance.surface.texgenMode != TexGenMode::None) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[RTX Opacity Micromap] Instance does not have compatible texture coordinates. Ignoring the Opacity Micromap request."));
      return false;
    }

    if (instance.testCategoryFlags(InstanceCategories::IgnoreOpacityMicromap) ||
        instance.testCategoryFlags(InstanceCategories::Terrain) ||
        instance.testCategoryFlags(InstanceCategories::IgnoreAlphaChannel)) {
      return false;
    }

    if ((instance.getMaterialType() != MaterialDataType::Opaque &&
         instance.getMaterialType() != MaterialDataType::RayPortal)) {
      return false;
    }

    // Technically, we could generate OMMs without opacity texture present, but it's not currently supported
    // and likely not a commonly useful scenario. This check may already be implicitly covered by 
    // getTexcoordHash() being empty but it's not clear if it's guaranteed.
    if (instance.getOmmOpacityTextureIndex() == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    const RasterGeometry& geometryData = instance.getBlas()->input.getGeometryData();
    const uint32_t numTriangles = geometryData.calculatePrimitiveCount();
    const uint32_t numTrianglesModifiedGeometry = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();

    if (numTriangles != numTrianglesModifiedGeometry || numTriangles == 0 || numTriangles == UINT32_MAX) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Found unsupported instance type. Input and mofified geometry have different or 0 primitive counts. Ignoring the instance."));
      return false;
    }

    bool useOpacityMicromap = false;

    auto& surface = instance.surface;
    auto& alphaState = instance.surface.alphaState;

    // Find valid OMM candidates
    if ((!alphaState.isFullyOpaque && alphaState.isParticle) || alphaState.emissiveBlend) {
      // Alpha-blended and emissive particles
      useOpacityMicromap = true;
    } else if (instance.isOpaque() &&
               !instance.surface.alphaState.isFullyOpaque && 
               instance.surface.alphaState.isBlendingDisabled) {
      // Alpha-tested geometry
      useOpacityMicromap = true;
    } else if (instance.isOpaque() && !alphaState.isFullyOpaque) {
      useOpacityMicromap = true;
    } else if (instance.getMaterialType() == MaterialDataType::RayPortal) {
      useOpacityMicromap = true;
    }

    // Filter by OMM settings
    {
      useOpacityMicromap &= !instance.isAnimated() || OpacityMicromapOptions::BuildRequests::enableAnimatedInstances();
      useOpacityMicromap &= !alphaState.isParticle || OpacityMicromapOptions::BuildRequests::enableParticles();
    }

    // Check if it needs per uTriangle opacity data
    if (useOpacityMicromap) {
      // ToDo: cover all cases to avoid OMM generation unnecessarily
      if (alphaState.alphaTestType == AlphaTestType::kAlways && alphaState.blendType == BlendType::kAlpha) {
        // When alpha comes wholly from the blend constant, the surface's opacity
        // is known up front: skip building a micromap for something that resolves
        // to fully transparent. Alpha sourced from the texture or vertex colour
        // varies per texel, so it still needs the bake.
        if (surface.alphaSource == D3D11ColorSource::BlendConstant) {
          useOpacityMicromap &=
            surface.blendConstant.w > RtxOptions::resolveTransparencyThreshold();
        }
      }
    }

    return useOpacityMicromap;
  }

  void OpacityMicromapManager::onBlasBuild(Rc<DxvkContext> ctx) {
    addBarriersForBuiltOMMs(ctx);
  }

  static bool isIndexOfFullyResidentTexture(uint32_t index, const std::vector<TextureRef>& textures) {
    if (index == BINDING_INDEX_INVALID) {
      return false;
    }
    const TextureRef& tex = textures[index];

    const ManagedTexture* managed = tex.getManagedTexture().ptr();
    if (!managed) {
      return tex.getImageView() != nullptr;
    }

    // TODO: determine many mips are needed for OMM
    constexpr auto REQUIRED_MIP_COUNT_FOR_OMM = 4;
    return managed->hasUploadedMips(REQUIRED_MIP_COUNT_FOR_OMM, false);
  }

  bool OpacityMicromapManager::areInstanceTexturesResident(const RtInstance& instance, const std::vector<TextureRef>& textures) const {
    // Opacity map not loaded yet
    if (!isIndexOfFullyResidentTexture(instance.getOmmOpacityTextureIndex(), textures))
      return false;

    // RayPortal materials use two opacity maps, see if the second one is already loaded
    if (instance.getMaterialType() == MaterialDataType::RayPortal &&
        !isIndexOfFullyResidentTexture(instance.getSecondaryOpacityTextureIndex(), textures))
      return false;

    return true;
  }

  void OpacityMicromapManager::updateSourceHash(RtInstance& instance, XXH64_hash_t ommSrcHash) {
    XXH64_hash_t prevOmmSrcHash = getOpacityMicromapHash(instance);

    if (prevOmmSrcHash != kEmptyHash && ommSrcHash != prevOmmSrcHash) {
      // Valid source hash changed, deassociate instance from the previous hash
      // Note: this will delete non-hash dependent per instance OMM data as well, 
      // which may not be necessary, but we cannot determine that right now
      destroyInstance(instance);
    }

    OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(instance);
    ommInstanceData.ommSrcHash = ommSrcHash;
  }

  fast_unordered_cache<OpacityMicromapManager::CachedSourceData>::iterator OpacityMicromapManager::registerCachedSourceData(const OmmRequest& ommRequest) {

    auto sourceDataIter = m_cachedSourceData.insert({ ommRequest.ommSrcHash, CachedSourceData() }).first;
    CachedSourceData& sourceData = sourceDataIter->second;

    sourceData.initialize(ommRequest, m_instanceOmmRequests, *this);

    if (sourceData.numTriangles == 0) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Input geometry has 0 triangles. Ignoring the build request."));
      // Unlink the instance
      sourceData.setInstance(nullptr, m_instanceOmmRequests, *this);
      m_cachedSourceData.erase(sourceDataIter);
 
      return m_cachedSourceData.end();
    }

    // Reattachment cancels expiration before the processing list resumes it.
    auto metadata = s_ommRetention[this].entries.find(ommRequest.ommSrcHash);
    if (metadata != s_ommRetention[this].entries.end()) {
      s_ommRetention[this].abandoned.erase({metadata->second.detachedSinceMs, ommRequest.ommSrcHash});
      metadata->second.detachedSinceMs = 0;
    }
    return sourceDataIter;
  }

  void OpacityMicromapManager::deleteCachedSourceData(fast_unordered_cache<OpacityMicromapManager::CachedSourceData>::iterator sourceDataIter, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer) {
    if (ommCacheState <= OpacityMicromapCacheState::eStep1_Baking)
      sourceDataIter->second.setInstance(nullptr, m_instanceOmmRequests, *this, destroyParentInstanceOmmRequestContainer);
    m_cachedSourceData.erase(sourceDataIter);
  }

  void OpacityMicromapManager::deleteCachedSourceData(XXH64_hash_t ommSrcHash, OpacityMicromapCacheState ommCacheState, bool destroyParentInstanceOmmRequestContainer) {
    auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
    if (sourceDataIter != m_cachedSourceData.end())
      deleteCachedSourceData(sourceDataIter, ommCacheState, destroyParentInstanceOmmRequestContainer);
  }

  // Returns true if a new OMM build request was accepted
  bool OpacityMicromapManager::addNewOmmBuildRequest(RtInstance& instance, const OmmRequest& ommRequest) {

    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    // Check if the request passes OMM build request filter settings
    {
      // Ignore black listed OMM source hashes
      if (m_blackListedList.find(ommSrcHash) != m_blackListedList.end()) {
        return false;
      }
  
      if (OpacityMicromapOptions::BuildRequests::filtering() && !supportsCompactOpacity(instance)) {
        uint32_t minInstanceFrameAge = OpacityMicromapOptions::BuildRequests::minInstanceFrameAge();
        uint32_t minNumRequests = OpacityMicromapOptions::BuildRequests::minNumRequests();
        uint32_t minNumFramesRequested = OpacityMicromapOptions::BuildRequests::minNumFramesRequested();

        if (usesSplitBillboardOpacityMicromap(instance) && OpacityMicromapOptions::BuildRequests::customFiltersForBillboards()) {
          // Lower the filter requirements for billboards since they are dynamic.
          // But still we want to avoid baking billboards that do not get reused for now
          minInstanceFrameAge = 0;
          minNumRequests = 2;
          minNumFramesRequested = 0;
        }

        if (!ommRequest.isBillboardOmmRequest()) {
          const uint32_t currentFrameIndex = m_device->getCurrentFrameId();

          // Limit new staging identities, not already tracked requests that can
          // now pass the filter and release a slot. Unfiltered/billboard requests
          // do not consume staging slots and must not be blocked by this table.
          if (m_ommBuildRequestStatistics.size() >= OpacityMicromapOptions::BuildRequests::maxRequests() &&
              m_ommBuildRequestStatistics.find(ommSrcHash) == m_ommBuildRequestStatistics.end()) {
            return false;
          }

          OMMBuildRequestStatistics& ommBuildRequestStatistics = m_ommBuildRequestStatistics[ommSrcHash];
          ommBuildRequestStatistics.numTimesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, ommBuildRequestStatistics.numTimesRequested);

          if (currentFrameIndex != ommBuildRequestStatistics.lastRequestFrameId) {
            ommBuildRequestStatistics.lastRequestFrameId = currentFrameIndex;
            ommBuildRequestStatistics.numFramesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, ommBuildRequestStatistics.numFramesRequested);
          }

          if (instance.getFrameAge() < minInstanceFrameAge) {
            return false;
          }

          if (ommBuildRequestStatistics.numTimesRequested < minNumRequests ||
              ommBuildRequestStatistics.numFramesRequested < minNumFramesRequested) {
            return false;
          }
        }
      }
    }

    std::list<XXH64_hash_t>::iterator cacheStateListIter;
    if (!insertToUnprocessedList(ommRequest, cacheStateListIter))
      return false;

    // V772: successful admission ends staging even for filter-exempt compact
    // requests. Failed admission retains its history for the next attempt.
    m_ommBuildRequestStatistics.erase(ommSrcHash);

    // Place the element to the end of the LRU list, and thus marking it as most recent 
    m_leastRecentlyUsedList.emplace_back(ommSrcHash);
    auto lastElementIterator = std::next(m_leastRecentlyUsedList.end(), -1);
    m_ommCache.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(ommSrcHash),
      std::forward_as_tuple(*m_device, OpacityMicromapCacheState::eStep0_Unprocessed, OpacityMicromapOptions::Building::subdivisionLevel(), 
                            OpacityMicromapOptions::Building::enableVertexAndTextureOperations(), m_device->getCurrentFrameId(),
                            lastElementIterator, cacheStateListIter, ommRequest));

    auto& item = m_ommCache.find(ommSrcHash)->second;
    prepareReuseData(instance, item);
    auto& retention = s_ommRetention[this];
    auto& metadata = retention.entries[ommSrcHash];
    metadata.generation = ++retention.generation;
    if (item.reuseData->enabled) {
      const size_t count = item.reuseData->layout.triangles.size();
      auto position = m_unprocessedList.begin();
      for (; position != m_unprocessedList.end(); ++position) {
        if (*position == ommSrcHash) continue;
        const auto other = m_ommCache.find(*position);
        if (other == m_ommCache.end()) continue;
        const auto& data = other->second.reuseData;
        const size_t otherCount = data && data->enabled ? data->layout.triangles.size() : other->second.numTriangles;
        if (count < otherCount || (count == otherCount && ommSrcHash < *position)) break;
      }
      m_unprocessedList.splice(position, m_unprocessedList, item.cacheStateListIter);
    }

    return true;
  }
  
  bool OpacityMicromapManager::insertToUnprocessedList(const OmmRequest& ommRequest, std::list<XXH64_hash_t>::iterator& cacheStateListIter) {
    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    auto sourceDataIter = registerCachedSourceData(ommRequest);

    if (sourceDataIter == m_cachedSourceData.end())
      return false;

    CachedSourceData& sourceData = sourceDataIter->second;

    // Billboard requests go to the end since they are expected to be changed at high frequency and trigger a lot of builds.
    // Therefore, we want to prioritize building ommRequests that passed standard OMM registration filter tests first
    if (!ommRequest.isBillboardOmmRequest()) {
      // Add the OMM request to the unprocessed list according to the numTriangle count in an ascending order 
      // so that requests with least triangles are processed first and thus with lower overall latency
      for (auto itemIter = m_unprocessedList.begin(); itemIter != m_unprocessedList.end(); itemIter++) {

        XXH64_hash_t itemOmmSrcHash = *itemIter;

        CachedSourceData& itemSourceData = m_cachedSourceData[itemOmmSrcHash];

        if (sourceData.numTriangles < itemSourceData.numTriangles ||
            (sourceData.numTriangles == itemSourceData.numTriangles && ommSrcHash < itemOmmSrcHash) ||
            // insert in front of any billboard requests
            usesSplitBillboardOpacityMicromap(*itemSourceData.getInstance())) {
          cacheStateListIter = m_unprocessedList.insert(itemIter, ommSrcHash);
          return true;
        }
      }
    }

    m_unprocessedList.emplace_back(ommSrcHash);
    cacheStateListIter = std::prev(m_unprocessedList.end());

    return true;
  }

  void OpacityMicromapManager::generateInstanceOmmRequests(RtInstance& instance, 
                                                           const InstanceManager& instanceManager, 
                                                           std::vector<OmmRequest>& ommRequests) {

    const bool usesSplitBillboardOMM = usesSplitBillboardOpacityMicromap(instance);
    const uint32_t numOmmRequests = std::max(usesSplitBillboardOMM ? instance.getBillboardCount() : 1u, 1u);
    ommRequests.reserve(numOmmRequests);
    XXH64_hash_t ommSrcHash;  // Compound hash for the instance

    // Create all OmmRequest objects corresponding to the instance
    if (usesSplitBillboardOMM) {
      const uint32_t numTriangles = instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount();
      assert((numTriangles & 1) == 0 &&
             "Only compound omms consisting of multiples of quads are supported");

      std::vector<XXH64_hash_t> ommSrcHashes;
      ommSrcHashes.reserve(numOmmRequests);

      for (uint32_t i = 0; i < instance.getBillboardCount(); i++) {
        OmmRequest ommRequest(instance, instanceManager, i);

        // Only track unique omm requests
        if (std::find(ommSrcHashes.begin(), ommSrcHashes.end(), ommRequest.ommSrcHash) == ommSrcHashes.end()) {
          ommSrcHashes.push_back(ommRequest.ommSrcHash);
          ommRequests.emplace_back(std::move(ommRequest));
        }
      }

      ommSrcHash = XXH3_64bits_withSeed(ommSrcHashes.data(), ommSrcHashes.size() * sizeof(ommSrcHashes[0]), kEmptyHash);

    } else {
      ommRequests.emplace_back(instance, instanceManager);
      ommSrcHash = ommRequests[0].ommSrcHash;
    }

    updateSourceHash(instance, ommSrcHash);
  }

  void OpacityMicromapManager::processOmmCandidates(const InstanceManager& instanceManager,
                                                     const std::vector<TextureRef>& textures) {
    ScopedCpuProfileZone();

    // Nothing to do if budget is zero — no OMMs can be created.
    const bool hasBudget = m_memoryManager.getBudget() != 0;

    const uint32_t currentFrameIndex = m_device->getCurrentFrameId();
    // Per-frame processing budget: limits how many non-deferred candidates are fully processed
    // to spread work across frames and avoid spikes when many deferrals expire at once.
    // When request filtering is disabled, tests expect every eligible request to be submitted
    // deterministically, so do not throttle candidate processing.
    constexpr uint32_t kMaxCandidatesPerFrame = 1024;
    const bool filterBuildRequests = OpacityMicromapOptions::BuildRequests::filtering();
    const uint32_t maxCandidatesPerFrame = filterBuildRequests ? kMaxCandidatesPerFrame : UINT32_MAX;
    uint32_t candidatesProcessedThisFrame = 0;

    // Counters used for staggering deferrals across frames.
    uint32_t numThrottled = 0;
    uint32_t numRegisterFailed = 0;
    std::vector<RtInstance*> retryCandidates;

    auto keepCandidateForRetry = [this](RtInstance* inst) {
      if (inst && !inst->isMarkedForGC()) {
        m_ommCandidates.insert(inst);
      }
    };

    // Walk the instance table instead of the unordered retry set so OMM request
    // registration is deterministic. Stale generation data catches new instances
    // and instances that need re-registration after an OMM cache reset.
    std::vector<RtInstance*> candidates;
    candidates.reserve(instanceManager.getInstanceTable().size());
    for (RtInstance* inst : instanceManager.getInstanceTable()) {
      if (!inst || inst->isMarkedForGC()) {
        if (inst) {
          m_ommCandidates.erase(inst);
        }
        continue;
      }

      // Renderer-created copies inherit their reference instance's OMM binding state,
      // but they do not receive the normal OMM ownership lifecycle.
      if (inst->isCreatedByRenderer()) {
        m_ommCandidates.erase(inst);
        continue;
      }

      const OpacityMicromapInstanceData& ommInstanceData = inst->getOpacityMicromapInstanceData();
      if (m_ommCandidates.find(inst) != m_ommCandidates.end() ||
          ommInstanceData.ommEligibilityGeneration != m_ommGeneration) {
        candidates.push_back(inst);
      }
    }

    for (RtInstance* inst : candidates) {

      // Remove GC'd instances
      if (inst->isMarkedForGC()) {
        m_ommCandidates.erase(inst);
        continue;
      }

      // Skip instances whose BLAS hasn't been assigned yet.
      // onInstanceAdded fires during draw call submission, before the BLAS loop
      // assigns geometry.  The instance will be processed on a subsequent frame
      // once its BLAS is set.
      if (!inst->getBlas()) {
        keepCandidateForRetry(inst);
        continue;
      }

      // Skip brand-new instances (frameAge == 0).  processOmmCandidates runs
      // BEFORE the BLAS build loop that calls fillGeometryInfoFromBlasEntry,
      // so geometry data (modifiedGeometryData, buildGeometries) isn't ready
      // yet on the instance's first frame.  Deferring to the next frame
      // ensures the OMM is built against fully initialised geometry.
      if (inst->getFrameAge() == 0) {
        keepCandidateForRetry(inst);
        continue;
      }

      OpacityMicromapInstanceData& ommInstanceData = getOmmInstanceData(*inst);

      // Already registered in the current generation — done.
      if (ommInstanceData.ommBuildRequested &&
          ommInstanceData.ommRegistrationGeneration == m_ommGeneration) {
        m_ommCandidates.erase(inst);
        continue;
      }
      ommInstanceData.ommBuildRequested = false;

      // Deferred — skip until retry frame is reached.
      if (currentFrameIndex < ommInstanceData.ommRetryAfterFrame) {
        keepCandidateForRetry(inst);
        continue;
      }

      // Per-frame budget exceeded — stagger deferral to spread across future frames.
      if (candidatesProcessedThisFrame >= maxCandidatesPerFrame) {
        // Spread excess candidates across the next 32 frames to avoid future spikes.
        ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numThrottled % 32);
        ++numThrottled;
        keepCandidateForRetry(inst);
        continue;
      }
      ++candidatesProcessedThisFrame;

      // No budget — keep candidate for retry when budget becomes available.
      if (!hasBudget) {
        keepCandidateForRetry(inst);
        continue;
      }

      // Eligibility check — only runs once per instance per generation.
      // Permanently ineligible instances are removed from the candidate set.
      // The generation check ensures re-evaluation when OMM options change (clear()/generation bump).
      if (ommInstanceData.ommEligibilityGeneration != m_ommGeneration) {
        ommInstanceData.ommEligibilityGeneration = m_ommGeneration;
        ommInstanceData.usesOMM = calculateInstanceUsesOpacityMicromap(*inst);
        if (!ommInstanceData.usesOMM || inst->isViewModelNonReference()) {
          m_ommCandidates.erase(inst);
          continue;
        }
      }

      // Textures not resident yet — defer.
      if (!areInstanceTexturesResident(*inst, textures)) {
        keepCandidateForRetry(inst);
        continue;
      }

      // --- Fast pre-check for non-billboard instances with a known hash ---
      // Avoids expensive generateInstanceOmmRequests / OmmRequest construction
      // when the filter stats haven't been met yet.
      const XXH64_hash_t cachedHash = ommInstanceData.ommSrcHash;
      if (cachedHash != kEmptyHash && !usesSplitBillboardOpacityMicromap(*inst)) {
        // Already in the OMM cache — full registration will succeed, proceed to full path.
        if (m_ommCache.find(cachedHash) != m_ommCache.end()) {
          // Fall through to full registration below.
        }
        // Blacklisted — permanently remove.
        else if (m_blackListedList.find(cachedHash) != m_blackListedList.end()) {
          m_ommCandidates.erase(inst);
          continue;
        }
        // Pre-check the filter stats without constructing OmmRequest.
        else if (filterBuildRequests && !supportsCompactOpacity(*inst)) {
          const uint32_t minInstanceFrameAge = OpacityMicromapOptions::BuildRequests::minInstanceFrameAge();
          const uint32_t minNumRequests = OpacityMicromapOptions::BuildRequests::minNumRequests();
          const uint32_t minNumFramesRequested = OpacityMicromapOptions::BuildRequests::minNumFramesRequested();

          // Check capacity — if stats map is full and this hash isn't tracked yet, defer.
          if (m_ommBuildRequestStatistics.size() >= OpacityMicromapOptions::BuildRequests::maxRequests() &&
              m_ommBuildRequestStatistics.find(cachedHash) == m_ommBuildRequestStatistics.end()) {
            ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numRegisterFailed % 32);
            ++numRegisterFailed;
            keepCandidateForRetry(inst);
            continue;
          }

          // Accumulate stats cheaply.
          OMMBuildRequestStatistics& stats = m_ommBuildRequestStatistics[cachedHash];
          stats.numTimesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, stats.numTimesRequested);
          if (currentFrameIndex != stats.lastRequestFrameId) {
            stats.lastRequestFrameId = currentFrameIndex;
            stats.numFramesRequested = 1 + std::min<uint16_t>(UINT16_MAX - 1, stats.numFramesRequested);
          }

          // Check thresholds — defer if not met.
          if (inst->getFrameAge() < minInstanceFrameAge ||
              stats.numTimesRequested < minNumRequests ||
              stats.numFramesRequested < minNumFramesRequested) {
            ++numRegisterFailed;
            keepCandidateForRetry(inst);
            continue;
          }
          // Filter passed — fall through to full registration which will erase stats and build.
        }
      }

      // --- Attempt OMM registration ---
      InstanceOmmRequests ommRequests;
      generateInstanceOmmRequests(*inst, instanceManager, ommRequests.ommRequests);

      m_instanceOmmRequests.emplace(getOpacityMicromapHash(*inst), ommRequests);

      bool allRegistersSucceeded = true;
      for (auto& ommRequest : ommRequests.ommRequests) {
        allRegistersSucceeded &= registerOmmRequestInternal(*inst, ommRequest);
      }

      // Purge bookkeeping if no active requests resulted
      auto instanceOmmRequestsIter = m_instanceOmmRequests.find(getOpacityMicromapHash(*inst));
      if (instanceOmmRequestsIter->second.numActiveRequests == 0) {
        m_instanceOmmRequests.erase(instanceOmmRequestsIter);
      }

      if (allRegistersSucceeded) {
        ommInstanceData.ommBuildRequested = true;
        ommInstanceData.ommRegistrationGeneration = m_ommGeneration;
        m_ommCandidates.erase(inst);
      } else {
        // Defer retry to avoid re-doing expensive generateInstanceOmmRequests work every frame
        // for instances that can't complete registration (e.g., maxRequests cap reached).
        // Stagger deferrals to spread retries across frames.
        ommInstanceData.ommRetryAfterFrame = currentFrameIndex + 1 + (numRegisterFailed % 32);
        if (!inst->isMarkedForGC() &&
            m_ommCandidates.find(inst) == m_ommCandidates.end()) {
          retryCandidates.push_back(inst);
        }
        ++numRegisterFailed;
      }
    }

    for (RtInstance* inst : retryCandidates) {
      if (!inst->isMarkedForGC()) {
        m_ommCandidates.insert(inst);
      }
    }
  }

  bool OpacityMicromapManager::registerOmmRequestInternal(RtInstance& instance, const OmmRequest& ommRequest) {

    XXH64_hash_t ommSrcHash = ommRequest.ommSrcHash;

    if (ommSrcHash == kEmptyHash) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Build source instance has an invalid hash. Ignoring the build request."));
      return false;
    }

    auto ommCacheIterator = m_ommCache.find(ommSrcHash);

    // OMM request is not yet known
    if (ommCacheIterator == m_ommCache.end()) {
      return addNewOmmBuildRequest(instance, ommRequest);
    } else {

      auto& ommCacheItem = ommCacheIterator->second;

      // Check OMM request's parametrization matches that of the cached omm data
      // in case of an OMM hash collision
      if (!ommCacheItem.isCompatibleWithOmmRequest(ommRequest)) {
        ONCE(Logger::warn("[RTX Opacity Micromap] Found a cached Opacity Micromap with same hash but with incompatible parametrization. Black listing the Opacity Micromap hash."));
        m_blackListedList.insert(ommSrcHash);
        destroyOmmData(ommSrcHash);
        return false;
      }

      if (ommCacheItem.cacheState == OpacityMicromapCacheState::eStep1_Baking) {
        auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);

        // Source data has been unlinked and removed from unprocessed list, try adding it back to the unprocessed list
        if (sourceDataIter == m_cachedSourceData.end()) {
          ommCacheItem.isUnprocessedCacheStateListIterValid = insertToUnprocessedList(ommRequest, ommCacheItem.cacheStateListIter);
          if (ommCacheItem.isUnprocessedCacheStateListIterValid) m_ommBuildRequestStatistics.erase(ommSrcHash);
          return ommCacheItem.isUnprocessedCacheStateListIterValid;
        }
      }
    }

    m_ommBuildRequestStatistics.erase(ommSrcHash);
    return true;
  }

  OpacityMicromapBinding OpacityMicromapManager::getBlasBinding(XXH64_hash_t sourceHash) const {
    const auto item = m_ommCache.find(sourceHash);
    if (sourceHash == kEmptyHash || item == m_ommCache.end())
      return {};
    return { sourceHash, item->second.blasOmmBuffers };
  }

  bool OpacityMicromapManager::hasNewlyReadyOmm(const RtInstance& instance, const InstanceManager& instanceManager) const {
    if (m_newlyReadySources.empty() || !usesOpacityMicromap(instance)) return false;
    if (!usesSplitBillboardOpacityMicromap(instance)) {
      const bool ready = m_newlyReadySources.count(getOpacityMicromapHash(instance)) != 0;
      if (ready) ++s_ommReuseStats.targetedBuckets;
      return ready;
    }
    for (uint32_t i = 0; i < instance.getBillboardCount(); ++i)
      if (m_newlyReadySources.count(OmmRequest(instance, instanceManager, i).ommSrcHash)) {
        ++s_ommReuseStats.targetedBuckets;
        return true;
      }
    return false;
  }

  void OpacityMicromapManager::trackCachedBlasUse(Rc<DxvkContext> ctx,
      const std::vector<OpacityMicromapBinding>& bindings) {
    const uint32_t frame = m_device->getCurrentFrameId();
    for (const auto& binding : bindings) {
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(binding.resource);
      auto item = m_ommCache.find(binding.sourceHash);
      // A reset may have produced a new resource under the same content hash.
      if (item != m_ommCache.end() && item->second.blasOmmBuffers.ptr() == binding.resource.ptr()
          && item->second.lastUseFrameIndex != frame) {
        item->second.lastUseFrameIndex = frame;
        m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList,
          item->second.leastRecentlyUsedListIter);
      }
    }
  }

  XXH64_hash_t OpacityMicromapManager::tryBindOpacityMicromap(Rc<DxvkContext> ctx,
                                                              const RtInstance& instance, uint32_t billboardIndex,
                                                              VkAccelerationStructureGeometryKHR& targetGeometry,
                                                              const InstanceManager& instanceManager) {
    ScopedCpuProfileZone();

    // Always clear any stale OMM binding from cached geometry data.
    // If binding succeeds below, pNext will be set to the new OMM descriptor.
    targetGeometry.geometry.triangles.pNext = nullptr;
    
    // Skip trying to bind an OMM if the budget is 0 since no OMMs can exist
    if (m_memoryManager.getBudget() == 0) {
      return kEmptyHash;
    }

    if (!usesOpacityMicromap(instance)) {
      return kEmptyHash;
    }

    return bindOpacityMicromap(ctx, instance, billboardIndex, targetGeometry, instanceManager);
  }
  
  XXH64_hash_t OpacityMicromapManager::bindOpacityMicromap(Rc<DxvkContext> ctx,
                                                           const RtInstance& instance, 
                                                           uint32_t billboardIndex,
                                                           VkAccelerationStructureGeometryKHR& targetGeometry,
                                                           const InstanceManager& instanceManager) {
    m_numRequestedOMMBindings++;

    if (!OpacityMicromapOptions::enableBinding()) {
      return kEmptyHash;
    }

    // ToDo: avoid fixing up the index here
    billboardIndex =
      usesSplitBillboardOpacityMicromap(instance) ? billboardIndex : OmmRequest::kInvalidIndex;
    const OmmRequest ommRequest(instance, instanceManager, billboardIndex);

    auto ommCacheItemIter = m_ommCache.find(ommRequest.ommSrcHash);

    // OMM is not available in the cache
    if (ommCacheItemIter == m_ommCache.end()) {
      // V773: a surviving instance may still remember successful registration
      // after its unused map was evicted. Requeue only when binding is requested
      // again; doing this during eviction would immediately refill cold entries.
      if (!instance.isCreatedByRenderer() && m_blackListedList.find(ommRequest.ommSrcHash) == m_blackListedList.end()) {
        auto& data = getOmmInstanceData(instance);
        data.ommBuildRequested = false;
        data.ommRetryAfterFrame = 0;
        m_ommCandidates.insert(const_cast<RtInstance*>(&instance));
      }
      return kEmptyHash;
    }

    bool boundOMM = false;
    OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;
    const OpacityMicromapCacheState ommCacheState = ommCacheItem.cacheState;

    // Check OMM request's parametrization matches that of the cached omm data
    // in case of an OMM hash collision
    if (!ommCacheItem.isCompatibleWithOmmRequest(ommRequest)) {
      ONCE(Logger::warn("[RTX Opacity Micromap] Found a cached Opacity Microamp with a matching hash but with an incompatible parametrization. Discarding Opacity Micromap binding request."));
      return kEmptyHash;
    }

    ommCacheItem.lastUseFrameIndex = m_device->getCurrentFrameId();

    // Make the item most recently used
    m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList, ommCacheItem.leastRecentlyUsedListIter);

    // Bind OMM if the data is ready
    switch (ommCacheState) {
    case OpacityMicromapCacheState::eStep0_Unprocessed:
    case OpacityMicromapCacheState::eStep1_Baking:
    case OpacityMicromapCacheState::eStep2_Baked:
      // OMM data is not yet ready
      break;

    case OpacityMicromapCacheState::eStep3_Built:
    case OpacityMicromapCacheState::eStep4_Ready:
    {
      targetGeometry.geometry.triangles.pNext = &ommCacheItem.blasOmmBuffers->blasDesc;
      boundOMM = true;
      m_numBoundOMMs++;
      ++s_ommReuseStats.readyBindings;

      // Track the lifetime of the used buffers
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(ommCacheItem.blasOmmBuffers);
      m_boundOMMs.push_back(ommCacheItem.blasOmmBuffers);
      break;
    }
    case OpacityMicromapCacheState::eUnknown:
      assert(false && "eUnknown OpacityMicromapCacheState in OpacityMicromapManager::bindOpacityMicromap");
    }

    if (ommCacheState == OpacityMicromapCacheState::eStep3_Built)
      m_boundOmmsRequireSynchronization = true;

    return boundOMM ? ommRequest.ommSrcHash : kEmptyHash;
  }

  void OpacityMicromapManager::addBarriersForBuiltOMMs(Rc<DxvkContext> ctx) {

    if (m_boundOmmsRequireSynchronization) {

      // Add a barrier blocking on OMM builds
      {
        VkMemoryBarrier2 memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL,
          VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
          VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
          VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_SHADER_READ_BIT };
        VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &memoryBarrier;

        ctx->getCommandList()->vkCmdPipelineBarrier2KHR(&dependencyInfo);
      }

      // All built instances have been synchronized, remove them from the built list
      {
        for (auto& ommSrcHash : m_builtList) {
          m_ommCache[ommSrcHash].cacheState = OpacityMicromapCacheState::eStep4_Ready;
        }
        m_builtList.clear();
      }

      m_boundOmmsRequireSynchronization = false;
    }
  }

  template <typename IndexType>
  void calculateMicromapTriangleArrayBufferSizes(uint32_t numAllocatedTriangles,
                                                 uint32_t& triangleArrayBufferSize,
                                                 uint32_t& triangleIndexBufferSize) {
    triangleArrayBufferSize = numAllocatedTriangles * sizeof(VkMicromapTriangleEXT);
    triangleIndexBufferSize = numAllocatedTriangles * sizeof(IndexType);
  };

  template <typename IndexType>
  OpacityMicromapManager::OmmResult initializeOpacityMicromapTriangleArrayBuffers(
    DxvkDevice* device,
    Rc<DxvkContext> ctx,
    VkOpacityMicromapFormatEXT ommFormat,
    uint16_t subdivisionLevel,
    uint32_t numTriangles,
    uint32_t opacityMicromapPerTriangleBufferSize,
    Rc<DxvkBuffer>& triangleArrayBuffer,
    Rc<DxvkBuffer>& triangleIndexBuffer,
    const std::vector<uint32_t>* remapping = nullptr,
    bool buildArray = true) {

    uint32_t triangleArrayBufferSize;
    uint32_t triangleIndexBufferSize;
    calculateMicromapTriangleArrayBufferSizes<IndexType>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);
    const uint32_t numGeometryTriangles = remapping ? uint32_t(remapping->size()) : numTriangles;
    triangleIndexBufferSize = numGeometryTriangles * sizeof(IndexType);

    // Create buffers
    {
      DxvkBufferCreateInfo ommBufferInfo;
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | 
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      ommBufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommBufferInfo.size = triangleArrayBufferSize;
      if (buildArray)
        triangleArrayBuffer = device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM triangle array buffer");
      
      if (buildArray && triangleArrayBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate triangle buffers due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OpacityMicromapManager::OmmResult::OutOfMemory;
      }

      ommBufferInfo.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      
      ommBufferInfo.size = triangleIndexBufferSize;
      triangleIndexBuffer = device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM triangle index buffer");

      if (triangleIndexBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate triangle buffers due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OpacityMicromapManager::OmmResult::OutOfMemory;
      }
    }

    // Micromap triangle buffer desc
    VkMicromapTriangleEXT micromapTriangleDescTemplate;
    micromapTriangleDescTemplate.dataOffset = 0;           // Offset in opacityMicromapBuffer
    micromapTriangleDescTemplate.format = ommFormat;
    micromapTriangleDescTemplate.subdivisionLevel = subdivisionLevel;

    std::vector<VkMicromapTriangleEXT> hostTriangleArrayBuffer(buildArray ? numTriangles : 0);
    std::vector<IndexType> hostTriangleIndexBuffer(numGeometryTriangles);
    for (uint32_t i = 0; buildArray && i < numTriangles; i++) {
      hostTriangleArrayBuffer[i] = micromapTriangleDescTemplate;
      hostTriangleArrayBuffer[i].dataOffset = i * opacityMicromapPerTriangleBufferSize;
    }
    for (uint32_t i = 0; i < numGeometryTriangles; ++i)
      hostTriangleIndexBuffer[i] = IndexType(remapping ? (*remapping)[i] : i);

    if (buildArray)
      ctx->writeToBuffer(triangleArrayBuffer, 0, triangleArrayBufferSize, hostTriangleArrayBuffer.data());
    ctx->writeToBuffer(triangleIndexBuffer, 0, triangleIndexBufferSize, hostTriangleIndexBuffer.data());

    return OpacityMicromapManager::OmmResult::Success;
  }

  void OpacityMicromapManager::calculateMicromapBuildInfo(
    VkMicromapUsageEXT& ommUsageGroup,
    VkMicromapBuildInfoEXT& ommBuildInfo,
    VkMicromapBuildSizesInfoEXT& sizeInfo) {
    ommBuildInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
    sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };

    // Get prebuild info
    ommBuildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    ommBuildInfo.flags = 0;
    ommBuildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    ommBuildInfo.dstMicromap = VK_NULL_HANDLE;
    ommBuildInfo.usageCountsCount = 1;
    ommBuildInfo.pUsageCounts = &ommUsageGroup;
    ommBuildInfo.data.deviceAddress = 0ull;
    ommBuildInfo.triangleArray.deviceAddress = 0ull;
    ommBuildInfo.triangleArrayStride = 0;

    m_device->vkd()->vkGetMicromapBuildSizesEXT(m_device->vkd()->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ommBuildInfo, &sizeInfo);
  }

  void OpacityMicromapManager::calculateRequiredVRamSize(
    uint32_t numTriangles,
    uint32_t numGeometryTriangles,
    uint16_t subdivisionLevel,
    VkOpacityMicromapFormatEXT ommFormat,
    VkIndexType triangleIndexType,
    VkDeviceSize& arrayBufferDeviceSize,
    VkDeviceSize& blasOmmBuffersDeviceSize) {
    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(subdivisionLevel);
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;
    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;

    // Account for any alignments at start and the end of buffers
    arrayBufferDeviceSize = opacityMicromapBufferSize + 2 * kBufferAlignment;

    // Fill out VkMicromapUsageEXT with size information
    // For now all triangles are in the same micromap group
    VkMicromapUsageEXT ommUsageGroup = {};
    ommUsageGroup.count = numTriangles;
    ommUsageGroup.subdivisionLevel = subdivisionLevel;
    ommUsageGroup.format = ommFormat;

    // Get micromap prebuild info
    VkMicromapBuildInfoEXT ommBuildInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT };
    VkMicromapBuildSizesInfoEXT sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
    calculateMicromapBuildInfo(ommUsageGroup, ommBuildInfo, sizeInfo);

    uint32_t triangleArrayBufferSize;
    uint32_t triangleIndexBufferSize;
    if (triangleIndexType == VK_INDEX_TYPE_UINT16)
      calculateMicromapTriangleArrayBufferSizes<uint16_t>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);
    else
      calculateMicromapTriangleArrayBufferSizes<uint32_t>(numTriangles, triangleArrayBufferSize, triangleIndexBufferSize);

    triangleIndexBufferSize = numGeometryTriangles * (triangleIndexType == VK_INDEX_TYPE_UINT16 ? 2u : 4u);

    // Account for any alignments at start and the end of buffers
    // Build descriptors are command-owned scratch, not retained BLAS storage.
    // Release their reservation with the input array after submitting the build.
    arrayBufferDeviceSize += triangleArrayBufferSize + 2 * kBufferAlignment;
    blasOmmBuffersDeviceSize =
      triangleIndexBufferSize + 2 * kBufferInBlasUsageAlignment +
      sizeInfo.micromapSize + 2 * kBufferInBlasUsageAlignment;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::getNumTexelsPerMicroTriangle(
    const RtInstance& instance,
    NumTexelsPerMicroTriangle** numTexelsPerMicroTriangle) {

    // Note: this is not expected to be called for non-reference instances which
    // goes along the design choice of non-reference OMM instances not being used for generating OMMs
    omm_validation_assert(!instance.isViewModelNonReference());

    NumTexelsPerMicroTriangleCalculationData* numTexelsPerMicroTriangleCalculationData;

    // Look up the object holding the data
    if (useStagingNumTexelsPerMicroTriangleObject(instance)) {
      auto numTexelsPerMicroTriangleStagingIter = m_numTexelsPerMicroTriangleStaging.find(&instance);

      if (numTexelsPerMicroTriangleStagingIter == m_numTexelsPerMicroTriangleStaging.end()) {
        return OmmResult::DependenciesUnavailable;
      }

      numTexelsPerMicroTriangleCalculationData = &numTexelsPerMicroTriangleStagingIter->second;
    } else {
      auto numTexelsPerMicroTriangleIter = m_numTexelsPerMicroTriangle.find(getOpacityMicromapHash(instance));

      if (numTexelsPerMicroTriangleIter == m_numTexelsPerMicroTriangle.end()) {
        return OmmResult::DependenciesUnavailable;
      }

      numTexelsPerMicroTriangleCalculationData = &numTexelsPerMicroTriangleIter->second;
    }

    *numTexelsPerMicroTriangle = &numTexelsPerMicroTriangleCalculationData->result;
    return numTexelsPerMicroTriangleCalculationData->status;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::shareOpacityMicromap(
      Rc<DxvkContext> ctx, OpacityMicromapCacheItem& item,
      OpacityMicromapCacheItem& owner, VkDeviceSize& availableUploadBytes) {
    auto& reuse = *item.reuseData;
    const auto& resource = owner.blasOmmBuffers;
    const bool shortIndices = reuse.layout.triangles.size() <= UINT16_MAX - 3;
    const VkDeviceSize bytes = reuse.layout.indices.size() * (shortIndices ? 2u : 4u);
    if (!availableUploadBytes || bytes > availableUploadBytes) {
      ++s_ommReuseStats.uploadWaits;
      return OmmResult::OutOfBudget;
    }
    const VkDeviceSize charge = bytes + 2 * kBufferInBlasUsageAlignment;
    if (!m_memoryManager.allocate(charge)) {
      m_amountOfMemoryMissing = ommPendingAllocation(m_amountOfMemoryMissing, charge, m_memoryManager.getBudget());
      return OmmResult::OutOfMemory;
    }
    Rc<DxvkOpacityMicromap> binding = new DxvkOpacityMicromap(*m_device);
    Rc<DxvkBuffer> unusedTriangleArray;
    const uint32_t count = uint32_t(reuse.layout.triangles.size());
    const OmmResult result = shortIndices
      ? initializeOpacityMicromapTriangleArrayBuffers<uint16_t>(m_device, ctx, item.ommFormat, item.subdivisionLevel,
          count, 0, unusedTriangleArray, binding->opacityMicromapTriangleIndexBuffer, &reuse.layout.indices, false)
      : initializeOpacityMicromapTriangleArrayBuffers<uint32_t>(m_device, ctx, item.ommFormat, item.subdivisionLevel,
          count, 0, unusedTriangleArray, binding->opacityMicromapTriangleIndexBuffer, &reuse.layout.indices, false);
    if (result != OmmResult::Success) {
      m_memoryManager.release(charge);
      return result;
    }
    binding->sharedMicromapOwner = resource;
    binding->opacityMicromap = resource->opacityMicromap;
    binding->blasDesc = resource->blasDesc;
    binding->blasDesc.indexType = shortIndices ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    binding->blasDesc.indexBuffer.deviceAddress = binding->opacityMicromapTriangleIndexBuffer->getDeviceAddress();
    binding->blasDesc.indexStride = shortIndices ? 2 : 4;
    binding->blasUsage = { item.numTriangles, item.subdivisionLevel, uint32_t(item.ommFormat) };
    binding->blasDesc.pUsageCounts = &binding->blasUsage;
    item.blasOmmBuffers = binding;
    item.blasOmmBuffersDeviceSize = charge;
    item.bakingState.initialized = true;
    item.bakingState.numMicroTrianglesToBake = item.bakingState.numMicroTrianglesBaked =
      count * calculateNumMicroTriangles(item.subdivisionLevel);
    // V748 tracks this binding through commands/BLASes; its strong owner keeps
    // the shared VkMicromap alive and charged even after the source is evicted.
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(binding);
    availableUploadBytes -= bytes;
    ++s_ommReuseStats.gpuShares;
    s_ommReuseStats.uploadBytes += bytes;
    owner.lastUseFrameIndex = m_device->getCurrentFrameId();
    m_leastRecentlyUsedList.splice(m_leastRecentlyUsedList.end(), m_leastRecentlyUsedList, owner.leastRecentlyUsedListIter);
    return OmmResult::Success;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::bakeOpacityMicromapArray(
    Rc<DxvkContext> ctx,
    XXH64_hash_t ommSrcHash,
    OpacityMicromapCacheItem& ommCacheItem,
    CachedSourceData& sourceData,
    const std::vector<TextureRef>& textures,
    uint32_t& availableBakingBudget,
    VkDeviceSize& availableUploadBytes, bool cacheOnly) {
    
    // Guard against null instance — can happen if the instance was destroyed
    // between registration and baking, or if the source data was invalidated.
    if (!sourceData.getInstance()) {
      return OmmResult::Failure;
    }

    const RtInstance& instance = *sourceData.getInstance();

    // Guard against null BLAS — instance may have been partially torn down.
    if (!instance.getBlas()) {
      return OmmResult::Failure;
    }

    if (!areInstanceTexturesResident(instance, textures)) {
      ++s_ommReuseStats.textureWaits;
      return OmmResult::DependenciesUnavailable;
    }

    prepareReuseData(instance, ommCacheItem);
    auto& reuse = *ommCacheItem.reuseData;
    if (cacheOnly && (!reuse.enabled || ommCacheItem.bakingState.initialized))
      return OmmResult::OutOfBudget;
    BlasEntry& blasEntry = *instance.getBlas();

    const uint32_t numTriangles = reuse.enabled ? uint32_t(reuse.layout.triangles.size()) : sourceData.numTriangles;
    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
    if (uint64_t(numTriangles) * numMicroTrianglesPerTriangle > UINT32_MAX) return OmmResult::Failure;
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;
    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;

    omm_validation_assert((usesSplitBillboardOpacityMicromap(instance) || sourceData.numTriangles == instance.getBlas()->input.getGeometryData().calculatePrimitiveCount()) &&
                          instance.getBlas()->input.getGeometryData().calculatePrimitiveCount() ==
                          instance.getBlas()->modifiedGeometryData.calculatePrimitiveCount() &&
                          "Number of triangles must match and be consistent");

    if (reuse.enabled && !reuse.cacheKeyReady) {
      const auto& texture = textures[instance.getOmmOpacityTextureIndex()];
      const auto& sampler = ctx->getCommonObjects()->getSceneManager().getSamplerTable()[instance.getSamplerIndex()]->info();
      XXH64_hash_t textureHash = 0;
      const auto fingerprintResult = getTextureFingerprint(ctx, texture, textureHash);
      if (fingerprintResult == OmmResult::DependenciesUnavailable || fingerprintResult == OmmResult::OutOfMemory) {
        ++s_ommReuseStats.fingerprintWaits;
        return fingerprintResult;
      }
      if (textureHash != kEmptyHash) {
        std::vector<uint8_t> keyData;
        auto add = [&](const auto& value) {
          const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
          keyData.insert(keyData.end(), bytes, bytes + sizeof(value));
        };
        // Version the baking algorithm independently of the file container.
        // No process-local handles, instance IDs, positions or batch sizes.
        add(uint32_t(74901)); add(ommcache::kVersion);
        add(textureHash);
        const auto materialHash = instance.getMaterialHash();
        add(materialHash);
        add(ommCacheItem.subdivisionLevel); add(ommCacheItem.ommFormat);
        add(ommCacheItem.useVertexAndTextureOperations);
        const auto& surface = instance.surface;
        add(surface.alphaState.isFullyOpaque); add(surface.alphaState.isBlendingDisabled);
        add(surface.alphaState.alphaTestType); add(surface.alphaState.alphaTestReferenceValue);
        add(surface.alphaState.blendType); add(surface.alphaState.invertedBlend); add(surface.alphaState.emissiveBlend);
        add(surface.alphaSource); add(surface.opacityTextureChannel); add(surface.blendConstant.w);
        for (uint32_t column = 0; column < 4; ++column) {
          add(surface.textureTransform.data[column].x); add(surface.textureTransform.data[column].y);
        }
        add(RtxOptions::resolveTransparencyThreshold()); add(RtxOptions::resolveOpaquenessThreshold());
        add(OpacityMicromapOptions::Building::ConservativeEstimation::enable());
        add(OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle());
        const auto& view = texture.getImageView()->info();
        const auto& extent = texture.getImageView()->imageInfo().extent;
        add(view.type); add(view.format); add(view.minLevel); add(view.numLevels); add(view.minLayer); add(view.numLayers);
        add(view.swizzle.r); add(view.swizzle.g); add(view.swizzle.b); add(view.swizzle.a);
        add(extent.width); add(extent.height); add(extent.depth);
        add(sampler.addressModeU); add(sampler.addressModeV); add(sampler.addressModeW); add(sampler.borderColor);
        for (const auto& triangle : reuse.layout.triangles) add(triangle);
        const auto hash = XXH3_128bits(keyData.data(), keyData.size());
        reuse.key = { hash.low64, hash.high64 };
        reuse.cacheKeyReady = true;
      }
    }
    if (reuse.cacheKeyReady && !ommCacheItem.bakingState.initialized && ommCacheItem.getDeviceSize() == 0) {
      ommCacheItem.sharedKey = reuse.key;
      ommCacheItem.hasSharedKey = true;
      auto shared = m_sharedOmmSources.find(reuse.key);
      if (shared != m_sharedOmmSources.end() && shared->second != ommSrcHash) {
        auto owner = m_ommCache.find(shared->second);
        if (owner != m_ommCache.end()) {
          if (owner->second.cacheState >= OpacityMicromapCacheState::eStep3_Built &&
              owner->second.cacheState <= OpacityMicromapCacheState::eStep4_Ready)
            return shareOpacityMicromap(ctx, ommCacheItem, owner->second, availableUploadBytes);
          const auto source = m_cachedSourceData.find(shared->second);
          if (owner->second.cacheState == OpacityMicromapCacheState::eStep2_Baked ||
              (source != m_cachedSourceData.end() && source->second.getInstance())) {
            ++s_ommReuseStats.sharedWaits;
            return OmmResult::DependenciesUnavailable;
          }
        }
      }
      // The first live job for an exact portable key is the producer. Others
      // wait for it instead of reserving/baking/uploading duplicate GPU arrays.
      m_sharedOmmSources[reuse.key] = ommSrcHash;
    }
    if (reuse.cacheKeyReady && !reuse.cacheRequested) {
      if (opacityMicromapBufferSize <= ommcache::kMaxBytes) {
        reuse.record = portableOmmStore().request(reuse.key, opacityMicromapBufferSize);
        // A full I/O queue is not a disk miss. Retry next frame rather than
        // starting another bake of a file which may already be on disk.
        if (!reuse.record) {
          ++s_ommReuseStats.ioWaits;
          return OmmResult::DependenciesUnavailable;
        }
      }
      reuse.cacheRequested = true;
    }
    if (reuse.record && !ommCacheItem.bakingState.initialized &&
        reuse.record->state.load(std::memory_order_acquire) == ommcache::State::Pending) {
      ++s_ommReuseStats.ioWaits;
      return OmmResult::DependenciesUnavailable;
    }
    const bool cacheHit = reuse.record && !ommCacheItem.bakingState.initialized &&
      reuse.record->state.load(std::memory_order_acquire) == ommcache::State::Hit &&
      reuse.record->bytes.size() == opacityMicromapBufferSize;
    if (cacheHit) s_ommRetention[this].entries[ommSrcHash].diskBacked = true;
    if (cacheOnly && !cacheHit) return OmmResult::OutOfBudget;
    // Allow one oversized cached array (files are capped at 64 MiB) so a
    // record larger than the ordinary 8 MiB upload allowance cannot starve.
    constexpr VkDeviceSize uploadAllowance = 8ull * 1024 * 1024;
    if (cacheHit && (availableUploadBytes == 0 ||
        (opacityMicromapBufferSize > availableUploadBytes && availableUploadBytes != uploadAllowance))) {
      ++s_ommReuseStats.uploadWaits;
      return OmmResult::OutOfBudget;
    }

    // Estimate sampling cost only for an actual cold bake, never a disk/GPU hit.
    if (!cacheHit && reuse.enabled && reuse.texelCosts.empty()) {
      const auto extent = textures[instance.getOmmOpacityTextureIndex()].getImageView()->imageInfo().extent;
      const Vector2 resolution(float(extent.width), float(extent.height));
      const float rcpEdge = 1.f / float(1u << ommCacheItem.subdivisionLevel);
      for (const auto& triangle : reuse.layout.triangles) {
        Vector2 uv[3];
        for (uint32_t i = 0; i < 3; ++i) {
          std::memcpy(&uv[i], &triangle[i * 2], sizeof(float) * 2);
          uv[i] = (instance.surface.textureTransform * Vector4(uv[i].x, uv[i].y, 1.f, 1.f)).xy();
        }
        const uint32_t cost = calculateNumTexelsPerMicroTriangle(uv, rcpEdge, resolution);
        reuse.texelCosts.push_back(uint16_t(std::max(1u, std::min<uint32_t>(cost,
          OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle()))));
      }
    }
    NumTexelsPerMicroTriangle* numTexelsPerMicroTriangle = &reuse.texelCosts;
    const OmmResult texelBudgetCheckResult = reuse.enabled ? OmmResult::Success :
      getNumTexelsPerMicroTriangle(instance, &numTexelsPerMicroTriangle);
    if (!cacheHit && texelBudgetCheckResult != OmmResult::Success) {
      if (instance.getFrameLastUpdated() != m_device->getCurrentFrameId())
        m_instancesToDestroy.push_back(&instance);
      return texelBudgetCheckResult;
    }

    // Preallocate all the device memory needed to build the OMM item
    if (ommCacheItem.getDeviceSize() == 0)
    {
      VkDeviceSize arrayBufferDeviceSize;
      VkDeviceSize blasOmmBuffersDeviceSize;

      const VkIndexType triangleIndexType = numTriangles <= UINT16_MAX - 3 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      calculateRequiredVRamSize(numTriangles, sourceData.numTriangles, ommCacheItem.subdivisionLevel, ommCacheItem.ommFormat, triangleIndexType,
                                arrayBufferDeviceSize, blasOmmBuffersDeviceSize);

      // Disk hits upload the baked array and never allocate the compact UV buffer.
      if (reuse.enabled && !cacheHit) arrayBufferDeviceSize += reuse.layout.triangles.size() * sizeof(ommcache::Triangle) + 2 * kBufferAlignment;

      VkDeviceSize requiredDeviceSize = arrayBufferDeviceSize + blasOmmBuffersDeviceSize;

      if (!m_memoryManager.allocate(requiredDeviceSize)) {
        m_amountOfMemoryMissing = ommPendingAllocation(m_amountOfMemoryMissing, requiredDeviceSize, m_memoryManager.getBudget());
        return OmmResult::OutOfMemory;
      }

      ommCacheItem.arrayBufferDeviceSize = arrayBufferDeviceSize;
      ommCacheItem.blasOmmBuffersDeviceSize = blasOmmBuffersDeviceSize;
    }

    // Create micromap buffer
    if (!ommCacheItem.ommArrayBuffer.ptr())
    {
      DxvkBufferCreateInfo ommBufferInfo;
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      ommBufferInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      ommBufferInfo.access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      ommBufferInfo.size = opacityMicromapBufferSize;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommCacheItem.ommArrayBuffer = m_device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM micromap buffer");

      if (ommCacheItem.ommArrayBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to allocate OMM array buffer due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OmmResult::OutOfMemory;
      }
    }

    if (cacheHit) {
      ctx->writeToBuffer(ommCacheItem.ommArrayBuffer, 0, opacityMicromapBufferSize, reuse.record->bytes.data());
      auto& state = ommCacheItem.bakingState;
      state.initialized = true;
      state.numMicroTrianglesToBake = state.numMicroTrianglesBaked = numMicroTriangles;
      state.numMicroTrianglesBakedInLastBake = 0;
      reuse.cacheLoaded = true;
      reuse.record.reset(); // writeToBuffer copied the bytes; do not pin RAM through the build queue.
      ++s_ommReuseStats.uploads;
      availableUploadBytes -= std::min<VkDeviceSize>(availableUploadBytes, opacityMicromapBufferSize);
      s_ommReuseStats.uploadBytes += opacityMicromapBufferSize;
      return OmmResult::Success;
    }
    if (reuse.enabled && reuse.texcoords == nullptr) {
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      info.size = reuse.layout.triangles.size() * sizeof(ommcache::Triangle);
      reuse.texcoords = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM compact UVs");
      if (reuse.texcoords == nullptr) return OmmResult::OutOfMemory;
      ctx->writeToBuffer(reuse.texcoords, 0, info.size, reuse.layout.triangles.data());
    }
    if (reuse.enabled && !ommCacheItem.bakingState.initialized) ++s_ommReuseStats.coldBakes;
    if (!reuse.enabled && !ommCacheItem.bakingState.initialized) ++s_ommReuseStats.fallbackBakes;

    // Generate OMM array
    {
      RtxGeometryUtils::BakeOpacityMicromapDesc desc(*numTexelsPerMicroTriangle);
      desc.subdivisionLevel = ommCacheItem.subdivisionLevel;
      desc.numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
      desc.ommFormat = ommCacheItem.ommFormat;
      desc.surfaceIndex = instance.getSurfaceIndex();
      desc.materialType = instance.getMaterialType();
      desc.applyVertexAndTextureOperations = ommCacheItem.useVertexAndTextureOperations;
      desc.useConservativeEstimation = OpacityMicromapOptions::Building::ConservativeEstimation::enable();
      desc.conservativeEstimationMaxTexelTapsPerMicroTriangle = OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle();
      desc.numTriangles = numTriangles;
      desc.triangleOffset = reuse.enabled ? 0 : sourceData.triangleOffset;
      desc.compactTexcoords = reuse.texcoords;
      desc.resolveTransparencyThreshold = RtxOptions::resolveTransparencyThreshold();
      desc.resolveOpaquenessThreshold = RtxOptions::resolveOpaquenessThreshold();
      desc.costPerTexelTapPerMicroTriangleBudget = OpacityMicromapOptions::Building::costPerTexelTapPerMicroTriangleBudget();

      // Overrides
      if (instance.surface.alphaState.isDecal)
        desc.resolveTransparencyThreshold = std::max(desc.resolveTransparencyThreshold, OpacityMicromapOptions::Building::decalsMinResolveTransparencyThreshold());

      const auto& samplers = ctx->getCommonObjects()->getSceneManager().getSamplerTable();
            
      // Bake micro triangles
      do {
        ctx->getCommonObjects()->metaGeometryUtils().dispatchBakeOpacityMicromap(
          ctx, instance, blasEntry.modifiedGeometryData,
          textures, samplers, instance.getOmmOpacityTextureIndex(), instance.getSamplerIndex(), instance.getSecondaryOpacityTextureIndex(), instance.getSecondarySamplerIndex(),
          desc, ommCacheItem.bakingState, availableBakingBudget, ommCacheItem.ommArrayBuffer);

        if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
          availableBakingBudget = UINT32_MAX;

          // There are more micro triangles to bake
          if (ommCacheItem.bakingState.numMicroTrianglesBaked < ommCacheItem.bakingState.numMicroTrianglesToBake) {
            continue;
          }
        }

        // Exit the loop
        break;
      } while (true);

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(ommCacheItem.ommArrayBuffer);
    }

    m_numMicroTrianglesBaked += ommCacheItem.bakingState.numMicroTrianglesBakedInLastBake;
    s_ommReuseStats.coldMicroTriangles += ommCacheItem.bakingState.numMicroTrianglesBakedInLastBake;
    if (reuse.enabled && ommCacheItem.bakingState.numMicroTrianglesBaked == ommCacheItem.bakingState.numMicroTrianglesToBake)
      saveBakedArray(ctx, ommCacheItem, opacityMicromapBufferSize);

    return OmmResult::Success;
  }

  OpacityMicromapManager::OmmResult OpacityMicromapManager::buildOpacityMicromap(
    Rc<DxvkContext> ctx,
    XXH64_hash_t ommSrcHash,
    OpacityMicromapCacheItem& ommCacheItem,
    VkMicromapUsageEXT& ommUsageGroup,
    VkMicromapBuildInfoEXT& ommBuildInfo,
    uint32_t& maxMicroTrianglesToBuild,
    bool forceBuild) {
    
    auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
    omm_validation_assert(sourceDataIter != m_cachedSourceData.end());
    CachedSourceData& sourceData = sourceDataIter->second;

    const uint32_t numMicroTrianglesPerTriangle = calculateNumMicroTriangles(ommCacheItem.subdivisionLevel);
    const auto& reuse = ommCacheItem.reuseData;
    const bool compact = reuse && reuse->enabled;
    const uint32_t numTriangles = compact ? uint32_t(reuse->layout.triangles.size()) : sourceData.numTriangles;
    const uint32_t numMicroTriangles = numTriangles * numMicroTrianglesPerTriangle;

    // OMM builds are at per OMM item granularity
    if (!forceBuild && numMicroTriangles > maxMicroTrianglesToBuild)
      return OmmResult::OutOfBudget;

    const uint8_t numOpacityMicromapBitsPerMicroTriangle = ommCacheItem.ommFormat == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT ? 1 : 2;
    const uint32_t opacityMicromapPerTriangleBufferSize = dxvk::util::ceilDivide(numMicroTrianglesPerTriangle * numOpacityMicromapBitsPerMicroTriangle, 8);
    const uint32_t opacityMicromapBufferSize = numTriangles * opacityMicromapPerTriangleBufferSize;
    const VkIndexType triangleIndexType = numTriangles <= UINT16_MAX - 3 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    const uint32_t numBytesPerIndexElement = triangleIndexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    ommCacheItem.blasOmmBuffers = new DxvkOpacityMicromap(*m_device);

    // Micromap forward definitions
    Rc<DxvkBuffer> triangleArrayBuffer;     // VkMicromapTriangleEXT per triangle
    
    // Fill out VkMicromapUsageEXT with size information
    // For now all triangles are in the same micromap group
    ommUsageGroup = {};
    ommUsageGroup.count = numTriangles;
    ommUsageGroup.subdivisionLevel = ommCacheItem.subdivisionLevel;
    ommUsageGroup.format = ommCacheItem.ommFormat;

    // Get micromap prebuild info
    ommBuildInfo = {};
    ommBuildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
    VkMicromapBuildSizesInfoEXT sizeInfo = { VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT };
    calculateMicromapBuildInfo(ommUsageGroup, ommBuildInfo, sizeInfo);

    // Initialize micromap triangle index buffers
    {
      OmmResult result;
      if (triangleIndexType == VK_INDEX_TYPE_UINT16)
        result = initializeOpacityMicromapTriangleArrayBuffers<uint16_t>(
          m_device, ctx, ommCacheItem.ommFormat, ommCacheItem.subdivisionLevel, numTriangles, opacityMicromapPerTriangleBufferSize,
          triangleArrayBuffer, ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer,
          compact ? &reuse->layout.indices : nullptr);
      else
        result = initializeOpacityMicromapTriangleArrayBuffers<uint32_t>(
          m_device, ctx, ommCacheItem.ommFormat, ommCacheItem.subdivisionLevel, numTriangles, opacityMicromapPerTriangleBufferSize,
          triangleArrayBuffer, ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer,
          compact ? &reuse->layout.indices : nullptr);

      if (result != OmmResult::Success)
        return result;
    }

    // Create micromap
    {
      // Create buffer
      DxvkBufferCreateInfo ommBufferInfo = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
      ommBufferInfo.usage = VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // ToDo: revisit. Access should be VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT, but the EXT flag is not compatible here
      // The access is covered by a proper VkMemoryBarrier2 later
      ommBufferInfo.access = VK_ACCESS_MEMORY_WRITE_BIT;
      ommBufferInfo.size = sizeInfo.micromapSize;
      ommBufferInfo.requiredAlignmentOverride = 256;
      ommCacheItem.blasOmmBuffers->opacityMicromapBuffer = m_device->createBuffer(ommBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXOpacityMicromap, "OMM micromap");

      if (ommCacheItem.blasOmmBuffers->opacityMicromapBuffer == nullptr) {
        ONCE(Logger::warn(str::format("[RTX - Opacity Micromap] Failed to build a micromap due to m_device->createBuffer() failing to allocate a buffer for size: ", ommBufferInfo.size)));
        return OmmResult::OutOfMemory;
      }

      // Create micromap
      VkMicromapCreateInfoEXT maCreateInfo = { VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT };
      maCreateInfo.createFlags = 0;
      maCreateInfo.buffer = ommCacheItem.blasOmmBuffers->opacityMicromapBuffer->getBufferRaw();
      maCreateInfo.offset = 0;
      maCreateInfo.size = sizeInfo.micromapSize;
      maCreateInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
      maCreateInfo.deviceAddress = 0ull;

      if (vkFailed(m_device->vkd()->vkCreateMicromapEXT(m_device->vkd()->device(), &maCreateInfo, nullptr, &ommCacheItem.blasOmmBuffers->opacityMicromap))) {
        ONCE(Logger::warn("[RTX Opacity Micromap] Failed to build a micromap. Ignoring the build request."));
        return OmmResult::Failure;
      }
    }
    
    // Calculate the required the scratch memory
    const size_t scratchAlignment = m_device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
    const size_t requiredScratchAllocSize = align(sizeInfo.buildScratchSize, scratchAlignment);

    // Build the array with vkBuildMicromapsEXT
    {
      // Fill in the pointers we didn't have at size query
      ommBuildInfo.dstMicromap = ommCacheItem.blasOmmBuffers->opacityMicromap;
      ommBuildInfo.data.deviceAddress = ommCacheItem.ommArrayBuffer->getDeviceAddress();
      assert(ommBuildInfo.data.deviceAddress % 256 == 0);
      ommBuildInfo.triangleArray.deviceAddress = triangleArrayBuffer->getDeviceAddress();
      assert(ommBuildInfo.triangleArray.deviceAddress % 256 == 0);
      // Null means the grow failed. Reject the bake rather than dereference it
      // or hand the build a zero scratch base, which would write at gpuVA 0.
      const Rc<DxvkBuffer> ommScratch =
        getScratchMemory(align(m_scratchMemoryUsedThisFrame + requiredScratchAllocSize, scratchAlignment));

      if (ommScratch == nullptr) {
        ONCE(Logger::err(
          "OpacityMicromapManager: skipping micromap build - no scratch memory."));
        return OmmResult::OutOfMemory;
      }

      ommBuildInfo.scratchData.deviceAddress = ommScratch->getDeviceAddress() + m_scratchMemoryUsedThisFrame;
      assert(ommBuildInfo.scratchData.deviceAddress % scratchAlignment == 0);
      m_scratchMemoryUsedThisFrame += requiredScratchAllocSize;
      ommBuildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
      
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(ommCacheItem.ommArrayBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(triangleArrayBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);

      // Release OMM array memory as it's no longer needed after the build
      {
        m_memoryManager.release(ommCacheItem.arrayBufferDeviceSize);
        ommCacheItem.arrayBufferDeviceSize = 0;
        ommCacheItem.ommArrayBuffer = nullptr;
      }
    }

    // Update the BLAS desc with the built micromap
    {
      VkAccelerationStructureTrianglesOpacityMicromapEXT& ommBlasDesc = ommCacheItem.blasOmmBuffers->blasDesc;
      ommBlasDesc = VkAccelerationStructureTrianglesOpacityMicromapEXT { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT };
      ommBlasDesc.micromap = ommCacheItem.blasOmmBuffers->opacityMicromap;
      ommBlasDesc.indexType = triangleIndexType;
      ommBlasDesc.indexBuffer.deviceAddress = ommCacheItem.blasOmmBuffers->opacityMicromapTriangleIndexBuffer->getDeviceAddress();
      ommBlasDesc.indexStride = numBytesPerIndexElement;
      ommBlasDesc.baseTriangle = 0;
      // BLAS usage counts include every geometry triangle after indirection;
      // micromap build usage above counts only the unique opacity patterns.
      ommCacheItem.blasOmmBuffers->blasUsage = { sourceData.numTriangles, ommCacheItem.subdivisionLevel, uint32_t(ommCacheItem.ommFormat) };
      ommBlasDesc.usageCountsCount = 1;
      ommBlasDesc.pUsageCounts = &ommCacheItem.blasOmmBuffers->blasUsage;
    }
  
    // Track the lifetime of all the build buffers needed for BLAS, including non-ref counted .opacityMicromap
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(ommCacheItem.blasOmmBuffers);

    m_numMicroTrianglesBuilt += numMicroTriangles;
    maxMicroTrianglesToBuild -= std::min(numMicroTriangles, maxMicroTrianglesToBuild);

    // Source data is no longer needed
    deleteCachedSourceData(sourceDataIter, ommCacheItem.cacheState, true);
    ommCacheItem.reuseData.reset();

#ifdef VALIDATION_MODE
    Logger::warn(str::format("[RTX Opacity Micromap] m_cachedSourceData.erase(", ommSrcHash, ") by thread_id ", std::this_thread::get_id()));
#endif
    
    return OmmResult::Success;
  }

  void OpacityMicromapManager::bakeOpacityMicromapArrays(Rc<DxvkContext> ctx,
                                                         const std::vector<TextureRef>& textures,
                                                         uint32_t& availableBakingBudget) {

    if (!OpacityMicromapOptions::enableBakingArrays())
      return;

#ifdef VALIDATION_MODE
    for (auto iter0 = m_unprocessedList.begin(); iter0 != m_unprocessedList.end(); iter0++) {
      auto iter1 = iter0;
      iter1++;
      for (; iter1 != m_unprocessedList.end(); iter1++) {
        if (*iter1 == *iter0) {
          omm_validation_assert(0 && "Duplicate entries found in a list");
        }
      }
    }
    for (auto iter0 = m_cachedSourceData.begin(); iter0 != m_cachedSourceData.end(); iter0++) {
      OpacityMicromapCacheItem& ommCacheItem = m_ommCache[iter0->first];
      if ((ommCacheItem.cacheState <= OpacityMicromapCacheState::eStep0_Unprocessed) &&
           iter0->second.getInstance() == nullptr)
        omm_validation_assert(0 && "Instance is null at unexpected stage");
    }
#endif

    ScopedGpuProfileZone(ctx, "Bake Opacity Micromap Arrays");

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      availableBakingBudget = UINT32_MAX;
    }

    VkDeviceSize availableUploadBytes = 8ull * 1024 * 1024;
    // Probe/cache pass has an independent byte budget and a rotating bounded
    // scan, so one cold bake cannot prevent later warm jobs from being read.
    for (uint32_t pass = 0; pass < 2; ++pass) {
      const bool cacheOnly = pass == 0;
      uint32_t visited = 0;
      auto ommSrcHashIter = m_unprocessedList.begin();
      if (cacheOnly) {
        auto start = m_ommCache.find(m_cacheScanStart);
        if (start != m_ommCache.end() && start->second.isUnprocessedCacheStateListIterValid)
          ommSrcHashIter = start->second.cacheStateListIter;
      }
      for (; ommSrcHashIter != m_unprocessedList.end() &&
          (cacheOnly ? visited++ < 256 : availableBakingBudget > 0); ) {
      XXH64_hash_t ommSrcHash = *ommSrcHashIter;

#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] Baking ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

      auto sourceDataIter = m_cachedSourceData.find(ommSrcHash);
      auto cacheItemIter = m_ommCache.find(ommSrcHash);

      if (sourceDataIter == m_cachedSourceData.end() || cacheItemIter == m_ommCache.end()) {
        // Note: this shouldn't be hit anymore as it was triggered by destroying an instance
        // on a baking failure and destroying source data for all OMMs associated with that instance.
        // That included OMMs that were still in the unordered list. Now just the failed OMM gets destroyed.
        assert(0 && "OMM inconsistent state");
        ONCE(Logger::err("[RTX Opacity Micromap] Encountered inconsistent state. Opacity Micromap item listed for baking is missing required state data. Skipping it."));
        // First update the iterator, then destroy any omm data associated with it
        ommSrcHashIter++;
        destroyOmmData(ommSrcHash);
        continue;
      }

      CachedSourceData& sourceData = sourceDataIter->second;
      OpacityMicromapCacheItem& ommCacheItem = cacheItemIter->second;
      ommCacheItem.cacheState = OpacityMicromapCacheState::eStep1_Baking;

      OmmResult result = bakeOpacityMicromapArray(ctx, ommSrcHash, ommCacheItem, sourceData, textures,
        availableBakingBudget, availableUploadBytes, cacheOnly);

      if (result == OmmResult::Success) {
        // Use >= as the number of baked micro triangles is aligned up
        if (ommCacheItem.bakingState.numMicroTrianglesBaked >= ommCacheItem.bakingState.numMicroTrianglesToBake) {

          // Unlink the referenced RtInstance
          sourceData.setInstance(nullptr, m_instanceOmmRequests, *this);

          m_numTexelsPerMicroTriangle.erase(ommSrcHash);

          auto ommSrcHashIterToMove = ommSrcHashIter++;
          ommCacheItem.isUnprocessedCacheStateListIterValid = false;
          if (ommCacheItem.blasOmmBuffers != nullptr) {
            // Shared GPU map: only the geometry remap was uploaded. No array
            // bake or VkMicromap build is needed, but the transfer needs a barrier.
            deleteCachedSourceData(ommSrcHash, ommCacheItem.cacheState, true);
            ommCacheItem.cacheState = OpacityMicromapCacheState::eStep3_Built;
            m_builtList.splice(m_builtList.end(), m_unprocessedList, ommSrcHashIterToMove);
            ommCacheItem.reuseData.reset();
            m_hasNewlyBuiltOmms = true;
            m_newlyBuiltSources.insert(ommSrcHash);
          } else {
            ommCacheItem.cacheState = OpacityMicromapCacheState::eStep2_Baked;
            // Cached arrays get to the build stage ahead of cold completions.
            const bool cached = ommCacheItem.reuseData && ommCacheItem.reuseData->cacheLoaded;
            m_bakedList.splice(cached ? m_bakedList.begin() : m_bakedList.end(), m_unprocessedList, ommSrcHashIterToMove);
          }
        }
        else {
          // Do nothing, else path means all the budget has been used up and thus the loop will exit due to availableBakingBudget == 0
          //   so don't need to increment the iterator
          if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
            ONCE(Logger::err("[RTX Opacity Micromap] Failed to fully bake an Opacity Micromap due to budget limits even with unlimited budgetting enabled."));
          }
        }
      } else if (result == OmmResult::OutOfMemory) {
        ++s_ommReuseStats.memoryWaits;
        // Do nothing, try the next one
        ommSrcHashIter++;
        ONCE(Logger::debug("[RTX Opacity Micromap] Baking Opacity Micromap Array failed as ran out of memory."));
      } else if (result == OmmResult::DependenciesUnavailable) {
        // Textures not available - try the next one
        ommSrcHashIter++;
      } else if (result == OmmResult::Failure || 
                 result == OmmResult::Rejected) {
        if (result == OmmResult::Failure) {
          ONCE(Logger::warn(str::format("[RTX Opacity Micromap] Baking Opacity Micromap Array failed for hash ", ommSrcHash, ". Ignoring and black listing the hash.")));
        }
#ifdef VALIDATION_MODE
        Logger::warn(str::format("[RTX Opacity Micromap] Baking Opacity Micromap Array failed for hash ", ommSrcHash, ". Ignoring and black listing the hash."));
#endif
        // Baking failed, ditch the OMM data
        // First update the iterator, then remove the element
        ommSrcHashIter++;
        destroyOmmData(cacheItemIter);
        m_blackListedList.insert(ommSrcHash);
      } else { // Cache miss in the probe pass, or this frame's upload allowance.
        ommSrcHashIter++;
      }
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] ~Baking ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif
    }
      if (cacheOnly)
        m_cacheScanStart = ommSrcHashIter != m_unprocessedList.end() ? *ommSrcHashIter : kEmptyHash;
    }

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      availableBakingBudget = UINT32_MAX;
    }
  }

  void OpacityMicromapManager::buildOpacityMicromapsInternal(Rc<DxvkContext> ctx,
                                                             uint32_t& maxMicroTrianglesToBuild) {

    if (!OpacityMicromapOptions::enableBuilding())
      return;

#ifdef VALIDATION_MODE
    for (auto iter0 = m_bakedList.begin(); iter0 != m_bakedList.end(); iter0++) {
      auto iter1 = iter0;
      iter1++;
      for (; iter1 != m_bakedList.end(); iter1++) {
        if (*iter1 == *iter0) {
          omm_validation_assert(0 && "Duplicate entries found in a list");
        }
      }
      for (auto iter2 = m_unprocessedList.begin(); iter2 != m_unprocessedList.end(); iter2++) {
        if (*iter2 == *iter0) {
          omm_validation_assert(0 && "Two lists contain same OMM src hash");
        }
      }
    }
#endif

    ScopedGpuProfileZone(ctx, "Build Opacity Micromaps");

    // Pre-allocate the arrays because build infos include pointers to usage groups,
    // and reallocating vectors would invalidate these pointers
    const uint32_t maxBuildItems = m_bakedList.size();
    std::vector<VkMicromapUsageEXT> micromapUsageGroups(maxBuildItems);
    std::vector<VkMicromapBuildInfoEXT> micromapBuildInfos(maxBuildItems);
    uint32_t buildItemCount = 0;

    if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
      maxMicroTrianglesToBuild = UINT32_MAX;
    }

    // Force at least one build since a build can't be split across frames even if doesn't fit within the budget
    // They're cheap regardless, so it should be fine.
    bool forceOmmBuild = maxMicroTrianglesToBuild > 0;  

    for (auto ommSrcHashIter = m_bakedList.begin(); ommSrcHashIter != m_bakedList.end() && maxMicroTrianglesToBuild > 0; ) {
      XXH64_hash_t ommSrcHash = *ommSrcHashIter;
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] Building ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif
      auto ommCacheItemIter = m_ommCache.find(ommSrcHash);
      OpacityMicromapCacheItem& ommCacheItem = ommCacheItemIter->second;

      OmmResult result = buildOpacityMicromap(ctx, *ommSrcHashIter, ommCacheItem, micromapUsageGroups[buildItemCount],
                                              micromapBuildInfos[buildItemCount], maxMicroTrianglesToBuild, forceOmmBuild);
      
      if (result == OmmResult::Success) {
        ommCacheItem.cacheState = OpacityMicromapCacheState::eStep3_Built;
        m_hasNewlyBuiltOmms = true;
        m_newlyBuiltSources.insert(ommSrcHash);
        ++s_ommReuseStats.completedBuilds;
        // Move the item from the baked list to the end of the built list
        auto ommSrcHashIterToMove = ommSrcHashIter++;
        m_builtList.splice(m_builtList.end(), m_bakedList, ommSrcHashIterToMove);
        ++buildItemCount;

        forceOmmBuild = false;
      }
      else if (result == OmmResult::Failure) {
#ifdef VALIDATION_MODE
        ONCE(Logger::warn(str::format("[RTX Opacity Micromap] Building Opacity Micromap failed for hash ", ommSrcHash, ".Ignoring and black listing the hash.")));
#endif
        // Building failed, ditch the OMM data
        // First update the iterator, then remove the element
        ommSrcHashIter++;
        destroyOmmData(ommCacheItemIter);
        m_blackListedList.insert(ommSrcHash);
      } else if (result == OmmResult::OutOfBudget) {
        // Do nothing, continue onto the next
        ommSrcHashIter++;

        if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
          ONCE(Logger::err("[RTX Opacity Micromap] Failed to fully build an Opacity Micromap due to budget limits even with unlimited budgetting enabled."));
        }
      } else if (result == OmmResult::OutOfMemory) {
        // Do nothing, try the next one
        ommSrcHashIter++;
        ONCE(Logger::warn("[RTX Opacity Micromap] Building Opacity Micromap Array failed as it ran out of memory."));
      } else {
        omm_validation_assert(0 && "Should not be hit");
        ommSrcHashIter++;
      }
#ifdef VALIDATION_MODE
      Logger::warn(str::format("[RTX Opacity Micromap] ~Building ", ommSrcHash, " on thread_id ", std::this_thread::get_id()));
#endif

      if (OpacityMicromapOptions::Building::enableUnlimitedBakingAndBuildingBudgets()) {
        maxMicroTrianglesToBuild = UINT32_MAX;
      }
    }

    if (buildItemCount > 0) {
      // Add a barrier needed for Micromap build reading the triangleArrayBuffer's and triangleIndexBuffer's
      {
        VkMemoryBarrier2 memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr,
          VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_SHADER_READ_BIT };
        VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &memoryBarrier;

        ctx->getCommandList()->vkCmdPipelineBarrier2KHR(&dependencyInfo);
      }

      // Build the micromaps
      ctx->getCommandList()->vkCmdBuildMicromapsEXT(buildItemCount, micromapBuildInfos.data());
    }
  }

  void OpacityMicromapManager::onFrameStart(Rc<DxvkContext> ctx) {
    ScopedCpuProfileZone();
    const uint32_t currentFrameIndex = m_device->getCurrentFrameId();
    releaseRetiredOmms();
    collectTextureFingerprints();
    collectBakedArrays();

    m_numBoundOMMs = 0;
    m_numRequestedOMMBindings = 0;
    m_scratchMemoryUsedThisFrame = 0;
    m_hasNewlyBuiltOmms = false;
    m_newlyReadySources = std::move(m_newlyBuiltSources);
    m_newlyBuiltSources.clear();

    // Clear caches if we need to rebuild OMMs
    {
      bool forceRebuildOMMs = OpacityMicromapOptions::enableResetEveryFrame();
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::enable(), 
                                          m_prevConservativeEstimationEnable);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::maxTexelTapsPerMicroTriangle(), 
                                          m_prevConservativeEstimationMaxTexelTapsPerMicroTriangle);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::ConservativeEstimation::minValidOMMTrianglesInMeshPercentage(),
                                          m_prevConservativeEstimationMinValidOMMTrianglesInMeshPercentage);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::subdivisionLevel(), 
                                          m_prevBuildingSubdivisionLevel);
      forceRebuildOMMs |= hasValueChanged(OpacityMicromapOptions::Building::enableVertexAndTextureOperations(), 
                                          m_prevBuildingEnableVertexAndTextureOperations);

      if (forceRebuildOMMs) {
        clear();
        // Reset the black listed list as well since black listing depends on the settings
        m_blackListedList.clear();
        m_needsBlasRebuild = true;
      }
    }

    // Detect binding/building option toggles that require BLAS rebuilds
    // so existing OMM bindings are added or removed.
    {
      bool bindingOptionsChanged = false;
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBinding(), m_prevEnableBinding);
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBuilding(), m_prevEnableBuilding);
      bindingOptionsChanged |= hasValueChanged(OpacityMicromapOptions::enableBakingArrays(), m_prevEnableBakingArrays);
      if (bindingOptionsChanged) {
        m_needsBlasRebuild = true;
      }
    }

    // Requests are retried by processOmmCandidates. Count only real requests;
    // advancing lastRequestFrameId here kept obsolete identities alive forever.
    // Age by hash rather than erasing on instance destruction: other instances
    // can still be requesting the same OMM.
    for (auto iter = m_ommBuildRequestStatistics.begin(); iter != m_ommBuildRequestStatistics.end();) {
      if (currentFrameIndex - iter->second.lastRequestFrameId >
          static_cast<uint32_t>(OpacityMicromapOptions::BuildRequests::maxRequestFrameAge())) {
        iter = m_ommBuildRequestStatistics.erase(iter);
      } else {
        ++iter;
      }
    }
    
    // Account for OMM usage in BLASes in a previous TLAS
    // Tag the previously bound OMMs as used in this frame as well
    if (RtxOptions::enablePreviousTLAS()) {
      for (auto& previousFrameBoundOMM : m_boundOMMs)
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(previousFrameBoundOMM);
    }
    m_boundOMMs.clear();

    // Update memory management
    {
      m_memoryManager.updateMemoryBudget(ctx);

      // V774: every frame gets a small, time-sliced part of the ownership
      // census, ordering, eviction or planner cleanup. No full-cache sort.
      {
        const auto maintenanceStart = std::chrono::steady_clock::now();
        const auto deadline = maintenanceStart + std::chrono::microseconds(200);
        auto& retention = s_ommRetention[this];
        auto& sweep = retention.sweep;
        const uint64_t now = ommRetentionTimeMs();
        const uint32_t minAge = OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEviction();
        const bool pressure = m_memoryManager.getBudget() < m_memoryManager.getPrevBudget()
          || m_memoryManager.getUsed() > m_memoryManager.getBudget();
        uint32_t operations = 0, deletions = 0;
        uint64_t released = 0;
        const uint64_t startingCycle = sweep.cycles;
        while (operations < 128 && deletions < 4 && released < 16 * ommretention::MiB && sweep.cycles == startingCycle) {
          if (std::chrono::steady_clock::now() >= deadline) break;
          ++operations;
          auto victim = m_ommCache.end();
          // Expiry has its own deadline index, so a large completed-map sweep
          // cannot extend the abandoned-bake grace by hundreds of frames.
          auto expired = retention.abandoned.begin();
          if (expired != retention.abandoned.end() && ommretention::abandonedExpired(expired->first.first, now)) {
            const auto deadlineKey = expired->first;
            const uint64_t generation = expired->second;
            retention.abandoned.erase(expired);
            auto metadata = retention.entries.find(deadlineKey.second);
            auto it = m_ommCache.find(deadlineKey.second);
            if (metadata != retention.entries.end() && metadata->second.generation == generation &&
                metadata->second.detachedSinceMs == deadlineKey.first && it != m_ommCache.end() &&
                it->second.cacheState == OpacityMicromapCacheState::eStep1_Baking &&
                !it->second.isUnprocessedCacheStateListIterValid && m_cachedSourceData.find(it->first) == m_cachedSourceData.end()) {
              victim = it;
            }
          } else if (sweep.phase == ommretention::Sweep::Phase::Collect) {
            auto metadata = sweep.started ? retention.entries.upper_bound(sweep.cursor) : retention.entries.begin();
            if (metadata == retention.entries.end()) { sweep.minAge = minAge; sweep.finishCollect(); continue; }
            sweep.cursor = metadata->first; sweep.started = true;
            auto it = m_ommCache.find(metadata->first);
            if (it == m_ommCache.end()) continue;
            const auto& item = it->second;
            if (item.cacheState >= OpacityMicromapCacheState::eStep3_Built && item.blasOmmBuffers != nullptr) {
              const auto* resource = item.blasOmmBuffers.ptr();
              ommretention::SweepEntry entry;
              entry.hash = it->first; entry.resource = uint64_t(uintptr_t(resource));
              entry.parent = uint64_t(uintptr_t(resource->sharedMicromapOwner.ptr()));
              entry.references = resource->refCount(); entry.bytes = item.blasOmmBuffersDeviceSize;
              entry.age = currentFrameIndex - item.lastUseFrameIndex;
              entry.diskBacked = metadata->second.diskBacked; entry.generation = metadata->second.generation;
              sweep.add(entry);
            }
          } else {
            ommretention::SweepEntry candidate;
            const bool selected = sweep.step(m_memoryManager.getBudget(), pressure, retention.trimming, candidate);
            if (selected) {
              auto it = m_ommCache.find(candidate.hash);
              auto metadata = retention.entries.find(candidate.hash);
              if (it != m_ommCache.end() && metadata != retention.entries.end() &&
                  it->second.cacheState >= OpacityMicromapCacheState::eStep3_Built && it->second.blasOmmBuffers != nullptr &&
                  ommretention::stillEvictable(candidate, metadata->second.generation,
                    uint64_t(uintptr_t(it->second.blasOmmBuffers.ptr())), currentFrameIndex - it->second.lastUseFrameIndex,
                    minAge, it->second.blasOmmBuffers->refCount())) victim = it;
            }
          }
          if (victim != m_ommCache.end()) {
            const uint64_t bytes = victim->second.getDeviceSize();
            destroyOmmData(victim);
            ++deletions; released += bytes;
          }
        }
      }

      if (m_memoryManager.getBudget() != 0) {
        // Keep pressure eviction active while still over budget: resources
        // protected on the contraction frame may become releasable later.
        const bool hasVRamBudgetDecreased = m_memoryManager.getBudget() < m_memoryManager.getPrevBudget()
          || m_memoryManager.getUsed() > m_memoryManager.getBudget();

        // Failed allocations recorded a full peak request, not a summed deficit.
        // Subtract free space once, then compare with pending releases only.
        m_amountOfMemoryMissing = ommReclaimTarget(m_amountOfMemoryMissing,
          m_memoryManager.getUsed(), m_memoryManager.getBudget());

        // LRU cache eviction
        if (m_amountOfMemoryMissing > 0) {

          // Start evicting least recently used items 
          for (auto lruOmmSrcHashIter = m_leastRecentlyUsedList.begin();
               lruOmmSrcHashIter != m_leastRecentlyUsedList.end() && m_amountOfMemoryMissing > m_memoryManager.calculatePendingReleasedSize();
               ) {
            auto cacheItemIter = m_ommCache.find(*lruOmmSrcHashIter);
            if (cacheItemIter == m_ommCache.end()) {
              auto iterToDelete = lruOmmSrcHashIter;
              // Increment the iterator before any deletion
              lruOmmSrcHashIter++;
              ONCE(Logger::err("[RTX] Failed to find Opacity Micromap cache entry on LRU eviction"));
              m_leastRecentlyUsedList.erase(iterToDelete);
              continue;
            }

            // A BLAS or an in-flight command still owns this map. Even a
            // shrinking budget cannot make its storage safe to recycle.
            if (cacheItemIter->second.blasOmmBuffers != nullptr &&
                cacheItemIter->second.blasOmmBuffers->refCount() > 1) {
              ONCE(KENSHI_DIAGNOSTIC_INFO("[OMM V748] Eviction retained a micromap owned by a BLAS or pending command."));
              ++lruOmmSrcHashIter;
              continue;
            }

            const uint32_t cacheItemUsageFrameAge = currentFrameIndex - cacheItemIter->second.lastUseFrameIndex;

            // Stop eviction once an item is recent enough
            if (cacheItemUsageFrameAge < OpacityMicromapOptions::Cache::minUsageFrameAgeBeforeEviction() &&
              // Force eviction if the VRAM budget decreased to speed fitting into the budget up
              !hasVRamBudgetDecreased) {
              break;
            }

            // Increment the iterator before any deletion
            lruOmmSrcHashIter++;

            destroyOmmData(cacheItemIter);
          }
        }
      } else { // budget == 0
        if (m_memoryManager.getPrevBudget() > 0) {
          clear();
        }
      }

      m_amountOfMemoryMissing = 0;

      // Call Memory Manager's onFrameStart last since any evicted buffers above 
      // were not used in this frame and thus should go to a pending release queue of the last frame
      m_memoryManager.onFrameStart();

      // Require at least 1MB (selected ad-hoc to cover at least a quad) of free budget to allow processing of new OMM items
      m_hasEnoughMemoryToPotentiallyGenerateAnOmm =
        m_memoryManager.getAvailable() >= 1 * 1024 * 1024;

      m_numMicroTrianglesBaked = 0;
      m_numMicroTrianglesBuilt = 0;
    }
  }

  void OpacityMicromapManager::onFrameEnd() {
    // Staging results are only needed for one frame, so purge them
    m_numTexelsPerMicroTriangleStaging.clear();

    m_numTrianglesToCalculateForNumTexelsPerMicroTriangle =
      OpacityMicromapOptions::Building::ConservativeEstimation::maxTrianglesToCalculateTexelDensityForPerFrame();

    // Register amount of free vidmem at the end of the frame to account for any intra-frame allocations.
    // This will be then used next frame to adjust budgeting
    m_memoryManager.registerVidmemFreeSize();

  }

  void OpacityMicromapManager::onFinishedBuilding() {
    // Release the scratch memory so it can be reused by rest of the frame.
    m_scratchBuffer = nullptr;
  }

  bool OpacityMicromapManager::isActive() const {
    return m_memoryManager.getBudget() > 0;
  }

  void OpacityMicromapManager::buildOpacityMicromaps(Rc<DxvkContext> ctx,
                                                     const std::vector<TextureRef>& textures,
                                                     uint32_t lastCameraCutFrameId) {

    // Get the workload scale in respect to 60 Hz for a given frame time.
    // 60 Hz is the baseline since that's what the per-second budgets have been parametrized at in RtxOptions
    const float kFrameTime60Hz = 1 / 60.f;
    const float frameTimeSecs = GlobalTime::get().deltaTime();
    float workloadScalePerSecond = frameTimeSecs / kFrameTime60Hz;

    // Modulate the scale for practical FPS range (i.e. <25, 200>) to even out the OMM's per frame percentage performance overhead
    {
      // Scale set to balance evening out performance overhead across FPS as well as not to stray too 
      // far from linear scaling so as not to slow down baking at very high FPS too much

      // Apply non-linear scaling only to an FPS range <25, 200> to avoid pow(t, x) blowing scaling out of proportion
      // Linear scaling will result in less overhead per frame for below 25 FPS, and in more overhead over 200 FPS
      if (frameTimeSecs >= 1 / 200.f && frameTimeSecs <= 1 / 25.f) {
        workloadScalePerSecond = powf(workloadScalePerSecond, 1.28f);
      } else if (frameTimeSecs > 1 / 25.f) {
        workloadScalePerSecond *= 1.278f; // == non-linear scale multiplier at 25 FPS
      } else {
        workloadScalePerSecond *= 0.714f; // == non-linear scale multiplier at 200 FPS
      }
    }

    // Convert the modulated workload scale back to frameTimeSecs's/per second base
    // since that's how the per-second budgets are expressed and can be multiplied with
    // to get the budget to use in this frame
    const float secondToFrameBudgetScale = workloadScalePerSecond * kFrameTime60Hz;

    // Initialize per frame budgets
    float numMillionMicroTrianglesToBakeAvailable = OpacityMicromapOptions::Building::maxMicroTrianglesToBakeMillionPerSecond() * secondToFrameBudgetScale;
    float numMillionMicroTrianglesToBuildAvailable = OpacityMicromapOptions::Building::maxMicroTrianglesToBuildMillionPerSecond() * secondToFrameBudgetScale;

    if (m_device->getCurrentFrameId() - lastCameraCutFrameId < OpacityMicromapOptions::Building::numFramesAtStartToBuildWithHighWorkload()) {
      numMillionMicroTrianglesToBakeAvailable *= OpacityMicromapOptions::Building::highWorkloadMultiplier();
      numMillionMicroTrianglesToBuildAvailable *= OpacityMicromapOptions::Building::highWorkloadMultiplier();
    }

    float fNumMicroTrianglesToBakeAvailable = numMillionMicroTrianglesToBakeAvailable * 1e6f;
    uint32_t numMicroTrianglesToBakeAvailable = fNumMicroTrianglesToBakeAvailable < UINT32_MAX ? static_cast<uint32_t>(fNumMicroTrianglesToBakeAvailable) : UINT32_MAX;
    float fNumMicroTrianglesToBuildAvailable = numMillionMicroTrianglesToBuildAvailable * 1e6f;
    uint32_t numMicroTrianglesToBuildAvailable = fNumMicroTrianglesToBuildAvailable < UINT32_MAX ? static_cast<uint32_t>(fNumMicroTrianglesToBuildAvailable) : UINT32_MAX;

    // Generate opacity micromaps
    if (!m_unprocessedList.empty() || !m_bakedList.empty()) {
      ScopedGpuProfileZone(ctx, "Process Opacity Micromaps");

      bakeOpacityMicromapArrays(ctx, textures, numMicroTrianglesToBakeAvailable);
      buildOpacityMicromapsInternal(ctx, numMicroTrianglesToBuildAvailable);

      // Purge instances queued for deletion
      for (const RtInstance* instance : m_instancesToDestroy) {
        destroyInstance(*instance);
      }
      m_instancesToDestroy.clear();
    }
  }
}  // namespace dxvk
