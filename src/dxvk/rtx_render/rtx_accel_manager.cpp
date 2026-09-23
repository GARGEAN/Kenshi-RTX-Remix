#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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
#include <assert.h>
#include <cstring>
#include <mutex>
#include <vector>
#include <chrono>
#include <array>
#include "rtx_vram_ownership.h"

#include "rtx.h"
#include "rtx_context.h"
#include "rtx_opacity_micromap_manager.h"
#include "rtx_scene_manager.h"
#include "rtx_accel_manager.h"
#include "../../util/util_kenshi_fault.h"
#include "../../util/util_kenshi_terrain_profile.h"
#include "rtx_point_instancer_system.h"

#include "../d3d11/d3d11_state.h"
#include "rtx_matrix_helpers.h"

#include "dxvk_scoped_annotation.h"
#include "rtx_options.h"
#include "rtx_kenshi_options.h"
#include "rtx_kenshi_flicker_trace.h"
#include <iomanip>

#include "rtx/pass/instance_definitions.h"
#include "rtx/concept/billboard.h"

#include "rtx/pass/common_binding_indices.h"

namespace dxvk {

  // Make this static and not a member of AccelManager to make it safe updating the count from ~PooledBlas()
  static int g_blasCount = 0;

  // DX11_V334_TLAS_STATS. Plain counters, never Vulkan objects, so they are
  // safe as translation-unit statics.
  static std::atomic<uint32_t> s_tlasHidden { 0u };
  static std::atomic<uint32_t> s_tlasGc { 0u };
  static std::atomic<uint32_t> s_tlasZeroMask { 0u };
  static std::atomic<uint32_t> s_tlasInTlas { 0u };
  // DX11_V344_TLAS_CONTENTS: entries present in the TLAS but inert.
  static std::atomic<uint32_t> s_tlasNullBlas { 0u };
  static std::atomic<uint32_t> s_tlasZeroXform { 0u };

  // DX11_V375_BLAS_ROUTE: account for every non-rejected instance by which BLAS
  // route it took, and whether that route actually produced a GPU build.
  //
  // Kenshi's wall segments are 1743 indices = 581 primitives, below
  // minPrimsInDynamicBLAS (1000), so they are force-merged; the geometry that
  // does render (10575/25710-index meshes) is above it and gets a dynamic BLAS
  // with its own TLAS entry. That is the one structural difference between
  // "renders" and "does not render" that survived every other test, so the
  // merged pipeline needs to be accountable end to end.
  //
  // Note what these counters exist to prevent: `instances` (889) versus
  // `inTlas` (334) looks like a huge loss but is NOT one - a merged bucket
  // collapses many instances into a single TLAS entry by design, and an
  // instance on that route legitimately carries
  // accelerationStructureReference == 0 in its own template. Both readings
  // were mistaken for defects. These counters make the split explicit so the
  // comparison stops being misleading.
  static std::atomic<uint32_t> s_mergedBuckets { 0u };
  static std::atomic<uint32_t> s_mergedBucketInstances { 0u };
  static std::atomic<uint32_t> s_mergedBucketsBuilt { 0u };
  static std::atomic<uint32_t> s_mergedBucketsSkipped { 0u };
  static std::atomic<uint32_t> s_mergedBucketNoBlas { 0u };
  static std::atomic<uint32_t> s_dynamicBlasCount { 0u };
  static std::atomic<uint32_t> s_dynamicBlasInstances { 0u };
  // Bounded per-bucket detail, armed by the diagnostics hotkey so a normal run
  // carries none of it.
  static std::atomic<uint32_t> s_bucketDetailBudget { 0u };

  // DX11_V380_CACHE_ACCOUNTING: the cached-bucket RESTORE path, which was dark.
  // `skipped` is instances that took the bucket-cache `continue`; the rest describe
  // what the restore actually put back. One TLAS entry is emitted per bucket, so
  // entries << skipped is normal - the question these answer is whether every
  // skipped instance belongs to a bucket that was restored at all.
  static std::atomic<uint32_t> s_bucketCacheSkippedInstances { 0u };
  static std::atomic<uint32_t> s_cachedBucketsRestored { 0u };
  static std::atomic<uint32_t> s_cachedBucketMemberInstances { 0u };
  static std::atomic<uint32_t> s_cachedBucketTlasEntries { 0u };
  static std::atomic<uint32_t> s_cachedBucketNoBlas { 0u };
  static std::atomic<uint32_t> s_restoreDetailBudget { 0u };
  // DX11_V382: its own budget, so the dynamic log cannot be starved by the merged
  // and cached-bucket logs sharing one pool.
  static std::atomic<uint32_t> s_dynamicDetailBudget { 0u };

  void AccelManager::requestBucketDetailDump(uint32_t lines) {
    s_bucketDetailBudget.store(lines, std::memory_order_relaxed);
    s_restoreDetailBudget.store(lines, std::memory_order_relaxed);
    s_dynamicDetailBudget.store(lines, std::memory_order_relaxed);
  }

  void AccelManager::fetchAndResetCacheRestoreStats(CacheRestoreStats& stats) {
    stats.skippedInstances = s_bucketCacheSkippedInstances.exchange(0u, std::memory_order_relaxed);
    stats.bucketsRestored = s_cachedBucketsRestored.exchange(0u, std::memory_order_relaxed);
    stats.memberInstances = s_cachedBucketMemberInstances.exchange(0u, std::memory_order_relaxed);
    stats.tlasEntries = s_cachedBucketTlasEntries.exchange(0u, std::memory_order_relaxed);
    stats.bucketsNoBlas = s_cachedBucketNoBlas.exchange(0u, std::memory_order_relaxed);
  }

  void AccelManager::fetchAndResetBlasRouteStats(BlasRouteStats& stats) {
    stats.mergedBuckets = s_mergedBuckets.exchange(0u, std::memory_order_relaxed);
    stats.mergedBucketInstances = s_mergedBucketInstances.exchange(0u, std::memory_order_relaxed);
    stats.mergedBucketsBuilt = s_mergedBucketsBuilt.exchange(0u, std::memory_order_relaxed);
    stats.mergedBucketsSkipped = s_mergedBucketsSkipped.exchange(0u, std::memory_order_relaxed);
    stats.mergedBucketNoBlas = s_mergedBucketNoBlas.exchange(0u, std::memory_order_relaxed);
    stats.dynamicBlasCount = s_dynamicBlasCount.exchange(0u, std::memory_order_relaxed);
    stats.dynamicBlasInstances = s_dynamicBlasInstances.exchange(0u, std::memory_order_relaxed);
  }

  void AccelManager::fetchAndResetTlasStats(uint32_t& hidden, uint32_t& gc,
                                            uint32_t& zeroMask, uint32_t& inTlas) {
    hidden = s_tlasHidden.exchange(0u, std::memory_order_relaxed);
    gc = s_tlasGc.exchange(0u, std::memory_order_relaxed);
    zeroMask = s_tlasZeroMask.exchange(0u, std::memory_order_relaxed);
    inTlas = s_tlasInTlas.load(std::memory_order_relaxed);
  }

  void AccelManager::fetchTlasContentStats(uint32_t& nullBlas, uint32_t& zeroXform) {
    nullBlas = s_tlasNullBlas.load(std::memory_order_relaxed);
    zeroXform = s_tlasZeroXform.load(std::memory_order_relaxed);
  }

  AccelManager::AccelManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    // Note: The scratch buffer's device address must be aligned to the minimum alignment required by the Vulkan runtime, otherwise
    //    // even if scratch allocation offsets are aligned they may add to a device address which will mess up this alignment (the alignment
    //    // requirement in Vulkan applies to the scratch buffer's device address, not just an offset as the name may imply). The lack of
    //    // this alignment override created issues on Intel GPUs where the min scratch alignment is 128 bytes but the underlying buffer was
    //    // only allocated with a 64 byte alignment.
    //    // Note: This could use the value of m_scratchAlignment, but this is duplicated to avoid potential future initialization order issues.
    , m_scratchAlignment(device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment) {
  }

  void AccelManager::clear() {
    m_blasPool.clear();

    // Invalidate incremental rebuild cache
    m_cachedBuckets.clear();
    m_instanceBucketIndex.clear();
    m_cachedDynamicBlasEntries.clear();
    resetUniqueDynamicBlasGroups();
    m_lastProcessedGeneration = UINT64_MAX;
    m_ommBindPending = false;
  }

  void AccelManager::resetUniqueDynamicBlasGroups() {
    for (uint32_t i = 0; i < m_uniqueDynamicBlasCount; ++i) {
      m_uniqueDynamicBlas[i].blasEntry = nullptr;
      m_uniqueDynamicBlas[i].instances.clear();
    }
    m_uniqueDynamicBlasCount = 0;
    m_uniqueDynamicBlasIndex.clear();
  }

  void AccelManager::removeInstanceFromBucketCache(RtInstance* instance) {
    if (m_instanceBucketIndex.erase(instance) == 0) {
      return;
    }

    m_cachedBuckets.clear();
    m_instanceBucketIndex.clear();
    m_lastProcessedGeneration = UINT64_MAX;
  }

  void AccelManager::garbageCollection() {
    // Can be configured per game: 'rtx.numFramesToKeepBLAS'
    // Note: keep the BLAS for at least two frames so that they're alive for previous-frame TLAS access.
    const uint32_t numFramesToKeepBLAS = std::max(RtxOptions::enablePreviousTLAS() ? 2u : 1u, RtxOptions::numFramesToKeepBLAS());

    // Remove instances past their lifetime or marked for GC explicitly
    const uint32_t currentFrame = m_device->getCurrentFrameId();

    // Remove all pooled BLAS that haven't been used for a few frames
    for (uint32_t i = 0; i < m_blasPool.size();) {
      Rc<PooledBlas>& blas = m_blasPool[i];

      if (blas->frameLastTouched + numFramesToKeepBLAS < currentFrame) {
        // Put this BLAS to the end of the vector
        std::swap(blas, m_blasPool.back());
        // Remove the last element
        m_blasPool.pop_back();
        continue;
      }
      ++i;
    }
  }
  
  void AccelManager::logMemoryOwnership(DrawCallCache& drawCalls, const char* reason, bool force) {
    if (!kenshi_telemetry::enabled()) return;
    using namespace vram_ownership;
    using Clock = std::chrono::steady_clock;
    static const AccelManager* owner = nullptr;
    static Clock::time_point lastSample;
    const auto start = Clock::now();
    if (!force && owner == this && start - lastSample < std::chrono::seconds(5)) return;
    owner = this;
    lastSample = start;
    const uint32_t frame = m_device->getCurrentFrameId();
    const uint32_t poolGrace = std::max(RtxOptions::enablePreviousTLAS() ? 2u : 1u, RtxOptions::numFramesToKeepBLAS());
    Census structures, buffers;
    std::unordered_map<const RtInstance*, uint32_t> instanceOwners;
    uint32_t seen = 0, retained = 0, unlinked = 0, oldLinked = 0, maxUnseenAge = 0;
    uint32_t linkedInstances = 0, hiddenInstances = 0;
    auto addBlas = [&](const Rc<PooledBlas>& blas, uint32_t mask) {
      if (blas != nullptr && blas->accelStructure != nullptr)
        structures.add(blas->accelStructure.ptr(), blas->accelStructure->info().size, mask);
    };
    auto addBuffer = [&](const Rc<DxvkBuffer>& buffer, uint32_t mask) {
      if (buffer != nullptr) buffers.add(buffer.ptr(), buffer->info().size, mask);
    };
    for (const auto& entry : drawCalls.getEntries()) {
      const auto& geo = entry.second;
      uint32_t youngest = UINT32_MAX;
      for (auto* instance : geo.getLinkedInstances()) {
        ++linkedInstances;
        hiddenInstances += instance->isHidden() ? 1 : 0;
        const auto* replacement = instance->getPrimInstanceOwner().getReplacementInstance();
        const auto unseen = age(frame, replacement ? replacement->frameLastSeen : instance->getFrameLastUpdated());
        instanceOwners[instance] = unseen <= 1 ? SeenGeometry : RetainedGeometry;
        youngest = std::min(youngest, unseen);
        maxUnseenAge = std::max(maxUnseenAge, unseen);
      }
      const uint32_t mask = geo.getLinkedInstances().empty() ? UnlinkedGeometry :
        youngest <= 1 ? SeenGeometry : RetainedGeometry;
      seen += mask == SeenGeometry;
      retained += mask == RetainedGeometry;
      unlinked += mask == UnlinkedGeometry;
      oldLinked += mask == RetainedGeometry && youngest > 600;
      addBlas(geo.dynamicBlas, mask);
      const auto& input = geo.input.getGeometryData();
      for (const auto* buffer : { &input.positionBuffer, &input.normalBuffer, &input.texcoordBuffer,
          &input.color0Buffer, &input.indexBuffer, &input.blendWeightBuffer, &input.blendIndicesBuffer,
          &input.kenshiPartMaskBuffer, &input.kenshiBloodProjectionBuffer })
        addBuffer(buffer->buffer(), mask);
      const auto& modified = geo.modifiedGeometryData;
      for (const auto* buffer : { &modified.positionBuffer, &modified.previousPositionBuffer,
          &modified.normalBuffer, &modified.texcoordBuffer, &modified.color0Buffer, &modified.indexBuffer })
        addBuffer(buffer->buffer(), mask);
      addBuffer(modified.historyBuffer[0], mask);
      addBuffer(modified.historyBuffer[1], mask);
      addBuffer(modified.indexCacheBuffer, mask);
    }
    for (const auto& blas : m_blasPool)
      addBlas(blas, age(frame, blas->frameLastTouched) <= poolGrace ? PoolRecent : PoolIdle);
    uint32_t bucketMixed = 0, bucketUnknownMembers = 0;
    for (const auto& bucket : m_cachedBuckets) {
      uint32_t members = 0;
      bool unknown = false;
      for (const auto* instance : bucket.instances) {
        // Lookup raw identities already validated by the geometry links. Do not
        // dereference a cache pointer merely to produce diagnostic output.
        const auto found = instanceOwners.find(instance);
        if (found == instanceOwners.end()) { ++bucketUnknownMembers; unknown = true; }
        else members |= found->second;
      }
      bucketMixed += members == (SeenGeometry | RetainedGeometry);
      // Any recently submitted member makes this a working-set bucket. Only call
      // it retained-only when every member was accounted for and none was seen.
      const uint32_t mask = members & SeenGeometry ? SeenGeometry :
        !unknown && members == RetainedGeometry ? RetainedGeometry : 0;
      addBlas(bucket.assignedBlas, CachedBucket | mask);
    }
    for (const auto& blas : m_activeDynamicBlases) addBlas(blas, ActiveDynamic);
    addBlas(m_intersectionBlas, Intersection);
    for (size_t i = 0; i < Tlas::Count; ++i) {
      const auto& tlas = m_device->getCommon()->getResources().getTLAS(static_cast<Tlas::Type>(i));
      if (tlas.accelStructure != nullptr)
        structures.add(tlas.accelStructure.ptr(), tlas.accelStructure->info().size, CurrentTlas);
      if (tlas.previousAccelStructure != nullptr)
        structures.add(tlas.previousAccelStructure.ptr(), tlas.previousAccelStructure->info().size, PreviousTlas);
    }
    // Masks are mutually exclusive rows; the bits describe overlapping roots, not additive totals.
    auto emit = [&](const Census& census, const char* kind) {
      struct Totals { uint64_t bytes = 0, largest = 0; uint32_t count = 0; };
      std::array<Totals, 1024> totals{};
      uint64_t allBytes = 0;
      for (const auto& object : census.objects) {
        const auto& value = object.second;
        auto& total = totals[value.owners];
        total.bytes += value.bytes;
        total.largest = std::max(total.largest, value.bytes);
        ++total.count;
        allBytes += value.bytes;
      }
      for (uint32_t mask = 1; mask < totals.size(); ++mask) {
        const auto& total = totals[mask];
        if (!total.count) continue;
        KENSHI_DIAGNOSTIC_INFO(str::format("[VramOwnership V777] frame=", frame, " reason=", reason,
          " kind=", kind, " owners=", mask, " objects=", total.count,
          " logicalKiB=", total.bytes >> 10, " largestKiB=", total.largest >> 10));
      }
      return allBytes;
    };
    const auto asBytes = emit(structures, "AS");
    const auto bufferBytes = emit(buffers, "geometryBuffer");
    KENSHI_DIAGNOSTIC_INFO(str::format("[VramOwnership V777] frame=", frame, " reason=", reason,
      " geometrySeen=", seen, " geometryRetained=", retained, " geometryUnlinked=", unlinked,
      " geometryUnseen600=", oldLinked, " linkedInstances=", linkedInstances,
      " hiddenInstances=", hiddenInstances, " maxUnseenFrames=", maxUnseenAge,
      " poolEntries=", m_blasPool.size(), " poolGraceFrames=", poolGrace,
      " bucketEntries=", m_cachedBuckets.size(), " activeDynamicEntries=", m_activeDynamicBlases.size(),
      " bucketMixed=", bucketMixed, " bucketUnknownMembers=", bucketUnknownMembers,
      " ownedAsMiB=", asBytes >> 20, " geometryBufferLogicalMiB=", bufferBytes >> 20,
      " scratchLogicalMiB=", m_scratchBuffer != nullptr ? m_scratchBuffer->info().size >> 20 : 0));
    m_device->getCommon()->memoryManager().logMemoryOwnership(frame, reason);
    KENSHI_DIAGNOSTIC_INFO(str::format("[VramOwnership V777] frame=", frame, " reason=", reason,
      " sampleUs=", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count()));
  }

  PooledBlas::PooledBlas() {
    ++g_blasCount;
    buildInfo.geometryCount = 0;
    buildInfo.pGeometries = nullptr;
  }

  PooledBlas::~PooledBlas() {
    if (buildInfo.pGeometries) {
      delete[] buildInfo.pGeometries;
      buildInfo.pGeometries = nullptr;
    }
    accelerationStructureReference = 0;
    accelStructure = nullptr;
    --g_blasCount;
  }

  // Keep a copy of the build info to validate it for potential updateability
  static void copyAccelerationStructureBuildGeometryInfo(const VkAccelerationStructureBuildGeometryInfoKHR& srcInfo, VkAccelerationStructureBuildGeometryInfoKHR& dstInfo)
  {
    const VkAccelerationStructureGeometryKHR* pGeometries = dstInfo.pGeometries;
    if (srcInfo.pGeometries) {
      if (srcInfo.geometryCount != dstInfo.geometryCount) {
        if (pGeometries) {
          delete[] pGeometries;
        }
        pGeometries = new VkAccelerationStructureGeometryKHR[srcInfo.geometryCount];
      }
      std::memcpy((void*) pGeometries, srcInfo.pGeometries, srcInfo.geometryCount * sizeof(VkAccelerationStructureGeometryKHR));

      dstInfo = srcInfo;
      dstInfo.pGeometries = pGeometries;
    }
  }

  uint32_t AccelManager::getBlasCount() {
    // Should never be negative, but just in case...
    return uint32_t(std::max(g_blasCount, 0));
  }

  bool AccelManager::BlasBucket::tryAddInstance(RtInstance* instance) {
    const uint8_t geometryInstanceMask = instance->getVkInstance().mask;
    const uint32_t geometryCustomIndexFlags = instance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
    const bool geometryUsesUnorderedApproximations = instance->usesUnorderedApproximations();
    const VkGeometryInstanceFlagsKHR geometryInstanceFlags = instance->getVkInstance().flags;
    const uint32_t geometryInstanceShaderBindingTableRecordOffset = instance->getVkInstance().instanceShaderBindingTableRecordOffset;

    if (!geometries.empty()) {
      if (instanceMask != geometryInstanceMask) {
        return false;
      }
      if (instanceShaderBindingTableRecordOffset != geometryInstanceShaderBindingTableRecordOffset) {
        return false;
      }
      if (customIndexFlags != geometryCustomIndexFlags) {
        return false;
      }
      if (instanceFlags != geometryInstanceFlags) {
        return false;
      }
      if (usesUnorderedApproximations != geometryUsesUnorderedApproximations) {
        return false;
      }
      if (hasSssInstances != instance->isSubsurface()) {
        return false;
      }
    }

    BlasEntry* blasEntry = instance->getBlas();

    geometries.insert(geometries.end(), blasEntry->buildGeometries.begin(), blasEntry->buildGeometries.end());
    ranges.insert(ranges.end(), blasEntry->buildRanges.begin(), blasEntry->buildRanges.end());

    for (auto& range : blasEntry->buildRanges) {
      originalInstances.push_back(instance);
      primitiveCounts.push_back(range.primitiveCount);
    }
    instanceBillboardIndices.insert(instanceBillboardIndices.end(), instance->billboardIndices.begin(), instance->billboardIndices.end());
    indexOffsets.insert(indexOffsets.end(), instance->indexOffsets.begin(), instance->indexOffsets.end());

    instanceShaderBindingTableRecordOffset = geometryInstanceShaderBindingTableRecordOffset;
    instanceMask = geometryInstanceMask;
    customIndexFlags = geometryCustomIndexFlags;
    instanceFlags = geometryInstanceFlags;
    usesUnorderedApproximations = geometryUsesUnorderedApproximations;
    hasSssInstances = instance->isSubsurface();
    return true;
  }

  static void fillGeometryInfoFromBlasEntry(BlasEntry& blasEntry, RtInstance& instance, const OpacityMicromapManager* opacityMicromapManager) {
    ScopedCpuProfileZone();
    blasEntry.buildGeometries.clear();
    blasEntry.buildRanges.clear();
    instance.billboardIndices.clear();
    instance.indexOffsets.clear();
    instance.clearBillboardGeometryDirty();

    const bool usesIndices = blasEntry.modifiedGeometryData.usesIndices();

    // Associate each billboard with a unique geometry entry
    // ToDo: get rid of usesIndices requirement, it's not needed to build OMMs. It's only used below
    if (usesIndices && 
        opacityMicromapManager &&
        opacityMicromapManager->isActive() &&
        OpacityMicromapManager::usesOpacityMicromap(instance) &&
        OpacityMicromapManager::usesSplitBillboardOpacityMicromap(instance)) {

      VkAccelerationStructureGeometryKHR geometry = {};
      geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.flags = instance.getGeometryFlags();

      VkAccelerationStructureGeometryTrianglesDataKHR& triangleData = geometry.geometry.triangles;
      triangleData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      triangleData.indexData.deviceAddress = blasEntry.modifiedGeometryData.indexBuffer.getDeviceAddress();
      triangleData.indexType = blasEntry.modifiedGeometryData.indexBuffer.indexType();
      triangleData.vertexData.deviceAddress = blasEntry.modifiedGeometryData.positionBuffer.getDeviceAddress() + blasEntry.modifiedGeometryData.positionBuffer.offsetFromSlice();
      triangleData.vertexStride = blasEntry.modifiedGeometryData.positionBuffer.stride();
      triangleData.vertexFormat = blasEntry.modifiedGeometryData.positionBuffer.vertexFormat();
      triangleData.maxVertex = blasEntry.modifiedGeometryData.vertexCount - 1;

      assert((blasEntry.modifiedGeometryData.calculatePrimitiveCount() & 1) == 0);
      VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
      buildRange.primitiveCount = 2;

      for (uint32_t billboardIndex = 0; billboardIndex < instance.getBillboardCount(); billboardIndex++) {
        const uint32_t kNumIndicesPerBillboardQuad = buildRange.primitiveCount * 3;
        buildRange.primitiveOffset = (billboardIndex * kNumIndicesPerBillboardQuad * blasEntry.modifiedGeometryData.indexBuffer.stride());
        blasEntry.buildGeometries.push_back(geometry);
        blasEntry.buildRanges.push_back(buildRange);
        instance.billboardIndices.push_back(billboardIndex);
        instance.indexOffsets.push_back(billboardIndex * kNumIndicesPerBillboardQuad);
      }
    } else {
      VkAccelerationStructureGeometryKHR geometry = {};

      geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.flags = instance.getGeometryFlags();

      VkAccelerationStructureGeometryTrianglesDataKHR& triangleData = geometry.geometry.triangles;

      const bool usesIndices = blasEntry.modifiedGeometryData.usesIndices();

      triangleData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;

      if (usesIndices) {
        triangleData.indexData.deviceAddress = blasEntry.modifiedGeometryData.indexBuffer.getDeviceAddress();
        triangleData.indexType = blasEntry.modifiedGeometryData.indexBuffer.indexType();
      } else {
        triangleData.indexData.deviceAddress = 0;
        triangleData.indexType = VK_INDEX_TYPE_NONE_KHR;
      }

      triangleData.vertexData.deviceAddress = blasEntry.modifiedGeometryData.positionBuffer.getDeviceAddress() + blasEntry.modifiedGeometryData.positionBuffer.offsetFromSlice();
      triangleData.vertexStride = blasEntry.modifiedGeometryData.positionBuffer.stride();
      triangleData.vertexFormat = blasEntry.modifiedGeometryData.positionBuffer.vertexFormat();
      triangleData.maxVertex = blasEntry.modifiedGeometryData.vertexCount - 1;

      VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
      buildRange.primitiveCount = blasEntry.modifiedGeometryData.calculatePrimitiveCount();
      buildRange.primitiveOffset = 0;

      blasEntry.buildGeometries.push_back(geometry);
      blasEntry.buildRanges.push_back(buildRange);
      instance.billboardIndices.push_back(0);
      instance.indexOffsets.push_back(0);
    }
  }
  int AccelManager::getCurrentFramePrimitiveIDPrefixSumBufferID() const {
    return m_device->getCurrentFrameId() & 0x1;
  }

  uint32_t additionalAccelerationStructureFlags() {
    return RtxOptions::lowMemoryGpu() ? VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR : 0;
  }

  void AccelManager::createAndBuildIntersectionBlas(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers) {
    if (m_intersectionBlas.ptr()) {
      return;
    }

    VkAccelerationStructureGeometryKHR geometry {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.pNext = nullptr;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = additionalAccelerationStructureFlags();
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = VK_NULL_HANDLE;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.ppGeometries = nullptr;

    uint32_t maxPrimitiveCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &maxPrimitiveCount, &sizeInfo);

    m_intersectionBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Intersection");
    
    buildInfo.dstAccelerationStructure = m_intersectionBlas->accelStructure->getAccelStructure();

    VkAabbPositionsKHR aabbPositions = { -1.f, -1.f, -1.f, 1.f, 1.f, 1.f };

    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = sizeof(aabbPositions);

    m_aabbBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "AABB Buffer");

    // Both of these feed an acceleration-structure build by device address, so a
    // null is a GPU write/read at address 0 rather than a skipped build. The
    // scratch buffer is not allocated here - it is expected to already exist from
    // the main build path, which will not be true if that path bailed on its own
    // allocation failure this frame.
    if (m_aabbBuffer == nullptr || m_scratchBuffer == nullptr) {
      ONCE(Logger::err(
        "AccelManager: skipping intersection BLAS build - AABB or scratch buffer unavailable."));
      return;
    }

    // Note: don't use ctx->updateBuffer() because that will place the command on the InitBuffer, not ExecBuffer.
    ctx->getCommandList()->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, m_aabbBuffer->getBufferRaw(), m_aabbBuffer->getSliceHandle().offset, sizeof(aabbPositions), &aabbPositions);
    
    execBarriers.accessBuffer(
      m_aabbBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    execBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV);

    execBarriers.recordCommands(ctx->getCommandList());
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);

    geometry.geometry.aabbs.data.deviceAddress = m_aabbBuffer->getDeviceAddress();

    const size_t requiredScratchAllocSize = sizeInfo.buildScratchSize + m_scratchAlignment;
    // getScratchMemory returns null when a grow allocation fails. Dereferencing
    // it here would be a null deref; worse, a zero scratch address makes the
    // build write at gpuVA 0 and takes the device with it. Skip the build.
    const Rc<DxvkBuffer> intersectionScratch = getScratchMemory(requiredScratchAllocSize);

    if (intersectionScratch == nullptr) {
      ONCE(Logger::err(
        "AccelManager: skipping intersection BLAS build - no scratch memory."));
      return;
    }

    buildInfo.scratchData.deviceAddress = intersectionScratch->getDeviceAddress();
    assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

    VkAccelerationStructureBuildRangeInfoKHR buildRange {};
    buildRange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildRange = &buildRange;

    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildRange);

    execBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV);

    execBarriers.recordCommands(ctx->getCommandList());
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
  }

  Rc<DxvkBuffer> AccelManager::getScratchMemory(const size_t requiredScratchAllocSize) {
    if (m_scratchBuffer == nullptr || m_scratchBuffer->info().size < requiredScratchAllocSize) {
      DxvkBufferCreateInfo bufferCreateInfo {};
      bufferCreateInfo.size = requiredScratchAllocSize;
      bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      // Allocation can fail under memory pressure. Assigning the result directly
      // would also destroy a perfectly good existing buffer, turning a transient
      // shortage into a permanent null. Acceleration-structure builds reference
      // this by DEVICE ADDRESS, so a null here is not a skipped build - it is a
      // GPU write to address 0, which faults the device (write-invalid gpuVA=0x0).
      Rc<DxvkBuffer> grownScratch = m_device->createBuffer(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "BVH Scratch");

      if (grownScratch == nullptr) {
        ONCE(Logger::err(str::format(
          "AccelManager: BVH scratch allocation of ", requiredScratchAllocSize,
          " bytes failed; keeping the previous buffer and skipping builds that need more room "
          "rather than binding a null scratch address.")));
        return nullptr;
      }

      m_scratchBuffer = std::move(grownScratch);
    }

    return m_scratchBuffer;
  }

  Rc<PooledBlas> AccelManager::createPooledBlas(size_t bufferSize, const char* name) const {
    auto newBlas = new PooledBlas();

    DxvkBufferCreateInfo bufferCreateInfo {};
    bufferCreateInfo.size = bufferSize;
    bufferCreateInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    bufferCreateInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    bufferCreateInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    newBlas->accelStructure = m_device->createAccelStructure(bufferCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, name);

    newBlas->accelerationStructureReference = newBlas->accelStructure->getAccelDeviceAddress();

    return newBlas;
  }

  static void trackBlasBuildResources(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers, const BlasEntry* blasEntry) {
    ScopedCpuProfileZone();
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(blasEntry->modifiedGeometryData.positionBuffer.buffer());
    ctx->getCommandList()->trackResource<DxvkAccess::Read>(blasEntry->modifiedGeometryData.indexBuffer.buffer());

    execBarriers.accessBuffer(
      blasEntry->modifiedGeometryData.positionBuffer.getSliceHandle(),
      blasEntry->modifiedGeometryData.positionBuffer.buffer()->info().stages,
      blasEntry->modifiedGeometryData.positionBuffer.buffer()->info().access,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    execBarriers.accessBuffer(
      blasEntry->modifiedGeometryData.indexBuffer.getSliceHandle(),
      blasEntry->modifiedGeometryData.indexBuffer.buffer()->info().stages,
      blasEntry->modifiedGeometryData.indexBuffer.buffer()->info().access,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    execBarriers.recordCommands(ctx->getCommandList());
  }

  void AccelManager::mergeInstancesIntoBlas(Rc<DxvkContext> ctx, 
                                            DxvkBarrierSet& execBarriers, 
                                            const std::vector<TextureRef>& textures,
                                            const CameraManager& cameraManager,
                                            InstanceManager& instanceManager,
                                            OpacityMicromapManager* opacityMicromapManager) {
    terrain_profile::Scope sceneCpuMerge(terrain_profile::Stage::AccelMerge);
    ScopedGpuProfileZone(ctx, "buildBLAS");

    auto& instances = instanceManager.getInstanceTable();
    const uint32_t currentFrame = m_device->getCurrentFrameId();
    if ((kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal())) {
      for (const auto* instance : instances)
        KenshiFlickerTrace::add("candidate", str::format("updatedNow=",
          instance->getFrameLastUpdated() == currentFrame, " ",
          KenshiFlickerTrace::describeInstance(instance)));
    }

    // Refresh cached OMM use before eviction, including frames which skip all
    // BLAS binding. Apply resets before deciding which buckets can be reused.
    if (opacityMicromapManager) {
      for (const auto& bucket : m_cachedBuckets) {
        if (bucket.assignedBlas != nullptr)
          opacityMicromapManager->trackCachedBlasUse(ctx, bucket.assignedBlas->opacityMicromaps);
      }
      for (const auto& blas : m_activeDynamicBlases)
        opacityMicromapManager->trackCachedBlasUse(ctx, blas->opacityMicromaps);
      opacityMicromapManager->onFrameStart(ctx);
      if (opacityMicromapManager->consumeNeedsBlasRebuild()) {
        m_ommBindPending = true;
        instanceManager.notifySceneChanged();
      }
    }

    // --- Full-skip fast path ---
    // If no scene changes occurred since the last build, we can reuse all cached
    // BLAS/TLAS data and skip the expensive per-instance iteration, bucket merging,
    // and GPU BLAS builds.  Per-surface GPU data (transforms, previous-frame buffer
    // indices, surface mapping) still needs uploading because fields like
    // previousPositionBufferIndex and prevObjectToWorld are updated by draw-call
    // processing every frame without bumping m_sceneGeneration.
    {
      const uint64_t currentGeneration = instanceManager.getSceneGeneration();
      const bool sceneUnchanged = (currentGeneration == m_lastProcessedGeneration);

      if (sceneUnchanged && !m_ommBindPending && !m_reorderedSurfaces.empty()) {
        m_sceneUnchangedThisFrame = true;
        // Touch only merged BLASes represented in the cached scene. Keeping
        // unused pool entries young forever also pins their old OMM resources.
        for (auto& bucket : m_cachedBuckets) {
          if (bucket.assignedBlas != nullptr)
            bucket.assignedBlas->frameLastTouched = currentFrame;
        }
        // Touch dynamic BLAS
        for (auto& dynBlas : m_activeDynamicBlases) {
          dynBlas->frameLastTouched = currentFrame;
        }
        // Reassign surface indices (cleared by InstanceManager::resetSurfaceIndices at frame end)
        for (uint32_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
          m_reorderedSurfaces[i]->setSurfaceIndex(i);
        }
        // OMM still needs per-frame management
        if (opacityMicromapManager) {
          // Process deferred OMM candidates even when the scene is static
          opacityMicromapManager->processOmmCandidates(instanceManager, textures);
        }
        // Advance the prefix-sum last-frame snapshot so temporal lookups stay current
        m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame = m_reorderedSurfacesPrimitiveIDPrefixSum;
        // Upload per-surface GPU data (surface buffer, mapping buffer, prefix sums)
        uploadSurfaceData(ctx);
        // Continue building OMMs even when the scene is static — surface data must be
        // uploaded first since the GPU baking pass reads it.
        if (opacityMicromapManager && opacityMicromapManager->isActive()) {
          opacityMicromapManager->buildOpacityMicromaps(ctx, textures, cameraManager.getLastCameraCutFrameId());
          opacityMicromapManager->onBlasBuild(ctx);
          opacityMicromapManager->onFinishedBuilding();
        }
        // Revisit the scene next frame. Only buckets referencing a newly ready
        // OMM become dirty; option/reset invalidation still rebuilds everything.
        if (opacityMicromapManager && opacityMicromapManager->hasNewlyBuiltOmms()) {
          instanceManager.notifySceneChanged();
        }
        return;
      }

      // Scene has changed — record state and proceed to incremental rebuild
      m_lastProcessedGeneration = currentGeneration;
      m_sceneUnchangedThisFrame = false;
    }

    // --- Per-bucket dirty detection ---
    // Build a validity set of current instances for removal detection, then scan
    // each cached bucket.  A bucket is dirty if any of its instances was removed,
    // had a transform / material / geometry change, or if the BlasEntry was updated
    // this frame.  Otherwise the bucket is clean and can be fully restored from cache.
    // DX11_V341_NO_BUCKET_CACHE (diagnostic): the incremental path skips every
    // instance sitting in a "clean" cached bucket and restores its surfaces and
    // TLAS entries from cache afterwards. The geometry-hash debug view shows an
    // inversion that points straight at it: objects whose hash churns every
    // frame (windmill engine, some signs and doors) always take the FULL path
    // and render reliably, while objects with stable hashes (the buildings)
    // qualify for the cache and are the ones that flicker. Within a fixed
    // camera angle their captured vertices, their hash and their BLAS are all
    // constant - constant input, alternating output - which is the signature of
    // a reuse/restore path rather than anything that produces geometry.
    //
    // Rebuild every bucket every frame and see whether the flicker stops. Pure
    // cost, no correctness risk: this is the path the churning objects already
    // take successfully every frame.
    // REVERTED: disabling the cache did not change the flicker and actively
    // regressed the runtime - dynamic objects (character weapons) stopped
    // moving smoothly and began teleporting between fixed points, and both runs
    // ended in a freeze after vertex-explosion events. The cache is load-bearing
    // for per-frame instance updates in this fork, so it is not a free
    // diagnostic to switch off.
    static constexpr bool kUseBucketCache = true;
    const bool hasValidBucketCache = kUseBucketCache && !m_cachedBuckets.empty();
    std::vector<bool> bucketDirty;
    bool anyBucketDirty = false;

    if (hasValidBucketCache) {
      // Global OMM option/reset invalidation still dirties all buckets.
      if (m_ommBindPending) {
        bucketDirty.resize(m_cachedBuckets.size(), true);
        anyBucketDirty = true;
        m_ommBindPending = false;
      } else {
        std::unordered_set<RtInstance*> currentInstanceSet(instances.begin(), instances.end());
        bucketDirty.resize(m_cachedBuckets.size(), false);

        for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
          const auto& cachedBucket = m_cachedBuckets[bi];

          if (cachedBucket.instances.size() != cachedBucket.instanceCacheIdentities.size()) {
            bucketDirty[bi] = true;
            anyBucketDirty = true;
            continue;
          }

          for (size_t ii = 0; ii < cachedBucket.instances.size(); ++ii) {
            RtInstance* inst = cachedBucket.instances[ii];

            // Validity check MUST come first: if the cached instance is no longer
            // in the live set (per-instance GC, or any path that bypasses the
            // bucket-vector cleanup), the pointer is dangling and must not be
            // dereferenced. Mark the bucket dirty so it gets rebuilt without
            // touching the stale entry.
            if (currentInstanceSet.find(inst) == currentInstanceSet.end()) {
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              break;
            }

            if (inst->getCacheIdentity() != cachedBucket.instanceCacheIdentities[ii]) {
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              break;
            }

            const uint32_t customIndexFlags = inst->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
            const bool bucketKeyChanged =
              inst->getVkInstance().mask != cachedBucket.tlasInstance.mask ||
              inst->getVkInstance().instanceShaderBindingTableRecordOffset != cachedBucket.tlasInstance.instanceShaderBindingTableRecordOffset ||
              inst->getVkInstance().flags != cachedBucket.tlasInstance.flags ||
              customIndexFlags != cachedBucket.tlasInstance.instanceCustomIndex ||
              inst->usesUnorderedApproximations() != cachedBucket.isUnordered ||
              inst->isSubsurface() != cachedBucket.hasSssInstances;

            if (inst->isBlasDirty() ||
                inst->getBlas()->frameLastUpdated == currentFrame ||
                (opacityMicromapManager && opacityMicromapManager->hasNewlyReadyOmm(*inst, instanceManager)) ||
                bucketKeyChanged) {
              bucketDirty[bi] = true;
              anyBucketDirty = true;
              break;
            }
          }
        }
      }
    } else {
      // With no cached buckets, every bucket is built from scratch below, so
      // pending OMM binding invalidation is naturally consumed by the full pass.
      m_ommBindPending = false;
    }

    terrain_profile::Scope sceneCpuClassify(terrain_profile::Stage::AccelClassify);
    // Allocate the transform buffer
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;

    info.size = align(instances.size() * sizeof(VkTransformMatrixKHR), kBufferAlignment);

    if (m_transformBuffer == nullptr || info.size > m_transformBuffer->info().size) {
      // TODO: allocate with some spare space to make reallocations less frequent
      m_transformBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Transform Buffer");
      Logger::debug("DxvkRaytrace: Vulkan Transform Buffer Realloc");
    }

    std::vector<VkTransformMatrixKHR> instanceTransforms;
    instanceTransforms.reserve(instances.size());

    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> blasToBuild;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR*> blasRangesToBuild;

    blasToBuild.reserve(instances.size());
    blasRangesToBuild.reserve(instances.size());

    m_reorderedSurfaces.clear();
    m_reorderedSurfacesFirstIndexOffset.clear();
    m_pointInstancerBatches.clear();
    m_activeDynamicBlases.clear();
    memset(m_pointInstancerSlotsPerType, 0, sizeof(m_pointInstancerSlotsPerType));
    for (auto& mergedInst : m_mergedInstances) {
      mergedInst.clear();
    }

    if (instances.size() > CUSTOM_INDEX_SURFACE_MASK) {
      ONCE(Logger::err(str::format("DxvkRaytrace: instance count (", instances.size(),
        ") exceeds the maximum surface index (", CUSTOM_INDEX_SURFACE_MASK,
        ") representable in ", SURFACE_INDEX_BIT_COUNT, "-bit instanceCustomIndex field. "
        "Surfaces beyond the limit will alias index 0.")));
    }

    if (opacityMicromapManager) {
      // usesOpacityMicromap() depends on candidate processing for the current
      // OMM generation. Run it before BLAS routing so OMM binding decisions are
      // made with current-frame eligibility.
      if (opacityMicromapManager->isActive()) {
        opacityMicromapManager->processOmmCandidates(instanceManager, textures);
      }
    }

    std::vector<std::unique_ptr<BlasBucket>> blasBuckets;
    blasBuckets.reserve(instances.size());

    size_t totalScratchMemory = 0;

    // NOTE: Would like to use the BLAS Linked instances here, but that misses viewmodel and virtual instances.
    // Keep emission order deterministic by storing dynamic BLAS groups in first-seen instance order.
    resetUniqueDynamicBlasGroups();
    if (m_uniqueDynamicBlas.capacity() < instances.size()) {
      m_uniqueDynamicBlas.reserve(instances.size());
    }
    if (m_uniqueDynamicBlasIndex.bucket_count() < instances.size()) {
      m_uniqueDynamicBlasIndex.reserve(instances.size());
    }

    // Hash map for O(1) bucket lookup instead of O(buckets) linear search per instance
    std::unordered_map<BlasBucketKey, BlasBucket*, BlasBucketKeyHash> bucketMap;

    for (RtInstance* instance : instances) {
      if (instance->isHidden()) {
        kenshi_telemetry::add(s_tlasHidden, 1u);
        continue;
      }

      // Skip instances that are pending GC — their m_linkedBlas may be dangling
      // (e.g. persistent renderer-created clones whose source BLAS was freed).
      if (instance->isMarkedForGC()) {
        kenshi_telemetry::add(s_tlasGc, 1u);
        continue;
      }

      // If the instance has zero mask, do not build BLAS for it: no ray can intersect this instance.
      if (instance->getVkInstance().mask == 0) {
        
        bool needsOpacityMicromap = instance->isViewModelReference() && opacityMicromapManager;
        bool hasBillboards = instance->getBillboardCount() > 0;

        // OMM requests and billboards need a valid surface.
        // Particles on the player model generate valid billboards but their geometric instance mask is set to 0.
        if (needsOpacityMicromap || hasBillboards) {
          instance->setSurfaceIndex(m_reorderedSurfaces.size());

          m_reorderedSurfaces.push_back(instance);
          m_reorderedSurfacesFirstIndexOffset.push_back(0);
        }

        kenshi_telemetry::add(s_tlasZeroMask, 1u);
        continue;
      }

      // Find the blas entry for this instance early so we can check the dynamic/merged cache.
      BlasEntry* blasEntry = instance->getBlas();
      assert(blasEntry);

      // On the incremental path, skip instances that belong to a clean cached bucket.
      // Their surfaces and TLAS instances will be restored from cache after the loop.
      if (hasValidBucketCache) {
        auto bucketIdxIt = m_instanceBucketIndex.find(instance);
        if (bucketIdxIt != m_instanceBucketIndex.end() &&
            bucketIdxIt->second < bucketDirty.size() &&
            !bucketDirty[bucketIdxIt->second]) {
          // This instance is in a clean cached bucket — skip all per-instance work
          // DX11_V380_CACHE_ACCOUNTING: count it. This `continue` is the busiest
          // exit in the whole pipeline (29 census windows showed the routing loop
          // processing ZERO instances, i.e. every one skipped here) and nothing has
          // ever measured whether the restore below actually re-emits them.
          kenshi_telemetry::add(s_bucketCacheSkippedInstances);
          instance->clearBlasDirty();
          continue;
        }
      }

      // Optimization: skip fillGeometryInfoFromBlasEntry when the BlasEntry geometry
      // has not changed since the last build.  The cached buildGeometries/buildRanges
      // on the BlasEntry and the cached billboardIndices/indexOffsets on the instance
      // are still valid from the previous frame.
      const bool blasGeometryChanged = (blasEntry->frameLastUpdated == currentFrame);
      const bool billboardGeometryChanged = instance->isBillboardGeometryDirty();
      const bool usesSplitBillboardOmmGeometry =
        blasEntry->modifiedGeometryData.usesIndices() &&
        opacityMicromapManager &&
        opacityMicromapManager->isActive() &&
        OpacityMicromapManager::usesOpacityMicromap(*instance) &&
        OpacityMicromapManager::usesSplitBillboardOpacityMicromap(*instance);
      const uint32_t expectedGeometryCount = usesSplitBillboardOmmGeometry ? instance->getBillboardCount() : 1u;
      const VkDeviceAddress currentVertexAddress =
        blasEntry->modifiedGeometryData.positionBuffer.getDeviceAddress()
        + blasEntry->modifiedGeometryData.positionBuffer.offsetFromSlice();
      const VkDeviceAddress currentIndexAddress =
        blasEntry->modifiedGeometryData.usesIndices()
          ? blasEntry->modifiedGeometryData.indexBuffer.getDeviceAddress()
          : 0u;
      const bool cachedAddressesMatch = !blasEntry->buildGeometries.empty()
        && blasEntry->buildGeometries[0].geometry.triangles.vertexData.deviceAddress
             == currentVertexAddress
        && blasEntry->buildGeometries[0].geometry.triangles.indexData.deviceAddress
             == currentIndexAddress;
      const bool hasCachedGeometryInfo =
        blasEntry->buildGeometries.size() == expectedGeometryCount &&
        blasEntry->buildRanges.size() == expectedGeometryCount &&
        instance->billboardIndices.size() == expectedGeometryCount &&
        instance->indexOffsets.size() == expectedGeometryCount &&
        cachedAddressesMatch;
      if (blasGeometryChanged || billboardGeometryChanged || !hasCachedGeometryInfo) {
        if (!cachedAddressesMatch && !blasEntry->buildGeometries.empty()) {
          static uint32_t sStaleGeometryAddressLogCount = 0;
          if (kenshi_telemetry::enabled() && sStaleGeometryAddressLogCount < 16u) {
            ++sStaleGeometryAddressLogCount;
            Logger::warn(str::format(
              "[RTX][BLAS] refreshing stale geometry device addresses before build: cachedVertex=0x",
              std::hex,
              blasEntry->buildGeometries[0].geometry.triangles.vertexData.deviceAddress,
              " currentVertex=0x", currentVertexAddress,
              " cachedIndex=0x",
              blasEntry->buildGeometries[0].geometry.triangles.indexData.deviceAddress,
              " currentIndex=0x", currentIndexAddress,
              std::dec));
          }
        }
        fillGeometryInfoFromBlasEntry(*blasEntry, *instance, opacityMicromapManager);
      }

      const uint32_t minPrimsInDynamicBLAS = std::max(RtxOptions::minPrimsInDynamicBLAS(), 100u);
      const uint32_t maxPrimsForMergedBLAS = RtxOptions::maxPrimsInMergedBLAS();
      const uint32_t blasPrims = blasEntry->modifiedGeometryData.calculatePrimitiveCount();

      // Figure out if this blas should be a dynamic one
      const bool requestDynamicBlas = instance->surface.instancesToObject != nullptr ||    // Point instancer geometry is replicated many times in a scene, we want to reuse the BLAS memory for these objects
                                      blasEntry->input.getSkinningState().numBones != 0 || // Skinned meshes are always desirable to give a dynamic BLAS, since we'll want to make use of BVH update for performance reasons
                                      blasEntry->getLinkedInstances().size() > 1  ||       // Meshes that are used in instances multiple times should benefit from BLAS reuse
                                      blasEntry->dynamicBlas != nullptr ||                 // If we already have a dynamic BLAS, keep using it.
                                      blasPrims > maxPrimsForMergedBLAS ||                 // Avoid large meshes ending up in the merged BLAS which is built every frame.  # prims is proportional to build cost.
                                      RtxOptions::minimizeBlasMerging();                   // Option to attempt putting as many objects into dynamic BLAS as possible.

      const bool forceMergedBlas = (blasEntry->buildGeometries.size() > 1 ||                                       // Currently we use multiple build geometries for particle billboards, which we prefer to merge into large BLAS
                                    (!RtxOptions::minimizeBlasMerging() && blasPrims < minPrimsInDynamicBLAS) ||   // Avoid creating lots of small dynamic BLAS
                                    RtxOptions::forceMergeAllMeshes()) &&                                          // Setting to force all meshes into the merged BLAS
                                      instance->surface.instancesToObject == nullptr;                              // Never merge point instancer geometry

      if (requestDynamicBlas && !forceMergedBlas) {
        // Since this loop is iterating over instances, and instances can share BLAS, we will build these later after identifying unique ones.
        auto uniqueBlasIter = m_uniqueDynamicBlasIndex.find(blasEntry);
        if (uniqueBlasIter == m_uniqueDynamicBlasIndex.end()) {
          const uint32_t uniqueBlasIdx = m_uniqueDynamicBlasCount++;
          uniqueBlasIter = m_uniqueDynamicBlasIndex.emplace(blasEntry, uniqueBlasIdx).first;

          if (uniqueBlasIdx == m_uniqueDynamicBlas.size()) {
            m_uniqueDynamicBlas.push_back({});
          }

          m_uniqueDynamicBlas[uniqueBlasIdx].blasEntry = blasEntry;
        }

        m_uniqueDynamicBlas[uniqueBlasIter->second].instances.push_back(instance);
        kenshi_telemetry::add(s_dynamicBlasInstances, 1u);
      } else {
        kenshi_telemetry::add(s_mergedBucketInstances, 1u);
        // Make sure we don't double up on blas entries, this should only happen if theres a bug
        // TODO (REMIX-3996) will break the assumptions we make here about all instances in a BlasEntry having the same instancesToObject array
        assert(m_uniqueDynamicBlasIndex.find(blasEntry) == m_uniqueDynamicBlasIndex.end());

        if (blasEntry->dynamicBlas != nullptr) {
          // Move the BLAS used by this geometry to the common pool.
          // This also ensures the dynamic blas resource that's still being used by previous TLAS is properly tracked for the next frame
          m_blasPool.push_back(std::move(blasEntry->dynamicBlas));
          blasEntry->dynamicBlas = nullptr;
        }

        // Calculate the device address for the current instance's transform and write the transform data
        // TODO: only do this for non-identity transforms
        VkDeviceAddress transformDeviceAddress = m_transformBuffer->getDeviceAddress() + instanceTransforms.size() * sizeof(VkTransformMatrixKHR);
        instanceTransforms.push_back(instance->getVkInstance().transform);

        for (auto& geometry : blasEntry->buildGeometries) {
          geometry.geometry.triangles.transformData.deviceAddress = transformDeviceAddress;
        }

        // Try to merge the instance into a compatible bucket using O(1) hash lookup
        BlasBucketKey bucketKey = {};
        bucketKey.instanceMask = instance->getVkInstance().mask;
        bucketKey.instanceShaderBindingTableRecordOffset = instance->getVkInstance().instanceShaderBindingTableRecordOffset;
        bucketKey.customIndexFlags = instance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
        bucketKey.instanceFlags = instance->getVkInstance().flags;
        bucketKey.usesUnorderedApproximations = instance->usesUnorderedApproximations();
        bucketKey.isSubsurface = instance->isSubsurface();

        bool merged = false;
        auto bucketIt = bucketMap.find(bucketKey);
        if (bucketIt != bucketMap.end()) {
          merged = bucketIt->second->tryAddInstance(instance);
        }

        // The instance couldn't be merged into any bucket - make a new one
        if (!merged) {
          auto newBucket = std::make_unique<BlasBucket>();
          merged = newBucket->tryAddInstance(instance);
          assert(merged);

          bucketMap[bucketKey] = newBucket.get();
          blasBuckets.push_back(std::move(newBucket));
        }

        // Track the lifetime and states of the source geometry buffers
        trackBlasBuildResources(ctx, execBarriers, blasEntry);
      }
    }

    sceneCpuClassify.finish();
    terrain_profile::Scope sceneCpuDynamic(terrain_profile::Stage::AccelDynamic);
    // Build/Update the dynamic BLAS
    for (uint32_t uniqueBlasIdx = 0; uniqueBlasIdx < m_uniqueDynamicBlasCount; ++uniqueBlasIdx) {
      const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
      BlasEntry* blasEntry = uniqueBlasEntry.blasEntry;
      if (uniqueBlasEntry.instances.size() == 0) {
        continue;
      }
      assert(blasEntry->buildGeometries.size() == 1); // dynamic BLAS should always have this
      assert(blasEntry->buildRanges.size() == 1); // dynamic BLAS should always have this

      bool forceRebuild = false;
      XXH64_hash_t boundOpacityMicromapHash = kEmptyHash;
      OpacityMicromapBinding ommBinding;
      if (opacityMicromapManager) {
        // Check validity of a built BLAS, only if:
        // We can only support OMM on dynamic BLAS whos surface is unique to that BLAS.  This is so we can benefit from instancing BLAS memory.  
        // In cases where there are multiple linked instances each with different surfaces OMM would break.
        // Multiple instances may share an OMM when every source signature agrees.
        bool ommsCompatible = true;
        const XXH64_hash_t firstOmmHash = OpacityMicromapManager::getOpacityMicromapHash(*uniqueBlasEntry.instances[0]);
        for (uint32_t i = 1; i < uniqueBlasEntry.instances.size(); i++) {
          const XXH64_hash_t thisOmmHash = OpacityMicromapManager::getOpacityMicromapHash(*uniqueBlasEntry.instances[i]);
          if (thisOmmHash != firstOmmHash) {
            ommsCompatible = false;
            break;
          }
        }

        if (ommsCompatible) {
          RtInstance* exemplarInstance = uniqueBlasEntry.instances[0];

          // Bind opacity micromap
          // Opacity micromaps must be bound before acceleration sizes are calculated
          // Note: since opacity micromaps for this frame are scheduled later 
          //       this will only pickup Opacity Micromaps built in previous frames
          boundOpacityMicromapHash = opacityMicromapManager->tryBindOpacityMicromap(ctx, *exemplarInstance, 0, blasEntry->buildGeometries[0], instanceManager);
          ommBinding = opacityMicromapManager->getBlasBinding(boundOpacityMicromapHash);

          if (blasEntry->dynamicBlas.ptr()) {
            // A previously built BLAS needs to be rebuild if a corresponding Opacity Micromap availability has changed
            forceRebuild = boundOpacityMicromapHash != blasEntry->dynamicBlas->opacityMicromapSourceHash;
          }
        } else if (blasEntry->dynamicBlas.ptr() && blasEntry->dynamicBlas->opacityMicromapSourceHash != kEmptyHash) {
          // If we had a OMM bound at some point, but now that OMM is invalid, force a rebuild
          forceRebuild = true;
          // Clear stale OMM binding from cached geometry data
          blasEntry->buildGeometries[0].geometry.triangles.pNext = nullptr;
        }
      } else {
        // No OMM manager — clear any stale OMM binding from cached geometry data
        blasEntry->buildGeometries[0].geometry.triangles.pNext = nullptr;
      }

      // Content hashes may repeat after a cache reset; the built BLAS must also
      // match the actual resource, including removal of its last OMM binding.
      if (blasEntry->dynamicBlas != nullptr) {
        const auto& previousBindings = blasEntry->dynamicBlas->opacityMicromaps;
        const auto* previousResource = previousBindings.empty() ? nullptr : previousBindings[0].resource.ptr();
        forceRebuild |= previousResource != ommBinding.resource.ptr();
      }

      // DX11_V378_STALE_BLAS_TRANSFORM: a dynamic BLAS must be built with NO
      // pre-transform. Its per-instance placement is carried by the TLAS instance
      // transform, so `transformData` has to be NULL here.
      //
      // It is not necessarily NULL on arrival. `transformData.deviceAddress` is
      // written in exactly ONE place - the merged-bucket path below - and is never
      // cleared. The only thing that would zero it is fillGeometryInfoFromBlasEntry,
      // which rebuilds buildGeometries from scratch, and that call is SKIPPED
      // whenever the geometry has not changed (always true for a static mesh).
      //
      // So a BlasEntry that ever passed through the merged path carries a non-NULL
      // transformData into every later dynamic build, pointing at a slot in that
      // frame's transform buffer which has since been recycled for some other
      // instance. The AS build then pre-transforms the vertices by an unrelated
      // matrix while the TLAS instance applies the real one on top.
      //
      // This bites geometry that OSCILLATES between the two paths. Routing is
      // decided by `blasPrims > maxPrimsForMergedBLAS` and
      // `getLinkedInstances().size() > 1`, so a small mesh with a varying instance
      // count goes merged in some frames and dynamic in others. Kenshi's wall
      // segments are exactly that: 581 primitives (below minPrimsInDynamicBLAS) with
      // a linked-instance count measured at 384, 56 and 14 across runs.
      //
      // Per the Vulkan spec this is also a hard UPDATE-mode requirement: if
      // transformData was NULL for the source build "then it must be NULL", and if
      // it was non-NULL "then it must not be NULL". validateUpdateMode did not test
      // it (see the check added there).
      for (auto& dynamicGeometry : blasEntry->buildGeometries) {
        if (dynamicGeometry.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR)
          dynamicGeometry.geometry.triangles.transformData.deviceAddress = 0ull;
      }

      VkAccelerationStructureBuildGeometryInfoKHR buildInfo {};
      buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | additionalAccelerationStructureFlags();
      buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      buildInfo.geometryCount = 1;
      buildInfo.pGeometries = blasEntry->buildGeometries.data();

      // Calculate the build sizes for this bucket
      VkAccelerationStructureBuildSizesInfoKHR sizeInfo {};
      sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                               &buildInfo, &blasEntry->buildRanges[0].primitiveCount, &sizeInfo);

      // Try to reuse our dynamic BLAS if it exists
      Rc<PooledBlas>& selectedBlas = blasEntry->dynamicBlas;

      bool build = forceRebuild || !selectedBlas.ptr() || selectedBlas->accelStructure->info().size != sizeInfo.accelerationStructureSize;

      // Validate that the selected blas is compatible with the current build info for update purposes
      bool update = blasEntry->frameLastUpdated == currentFrame;
      // DX11_V378_STALE_BLAS_TRANSFORM: primitiveCount must also match the source
      // build (VUID-vkCmdBuildAccelerationStructuresKHR-primitiveCount-03769), and
      // validateUpdateMode structurally cannot check it - primitiveCount lives in
      // VkAccelerationStructureBuildRangeInfoKHR, not in the geometry info it is
      // given. The merged path compensates with its own
      // `selectedBlas->primitiveCounts == bucket->primitiveCounts` comparison; this
      // path had no equivalent, so bring it to parity.
      const std::vector<uint32_t> currentPrimitiveCounts {
        blasEntry->buildRanges[0].primitiveCount };
      if (update && !build
       && (!validateUpdateMode(selectedBlas->buildInfo, buildInfo)
        || selectedBlas->primitiveCounts != currentPrimitiveCounts)) {
        // If an update is requested but the BLAS is not compatible with the current build info then force a rebuild
        update = false;
        build = true;
      }

      // There is no such BLAS - create one
      if (build) {
        if (selectedBlas.ptr()) {
          // Move the BLAS used by this geometry to the common pool.
          // This also ensures the dynamic blas resource that's still being used by previous TLAS is properly tracked for the next frame
          m_blasPool.push_back(std::move(selectedBlas));
        }
        selectedBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Dynamic");
      }

      assert(selectedBlas.ptr());
      selectedBlas->frameLastTouched = currentFrame;
      blasEntry->dynamicBlas->opacityMicromapSourceHash = boundOpacityMicromapHash;

      if (update || build) {
        selectedBlas->opacityMicromaps.clear();
        if (ommBinding.resource != nullptr)
          selectedBlas->opacityMicromaps.push_back(ommBinding);
        if (update && !build) {
          buildInfo.srcAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();
          buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        }
        // Use the selected BLAS for the build
        buildInfo.dstAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();

        // Allocate a scratch buffer slice
        const size_t requiredScratchAllocSize = align(sizeInfo.buildScratchSize + m_scratchAlignment, m_scratchAlignment);
        buildInfo.scratchData.deviceAddress = totalScratchMemory;
        totalScratchMemory += requiredScratchAllocSize;

        assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

        // Track the lifetime of the BLAS buffers
        ctx->getCommandList()->trackResource<DxvkAccess::Write>(selectedBlas->accelStructure);

        // Put the merged BLAS into the build queue
        blasToBuild.push_back(buildInfo);
        blasRangesToBuild.push_back(&blasEntry->buildRanges[0]);

        // DX11_V382_DYNAMIC_BLAS_LOG: the dynamic path had NO per-entry logging, and
        // it is the only population that can hold Kenshi's 1743-vertex wall segment.
        //
        // Both bucket logs are structurally blind to it: `[blas-route]` only sees
        // dirty/new MERGED buckets, and `[blas-restore]` only sees clean cached ones.
        // Reasoning from the wall's absence in those two logs produced one wrong
        // conclusion in each direction ("it must be merged", then "it must be
        // dynamic"). This is the measurement that settles it, and it also carries the
        // fields needed to explain blasRef=0 on half the wall instances: dynamic
        // instances get their acceleration-structure reference from addBlas(), so a
        // null reference here means either this loop never ran for them or the
        // reference was zero when it did.
        copyAccelerationStructureBuildGeometryInfo(buildInfo, selectedBlas->buildInfo);
        // DX11_V378_STALE_BLAS_TRANSFORM: record what this BLAS was actually built
        // with, so the primitiveCount comparison above has something true to test
        // against next frame. PooledBlas::primitiveCounts was previously written
        // only by the merged path, and pooled BLASes are recycled between the two
        // paths - so on this path it was either empty or left over from a previous
        // MERGED use of the same pooled structure.
        selectedBlas->primitiveCounts = currentPrimitiveCounts;
      }

      // DX11_V382_DYNAMIC_BLAS_LOG: the dynamic path had NO per-entry logging, and it
      // is the only population that can hold Kenshi's 1743-vertex wall segment.
      //
      // Both bucket logs are structurally blind to it: `[blas-route]` only sees
      // dirty/new MERGED buckets, and `[blas-restore]` only sees clean cached ones.
      // Reasoning from the wall's absence in those two logs produced one wrong
      // conclusion in each direction ("it must be merged", then "it must be
      // dynamic"). This is the measurement that settles it.
      //
      // Placed OUTSIDE the `if (update || build)` block on purpose, so a dynamic BLAS
      // that is reused without any rebuild - the case most likely to be at fault -
      // still reports. It also carries what is needed to explain blasRef=0 on half
      // the wall instances: dynamic instances take their acceleration-structure
      // reference from addBlas() just below, so a null reference here means the
      // reference was already zero when this loop ran.
      {
        uint32_t dynamicDetailBudget = s_dynamicDetailBudget.load(std::memory_order_relaxed);
        if (kenshi_telemetry::enabled() && dynamicDetailBudget > 0u) {
          s_dynamicDetailBudget.store(dynamicDetailBudget - 1u, std::memory_order_relaxed);
          const RtInstance* firstDynInst = uniqueBlasEntry.instances.empty()
            ? nullptr : uniqueBlasEntry.instances.front();
          const VkTransformMatrixKHR* dynXform =
            firstDynInst != nullptr ? &firstDynInst->getVkInstance().transform : nullptr;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX][blas-dynamic] verts=", blasEntry->modifiedGeometryData.vertexCount,
            " idx=", blasEntry->modifiedGeometryData.indexCount,
            " prims=", blasEntry->buildRanges.empty()
              ? 0u : blasEntry->buildRanges[0].primitiveCount,
            " ranges=", blasEntry->buildRanges.size(),
            " instances=", uniqueBlasEntry.instances.size(),
            " mode=", build ? "build" : (update ? "update" : "reused"),
            " blasRef=", selectedBlas.ptr() != nullptr
              && selectedBlas->accelerationStructureReference != 0ull ? 1 : 0,
            " pointInstancer=", firstDynInst != nullptr
              && firstDynInst->surface.instancesToObject != nullptr ? 1 : 0,
            " firstXformT=[",
            dynXform != nullptr ? dynXform->matrix[0][3] : 0.f, ",",
            dynXform != nullptr ? dynXform->matrix[1][3] : 0.f, ",",
            dynXform != nullptr ? dynXform->matrix[2][3] : 0.f, "]"));
        }
      }

      for (RtInstance* rtInstance : uniqueBlasEntry.instances) {
        // Append an instance of this merged BLAS to the merged instance list
        if (rtInstance->surface.instancesToObject == nullptr) {
          addBlas(rtInstance, blasEntry, nullptr);
        } else {
          addPointInstancerBlas(rtInstance, blasEntry);
        }
      }

      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blasEntry->dynamicBlas->accelStructure);

      // Record this dynamic BLAS so the full-skip cache path can touch it to prevent GC
      m_activeDynamicBlases.push_back(blasEntry->dynamicBlas);

      // Track the lifetime and states of the source geometry buffers
      trackBlasBuildResources(ctx, execBarriers, blasEntry);
    }

    sceneCpuDynamic.finish();
    terrain_profile::Scope sceneCpuRestore(terrain_profile::Stage::AccelRestore);
    // Copy the instance transform data to the device (only needed on full rebuild path;
    // dynamics-only path doesn't populate instanceTransforms for merged instances)
    if (instanceTransforms.size() > 0) {
      ctx->writeToBuffer(m_transformBuffer, 0, instanceTransforms.size() * sizeof(VkTransformMatrixKHR), instanceTransforms.data());

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_transformBuffer);
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transformBuffer);

      // Place a barrier on the transform buffer
      DxvkBufferSliceHandle transformBufferSlice;
      transformBufferSlice.handle = m_transformBuffer->getBufferRaw();
      execBarriers.accessBuffer(
        transformBufferSlice,
        m_transformBuffer->info().stages,
        m_transformBuffer->info().access,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_SHADER_READ_BIT);
    }

    // --- Restore clean cached buckets and collect surfaces ---
    // Clean cached buckets: restore surfaces + TLAS instances directly, touch BLAS.
    // Dirty/new buckets: their surfaces were already added by the main loop via
    // the bucket pipeline; their TLAS instances will be emitted by createBlasBuffersAndInstances.
    if (hasValidBucketCache) {
      for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
        if (bucketDirty[bi]) {
          continue; // Dirty bucket — its instances went through the normal pipeline above
        }
        auto& cached = m_cachedBuckets[bi];

        // Restore surfaces from this clean bucket
        const uint32_t surfaceOffset = static_cast<uint32_t>(m_reorderedSurfaces.size());
        m_reorderedSurfaces.insert(m_reorderedSurfaces.end(),
                                   cached.surfaces.begin(), cached.surfaces.end());
        m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(),
                                                   cached.indexOffsets.begin(), cached.indexOffsets.end());

        // Touch the BLAS so GC doesn't collect it
        cached.assignedBlas->frameLastTouched = currentFrame;

        // Emit TLAS instance with updated surface offset
        auto tlasInst = cached.tlasInstance;
        tlasInst.instanceCustomIndex =
          (tlasInst.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
          (surfaceOffset & uint32_t(CUSTOM_INDEX_SURFACE_MASK));

        if (cached.isUnordered && RtxOptions::enableSeparateUnorderedApproximations()) {
          m_mergedInstances[Tlas::Unordered].push_back(tlasInst);
        } else {
          m_mergedInstances[Tlas::Opaque].push_back(tlasInst);
          if (cached.hasSssInstances) {
            m_mergedInstances[Tlas::SSS].push_back(tlasInst);
          }
        }

        // DX11_V380_CACHE_ACCOUNTING: this restore path was completely dark.
        //
        // The `[blas-route]` bucket log added earlier lives inside
        // createBlasBuffersAndInstances, which only ever sees DIRTY/NEW buckets - a
        // clean cached bucket structurally cannot appear in it. That blind spot is
        // what made Kenshi's wall segments look like they were absent from the
        // merged path and therefore "must be dynamic", an inference that was wrong.
        // The wall spends most frames in exactly this population.
        //
        // One entry is emitted here per BUCKET, covering all of its member
        // instances through the merged BLAS - so `restored` counting far below
        // `skipped` is expected and not a loss by itself. What would be a loss is
        // skipped instances belonging to NO restored bucket; the counters exist to
        // make that difference arithmetic instead of argument.
        kenshi_telemetry::add(s_cachedBucketsRestored);
        kenshi_telemetry::add(s_cachedBucketMemberInstances, uint32_t(cached.instances.size()));
        kenshi_telemetry::add(s_cachedBucketTlasEntries);
        if (cached.assignedBlas.ptr() == nullptr
         || cached.assignedBlas->accelerationStructureReference == 0ull)
          kenshi_telemetry::add(s_cachedBucketNoBlas);

        uint32_t restoreDetailBudget = s_restoreDetailBudget.load(std::memory_order_relaxed);
        if (kenshi_telemetry::enabled() && restoreDetailBudget > 0u) {
          s_restoreDetailBudget.store(restoreDetailBudget - 1u, std::memory_order_relaxed);
          // Member vertex counts identify WHICH meshes this bucket carries - the
          // wall segment is 1743, so its presence or absence here is the answer.
          std::string members;
          uint32_t listed = 0;
          for (const RtInstance* memberInst : cached.instances) {
            if (listed >= 8u) { members += " ..."; break; }
            const BlasEntry* memberBlas = memberInst != nullptr ? memberInst->getBlas() : nullptr;
            members += " " + std::to_string(memberBlas != nullptr
              ? memberBlas->modifiedGeometryData.vertexCount : 0u);
            ++listed;
          }
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX][blas-restore] cached bucket ", bi,
            " instances=", cached.instances.size(),
            " surfaces=", cached.surfaces.size(),
            " unordered=", cached.isUnordered ? 1 : 0,
            " blasRef=", cached.assignedBlas.ptr() != nullptr
              && cached.assignedBlas->accelerationStructureReference != 0ull ? 1 : 0,
            " mask=", uint32_t(cached.tlasInstance.mask),
            " memberVerts:", members));
        }
      }
    }

    // Collect surfaces from newly-built (dirty) buckets
    for (const auto& blasBucket : blasBuckets) {
      blasBucket->reorderedSurfacesOffset = static_cast<uint32_t>(m_reorderedSurfaces.size());
      m_reorderedSurfaces.insert(m_reorderedSurfaces.end(), blasBucket->originalInstances.begin(), blasBucket->originalInstances.end());
      m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(), blasBucket->indexOffsets.begin(), blasBucket->indexOffsets.end());
    }

    // Build prefix sum array
    // Collect primitive count for each surface object
    // Because we use exclusive prefix sum here, we add one more element to record the scene's total primitive count
    m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame = m_reorderedSurfacesPrimitiveIDPrefixSum;
    m_reorderedSurfacesPrimitiveIDPrefixSum.resize(m_reorderedSurfaces.size() + 1);
    m_reorderedSurfacesPrimitiveIDPrefixSum[0] = 0;
    for (uint32_t i = 0; i < m_reorderedSurfaces.size(); i++) {
      auto surface = m_reorderedSurfaces[i];
      int primitiveCount = 0;
      for (const auto& buildRange: surface->getBlas()->buildRanges) {
        primitiveCount += buildRange.primitiveCount;
      }
      m_reorderedSurfacesPrimitiveIDPrefixSum[i + 1] = primitiveCount;
    }

    // Calculate exclusive prefix sum
    uint totalPrimitiveIDOffset = 0;
    for (uint32_t i = 0; i < m_reorderedSurfacesPrimitiveIDPrefixSum.size(); i++) {
      uint primitiveCount = m_reorderedSurfacesPrimitiveIDPrefixSum[i];
      m_reorderedSurfacesPrimitiveIDPrefixSum[i] += totalPrimitiveIDOffset;
      totalPrimitiveIDOffset += primitiveCount;
    }

    // Validate total primitive count against the engine-wide PRIMITIVE_INDEX_BIT_COUNT limit.
    if (totalPrimitiveIDOffset > PRIMITIVE_INDEX_MAX_VALUE) {
      ONCE(Logger::err(str::format("DxvkRaytrace: total primitive count (", totalPrimitiveIDOffset,
        ") exceeds the maximum primitive index (", PRIMITIVE_INDEX_MAX_VALUE,
        ") representable in ", PRIMITIVE_INDEX_BIT_COUNT, " bits. "
        "Downstream systems (NEE cache, prefix-sum lookups) may produce incorrect results.")));
    }

    sceneCpuRestore.finish();
    buildBlases(ctx, execBarriers, cameraManager, opacityMicromapManager, instanceManager, 
                textures, instances, blasBuckets, blasToBuild, blasRangesToBuild, totalScratchMemory);

    // Schedule targeted OMM binding on the next incremental pass.
    if (opacityMicromapManager && opacityMicromapManager->hasNewlyBuiltOmms()) {
      instanceManager.notifySceneChanged();
    }

    // Save baseline counts (before billboards are appended in prepareSceneData).
    // The full-skip cache path and prepareSceneData both use these to avoid
    // duplicate billboard entries across frames.
    for (int t = 0; t < Tlas::Count; ++t) {
      m_mergedInstancesBaselineCount[t] = m_mergedInstances[t].size();
    }

    // --- Cache per-bucket state for future incremental rebuilds ---
    // Rebuild the cached bucket list: keep clean buckets as-is, replace dirty
    // buckets with fresh data from this frame's blasBuckets.
    {
      // Start with clean buckets from the previous cache
      std::vector<CachedBucketState> newCachedBuckets;
      m_instanceBucketIndex.clear();

      if (hasValidBucketCache) {
        for (uint32_t bi = 0; bi < m_cachedBuckets.size(); ++bi) {
          if (!bucketDirty[bi]) {
            const uint32_t newIdx = static_cast<uint32_t>(newCachedBuckets.size());
            // Map instances to the new bucket index
            for (RtInstance* inst : m_cachedBuckets[bi].instances) {
              m_instanceBucketIndex[inst] = newIdx;
            }
            newCachedBuckets.push_back(std::move(m_cachedBuckets[bi]));
          }
        }
      }

      // Add newly-built dirty buckets from this frame.  The PooledBlas was
      // recorded on each bucket by createBlasBuffersAndInstances.
      for (const auto& bucket : blasBuckets) {
        CachedBucketState cached;

        // Deduplicate instances list (billboard instances may repeat per build-geometry)
        for (RtInstance* inst : bucket->originalInstances) {
          if (cached.instances.empty() || cached.instances.back() != inst) {
            cached.instances.push_back(inst);
            cached.instanceCacheIdentities.push_back(inst->getCacheIdentity());
          }
        }
        cached.surfaces = bucket->originalInstances;
        cached.indexOffsets = bucket->indexOffsets;
        cached.isUnordered = bucket->usesUnorderedApproximations;
        cached.hasSssInstances = bucket->hasSssInstances;

        // Capture the assigned BLAS (stored on bucket by createBlasBuffersAndInstances)
        if (bucket->assignedBlas) {
          for (auto& pooledBlas : m_blasPool) {
            if (pooledBlas.ptr() == bucket->assignedBlas) {
              cached.assignedBlas = pooledBlas;
              break;
            }
          }
        }

        // Build TLAS instance template (surface offset will be adjusted when restored)
        cached.tlasInstance = {};
        if (cached.assignedBlas.ptr()) {
          cached.tlasInstance.accelerationStructureReference = cached.assignedBlas->accelerationStructureReference;
        }
        cached.tlasInstance.flags = bucket->instanceFlags;
        cached.tlasInstance.instanceShaderBindingTableRecordOffset = bucket->instanceShaderBindingTableRecordOffset;
        cached.tlasInstance.mask = bucket->instanceMask;
        cached.tlasInstance.instanceCustomIndex = bucket->customIndexFlags;
        static float identityTransform[3][4] = {
          { 1.f, 0.f, 0.f, 0.f },
          { 0.f, 1.f, 0.f, 0.f },
          { 0.f, 0.f, 1.f, 0.f }
        };
        memcpy(&cached.tlasInstance.transform, identityTransform, sizeof(VkTransformMatrixKHR));

        const uint32_t newIdx = static_cast<uint32_t>(newCachedBuckets.size());
        for (RtInstance* inst : cached.instances) {
          m_instanceBucketIndex[inst] = newIdx;
          inst->clearBlasDirty();
        }
        newCachedBuckets.push_back(std::move(cached));
      }

      m_cachedBuckets = std::move(newCachedBuckets);

      // Update the dynamic BlasEntry set for the full-skip path
      m_cachedDynamicBlasEntries.clear();
      s_dynamicBlasCount.store(m_uniqueDynamicBlasCount, std::memory_order_relaxed);
      for (uint32_t uniqueBlasIdx = 0; uniqueBlasIdx < m_uniqueDynamicBlasCount; ++uniqueBlasIdx) {
        const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
        m_cachedDynamicBlasEntries.insert(uniqueBlasEntry.blasEntry);
      }
    }
  }

  void AccelManager::addBlas(RtInstance* instance, BlasEntry* blasEntry, const Matrix4* instanceToObject) {
    // Create an instance for this BLAS
    VkAccelerationStructureInstanceKHR blasInstance = instance->getVkInstance();
    blasInstance.accelerationStructureReference = blasEntry->dynamicBlas->accelerationStructureReference;
    blasInstance.instanceCustomIndex =
      (blasInstance.instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
      uint32_t(m_reorderedSurfaces.size()) & uint32_t(CUSTOM_INDEX_SURFACE_MASK);

    if (instanceToObject) {
      // The D3D matrix on input, needs to be transposed before feeding to the VK API (left/right handed conversion)
      // NOTE: VkTransformMatrixKHR is 4x3 matrix, and Matrix4 is 4x4
      const Matrix4 transform = transpose(instance->surface.objectToWorld * (*instanceToObject));
      memcpy(&blasInstance.transform, &transform, sizeof(VkTransformMatrixKHR));
    }

    // Get the instance's flags and apply the objectToWorldMirrored flag.
    if (instance->isObjectToWorldMirrored()) {
      blasInstance.flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    }

    if (instance->usesUnorderedApproximations() && RtxOptions::enableSeparateUnorderedApproximations()) {
      m_mergedInstances[Tlas::Unordered].push_back(blasInstance);
    } else {
      m_mergedInstances[Tlas::Opaque].push_back(blasInstance);
      if (instance->isSubsurface()) {
        m_mergedInstances[Tlas::SSS].push_back(blasInstance);
      }
    }

    // Append the instance to the reordered surface list
    // Note: this happens *after* the instance is appended, because the size of m_reorderedSurfaces is used above
    m_reorderedSurfaces.push_back(instance);
    m_reorderedSurfacesFirstIndexOffset.push_back(0);
  }

  void AccelManager::createBlasBuffersAndInstances(Rc<DxvkContext> ctx, 
                                                   const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets,
                                                   std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                                                   std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                                                   size_t& totalScratchMemory) {
    terrain_profile::Scope sceneCpuBuffers(terrain_profile::Stage::AccelBuffers);

    const uint32_t currentFrame = m_device->getCurrentFrameId();

    struct BucketGeometryContentHashData {
      XXH64_hash_t vertexHash;
      XXH64_hash_t indexHash;
      XXH64_hash_t boneHash;
      VkTransformMatrixKHR transform;
      uint32_t primitiveCount;
      uint32_t pad;
    };
    static_assert(sizeof(BucketGeometryContentHashData) == 80, "BucketGeometryContentHashData must remain fully padded for stable hashing.");
    std::vector<BucketGeometryContentHashData> contentHashData;

    // Create or find a matching BLAS for each bucket, then build it
    for (const auto& bucket : blasBuckets) {
      // Fill out the build info
      VkAccelerationStructureBuildGeometryInfoKHR buildInfo {};
      buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | additionalAccelerationStructureFlags();
      buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      buildInfo.geometryCount = bucket->geometries.size();
      buildInfo.pGeometries = bucket->geometries.data();

      // Calculate the build sizes for this bucket
      VkAccelerationStructureBuildSizesInfoKHR sizeInfo {};
      sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      m_device->vkd()->vkGetAccelerationStructureBuildSizesKHR(m_device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                               &buildInfo, bucket->primitiveCounts.data(), &sizeInfo);

      // Try to find an existing BLAS that is minimally sufficient to fit this bucket of geometries
      PooledBlas* selectedBlas = nullptr;
      for (const auto& blas : m_blasPool) {
        size_t bufferSize = blas->accelStructure->info().size;
        uint32_t paddedLastTouched = blas->frameLastTouched + 1 + (RtxOptions::enablePreviousTLAS() ? 1u : 0u); /* note: +2 because frameLastTouched is unsigned and init'd with UINT32_MAX, and keep the BLAS'es for one extra frame for previous TLAS access */
        if (bufferSize >= sizeInfo.accelerationStructureSize &&
            (!selectedBlas || bufferSize < selectedBlas->accelStructure->info().size) &&
            paddedLastTouched <= currentFrame) {
          selectedBlas = blas.ptr();
        }
      }

      // Must ensure that if we are updating an existing blas, rather than rebuilding, the blas is compatible with our new build info
      // Cannot update a blas that contains OMM instances, this leads to sporadic device lost errors
      if (!bucket->hasOmmInstances && selectedBlas && selectedBlas->opacityMicromaps.empty() && validateUpdateMode(selectedBlas->buildInfo, buildInfo) && selectedBlas->primitiveCounts == bucket->primitiveCounts) {
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      }

      // There is no such BLAS - create one and put it into the pool
      if (!selectedBlas) {
        auto newBlas = createPooledBlas(sizeInfo.accelerationStructureSize, "BLAS Merged");

        selectedBlas = newBlas.ptr();

        m_blasPool.push_back(std::move(newBlas));
      }
      assert(selectedBlas);
      selectedBlas->frameLastTouched = currentFrame;

      // Record the assigned BLAS on the bucket so the per-bucket cache can capture it
      bucket->assignedBlas = selectedBlas;

      // Compute a content hash for this bucket's geometry to detect when the
      // merged BLAS can skip its GPU build entirely.  Uses the existing
      // content-based geometry hashes (vertex position, index, bone) from
      // the BlasEntry rather than device addresses, since double-buffered
      // vertex buffers can reuse the same address with different data.
      XXH64_hash_t newContentHash = kEmptyHash;
      {
        contentHashData.resize(bucket->geometries.size());
        if (!contentHashData.empty()) {
          std::memset(contentHashData.data(), 0, contentHashData.size() * sizeof(contentHashData[0]));
        }

        for (uint32_t gi = 0; gi < bucket->geometries.size(); ++gi) {
          const RtInstance* inst = bucket->originalInstances[gi];
          const BlasEntry* blasEntry = inst->getBlas();
          const auto& geoHashes = blasEntry->modifiedGeometryData.hashes;
          BucketGeometryContentHashData& hashData = contentHashData[gi];

          hashData.vertexHash = geoHashes[HashComponents::VertexPosition];
          hashData.indexHash = geoHashes[HashComponents::Indices];
          hashData.boneHash = blasEntry->modifiedGeometryData.lastBoneHash;
          hashData.transform = inst->getVkInstance().transform;
          hashData.primitiveCount = bucket->primitiveCounts[gi];
        }

        // Geometry order is part of the merged BLAS layout and affects primitive
        // to surface mapping, so include the bucket order in the content hash.
        if (!contentHashData.empty()) {
          newContentHash = XXH3_64bits(contentHashData.data(), contentHashData.size() * sizeof(contentHashData[0]));
        }
      }

      const bool canSkipBuild = (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) &&
                                 (selectedBlas->contentHash == newContentHash) &&
                                 (newContentHash != kEmptyHash);
      selectedBlas->contentHash = newContentHash;

      if (!canSkipBuild) {
        selectedBlas->opacityMicromaps = bucket->opacityMicromaps;
        // Use the selected BLAS for the build
        buildInfo.dstAccelerationStructure = selectedBlas->accelStructure->getAccelStructure();

        if (buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) {
          // Set the src to the dst if we're updating
          buildInfo.srcAccelerationStructure = buildInfo.dstAccelerationStructure;
        }

        copyAccelerationStructureBuildGeometryInfo(buildInfo, selectedBlas->buildInfo);
        selectedBlas->primitiveCounts = bucket->primitiveCounts;

        // Allocate a scratch buffer slice
        const size_t requiredScratchAllocSize = align(sizeInfo.buildScratchSize + m_scratchAlignment, m_scratchAlignment);
        buildInfo.scratchData.deviceAddress = totalScratchMemory;
        totalScratchMemory += requiredScratchAllocSize;

        assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

        // Track the lifetime of the BLAS buffers
        ctx->getCommandList()->trackResource<DxvkAccess::Write>(selectedBlas->accelStructure);

        // Put the merged BLAS into the build queue
        blasToBuild.push_back(buildInfo);
        blasRangesToBuild.push_back(bucket->ranges.data());
        kenshi_telemetry::add(s_mergedBucketsBuilt, 1u);
      } else {
        // BLAS content is unchanged — skip the GPU build but still track the resource for read
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(selectedBlas->accelStructure);
        kenshi_telemetry::add(s_mergedBucketsSkipped, 1u);
      }

      // DX11_V375_BLAS_ROUTE: per-bucket detail, hotkey-armed and bounded. This is
      // the line that says whether the bucket holding Kenshi's wall segments got a
      // real GPU build, how much geometry it carries, and where its first instance
      // sits - which is what distinguishes "referenced but never built" from
      // "built and still not hit".
      kenshi_telemetry::add(s_mergedBuckets, 1u);
      if (selectedBlas->accelerationStructureReference == 0ull)
        kenshi_telemetry::add(s_mergedBucketNoBlas, 1u);
      {
        uint32_t detailBudget = s_bucketDetailBudget.load(std::memory_order_relaxed);
        if (kenshi_telemetry::enabled() && detailBudget > 0u) {
          s_bucketDetailBudget.store(detailBudget - 1u, std::memory_order_relaxed);
          uint32_t bucketPrims = 0;
          for (const uint32_t primCount : bucket->primitiveCounts)
            bucketPrims += primCount;
          const RtInstance* firstInst = bucket->originalInstances.empty()
            ? nullptr : bucket->originalInstances.front();
          const VkTransformMatrixKHR* firstXform =
            firstInst != nullptr ? &firstInst->getVkInstance().transform : nullptr;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX][blas-route] merged bucket: geometries=", bucket->geometries.size(),
            " instances=", bucket->originalInstances.size(),
            " prims=", bucketPrims,
            " build=", canSkipBuild ? "SKIPPED(contentHash)" : "submitted",
            " mode=", buildInfo.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                        ? "update" : "build",
            " blasRef=", selectedBlas->accelerationStructureReference != 0ull ? 1 : 0,
            " mask=", bucket->instanceMask,
            " firstVerts=", firstInst != nullptr && firstInst->getBlas() != nullptr
              ? firstInst->getBlas()->modifiedGeometryData.vertexCount : 0u,
            " firstXformT=[",
            firstXform != nullptr ? firstXform->matrix[0][3] : 0.f, ",",
            firstXform != nullptr ? firstXform->matrix[1][3] : 0.f, ",",
            firstXform != nullptr ? firstXform->matrix[2][3] : 0.f, "]"));
        }
      }

      static float identityTransform[3][4] = {
        { 1.f, 0.f, 0.f, 0.f },
        { 0.f, 1.f, 0.f, 0.f },
        { 0.f, 0.f, 1.f, 0.f }
      };

      // Append an instance of this merged BLAS to the merged instance list
      VkAccelerationStructureInstanceKHR instance {};
      instance.accelerationStructureReference = selectedBlas->accelerationStructureReference;
      instance.flags = bucket->instanceFlags;
      instance.instanceShaderBindingTableRecordOffset = bucket->instanceShaderBindingTableRecordOffset;
      instance.mask = bucket->instanceMask;
      instance.instanceCustomIndex =
        (bucket->customIndexFlags & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK)) |
        (bucket->reorderedSurfacesOffset & uint32_t(CUSTOM_INDEX_SURFACE_MASK));
      memcpy(static_cast<void*>(&instance.transform.matrix[0][0]), &identityTransform[0][0], sizeof(VkTransformMatrixKHR));

      if (bucket->usesUnorderedApproximations && RtxOptions::enableSeparateUnorderedApproximations()) {
        m_mergedInstances[Tlas::Unordered].push_back(instance);
      } else {
        m_mergedInstances[Tlas::Opaque].push_back(instance);
        if (bucket->hasSssInstances) {
          m_mergedInstances[Tlas::SSS].push_back(instance);
        }
      }
    }
  }

  void AccelManager::prepareSceneData(Rc<DxvkContext> ctx, DxvkBarrierSet& execBarriers, InstanceManager& instanceManager) {
    terrain_profile::Scope sceneCpuPrepare(terrain_profile::Stage::AccelPrepare);
    ScopedCpuProfileZone();

    // Truncate merged instances back to the baseline (removing any billboard
    // instances that were appended in a previous frame's prepareSceneData call).
    // This is necessary so that billboard entries are not accumulated
    // across frames; fresh billboards are appended below.
    for (int t = 0; t < Tlas::Count; ++t) {
      if (m_mergedInstances[t].size() > m_mergedInstancesBaselineCount[t]) {
        m_mergedInstances[t].resize(m_mergedInstancesBaselineCount[t]);
      }
    }

    bool haveInstances = false;
    for (const auto& instances : m_mergedInstances) {
      if (!instances.empty()) {
        haveInstances = true;
        break;
      }
    }

    if (!haveInstances && instanceManager.getBillboards().empty()) {
      return;
    }

    createAndBuildIntersectionBlas(ctx, execBarriers);

    // Prepare billboard data and instances
    std::vector<MemoryBillboard> memoryBillboards;
    uint32_t numActiveBillboards = 0;

    // Check the enablement here - because the instance manager needs to run the billboard analysis all the time
    if (RtxOptions::enableBillboardOrientationCorrection()) {
      memoryBillboards.resize(instanceManager.getBillboards().size());
      uint32_t index = 0;

      for (const auto& billboard : instanceManager.getBillboards()) {
        if (billboard.instanceMask == 0 || !billboard.allowAsIntersectionPrimitive) {
          continue;
        }

        // Shader data
        MemoryBillboard& memory = memoryBillboards[index];
        memory.center = billboard.center;
        memory.surfaceIndexAndMaterialType =
          (billboard.instance->getSurfaceIndex() & CUSTOM_INDEX_SURFACE_MASK) |
          (((billboard.instance->getVkInstance().instanceCustomIndex >> CUSTOM_INDEX_MATERIAL_TYPE_BIT) & surfaceMaterialTypeMask) << CUSTOM_INDEX_MATERIAL_TYPE_BIT);
        assert(billboard.instance->getSurfaceIndex() <= SURFACE_INDEX_MAX_VALUE && "Billboard surfaceIndex exceeds SURFACE_INDEX_MAX_VALUE");
        memory.inverseHalfWidth = 2.f / billboard.width;
        memory.inverseHalfHeight = 2.f / billboard.height;
        memory.xAxis = billboard.xAxis;
        memory.yAxis = billboard.yAxis;
        memory.xAxisUV = billboard.xAxisUV;
        memory.yAxisUV = billboard.yAxisUV;
        memory.centerUV = billboard.centerUV;
        memory.vertexColor = billboard.vertexColor;
        memory.flags = 0;
        if (billboard.isBeam) {
          memory.flags |= billboardFlagIsBeam;
        }
        if (billboard.isCameraFacing) {
          memory.flags |= billboardFlagIsCameraFacing;
        }

        // TLAS instance
        VkAccelerationStructureInstanceKHR instance {};
        instance.accelerationStructureReference = m_intersectionBlas->accelerationStructureReference;
        instance.flags = 0;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.mask = billboard.instanceMask;
        instance.instanceCustomIndex = index;

        Matrix4 transform;
        if (billboard.isBeam) {
          // Scale and orient the primitive so that its local X and Y axes match the billboard's X and Y axes,
          // and the Z axis is (obviously) orthogonal to those. Note that the beam is cylindrical, so its 'width'
          // applies to both the X and Z axes.
          transform[0] = Vector4(billboard.xAxis * billboard.width * 0.5f, 0.f);
          transform[1] = Vector4(billboard.yAxis * billboard.height * 0.5f, 0.f);
          transform[2] = Vector4(normalize(cross(billboard.xAxis, billboard.yAxis)) * billboard.width * 0.5f, 0.f);
        }
        else {
          // Note: to be fully conservative, the size of the intersection primitive should be equal to the diagonal
          // of the original particle, not its largest side. But the particle textures are usually round, so
          // the reduced size works well in practice and results in fewer unnecessary ray interactions.
          const float radius = std::max(billboard.width, billboard.height) * 0.5f;
          transform[0][0] = transform[1][1] = transform[2][2] = radius;
        }
        transform[3] = Vector4(billboard.center, 1.f);
        transform = transpose(transform);
        memcpy(instance.transform.matrix, &transform, sizeof(VkTransformMatrixKHR));

        m_mergedInstances[Tlas::Unordered].push_back(instance);

        ++index;
      }

      numActiveBillboards = index;
    }

    // Allocate the instance buffer and copy its contents from host to device memory
    // STORAGE_BUFFER_BIT is required for the PointInstancer GPU culling compute shader
    // which writes VkAccelerationStructureInstanceKHR entries directly into this buffer.
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
               | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
               | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = 0;

    // Vk instance buffer: normal instances + reserved PointInstancer slots per type
    for (int t = 0; t < Tlas::Count; ++t) {
      info.size += m_mergedInstances[t].size() + m_pointInstancerSlotsPerType[t];
    }
    info.size = align(info.size * sizeof(VkAccelerationStructureInstanceKHR), kBufferAlignment);

    if ((m_vkInstanceBuffer == nullptr || info.size > m_vkInstanceBuffer->info().size) && info.size != 0) {
      m_vkInstanceBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Instance Buffer");
      Logger::debug("DxvkRaytrace: Vulkan AS Instance Realloc");
    }

    // Write only the CPU-populated (normal) instance data.  PointInstancer
    // regions are left for the GPU culling shader to fill directly.
    size_t offset = 0;
    for (int t = 0; t < Tlas::Count; ++t) {
      if (!m_mergedInstances[t].empty()) {
        const size_t size = m_mergedInstances[t].size() * sizeof(VkAccelerationStructureInstanceKHR);
        ctx->writeToBuffer(m_vkInstanceBuffer, offset, size, m_mergedInstances[t].data());
      }
      // Advance past both normal and PointInstancer regions for this TLAS type
      offset += (m_mergedInstances[t].size() + m_pointInstancerSlotsPerType[t]) * sizeof(VkAccelerationStructureInstanceKHR);
    }

    // Vk billboard buffer
    if (numActiveBillboards) {
      info.size = align(numActiveBillboards * sizeof(MemoryBillboard), kBufferAlignment);
      if (info.size > 0 && (m_billboardsBuffer == nullptr || info.size > m_billboardsBuffer->info().size)) {
        m_billboardsBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Billboards Buffer");
      }

      // Write billboard data
      ctx->writeToBuffer(m_billboardsBuffer, 0, numActiveBillboards * sizeof(MemoryBillboard), memoryBillboards.data());
    }
  }

  void AccelManager::dispatchPointInstancerCulling(Rc<DxvkContext> ctx, const CameraManager& cameraManager,
                                                   const Rc<DxvkBuffer>& surfaceMaterialBuffer) {
    if (m_pointInstancerBatches.empty() || m_vkInstanceBuffer == nullptr) {
      return;
    }

    // Compute the byte offset for each TLAS type within m_vkInstanceBuffer.
    // Layout per type: [normal instances][PointInstancer instances]
    size_t typeBaseOffset[Tlas::Count] = {};
    for (size_t n = 1; n < Tlas::Count; ++n) {
      typeBaseOffset[n] = typeBaseOffset[n - 1]
                        + (m_mergedInstances[n - 1].size() + m_pointInstancerSlotsPerType[n - 1])
                        * sizeof(VkAccelerationStructureInstanceKHR);
    }

    // Resolve each batch's instanceBufferByteOffset.
    // PointInstancer slots sit after the normal instances within each type's region.
    for (auto& batch : m_pointInstancerBatches) {
      batch.instanceBufferByteOffset = static_cast<uint32_t>(
        typeBaseOffset[batch.tlasType]
        + m_mergedInstances[batch.tlasType].size() * sizeof(VkAccelerationStructureInstanceKHR)
        + batch.firstIndexInType * sizeof(VkAccelerationStructureInstanceKHR));
    }

    // Dispatch the GPU culling compute shader via PointInstancerSystem
    RtxPointInstancerSystem& system = m_device->getCommon()->metaPointInstancerSystem();
    const Vector3 cameraPos = cameraManager.getMainCamera().getPosition();

    system.dispatchCulling(ctx, m_vkInstanceBuffer, m_surfaceBuffer, surfaceMaterialBuffer, m_pointInstancerBatches, cameraPos);
  }

  void AccelManager::buildParticleSurfaceMapping(std::vector<uint32_t>& surfaceIndexMapping) {
    terrain_profile::Scope sceneCpuParticleMap(terrain_profile::Stage::AccelParticleMap);
    // Simplify syntax for accessing the persistent containers
    auto& surfaceInfoLists = buildParticleSurfaceMappingFuncState.surfaceInfoLists;
    uint32_t& currIndex = buildParticleSurfaceMappingFuncState.currIndex;
    uint32_t& prevIndex = buildParticleSurfaceMappingFuncState.prevIndex;

    // Build surface index mapping for particle objects.
    surfaceInfoLists[currIndex].resize(m_reorderedSurfaces.size());
    std::unordered_map<uint32_t, std::vector<int>> curMaterialHashToSurfaceMap;
    for (uint32_t surfaceIndex = 0; surfaceIndex < m_reorderedSurfaces.size(); surfaceIndex++) {
      RtInstance& surface = *m_reorderedSurfaces[surfaceIndex];

      // Only record objects that use unordered approximations.
      // In some cases, objects with unorder resolve flag will generate a set of billboards, each one occupies one "Surface" entry
      // in the shaders' surface array. These entries has identical information except the "firstIndex" member.
      // See "fillGeometryInfoFromBlasEntry()" for more details in generating indexOffsets.
      // See "uploadSurfaceData()" for how the "firstIndex" is fed to the shaders surface array.
      if (surface.usesUnorderedApproximations() && m_reorderedSurfacesFirstIndexOffset[surfaceIndex] == 0) {
        const RasterGeometry& geometryData = surface.getBlas()->input.getGeometryData();

        // Need to find the closest object with the same material, so use material ID as hash value, and record bounding box's center.
        surfaceInfoLists[currIndex][surfaceIndex] = { 
          surface.surface.surfaceMaterialIndex,
          geometryData.boundingBox.getTransformedCentroid(surface.getTransform()) };

        if (surface.getBlas()->buildRanges.size() > 0 && surface.getBlas()->buildGeometries.size() > 0) {
          curMaterialHashToSurfaceMap[surface.surface.surfaceMaterialIndex].push_back(surfaceIndex);
        }
      } else {
        surfaceInfoLists[currIndex][surfaceIndex].surfaceMaterialIndex = kSurfaceInvalidSurfaceMaterialIndex;
      }
    }

    // Fix missed surface mapping by searching among objects with the same hash value, and choose the closest one.
    for (int i = 0; i < surfaceIndexMapping.size(); i++) {
      // Skip objects that have surface mapping
      if (surfaceIndexMapping[i] != SURFACE_INDEX_INVALID) {
        continue;
      }

      if (i >= surfaceInfoLists[prevIndex].size()) {
        continue;
      }

      // Skip objects with different materials
      auto lastInfo = surfaceInfoLists[prevIndex][i];
      auto pCandidateList = curMaterialHashToSurfaceMap.find(lastInfo.surfaceMaterialIndex);
      if (pCandidateList == curMaterialHashToSurfaceMap.end()) {
        continue;
      }

      auto& candidateList = pCandidateList->second;
      float minDistanceSq = FLT_MAX;
      int bestSurfaceID = -1;

      // Iterate through the candidate list and find the closest one
      for (int ithCandidate = 0; ithCandidate < candidateList.size(); ithCandidate++) {
        int curSurfaceID = candidateList[ithCandidate];
        RtInstance& surface = *m_reorderedSurfaces[curSurfaceID];
        if (surface.getBlas()->buildGeometries.size() == 0) {
          continue;
        }

        // Calculate bounding box centers' distance
        const RasterGeometry& geometryData = surface.getBlas()->input.getGeometryData();
        Vector3 center = geometryData.boundingBox.getTransformedCentroid(surface.getTransform());
        float distanceSq = lengthSqr(center - lastInfo.worldPosition);
        if (distanceSq < minDistanceSq) {
          minDistanceSq = distanceSq;
          bestSurfaceID = curSurfaceID;
        }
      }

      // Use the closest surface
      if (bestSurfaceID != -1) {
        surfaceIndexMapping[i] = bestSurfaceID;
      }
    }
    // Make current previous
    std::swap(currIndex, prevIndex);
  }

  Rc<DxvkBuffer> AccelManager::getKenshiBloodBuffer(Rc<DxvkContext> ctx) {
    // DX11_V519. The RT shaders declare kenshiBloodArgsBuffer unconditionally at
    // BINDING_KENSHI_BLOOD_BUFFER, so this binding must NEVER be null: a null
    // descriptor faults inside the driver on submit (observed as an AV READ at
    // 0x0 in nvoglv64.dll on the DXVK queue thread, which then loses the device
    // and takes the game's GUI down with it).
    //
    // uploadSurfaceData creates the buffer, but it returns early while
    // m_reorderedSurfaces is empty - which is every startup and loading frame,
    // i.e. exactly the frames the game boots through. So the buffer has to be
    // guaranteed here, the same way getKenshiTerrainBuffer guarantees its own.
    static constexpr VkDeviceSize kBloodRecordSize = 8u * sizeof(uint32_t);

    if (m_kenshiBloodBuffer == nullptr) {
      DxvkBufferCreateInfo info;
      info.size = align(kBloodRecordSize, kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiBloodBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXAccelerationStructure, "Kenshi Blood Buffer");

      // Zero means kenshiBloodMode == 0 for every surface, i.e. no blood.
      const uint32_t empty[8] = {};
      ctx->writeToBuffer(m_kenshiBloodBuffer, 0, sizeof(empty), empty);
    }

    return m_kenshiBloodBuffer;
  }

  void AccelManager::uploadSurfaceData(Rc<DxvkContext> ctx) {
    terrain_profile::Scope sceneCpuSurfaceUpload(terrain_profile::Stage::AccelSurfaceUpload);
    ScopedCpuProfileZone();
    if (m_reorderedSurfaces.empty()) {
      return;
    }

    // Simplify syntax for accessing the persistent containers
    auto& surfacesGPUData = uploadSurfaceDataFuncState.surfacesGPUData;
    auto& kenshiBloodGPUData = uploadSurfaceDataFuncState.kenshiBloodGPUData;
    auto& surfaceIndexMapping = uploadSurfaceDataFuncState.surfaceIndexMapping;

    // Surface buffer
    const auto surfacesGPUSize = m_reorderedSurfaces.size() * kSurfaceGPUSize;
    const auto kenshiBloodGPUSize = m_reorderedSurfaces.size() * 8u * sizeof(uint32_t);

    // Allocate the instance buffer and copy its contents from host to device memory
    // STORAGE_BUFFER_BIT is required for the GPU PointInstancer culling shader
    // which writes per-instance surface data (transforms) directly into this buffer.
    DxvkBufferCreateInfo info;
    info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    info.size = align(surfacesGPUSize, kBufferAlignment);
    if (m_surfaceBuffer == nullptr || info.size > m_surfaceBuffer->info().size) {
      m_surfaceBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Surface Buffer");
    }

    info.size = align(kenshiBloodGPUSize, kBufferAlignment);
    if (m_kenshiBloodBuffer == nullptr || info.size > m_kenshiBloodBuffer->info().size) {
      m_kenshiBloodBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Kenshi Blood Buffer");
    }

    uint32_t maxPreviousSurfaceIndex = 0;

    // Write surface data
    std::size_t dataOffset = 0;
    surfacesGPUData.resize(surfacesGPUSize);
    kenshiBloodGPUData.resize(m_reorderedSurfaces.size() * 8u);

    for (uint32_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
      const auto& currentInstance = *m_reorderedSurfaces[i];
      RtSurface& currentSurface = m_reorderedSurfaces[i]->surface;
      currentSurface.writeKenshiBloodGPUData(kenshiBloodGPUData.data() + i * 8u);

      // For PointInstancer entries beyond the first, do nothing.  The GPU culling shader will 
      // patch per-instance transforms and set per-instance customInstanceIndex later.
      if (currentSurface.instancesToObject != nullptr &&  currentSurface.surfaceIndexOfFirstInstance != SIZE_MAX && i > currentSurface.surfaceIndexOfFirstInstance) {
        dataOffset += kSurfaceGPUSize;
      } else {
        // Split instance geometry need to have their first index offset set in their corresponding surface instances
        currentSurface.firstIndex += m_reorderedSurfacesFirstIndexOffset[i];
        currentSurface.writeGPUData(surfacesGPUData.data(), dataOffset, i);
        currentSurface.firstIndex -= m_reorderedSurfacesFirstIndexOffset[i];
      }

      // Find the size of the surface mapping buffer
      // Skip SURFACE_INDEX_INVALID (new instances with no previous-frame data) to avoid
      // oversizing the mapping vector 
      const uint32_t prevIdx = currentInstance.getPreviousSurfaceIndex();
      if (prevIdx != SURFACE_INDEX_INVALID) {
        maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, prevIdx);
      }
    }

    // The GPU's SharedSurfaceIndex texture may reference any surface index from the
    // previous frame.  Ensure the mapping covers at least the previous frame's surface
    // count so those GPU lookups read SURFACE_INDEX_INVALID rather than stale buffer data.
    auto& previousFrameSurfaceCount = uploadSurfaceDataFuncState.previousFrameSurfaceCount;
    if (previousFrameSurfaceCount > 0) {
      maxPreviousSurfaceIndex = std::max(maxPreviousSurfaceIndex, previousFrameSurfaceCount - 1);
    }
    previousFrameSurfaceCount = static_cast<uint32_t>(m_reorderedSurfaces.size());

    assert(dataOffset == surfacesGPUSize);
    assert(surfacesGPUData.size() == surfacesGPUSize);

    ctx->writeToBuffer(m_surfaceBuffer, 0, surfacesGPUData.size(), surfacesGPUData.data());
    ctx->writeToBuffer(m_kenshiBloodBuffer, 0, kenshiBloodGPUSize, kenshiBloodGPUData.data());

    // Allocate and initialize the surface mapping buffer
    surfaceIndexMapping.resize(maxPreviousSurfaceIndex + 1);
    std::fill(surfaceIndexMapping.begin(), surfaceIndexMapping.end(), SURFACE_INDEX_INVALID);
    
    // Assign surface indices to instances that don't have one yet (i.e. those that
    // entered m_reorderedSurfaces via addBlas or bucket insertion rather than the
    // early setSurfaceIndex path for zero-mask OMM/billboard instances).
    // Also populate the previous-->current frame surface index mapping.
    for (uint32_t surfaceIndex = 0; surfaceIndex < m_reorderedSurfaces.size(); surfaceIndex++) {
      RtInstance& surface = *m_reorderedSurfaces[surfaceIndex];

      if (surface.getSurfaceIndex() == SURFACE_INDEX_INVALID) {
        surface.setSurfaceIndex(surfaceIndex);

        // For PointInstancers, all instances share a single surface entry
        if (surface.surface.instancesToObject) {
          assert(surfaceIndex == surface.surface.surfaceIndexOfFirstInstance);
          if (surface.getPreviousSurfaceIndex() != SURFACE_INDEX_INVALID) {
            surfaceIndexMapping[surface.getPreviousSurfaceIndex()] = surfaceIndex;
          }
          surface.setPreviousSurfaceIndex(surfaceIndex);
        }
      }

      if (surface.getBillboardCount() == 0 && !surface.surface.instancesToObject) {
        if (surface.getPreviousSurfaceIndex() != SURFACE_INDEX_INVALID) {
          surfaceIndexMapping[surface.getPreviousSurfaceIndex()] = surfaceIndex;
        }
        surface.setPreviousSurfaceIndex(surfaceIndex);
      }
    }

    if (RtxOptions::trackParticleObjects()) {
      buildParticleSurfaceMapping(surfaceIndexMapping);
    }

    // Create and upload the primitive id prefix sum buffer
    auto updatePrefixSumBuffer = [&info, this, ctx](std::vector<uint32_t>& prefixSumList, Rc<DxvkBuffer>& prefixSumBuffer) {
      info.size = std::max(prefixSumList.size(), 1llu) * sizeof(prefixSumList[0]);

      if (prefixSumBuffer == nullptr || info.size > prefixSumBuffer->info().size) {
        prefixSumBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Prefixsum Buffer");
      }

      if (prefixSumList.size() > 0) {
        ctx->writeToBuffer(prefixSumBuffer, 0, prefixSumList.size() * sizeof(prefixSumList[0]), prefixSumList.data());
      }
    };

    updatePrefixSumBuffer(m_reorderedSurfacesPrimitiveIDPrefixSum, m_primitiveIDPrefixSumBuffer);
    updatePrefixSumBuffer(m_reorderedSurfacesPrimitiveIDPrefixSumLastFrame, m_primitiveIDPrefixSumBufferLastFrame);

    // Create and upload the surface mapping buffer
    if (!surfaceIndexMapping.empty()) {
      info.size = align(surfaceIndexMapping.size() * sizeof(int), kBufferAlignment);
      if (m_surfaceMappingBuffer == nullptr || info.size > m_surfaceMappingBuffer->info().size) {
        m_surfaceMappingBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, "Surface Mapping Buffer");
      }

      ctx->writeToBuffer(m_surfaceMappingBuffer, 0, surfaceIndexMapping.size() * sizeof(surfaceIndexMapping[0]), surfaceIndexMapping.data());
    }
  }

  // V713: scoped to one immutable build list, never retained across frames.
  // Preserve the first match from EACH pool, including both when handles overlap.
  using BlasDestinationOwners = std::array<Rc<DxvkAccelStructure>, 2>;
  static std::vector<BlasDestinationOwners> resolveBlasDestinationOwners(
      const std::vector<Rc<PooledBlas>>& pooled,
      const std::vector<Rc<PooledBlas>>& dynamic,
      const std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& builds) {
    terrain_profile::Scope sceneCpuOwners(terrain_profile::Stage::BlasOwners);
    std::unordered_map<VkAccelerationStructureKHR, BlasDestinationOwners> byHandle;
    byHandle.reserve(pooled.size() + dynamic.size());
    const auto indexPool = [&](const std::vector<Rc<PooledBlas>>& pool, size_t slot) {
      for (const auto& blas : pool) {
        if (blas != nullptr) {
          auto& owner = byHandle[blas->accelStructure->getAccelStructure()][slot];
          if (owner == nullptr) owner = blas->accelStructure;
        }
      }
    };
    indexPool(pooled, 0);
    indexPool(dynamic, 1);
    std::vector<BlasDestinationOwners> result(builds.size());
    for (size_t i = 0; i < builds.size(); ++i) {
      const auto found = byHandle.find(builds[i].dstAccelerationStructure);
      if (found != byHandle.end()) result[i] = found->second;
    }
    terrain_profile::count(terrain_profile::OwnerPoolEntries, pooled.size() + dynamic.size());
    terrain_profile::count(terrain_profile::OwnerDestinations, builds.size());
    return result;
  }

  // V714: the FULL original post-flush input set, local to one buildBlases call.
  // These consecutive input registrations contain no intervening GPU commands.
  // accessBuffer/recordCommands emits global memory barriers here, so their
  // stage/access union preserves coverage without one barrier per geometry.
  struct BlasBuildInputs {
    std::vector<Rc<DxvkBuffer>> buffers;
    std::unordered_set<const DxvkBuffer*> seen;
    VkPipelineStageFlags srcStages = 0;
    VkAccessFlags srcAccess = 0;
    uint64_t geometryCount = 0;
    bool prepared = false;

    void add(const BlasEntry* entry) {
      ++geometryCount;
      for (const auto& buffer : { entry->modifiedGeometryData.positionBuffer.buffer(),
                                  entry->modifiedGeometryData.indexBuffer.buffer() }) {
        srcStages |= buffer->info().stages;
        srcAccess |= buffer->info().access;
        if (seen.insert(buffer.ptr()).second) buffers.push_back(buffer);
      }
    }

    void track(Rc<DxvkContext> ctx, DxvkBarrierSet& barriers, const BlasBuildInputs* subset = nullptr) const {
      const auto& tracked = subset ? subset->buffers : buffers;
      for (const auto& buffer : tracked)
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(buffer);
      if (geometryCount != 0) {
        barriers.accessMemory(srcStages, srcAccess,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
        barriers.recordCommands(ctx->getCommandList());
        terrain_profile::count(terrain_profile::InputBarriers);
      }
      terrain_profile::count(terrain_profile::GeometryRetracks, geometryCount);
      terrain_profile::count(terrain_profile::InputBufferTracks, tracked.size());
      terrain_profile::count(subset ? terrain_profile::BatchInputLists : terrain_profile::FullInputLists);
      if (subset) terrain_profile::count(terrain_profile::InputTracksAvoided, buffers.size() - tracked.size());
    }
  };

  // V717: lookup lives only through this immutable build list. Never weaken the
  // final command list's full ray-tracing ownership or the original barrier union.
  struct BlasBatchLookup {
    struct View { const BlasEntry* entry=nullptr;
      const std::vector<VkAccelerationStructureGeometryKHR>* geometries=nullptr;
      const std::vector<RtInstance*>* instances=nullptr; bool ambiguous=false; };
    std::unordered_map<const VkAccelerationStructureGeometryKHR*, View> views;
    bool prepared=false;
    void add(const VkAccelerationStructureGeometryKHR* key, View view) {
      auto result=views.emplace(key,view);
      if (!result.second) result.first->second.ambiguous=true;
    }
    bool collect(const VkAccelerationStructureBuildGeometryInfoKHR& desc, BlasBuildInputs& out) const {
      if (!desc.pGeometries || desc.ppGeometries || !desc.geometryCount) return false;
      const auto found=views.find(desc.pGeometries);
      if (found==views.end() || found->second.ambiguous) return false;
      const auto& view=found->second;
      if (view.entry && view.entry->buildGeometries.size()!=desc.geometryCount) return false;
      if (!view.entry && (!view.geometries || !view.instances
         || view.geometries->size()!=desc.geometryCount || view.instances->size()!=desc.geometryCount)) return false;
      for (uint32_t i=0;i<desc.geometryCount;++i) {
        const auto* instance=view.instances ? (*view.instances)[i] : nullptr;
        const BlasEntry* entry=view.entry ? view.entry : (instance ? instance->getBlas() : nullptr);
        if (!entry) return false;
        const auto& geo=desc.pGeometries[i];
        const auto& input=entry->modifiedGeometryData;
        if (geo.geometryType!=VK_GEOMETRY_TYPE_TRIANGLES_KHR || !input.positionBuffer.defined()
         || !input.indexBuffer.defined()) return false; // retain original set for unsupported layouts
        const auto& tri=geo.geometry.triangles;
        const auto vertex=input.positionBuffer.getDeviceAddress()+input.positionBuffer.offsetFromSlice();
        const auto index=input.usesIndices() ? input.indexBuffer.getDeviceAddress() : 0;
        if (tri.vertexData.deviceAddress!=vertex || tri.indexData.deviceAddress!=index
         || tri.vertexStride!=input.positionBuffer.stride()
         || tri.vertexFormat!=input.positionBuffer.vertexFormat()
         || tri.maxVertex!=input.vertexCount-1
         || tri.indexType!=(input.usesIndices() ? input.indexBuffer.indexType() : VK_INDEX_TYPE_NONE_KHR)) return false;
        out.add(entry);
      }
      return true;
    }
  };

  void AccelManager::buildBlases(Rc<DxvkContext> ctx,
                                 DxvkBarrierSet& execBarriers,
                                 const CameraManager& cameraManager,
                                 OpacityMicromapManager* opacityMicromapManager,
                                 const InstanceManager& instanceManager,
                                 const std::vector<TextureRef>& textures,
                                 const std::vector<RtInstance*>& instances,
                                 const std::vector<std::unique_ptr<BlasBucket>>& blasBuckets,
                                 std::vector<VkAccelerationStructureBuildGeometryInfoKHR>& blasToBuild,
                                 std::vector<VkAccelerationStructureBuildRangeInfoKHR*>& blasRangesToBuild,
                                 size_t& totalScratchMemory) {
    terrain_profile::Scope sceneCpuBlas(terrain_profile::Stage::AccelBlas);
    ScopedGpuProfileZone(ctx, "buildBLAS");
    // Upload surfaces before opacity micromap generation which reads the surface data on the GPU
    uploadSurfaceData(ctx);

    // Clear any stale OMM bindings from cached geometry data.  This must happen
    // unconditionally because buildGeometries are cached across frames and the
    // bucket copies them via tryAddInstance — a stale pNext from a previous bind
    // would otherwise survive even when OMMs are completely disabled.
    for (auto& blasBucket : blasBuckets) {
      for (auto& geo : blasBucket->geometries) {
        geo.geometry.triangles.pNext = nullptr;
      }
    }

    terrain_profile::Scope sceneCpuOmm(terrain_profile::Stage::BlasOmm);
    // Build and bind opacity micromaps
    if (opacityMicromapManager && opacityMicromapManager->isActive()) {
      opacityMicromapManager->buildOpacityMicromaps(ctx, textures, cameraManager.getLastCameraCutFrameId());

      // Bind opacity micromaps
      for (auto& blasBucket : blasBuckets) {
        for (uint32_t i = 0; i < blasBucket->geometries.size(); i++) {
          auto ommSourceHash = opacityMicromapManager->tryBindOpacityMicromap(ctx, *blasBucket->originalInstances[i], blasBucket->instanceBillboardIndices[i],
                                                         blasBucket->geometries[i], instanceManager);
          if (ommSourceHash != kEmptyHash) {
            blasBucket->hasOmmInstances = true;
            blasBucket->opacityMicromaps.push_back(opacityMicromapManager->getBlasBinding(ommSourceHash));
          }
        }
        // One ownership reference per map per BLAS, regardless of instance count.
        auto& bindings = blasBucket->opacityMicromaps;
        std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
          return std::less<DxvkResource*>()(a.resource.ptr(), b.resource.ptr());
        });
        bindings.erase(std::unique(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
          return a.resource.ptr() == b.resource.ptr();
        }), bindings.end());
      }

      opacityMicromapManager->onBlasBuild(ctx);
    }

    sceneCpuOmm.finish();
    // Blas buffers must be created after opacity micromaps were generated to calculate correct acceleration structure sizes
    createBlasBuffersAndInstances(ctx, blasBuckets, blasToBuild, blasRangesToBuild, totalScratchMemory);

    // Make sure we have enough scratch memory for this build job
    if (totalScratchMemory > 0) {
      const Rc<DxvkBuffer> requestedScratch =
        getScratchMemory(align(totalScratchMemory, m_scratchAlignment));

      // Abandon this batch rather than build against a null scratch address.
      // Dropping the builds costs a frame of missing geometry; proceeding costs
      // the device.
      if (requestedScratch == nullptr) {
        ONCE(Logger::err(
          "AccelManager: skipping acceleration structure builds this frame - no scratch memory."));
        return;
      }

      m_scratchBuffer = requestedScratch;

      execBarriers.accessBuffer(
       m_scratchBuffer->getSliceHandle(),
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV,
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV);

      ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
    }

    // Execute all barriers generated to this point as part of:
    //  o mergeInstancesIntoBlas()
    //  o Opacity micromap generation above
    execBarriers.recordCommands(ctx->getCommandList());

    // Build the BLASes
    if (!blasToBuild.empty()) {
      // Now apply the buffer offset to the scratch address we calculated earlier
      for (auto& desc : blasToBuild) {
        desc.scratchData.deviceAddress += m_scratchBuffer->getDeviceAddress();
      }
      assert(blasToBuild.size() == blasRangesToBuild.size());

      // A command-buffer barrier does not create a Windows GPU scheduling
      // boundary.  Modern DX11 engines can update hundreds of post-VS BLASes in
      // one frame, and keeping every vkCmdBuildAccelerationStructuresKHR call in
      // the same queue submission has produced nvlddmkm Event 153 followed by
      // VK_ERROR_DEVICE_LOST.  Bound both the Vulkan call and the queue
      // submission, while preserving every BLAS in the scene.
      // Keep a bounded GPU scheduling unit without turning every small BLAS
      // into its own queue submission.  Per-BLAS submission caused dozens of
      // flushes in ordinary Unreal frames and still ended in Event 153.  The
      // primitive ceiling remains authoritative for expensive builds.
      static constexpr uint32_t kMaxBlasesPerSubmission = 8u;
      static constexpr uint64_t kMaxPrimitivesPerSubmission = 64u * 1024u;
      const uint32_t buildCount = static_cast<uint32_t>(blasToBuild.size());
      std::vector<BlasDestinationOwners> destinationOwners;
      BlasBuildInputs buildInputs;
      BlasBatchLookup batchLookup;
      if (terrain_profile::enabled()) {
        terrain_profile::count(terrain_profile::VkBlasDescriptors, buildCount);
        for (const auto& desc : blasToBuild)
          terrain_profile::count(desc.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            ? terrain_profile::VkBlasUpdates : terrain_profile::VkBlasBuilds);
      }
      const auto countBatch = [&](uint32_t first, uint64_t& batchPrimitives) {
        uint32_t batchCount = 0;
        batchPrimitives = 0;
        while (first + batchCount < buildCount
            && batchCount < kMaxBlasesPerSubmission) {
          const uint32_t candidate = first + batchCount;
          uint64_t candidatePrimitives = 0;
          const VkAccelerationStructureBuildRangeInfoKHR* ranges =
            blasRangesToBuild[candidate];
          if (ranges != nullptr) {
            for (uint32_t geometry = 0;
                 geometry < blasToBuild[candidate].geometryCount;
                 ++geometry) {
              candidatePrimitives += ranges[geometry].primitiveCount;
            }
          }

          // Always admit one BLAS so a single large mesh makes progress.  Any
          // additional BLAS must keep this queue submission below both limits.
          if (batchCount > 0u
           && batchPrimitives + candidatePrimitives
                > kMaxPrimitivesPerSubmission) {
            break;
          }
          batchPrimitives += candidatePrimitives;
          ++batchCount;
        }
        return batchCount;
      };
      uint32_t submissionCount = 0;
      for (uint32_t first = 0; first < buildCount;) {
        uint64_t batchPrimitives = 0;
        const uint32_t batchCount = countBatch(first,batchPrimitives);

        kenshi_fault::checkpoint(ctx.ptr(),kenshi_fault::Blas,m_device->getCurrentFrameId());
        terrain_profile::Scope sceneCpuIssue(terrain_profile::Stage::BlasIssue);
        ctx->vkCmdBuildAccelerationStructuresKHR(
          batchCount,
          blasToBuild.data() + first,
          blasRangesToBuild.data() + first);
        sceneCpuIssue.finish();
        terrain_profile::count(terrain_profile::VkBlasCalls);
        terrain_profile::count(terrain_profile::VkBlasPrimitives, batchPrimitives);
        ++submissionCount;

        if (first + batchCount < buildCount) {
          // Make this batch's writes available to all later AS work, then end
          // the command list.  The next submission is ordered on the same queue
          // and therefore cannot race this batch.
          ctx->emitMemoryBarrier(
            0,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);

          ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
          terrain_profile::Scope sceneCpuFlush(terrain_profile::Stage::BlasFlush);
          ctx->flushCommandList();
          sceneCpuFlush.finish();
          terrain_profile::count(terrain_profile::VkBlasFlushes);

          // beginRecording() starts a fresh lifetime/resource-tracking scope.
          // Re-register every input that subsequent BLAS build descriptors can
          // reference; CPU ownership alone is not sufficient for renamed DXVK
          // allocations that are retired by command-list completion.
          terrain_profile::Scope sceneCpuRetrack(terrain_profile::Stage::BlasRetrack);
          if (!buildInputs.prepared) {
            terrain_profile::Scope sceneCpuInputs(terrain_profile::Stage::BlasInputs);
            for (uint32_t uniqueBlasIdx = 0;
                 uniqueBlasIdx < m_uniqueDynamicBlasCount; ++uniqueBlasIdx) {
              const UniqueBlasInstances& uniqueBlasEntry = m_uniqueDynamicBlas[uniqueBlasIdx];
              if (uniqueBlasEntry.blasEntry != nullptr && !uniqueBlasEntry.instances.empty())
                buildInputs.add(uniqueBlasEntry.blasEntry);
            }
            for (const auto& bucket : blasBuckets) {
              for (const RtInstance* instance : bucket->originalInstances) {
                if (instance != nullptr && instance->getBlas() != nullptr)
                  buildInputs.add(instance->getBlas());
              }
            }
            buildInputs.prepared = true;
            terrain_profile::count(terrain_profile::InputListBuilds);
            terrain_profile::count(terrain_profile::InputUniqueBuffers, buildInputs.buffers.size());
          }
          BlasBuildInputs subset;
          const uint32_t nextFirst=first+batchCount;
          uint64_t nextPrimitives=0;
          const uint32_t nextCount=countBatch(nextFirst,nextPrimitives);
          const bool finalList=nextFirst+nextCount==buildCount;
          bool useSubset=!finalList && kenshi_fault::batchOptimized.load(std::memory_order_relaxed)
            && kenshi_fault::batchValid;
          if (useSubset) {
            if (!batchLookup.prepared) {
              for (uint32_t i=0;i<m_uniqueDynamicBlasCount;++i) {
                const auto& entry=m_uniqueDynamicBlas[i];
                if (entry.blasEntry && !entry.instances.empty())
                  batchLookup.add(entry.blasEntry->buildGeometries.data(),{entry.blasEntry,nullptr,nullptr,false});
              }
              for (const auto& bucket:blasBuckets)
                batchLookup.add(bucket->geometries.data(),{nullptr,&bucket->geometries,&bucket->originalInstances,false});
              batchLookup.prepared=true;
            }
            for (uint32_t i=nextFirst;i<nextFirst+nextCount && useSubset;++i)
              useSubset=batchLookup.collect(blasToBuild[i],subset);
            // A selected owner must also be present in the original full set.
            // Keep this inexpensive guard in normal mode; no narrowing on doubt.
            for (const auto& buffer:subset.buffers)
              if (!buildInputs.seen.count(buffer.ptr())) useSubset=false;
            if (!useSubset) terrain_profile::count(terrain_profile::BatchInputFallback);
            if (kenshi_telemetry::enabled() && kenshi_fault::batchVerifyRemaining) {
              --kenshi_fault::batchVerifyRemaining; ++kenshi_fault::batchVerified;
              uint64_t expected=0;
              for(uint32_t i=nextFirst;i<nextFirst+nextCount;++i) expected+=blasToBuild[i].geometryCount;
              if (useSubset && (subset.geometryCount!=expected || subset.buffers.size()>buildInputs.buffers.size())) {
                ++kenshi_fault::batchMismatches;kenshi_fault::batchValid=false;useSubset=false;
                Logger::err("[BlasBatch V717] owner coverage mismatch; full input path retained");
              }
            }
          }
          kenshi_fault::add(kenshi_fault::Batch,m_device->getCurrentFrameId(),nextFirst,nextCount,
            useSubset?subset.buffers.size():buildInputs.buffers.size(),finalList?2:useSubset?1:0);
          buildInputs.track(ctx, execBarriers,useSubset?&subset:nullptr);

          ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
          ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
          if (m_transformBuffer != nullptr) {
            ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_transformBuffer);
          }
          sceneCpuRetrack.finish();

          // Track the destination BLAS objects referenced by the remaining
          // descriptors in this new command list.
          if (destinationOwners.empty())
            destinationOwners = resolveBlasDestinationOwners(m_blasPool, m_activeDynamicBlases, blasToBuild);
          terrain_profile::Scope sceneCpuDestinations(terrain_profile::Stage::BlasDestinations);
          for (uint32_t remaining = first + batchCount;
               remaining < buildCount; ++remaining) {
            for (const auto& owner : destinationOwners[remaining]) {
              if (owner != nullptr) {
                ctx->getCommandList()->trackResource<DxvkAccess::Write>(owner);
                terrain_profile::count(terrain_profile::OwnerTracks);
              }
            }
          }
          sceneCpuDestinations.finish();

          ctx->emitMemoryBarrier(
            0,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        }
        first += batchCount;
      }

      static uint32_t sBatchedBuildLogs = 0;
      if (buildCount > kMaxBlasesPerSubmission && sBatchedBuildLogs < 16u) {
        ++sBatchedBuildLogs;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX][BLAS] split ", buildCount, " builds across ",
          submissionCount,
          " watchdog-safe submissions; scratchMiB=",
          totalScratchMemory >> 20));
      }

      execBarriers.accessBuffer(
       m_scratchBuffer->getSliceHandle(),
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV,
       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV);

      ctx->getCommandList()->trackResource<DxvkAccess::Read>(m_scratchBuffer);
    }
  }

  void AccelManager::buildTlas(Rc<DxvkContext> ctx) {
    kenshi_fault::checkpoint(ctx.ptr(),kenshi_fault::Tlas,m_device->getCurrentFrameId());
    terrain_profile::Scope sceneCpuTlas(terrain_profile::Stage::AccelTlas);
    if ((kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal())) {
      // Inspect final CPU upload sources even when the bucket pipeline did zero work.
      for (size_t i = 0; i < m_reorderedSurfaces.size(); ++i) {
        const auto* surface = m_reorderedSurfaces[i];
        KenshiFlickerTrace::add("surface", str::format("slot=", i,
          " indexOffset=", m_reorderedSurfacesFirstIndexOffset[i],
          " updatedNow=", surface->getFrameLastUpdated() == m_device->getCurrentFrameId(),
          " ", KenshiFlickerTrace::describeInstance(surface)));
      }
      for (size_t type = 0; type < Tlas::Count; ++type) {
        for (const auto& instance : m_mergedInstances[type]) {
          const auto& t = instance.transform.matrix;
          KenshiFlickerTrace::add("tlas", str::format(std::setprecision(9),
            "type=", type, " base=", instance.instanceCustomIndex & uint32_t(CUSTOM_INDEX_SURFACE_MASK),
            " custom=", uint32_t(instance.instanceCustomIndex),
            " blas=", instance.accelerationStructureReference,
            " mask=", uint32_t(instance.mask), " flags=", uint32_t(instance.flags),
            " sbt=", uint32_t(instance.instanceShaderBindingTableRecordOffset),
            " transform=", t[0][0], ",", t[0][1], ",", t[0][2], ",", t[0][3], ",",
            t[1][0], ",", t[1][1], ",", t[1][2], ",", t[1][3], ",",
            t[2][0], ",", t[2][1], ",", t[2][2], ",", t[2][3]));
        }
      }
      std::unordered_map<const RtInstance*, uint64_t> liveSurfaceIds;
      for (const auto* surface : m_reorderedSurfaces)
        liveSurfaceIds.emplace(surface, surface->getId());
      for (const auto& bucket : m_cachedBuckets) {
        for (size_t i = 0; i < bucket.surfaces.size(); ++i) {
          // A disabled/invalid cache can retain old pointer values. Never dereference them.
          const auto found = liveSurfaceIds.find(bucket.surfaces[i]);
          KenshiFlickerTrace::add("bucket", str::format("blas=",
            bucket.tlasInstance.accelerationStructureReference, " member=", i,
            " live=", found != liveSurfaceIds.end(),
            " inst=", found != liveSurfaceIds.end() ? found->second : 0ull));
        }
      }
      KenshiFlickerTrace::add("upload", str::format("buffer=", m_vkInstanceBuffer != nullptr,
        " pointSlots=", m_pointInstancerSlotsPerType[0], ",", m_pointInstancerSlotsPerType[1], ",",
        m_pointInstancerSlotsPerType[2]));
      KenshiFlickerTrace::finish(m_device->getCurrentFrameId());
    }
    if (m_vkInstanceBuffer == nullptr) {
      return;
    }

    ScopedGpuProfileZone(ctx, "buildTLAS");

    // DX11_V334_TLAS_STATS: what actually reaches the TLAS this frame, across
    // all three types. If this alternates while the instance count is flat, the
    // flicker lives here rather than anywhere upstream.
    {
      size_t merged = 0;
      // DX11_V344_TLAS_CONTENTS: inspect what traversal will actually consume,
      // not how many entries there are. Every producer-side stage measures
      // clean, so the remaining possibility is that an entry is present but
      // inert. Two ways that happens:
      //
      //  nullBlas - accelerationStructureReference == 0. The cached-bucket
      //    restore builds its template with `cached.tlasInstance = {}` and only
      //    fills the reference `if (cached.assignedBlas.ptr())`, so a bucket
      //    that failed to get a pooled BLAS yields an instance pointing at
      //    nothing. Rays pass straight through it while it still counts as
      //    present, unhidden, non-GC'd and non-zero-mask - which is precisely
      //    the combination every counter so far has reported.
      //
      //  zeroXform - an all-zero 3x4, which collapses the instance to a point.
      //
      // Either would be invisible to `inTlas`, and both are consumed by the
      // tracer rather than produced by the bridge.
      uint32_t nullBlas = 0, zeroXform = 0;
      for (size_t n = 0; n < Tlas::Count; ++n) {
        merged += m_mergedInstances[n].size();
        for (const auto& inst : m_mergedInstances[n]) {
          if (inst.accelerationStructureReference == 0ull)
            ++nullBlas;
          bool anyNonZero = false;
          for (uint32_t r = 0; r < 3 && !anyNonZero; ++r)
            for (uint32_t c = 0; c < 4 && !anyNonZero; ++c)
              anyNonZero = inst.transform.matrix[r][c] != 0.f;
          if (!anyNonZero)
            ++zeroXform;
        }
      }
      s_tlasInTlas.store(uint32_t(merged), std::memory_order_relaxed);
      s_tlasNullBlas.store(nullBlas, std::memory_order_relaxed);
      s_tlasZeroXform.store(zeroXform, std::memory_order_relaxed);
    }

    // Four barriers in one:
    // Accel build bit - to protect from BLAS builds
    // Transfer bit - to protect from updateBuffer in prepareSceneData
    // Compute bit - to protect from GPU PointInstancer culling writes to instance, surface, and material buffers
    // RT shader read - make compute writes to surface/material buffers visible to ray tracing passes
    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT);

    for (auto&& blas : m_blasPool) {
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
      for (const auto& binding : blas->opacityMicromaps)
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(binding.resource);
    }
    for (const auto& blas : m_activeDynamicBlases) {
      ctx->getCommandList()->trackResource<DxvkAccess::Read>(blas->accelStructure);
      for (const auto& binding : blas->opacityMicromaps)
        ctx->getCommandList()->trackResource<DxvkAccess::Read>(binding.resource);
    }

    size_t totalScratchSize = 0;
    internalBuildTlas<Tlas::Opaque>(ctx, totalScratchSize);
    internalBuildTlas<Tlas::Unordered>(ctx, totalScratchSize);
    // Only build TLAS for SSS when necessary
    const bool isBuildSssTlas = RtxOptions::SubsurfaceScattering::enableDiffusionProfile() && (m_mergedInstances[Tlas::SSS].size() + m_pointInstancerSlotsPerType[Tlas::SSS]) > 0;
    if (isBuildSssTlas) {
      internalBuildTlas<Tlas::SSS>(ctx, totalScratchSize);
    }

    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // Release the scratch memory so it can be reused by rest of the frame.
    m_scratchBuffer = nullptr;

    OpacityMicromapManager* opacityMicromapManager = ctx->getCommonObjects()->getSceneManager().getOpacityMicromapManager();
    if (opacityMicromapManager) {
      opacityMicromapManager->onFinishedBuilding();
    }
  }

  template<Tlas::Type type>
  void AccelManager::internalBuildTlas(Rc<DxvkContext> ctx, size_t& totalScratchSize) {
    static constexpr const char* names[] = { "TLAS_Opaque", "TLAS_NonOpaque", "TLAS_SSS" };
    ScopedGpuProfileZone(ctx, names[type]);
    const VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | additionalAccelerationStructureFlags();

    const auto& vkd = m_device->vkd();

    // Create VkAccelerationStructureGeometryInstancesDataKHR
    // This wraps a device pointer to the above uploaded instances.
    VkAccelerationStructureGeometryInstancesDataKHR instancesVk { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instancesVk.arrayOfPointers = VK_FALSE;
    instancesVk.data.deviceAddress = m_vkInstanceBuffer->getDeviceAddress();

    // Rewind address to tlas start (normal + PointInstancer slots per preceding type)
    for (size_t n = 0; n < type; ++n) {
      instancesVk.data.deviceAddress += (m_mergedInstances[n].size() + m_pointInstancerSlotsPerType[n]) * sizeof(VkAccelerationStructureInstanceKHR);
    }

    // Put the above into a VkAccelerationStructureGeometryKHR. We need to put the
    // instances struct in a union and label it as instance data.
    VkAccelerationStructureGeometryKHR topASGeometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    topASGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topASGeometry.geometry.instances = instancesVk;

    // Find sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.flags = flags;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &topASGeometry;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    const uint32_t numInstances = uint32_t(m_mergedInstances[type].size() + m_pointInstancerSlotsPerType[type]);
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkd->vkGetAccelerationStructureBuildSizesKHR(vkd->device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &numInstances, &sizeInfo);

    // Create TLAS
    Tlas& tlas = m_device->getCommon()->getResources().getTLAS(type);

    if (type == Tlas::Opaque) {
      std::swap(tlas.accelStructure, tlas.previousAccelStructure);
    }

    if (tlas.accelStructure == nullptr || sizeInfo.accelerationStructureSize > tlas.accelStructure->info().size) {
      ScopedGpuProfileZone(ctx, "buildTLAS_createAccelStructure");
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      info.size = sizeInfo.accelerationStructureSize;

      tlas.accelStructure = m_device->createAccelStructure(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, names[type]);

      Logger::debug(str::format("DxvkRaytrace: TLAS Realloc"));
    }

    // Allocate the scratch memory, we share the same buffer between all TLAS types, so just ensure we handle the offsetting correctly here.
    const size_t requiredScratchAllocSize = align(sizeInfo.buildScratchSize + m_scratchAlignment, m_scratchAlignment);
    // A failed grow returns null here. Skipping the TLAS build costs one frame
    // of stale top-level geometry; dereferencing null crashes, and a zero base
    // address would point the build's scratch writes at gpuVA 0.
    const Rc<DxvkBuffer> tlasScratch =
      getScratchMemory(totalScratchSize + requiredScratchAllocSize);

    if (tlasScratch == nullptr) {
      ONCE(Logger::err(
        "AccelManager: skipping TLAS build this frame - no scratch memory."));
      return;
    }

    buildInfo.scratchData.deviceAddress = tlasScratch->getDeviceAddress() + totalScratchSize;
    totalScratchSize += requiredScratchAllocSize;

    // Update build information
    buildInfo.srcAccelerationStructure = nullptr;
    buildInfo.dstAccelerationStructure = tlas.accelStructure->getAccelStructure();

    assert(buildInfo.scratchData.deviceAddress % m_scratchAlignment == 0); // Note: Required by the Vulkan specification.

    // Build Offsets info: n instances
    VkAccelerationStructureBuildRangeInfoKHR        buildOffsetInfo { numInstances, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildOffsetInfo = &buildOffsetInfo;

    // Build the TLAS
    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildOffsetInfo);

    ctx->getCommandList()->trackResource<DxvkAccess::Write>(tlas.accelStructure);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
  }

  // Check if the existing build geometry info for this blas is compatible with the new one for the purpose of updating rather than rebuilding
  bool AccelManager::validateUpdateMode(const VkAccelerationStructureBuildGeometryInfoKHR& oldInfo, const VkAccelerationStructureBuildGeometryInfoKHR& newInfo) {
    if (!(oldInfo.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)) {
      return false;
    }

    if (oldInfo.type != newInfo.type || oldInfo.flags != newInfo.flags || oldInfo.geometryCount != newInfo.geometryCount) {
      return false;
    }

    for (uint32_t i = 0; i < oldInfo.geometryCount; ++i) {
      const VkAccelerationStructureGeometryKHR* oldGeom = oldInfo.pGeometries ? &oldInfo.pGeometries[i] : oldInfo.ppGeometries[i];
      const VkAccelerationStructureGeometryKHR* newGeom = newInfo.pGeometries ? &newInfo.pGeometries[i] : newInfo.ppGeometries[i];

      if (oldGeom->geometryType != newGeom->geometryType || oldGeom->flags != newGeom->flags) {
        return false;
      }

      // Per validation layers we need to check attributes of geometry types
      switch (oldGeom->geometryType) {
      case VK_GEOMETRY_TYPE_TRIANGLES_KHR:
      {
        const auto& oldTriangles = oldGeom->geometry.triangles;
        const auto& newTriangles = newGeom->geometry.triangles;
        if (oldTriangles.vertexFormat != newTriangles.vertexFormat ||
            oldTriangles.indexType != newTriangles.indexType ||
            oldTriangles.maxVertex != newTriangles.maxVertex ||
            oldTriangles.vertexStride != newTriangles.vertexStride) {
          return false;
        }
        // DX11_V378_STALE_BLAS_TRANSFORM: the spec requires transformData's
        // NULL-ness to match the source build - "if it was NULL then it must be
        // NULL", "if it was not NULL then it must not be NULL". This check was
        // absent, so an update could be issued across a NULL/non-NULL change,
        // which is undefined behaviour and on NVIDIA typically yields an
        // acceleration structure that traverses to nothing: present in the TLAS,
        // referenced, and hit by no rays.
        //
        // That combination is exactly the measured symptom for Kenshi's walls,
        // tents and near ground, and it is reachable here because the merged path
        // sets transformData and nothing ever cleared it.
        if ((oldTriangles.transformData.deviceAddress == 0ull) !=
            (newTriangles.transformData.deviceAddress == 0ull)) {
          return false;
        }
        break;
      }
      case VK_GEOMETRY_TYPE_AABBS_KHR:
      {
        const auto& oldAabbs = oldGeom->geometry.aabbs;
        const auto& newAabbs = newGeom->geometry.aabbs;
        if (oldAabbs.stride != newAabbs.stride) {
          return false;
        }
        break;
      }
      case VK_GEOMETRY_TYPE_INSTANCES_KHR:
      {
        const auto& oldInstances = oldGeom->geometry.instances;
        const auto& newInstances = newGeom->geometry.instances;
        if (oldInstances.arrayOfPointers != newInstances.arrayOfPointers) {
          return false;
        }
        break;
      }
      default:
        return false;
      }
    }
    return true;
  }

  void AccelManager::addPointInstancerBlas(RtInstance* rtInstance, BlasEntry* blasEntry) {
    // This RtInstance is a PointInstancer — GPU-driven culling path.
    // Reserve N surface slots (one per instance) so each gets its own
    // surface/material ID in customInstanceIndex.  Only the first slot is
    // fully populated on the CPU; the GPU culling shader copies the template
    // surface data and patches per-instance transforms for the rest.
    const auto& transforms = rtInstance->surface.instancesToObject;
    uint32_t instanceCount = static_cast<uint32_t>(transforms->size());

    // Reserve N surface entries — same RtInstance* for each, but each gets
    // a unique surfaceIndex.  The first entry is the "template" that
    // uploadSurfaceData writes fully; entries 1..N-1 are copies.
    const uint32_t surfaceIndex = static_cast<uint32_t>(m_reorderedSurfaces.size());

    // Clamp instance count so the last reserved surface index stays within the 21-bit limit.
    // Exceeding SURFACE_INDEX_MAX_VALUE would cause the GPU culling shader to write
    // truncated customInstanceIndex values, leading to surface/material aliasing or OOB access.
    if (surfaceIndex + instanceCount - 1 > SURFACE_INDEX_MAX_VALUE) {
      const uint32_t maxAllowed = (surfaceIndex <= SURFACE_INDEX_MAX_VALUE) ? (SURFACE_INDEX_MAX_VALUE - surfaceIndex + 1) : 0;
      ONCE(Logger::err(str::format("DxvkRaytrace: PointInstancer needs ", instanceCount, " surface slots starting at ", surfaceIndex, " but only ", maxAllowed,
                                   " fit within SURFACE_INDEX_MAX_VALUE (", SURFACE_INDEX_MAX_VALUE, "). Clamping to ", maxAllowed, " instances.")));
      instanceCount = maxAllowed;
      if (instanceCount == 0) {
        return;
      }
    }

    rtInstance->surface.surfaceIndexOfFirstInstance = surfaceIndex;
    m_reorderedSurfaces.insert(m_reorderedSurfaces.end(), instanceCount, rtInstance);
    m_reorderedSurfacesFirstIndexOffset.insert(m_reorderedSurfacesFirstIndexOffset.end(), instanceCount, 0);

    // Determine TLAS type
    const bool isUnordered = rtInstance->usesUnorderedApproximations() &&
      RtxOptions::enableSeparateUnorderedApproximations();
    const auto primaryType = isUnordered ? Tlas::Unordered : Tlas::Opaque;

    // Reserve N slots for the GPU shader to fill — nothing pushed to m_mergedInstances
    const uint32_t firstIndexInType = m_pointInstancerSlotsPerType[primaryType];
    m_pointInstancerSlotsPerType[primaryType] += instanceCount;

    // Build upper bits for instanceCustomIndex
    const uint32_t upperBits = rtInstance->getVkInstance().instanceCustomIndex & ~uint32_t(CUSTOM_INDEX_SURFACE_MASK);
    VkGeometryInstanceFlagsKHR flags = rtInstance->getVkInstance().flags;
    if (rtInstance->isObjectToWorldMirrored()) {
      flags ^= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
    }

    // Record batch for GPU dispatch
    PointInstancerBatch batch {};
    batch.transforms = transforms;
    batch.objectToWorld = rtInstance->surface.objectToWorld;
    batch.prevObjectToWorld = rtInstance->surface.prevObjectToWorld;
    batch.instanceCount = instanceCount;
    batch.baseSurfaceIndex = surfaceIndex;
    batch.customIndexFlags = upperBits;
    batch.instanceMask = rtInstance->getVkInstance().mask;
    batch.sbtOffsetAndFlags = (rtInstance->getVkInstance().instanceShaderBindingTableRecordOffset & 0x00FFFFFFu) | (static_cast<uint32_t>(flags) << 24);
    batch.blasReference = blasEntry->dynamicBlas->accelerationStructureReference;
    batch.firstIndexInType = firstIndexInType;
    batch.tlasType = primaryType;
    batch.instanceBufferByteOffset = 0; // resolved before dispatch
    m_pointInstancerBatches.push_back(batch);

    // Also reserve SSS TLAS slots if needed
    if (!isUnordered && rtInstance->isSubsurface()) {
      PointInstancerBatch sssBatch = batch;
      sssBatch.firstIndexInType = m_pointInstancerSlotsPerType[Tlas::SSS];
      sssBatch.tlasType = Tlas::SSS;
      m_pointInstancerSlotsPerType[Tlas::SSS] += instanceCount;
      m_pointInstancerBatches.push_back(sssBatch);
    }
  }

}  // namespace dxvk
