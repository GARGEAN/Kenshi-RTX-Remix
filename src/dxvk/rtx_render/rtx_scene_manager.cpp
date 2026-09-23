#include "../../util/util_kenshi_telemetry.h"
#include "rtx/dx11/dx11_light_state.h"
#include "../../util/util_kenshi_prepared_terrain.h"
#include "rtx/dx11/dx11_material_fog_state.h"
/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
#include <atomic>
#include <mutex>
#include <vector>

#include "rtx_asset_replacer.h"
#include "rtx_scene_manager.h"
#include "../../util/util_kenshi_origin.h"
#include "../../util/util_kenshi_material_probe.h"
#include "../../util/util_kenshi_fault.h"
#include "../../util/util_kenshi_terrain_profile.h"
#include "rtx_opacity_micromap_manager.h"
#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_buffer.h"
#include "dxvk_memory_tracker.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "rtx_kenshi_options.h"
#include "rtx_kenshi_flicker_trace.h"
#include <map>
#include <iomanip>
#include "rtx_terrain_baker.h"
#include "rtx_texture_manager.h"
#include "rtx_texture_owner_census.h"
#include "rtx_cache_maintenance.h"
#include "../../util/util_kenshi_terrain_audit.h"
#include <chrono>
#include "rtx_xess.h"

#include <assert.h>
#include <cstdio>
#include <unordered_set>
#include <mutex>

#include "../d3d11/d3d11_state.h"
#include "vulkan/vulkan_core.h"

#include "rtx_game_capturer.h"
#include "rtx_matrix_helpers.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_lights_data.h"
#include "rtx_light_utils.h"

#include "../util/util_global_time.h"

#include "rtx/pass/particles/particle_system_common.h"

namespace {
  // helper function to ensure generating spatialMapHash for external draws is done the same way in multiple places.
  XXH64_hash_t spatialMapHashForExternalDrawMesh(remixapi_MeshHandle mesh) {
    const uintptr_t meshId = reinterpret_cast<uintptr_t>(mesh);
    return XXH3_64bits(&meshId, sizeof(meshId));
  }
} // namespace

namespace dxvk {

  #include "rtx_opaque_preparation.inl"
  #include "rtx_terrain_origin_history.inl"

  namespace KenshiFlickerTrace {
    // Multiplicities matter: two identical rows are two draws, not one.
    struct Census {
      std::map<std::string, uint32_t> previous, current;
      std::map<std::string, uint64_t> rowIds;
      uint64_t nextRowId = 0;
    };
    static std::map<std::string, Census> s_census;
    static uint32_t s_entry = 0;
    static uint32_t s_sourceFrame = 0;
    static bool s_inDraw = false;

    void add(const char* stage, std::string row) {
      if ((kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal()))
        ++s_census[stage].current[std::move(row)];
    }

    static std::string describeDraw(const DrawCallState& draw) {
      const auto& geo = draw.getGeometryData();
      const auto& t = draw.getTransformData().objectToWorld;
      const bool valid = geo.boundingBox.isValid();
      const Vector3 centre = valid ? geo.boundingBox.getTransformedCentroid(t) : Vector3(0.f);
      return str::format(std::setprecision(9),
        " draw=", draw.drawCallID, " posHash=", std::hex,
        geo.hashes[HashComponents::VertexPosition], " idxHash=",
        geo.hashes[HashComponents::Indices], " mat=", draw.getMaterialData().getHash(),
        " tex=", draw.getMaterialData().getColorTexture().getImageHash(), std::dec,
        " verts=", geo.vertexCount, " indices=", geo.indexCount,
        " bboxValid=", valid, " centre=", centre.x, ",", centre.y, ",", centre.z,
        " o2w=", t[0][0], ",", t[0][1], ",", t[0][2], ",",
        t[1][0], ",", t[1][1], ",", t[1][2], ",",
        t[2][0], ",", t[2][1], ",", t[2][2], ",",
        t[3][0], ",", t[3][1], ",", t[3][2],
        " clip=", draw.getTransformData().enableClipPlane);
    }

    void beginDraw(uint32_t sourceFrame, uint32_t entry, const DrawCallState& draw) {
      if (!kenshi_telemetry::enabled() || !(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal())) return;
      s_entry = entry;
      s_sourceFrame = sourceFrame;
      s_inDraw = true;
      add("submit", str::format("sourceFrame=", sourceFrame, " entry=", entry, describeDraw(draw)));
    }

    void endDraw() { s_inDraw = false; }

    std::string describeInstance(const RtInstance* instance) {
      if (!instance) return "inst=none";
      const auto& t = instance->surface.objectToWorld;
      const auto* blas = instance->isMarkedForGC() ? nullptr : instance->getBlas();
      const bool valid = blas && blas->input.getGeometryData().boundingBox.isValid();
      const Vector3 centre = valid
        ? blas->input.getGeometryData().boundingBox.getTransformedCentroid(t) : Vector3(0.f);
      return str::format(std::setprecision(9), "inst=", instance->getId(),
        " identity=", instance->getCacheIdentity(), " hidden=", instance->isHidden(),
        " gc=", instance->isMarkedForGC(), " mask=", uint32_t(instance->getVkInstance().mask),
        " posHash=", std::hex, blas ? blas->input.getGeometryData().hashes[HashComponents::VertexPosition] : 0ull,
        " idxHash=", instance->getIndexHash(), " mat=", instance->getMaterialHash(), std::dec,
        " bboxValid=", valid, " centre=", centre.x, ",", centre.y, ",", centre.z,
        " o2w=", t[0][0], ",", t[0][1], ",", t[0][2], ",",
        t[1][0], ",", t[1][1], ",", t[1][2], ",",
        t[2][0], ",", t[2][1], ",", t[2][2], ",",
        t[3][0], ",", t[3][1], ",", t[3][2]);
    }

    void resolved(const DrawCallState& draw, uint64_t replacementId,
                  uint64_t previousId, const RtInstance* instance) {
      if (!(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal()) || !s_inDraw) return;
      add("scene", str::format("sourceFrame=", s_sourceFrame, " entry=", s_entry, " ri=", replacementId,
        " previous=", previousId,
        describeDraw(draw), " ", describeInstance(instance)));
    }

    void finish(uint32_t frame) {
      if (!(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal())) return;
      // Every frame closes explicitly. No capture window, sampling or lifetime cap.
      // + and - update a multiset; unchanged rows persist from the previous frame.
      for (auto& stage : s_census) {
        auto& census = stage.second;
        uint32_t total = 0, added = 0, removed = 0;
        for (const auto& row : census.current) {
          total += row.second;
          const auto old = census.previous.find(row.first);
          const uint32_t before = old == census.previous.end() ? 0u : old->second;
          if (row.second > before) {
            added += row.second - before;
            const auto id = census.rowIds.emplace(row.first, ++census.nextRowId).first->second;
            KENSHI_DIAGNOSTIC_INFO(str::format("[Kenshi][flicker-v653] f=", frame, " stage=", stage.first,
              " +", row.second - before, " row=", id, " ", row.first));
          }
        }
        for (const auto& row : census.previous) {
          const auto next = census.current.find(row.first);
          const uint32_t after = next == census.current.end() ? 0u : next->second;
          if (row.second > after) {
            removed += row.second - after;
            KENSHI_DIAGNOSTIC_INFO(str::format("[Kenshi][flicker-v653] f=", frame, " stage=", stage.first,
              " -", row.second - after, " row=", census.rowIds.at(row.first)));
            if (after == 0u) census.rowIds.erase(row.first);
          }
        }
        KENSHI_DIAGNOSTIC_INFO(str::format("[Kenshi][flicker-v653] f=", frame, " stage=", stage.first,
          " end total=", total, " added=", added, " removed=", removed));
        census.previous = std::move(census.current);
        census.current.clear();
      }
    }
  }

  namespace {
    using KenshiTerrainNormalData = LegacyMaterialData::KenshiTerrainNormalSet;

    std::mutex s_kenshiTerrainNormalMutex;
    std::unordered_map<XXH64_hash_t, std::weak_ptr<KenshiTerrainNormalData>> s_kenshiTerrainNormalSets;

    using KenshiTerrainBiomeData =
      std::array<LegacyMaterialData::KenshiTerrainBiomeSet,
                 LegacyMaterialData::kKenshiTerrainBiomeSetCount>;
    std::atomic<float> s_kenshiWaterParams[19] = {};

    // DX11_V636_KENSHI_INTERIOR_CLIP. Mutex rather than atomics: this is a
    // struct that must be read as a consistent whole, unlike the water params
    // where each scalar stands alone.
    std::mutex s_kenshiInteriorVolumeMutex;
    std::vector<SceneManager::KenshiInteriorVolume> s_kenshiInteriorVolumes;
    uint32_t s_kenshiInteriorVolumeFrame = 0u;
    // DX11_V618: the per-BIOME table, five fixed slots addressed by the
    // material's own blendChannel. Written from the submit thread, read when the
    // buffer is built, so it takes a mutex like the terrain sets do.
    struct KenshiWaterBiomeRecord {
      SceneManager::KenshiWaterParams params;
      SceneManager::KenshiWaterBiomeTextures textures;
      bool valid = false;
    };
    KenshiWaterBiomeRecord s_kenshiWaterBiomes[SceneManager::kKenshiWaterBiomeCount];
    // DX11_V623: the whole-map trio, shared by every biome.
    SceneManager::KenshiWaterBiomeTextures s_kenshiWaterGlobalTextures;
    std::mutex s_kenshiWaterZoneMutex;
    std::atomic<float> s_kenshiWetness { 0.0f };
    std::atomic<float> s_kenshiWaterHeight { 0.0f };
    // DX11_V557: Kenshi's dust is a BIOME property, published the same
    // last-draw-wins way V541 proved correct for wetness - every shader
    // carrying it reports the same value at the same instant.
    std::atomic<float> s_kenshiDustColourR { 0.0f };
    std::atomic<float> s_kenshiDustColourG { 0.0f };
    std::atomic<float> s_kenshiDustColourB { 0.0f };
    std::atomic<float> s_kenshiDustAmountX { 0.0f };
    std::atomic<float> s_kenshiDustAmountY { 0.0f };
    std::atomic<float> s_kenshiDustAmountZ { 0.0f };

    std::mutex s_kenshiTerrainBiomeMutex;
    std::unordered_map<uint64_t, std::weak_ptr<KenshiTerrainBiomeData>> s_kenshiTerrainBiomeSets;

    XXH64_hash_t getKenshiTerrainBaseIdentity(const LegacyMaterialData& materialData) {
      XXH64_hash_t identity = XXH64(
        &materialData.kenshiTerrainDetailScale, sizeof(Vector2), 0);
      identity = XXH64(
        &materialData.kenshiTerrainDetailOffset, sizeof(Vector2), identity);
      for (const Vector2& scale : materialData.kenshiTerrainLayerScales)
        identity = XXH64(&scale, sizeof(scale), identity);
      identity = XXH64(
        &materialData.kenshiTerrainSlopeMin, sizeof(Vector4), identity);
      identity = XXH64(
        &materialData.kenshiTerrainSlopeMax, sizeof(Vector4), identity);
      identity = XXH64(
        &materialData.kenshiTerrainSlopeBlend, sizeof(Vector4), identity);
      identity = XXH64(
        &materialData.kenshiTerrainOverlayMult, sizeof(Vector4), identity);
      identity = XXH64(
        &materialData.kenshiTerrainBrightnessFix, sizeof(float), identity);
      identity = XXH64(
        &materialData.kenshiTerrainHeightOffset, sizeof(float), identity);
      for (const TextureRef& layer : materialData.kenshiTerrainLayers) {
        const uint64_t key = layer.getUniqueKey();
        identity = XXH64(&key, sizeof(key), identity);
      }
      const uint64_t overlayKey = materialData.kenshiTerrainOverlay.getUniqueKey();
      return XXH64(&overlayKey, sizeof(overlayKey), identity);
    }
  }

  // Compute a hash that can be used to check if an external draw is identical to the previous frame's draw.
  XXH64_hash_t ExternalDrawState::computeExternalDrawIdentityHash() const {
    struct ExternalDrawIdentityHashData {
      uintptr_t meshId;
      XXH64_hash_t materialHash;
      XXH64_hash_t boneHash;
      CameraType::Enum cameraType;
      uint32_t categoriesRaw;
      XXH64_hash_t particleDescHash;
      XXH64_hash_t gpuInstancingHash;
      TexGenMode texgenMode;
      uint8_t usesVertexShader;
      uint8_t usesPixelShader;
      uint8_t zWriteEnable;
      uint8_t zEnable;
      uint8_t skyAutoDetected;
      uint8_t _pad0;
      uint8_t _pad1;
      Matrix4 objectToWorld;
      Matrix4 textureTransform;
    };
    static_assert(
      sizeof(ExternalDrawIdentityHashData) == 184,
      "recheck the memory layout, and ensure that there are no holes, "
      "as the padding might be default-initialized to non-zero, and XXH3_64bits is used on memory range"
    );

    ExternalDrawIdentityHashData data{};

    const DrawCallTransforms& transforms = drawCall.getTransformData();
    data.meshId = reinterpret_cast<uintptr_t>(mesh);
    data.materialHash = drawCall.getMaterialData().getHash();
    data.boneHash = drawCall.getSkinningState().boneHash;
    data.cameraType = cameraType;
    data.categoriesRaw = categories.raw();

    if (optionalParticleDesc.has_value()) {
      data.particleDescHash = optionalParticleDesc->calcHash();
    }

    if (!gpuInstancingTransforms.empty()) {
      data.gpuInstancingHash = XXH3_64bits(
          gpuInstancingTransforms.data(),
          gpuInstancingTransforms.size() * sizeof(Matrix4));
    }

    data.texgenMode = transforms.texgenMode;
    data.usesVertexShader = drawCall.usesVertexShader ? 1u : 0u;
    data.usesPixelShader = drawCall.usesPixelShader ? 1u : 0u;
    data.zWriteEnable = drawCall.zWriteEnable ? 1u : 0u;
    data.zEnable = drawCall.zEnable ? 1u : 0u;
    data.skyAutoDetected = drawCall.skyAutoDetected ? 1u : 0u;
    data.objectToWorld = transforms.objectToWorld;
    data.textureTransform = transforms.textureTransform;

    return XXH3_64bits(&data, sizeof(data));
  }

  SceneManager::SceneManager(DxvkDevice* device)
    : CommonDeviceObject(device)
    , m_instanceManager(device, this)
    , m_accelManager(device)
    , m_lightManager(device)
    , m_graphManager()
    , m_rayPortalManager(device, this)
    , m_drawCallCache(device)
    , m_drawCallTracker(device)
    , m_bindlessResourceManager(device)
    , m_pReplacer(new AssetReplacer())
    , m_terrainBaker(new TerrainBaker())
    , m_cameraManager(device)
    , m_uniqueObjectSearchDistance(RtxOptions::uniqueObjectDistance()) {
    InstanceEventHandler instanceEvents(this);
    instanceEvents.onInstanceAddedCallback = [this](RtInstance& instance) { onInstanceAdded(instance); };
    instanceEvents.onInstanceUpdatedCallback = [this](RtInstance& instance, const DrawCallState& drawCall, const MaterialData& material, bool hasTransformChanged, bool hasVerticesChanged, bool isFirstUpdateThisFrame) { onInstanceUpdated(instance, drawCall, material, hasTransformChanged, hasVerticesChanged, isFirstUpdateThisFrame); };
    instanceEvents.onInstanceDestroyedCallback = [this](RtInstance& instance) { onInstanceDestroyed(instance); };
    m_instanceManager.addEventHandler(instanceEvents);
    
    if (env::getEnvVar("DXVK_RTX_CAPTURE_ENABLE_ON_FRAME") != "") {
      m_beginUsdExportFrameNum = stoul(env::getEnvVar("DXVK_RTX_CAPTURE_ENABLE_ON_FRAME"));
    }
  }

  SceneManager::~SceneManager() {
    opaque_preparation::forget(this);
    prepared_terrain::invalidate(prepared_terrain::Invalidation::Scene);
    {
      std::lock_guard<std::mutex> lock(s_kenshiTerrainNormalMutex);
      s_kenshiTerrainNormalSets.clear();
    }
    std::lock_guard<std::mutex> lock(s_kenshiTerrainBiomeMutex);
    s_kenshiTerrainBiomeSets.clear();
  }

  bool SceneManager::areAllReplacementsLoaded() const {
    return m_pReplacer->areAllReplacementsLoaded();
  }

  std::vector<Mod::State> SceneManager::getReplacementStates() const {
    return m_pReplacer->getReplacementStates();
  }

  void SceneManager::initialize(Rc<DxvkContext> ctx) {
    ScopedCpuProfileZone();
    m_pReplacer->initialize(ctx);
  }

  void SceneManager::logStatistics() {
    if (m_opacityMicromapManager.get()) {
      m_opacityMicromapManager->logStatistics();
    }
  }

  Vector3 SceneManager::getSceneUp() {
    return RtxOptions::zUp() ? Vector3(0.f, 0.f, 1.f) : Vector3(0.f, 1.f, 0.f);
  }

  Vector3 SceneManager::getSceneForward() {
    return RtxOptions::zUp() ? Vector3(0.f, 1.f, 0.f) : Vector3(0.f, 0.f, 1.f);
  }

  Vector3 SceneManager::calculateSceneRight() {
    const Vector3 up = SceneManager::getSceneUp();
    const Vector3 forward = SceneManager::getSceneForward();
    return RtxOptions::leftHandedCoordinateSystem() ? cross(up, forward) : cross(forward, up);
  }

  Vector3 SceneManager::worldToSceneOrientedVector(const Vector3& worldVector) {
    return RtxOptions::zUp() ? worldVector : Vector3(worldVector.x, worldVector.z, worldVector.y);
  }

  Vector3 SceneManager::sceneToWorldOrientedVector(const Vector3& sceneVector) {
    // Same transform applies to and from
    return worldToSceneOrientedVector(sceneVector);
  }

  float SceneManager::getTotalMipBias() {
    auto& resourceManager = m_device->getCommon()->getResources();
  
    const bool temporalUpscaling = RtxOptions::isDLSSOrRayReconstructionEnabled() || RtxOptions::isXeSSEnabled() || RtxOptions::isTAAEnabled();
    
    float totalUpscaleMipBias = 0.0f;
    
    if (temporalUpscaling) {
      if (RtxOptions::isXeSSEnabled()) {
        // XeSS uses the new formula from the XeSS developer guide
        totalUpscaleMipBias = -log2(resourceManager.getUpscaleRatio());
        
        // Add XeSS-specific mip bias when XeSS is active
        DxvkXeSS& xess = m_device->getCommon()->metaXeSS();
        if (xess.isActive()) {
          float xessMipBias = xess.calcRecommendedMipBias();
          totalUpscaleMipBias += xessMipBias;
        }
      } else {
        // Restore original behavior for DLSS, TAA, and other upscalers
        totalUpscaleMipBias = log2(resourceManager.getUpscaleRatio()) + RtxOptions::upscalingMipBias();
      }
    }
    
    return totalUpscaleMipBias + RtxOptions::nativeMipBias();
  }

  float SceneManager::getCalculatedUpscalingMipBias() {
    auto& resourceManager = m_device->getCommon()->getResources();
    
    const bool temporalUpscaling = RtxOptions::isXeSSEnabled();
    if (!temporalUpscaling) {
      return 0.0f;
    }
    
    float calculatedUpscalingBias = -log2(resourceManager.getUpscaleRatio());
    return calculatedUpscalingBias;
  }

  void SceneManager::clear(Rc<DxvkContext> ctx, bool needWfi, bool preserveBuiltOmms) {
    m_accelManager.logMemoryOwnership(m_drawCallCache, "clear-before", true);
    if (terrain_origin::history.owner == this)
      terrain_origin::history = terrain_origin::State{};
    kenshi_fault::add(kenshi_fault::SceneReset,m_device->getCurrentFrameId(),needWfi);
    prepared_terrain::invalidate(prepared_terrain::Invalidation::Scene);
    ScopedCpuProfileZone();

    auto& textureManager = m_device->getCommon()->getTextureManager();

    // Only clear once after the scene disappears, to avoid adding a WFI on every frame through clear().
    if (needWfi) {
      if (ctx.ptr())
        ctx->flushCommandList();
      m_device->waitForIdle();
    }

    // We still need to clear caches even if the scene wasn't rendered
    m_bufferCache.clear();
    m_surfaceMaterialCache.clear();
    m_preCreationSurfaceMaterialMap.clear();
    m_surfaceMaterialExtensionCache.clear();
    m_volumeMaterialCache.clear();
    
    // Clear ReplacementInstances first: their destructors call clear() which
    // accesses prims[] to mark entities for GC and clear back-pointers.
    // Entities must still be alive at this point.
    m_drawCallTracker.clear();

    // Called before instance manager's clear, so that it resets all tracked instances in Opacity Micromap manager at once
    if (m_opacityMicromapManager.get()) {
      if (preserveBuiltOmms) m_opacityMicromapManager->onCameraCutSceneClear();
      else m_opacityMicromapManager->clear();
    }

    // Invalidate AccelManager's bucket cache before InstanceManager::clear() deletes
    // every RtInstance. The cache holds raw RtInstance* in m_cachedBuckets[].instances /
    // .surfaces and m_instanceBucketIndex, and the next frame's mergeInstancesIntoBlas
    // dirty check would dereference those (now-freed) pointers. The per-instance
    // onInstanceDestroyed -> removeInstanceFromBucketCache hook only patches the index
    // map, not the vectors, so a bulk reset must drop the cache wholesale.
    m_accelManager.clear();

    m_instanceManager.clear();
    m_lightManager.clear();
    m_graphManager.clear();
    m_rayPortalManager.clear();
    m_drawCallCache.clear();
    textureManager.clear();

    m_accelManager.logMemoryOwnership(m_drawCallCache, "clear-after", true);

    m_previousFrameSceneAvailable = false;
    m_startInMediumMaterialIndex = SURFACE_INDEX_INVALID;
    m_fogStartInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;
    m_externalStartInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;
    m_startInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;
  }

  void SceneManager::garbageCollection() {
    ScopedCpuProfileZone();

    // BlasEntry GC: remove entries not touched recently.
    // Only GC entries with no linked instances — instances still reference the BlasEntry
    // for TLAS build, and destroying it would cause a one-frame visibility gap.
    if (m_device->getCurrentFrameId() > RtxOptions::numFramesToKeepGeometryData()) {
      const size_t oldestFrame = m_device->getCurrentFrameId() - RtxOptions::numFramesToKeepGeometryData();
      auto& entries = m_drawCallCache.getEntries();
      for (auto iter = entries.begin(); iter != entries.end(); ) {
        if (iter->second.frameLastTouched < oldestFrame &&
            iter->second.getLinkedInstances().empty()) {
          iter = entries.erase(iter);
        } else {
          ++iter;
        }
      }
    }

    // ReplacementInstance GC: marks owned instances/lights for GC
    // and clears their back-pointers while they are still alive.
    m_drawCallTracker.garbageCollectReplacementInstances(getCamera(), m_isAntiCullingSupported);

    // Instance/light GC: removes entities marked for GC by ReplacementInstance::clear()
    // or marked on creation (ephemeral copies). Back-pointers are already null.
    m_instanceManager.garbageCollection();
    m_accelManager.garbageCollection();
    m_lightManager.garbageCollection(getCamera());
    m_rayPortalManager.garbageCollection();
  }

  void SceneManager::onDestroy() {
    m_accelManager.onDestroy();
    if (m_opacityMicromapManager) {
      m_opacityMicromapManager->onDestroy();
    }
  }

  template<bool isNew>
  SceneManager::ObjectCacheState SceneManager::processGeometryInfo(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, RaytraceGeometry& inOutGeometry, bool geometryBufferAllocationOnly) {
    terrain_profile::Scope terrainCpuGeometry(terrain_profile::Stage::Geometry);
    ScopedCpuProfileZone();
    ObjectCacheState result = ObjectCacheState::KBuildBVH;
    const RasterGeometry& input = drawCallState.getGeometryData();

    // Determine the optimal object state for this geometry
    if (!isNew) {
      // This is a geometry we've seen before, that requires updating
      //  'inOutGeometry' has valid historical data
      if (input.hashes[HashComponents::Indices] == inOutGeometry.hashes[HashComponents::Indices]) {
        // Check if the vertex positions have changed, requiring a BVH refit
        if (input.hashes[HashComponents::VertexPosition] == inOutGeometry.hashes[HashComponents::VertexPosition]
         && input.hashes[HashComponents::VertexShader] == inOutGeometry.hashes[HashComponents::VertexShader]
         && drawCallState.getSkinningState().boneHash == inOutGeometry.lastBoneHash) {
          result = ObjectCacheState::kUpdateInstance;
        } else {
          result = ObjectCacheState::kUpdateBVH;
        }
      }
    }

    if (terrain_profile::enabled()) {
      const bool skinned = drawCallState.getSkinningState().numBones > 0;
      const bool particle = drawCallState.getCategoryFlags().test(InstanceCategories::Particle);
      const auto reason = [&](terrain_profile::GeometryReason r) {
        terrain_profile::geometryReason(skinned, particle, r);
      };
      if (isNew) reason(terrain_profile::GeometryReason::New);
      else if (result == ObjectCacheState::KBuildBVH) reason(terrain_profile::GeometryReason::Topology);
      else if (result == ObjectCacheState::kUpdateBVH) {
        if (drawCallState.getSkinningState().boneHash != inOutGeometry.lastBoneHash)
          reason(terrain_profile::GeometryReason::Pose);
        if (input.hashes[HashComponents::VertexPosition] != inOutGeometry.hashes[HashComponents::VertexPosition])
          reason(terrain_profile::GeometryReason::Vertex);
        if (input.hashes[HashComponents::VertexShader] != inOutGeometry.hashes[HashComponents::VertexShader])
          reason(terrain_profile::GeometryReason::Shader);
      }
    }

    // Copy the input directly to the output as a starting point for our modified geometry data
    RaytraceGeometry output = inOutGeometry;

    output.lastBoneHash = drawCallState.getSkinningState().boneHash;

    // Update draw parameters
    output.cullMode = input.cullMode;
    output.frontFace = input.frontFace;

    // Copy the hashes over
    output.hashes = input.hashes;

    if (!input.positionBuffer.defined()) {
      ONCE(Logger::err("processGeometryInfo: no position data on input detected"));
      return ObjectCacheState::kInvalid;
    }

    if (input.vertexCount == 0) {
      ONCE(Logger::err("processGeometryInfo: input data is violating some assumptions"));
      return ObjectCacheState::kInvalid;
    }

    // Set to 1 if inspection of the GeometryData structures contents on CPU is desired
    //
    // DX11_V379_BLAS_INPUT_DUMP: turned ON. This is the last unexamined link for
    // Kenshi's invisible geometry.
    //
    // The BLAS never reads the game's vertex buffer - it reads THIS buffer, which
    // Remix fills either by a straight copyBuffer (interleaved GPU-friendly input) or
    // by the interleaveGeometry compute dispatch. Every stage on either side of it has
    // now been measured correct for the wall: admission, camera, per-instance
    // transform (cross-checked against Kenshi's own capture data), instance mask,
    // hidden/GC, TLAS presence, BLAS reference, merged-vs-dynamic routing, bucket
    // build, culling, winding, and BLAS update-mode validity. Rays still report a
    // genuine miss. The bytes handed to the acceleration structure are what is left.
    //
    // Kenshi's own vertex buffers are device-local and cannot be read (`mapped=0` for
    // every draw, and RenderDoc's converted captures omit buffer contents), but this
    // one is Remix's, so making it HOST_VISIBLE lets it be read directly with no
    // staging copy or compute pass. Upstream left the switch here for exactly this.
    //
    // Cost: geometry buffers move out of device-local memory, so expect lower
    // performance while this is enabled. It is a diagnostic build, not a shipping one.
    //
    // REVERTED to 0 after it answered its question. Measured cost with it on: about
    // 20 fps, because every BLAS build and every ray hit then reads vertex data from
    // system RAM across PCIe. The `[blas-input]` dump below reports "unmapped" while
    // this is 0, which is the honest result rather than a wrong one - set it back to
    // 1 only for a deliberate one-off inspection, never for a play session.
    //
    // What it established, so it need not be re-run: the wall segment's BLAS input is
    // byte-perfect - readable=97664 >= needed=97608, stride=56, attrOffset=0, and
    // vertices matching RenderDoc's VS Input to the digit.
    #define DEBUG_GEOMETRY_MEMORY 0
    constexpr VkMemoryPropertyFlags memoryProperty = DEBUG_GEOMETRY_MEMORY ? (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    // Assume we won't need this, and update the value if required
    output.previousPositionBuffer = RaytraceBuffer();

    // When the SmoothNormals category is set and the input has no normals, force the interleaved
    // vertex layout to include space for normals. The smooth normals compute pass will fill them in later.
    const bool needsSmoothNormals = drawCallState.categories.test(InstanceCategories::SmoothNormals);
    const bool forceNormals = needsSmoothNormals && !input.normalBuffer.defined();

    // When smooth normals state changes (added or removed), promote to kUpdateBVH so the vertex
    // data is re-interleaved and the smooth normals dispatch runs (or original normals are restored).
    if (needsSmoothNormals != output.smoothNormalsApplied && result == ObjectCacheState::kUpdateInstance) {
      result = ObjectCacheState::kUpdateBVH;
    }

    // DX11_V395_LATE_VERTEX_STREAMS: the same reasoning for streams that ARRIVE
    // (or vanish) after a geometry has already been interleaved once.
    //
    // The cache state is decided from the index, vertex-position and vertex
    // shader hashes only. Streams the D3D11 bridge recovers by replaying the
    // vertex shader - UVs, and now world normals - are GPU-written and cannot
    // be hashed CPU-side, so they are deliberately absent from that identity.
    // Static geometry therefore lands on kUpdateInstance and is never
    // re-interleaved, which freezes whatever streams it happened to have the
    // FIRST time it was seen: a terrain chunk whose first submission lost the
    // per-frame capture budget kept an interleaved layout with no texcoord and
    // no normal for the life of its BLAS. On screen that is a tile stuck at a
    // single colour (its UV resolves to 0,0) and flat shaded, permanently,
    // while every per-draw counter reads clean because the bridge did ship
    // both streams on later frames.
    //
    // Presence is the whole trigger - the contents of these buffers change
    // identity through the position/index hashes as usual.
    const bool normalPresenceChanged =
      input.normalBuffer.defined() != output.normalBuffer.defined();
    const bool texcoordPresenceChanged =
      input.texcoordBuffer.defined() != output.texcoordBuffer.defined();
    const bool colorPresenceChanged =
      input.color0Buffer.defined() != output.color0Buffer.defined();
    if ((normalPresenceChanged || texcoordPresenceChanged || colorPresenceChanged)
     && result == ObjectCacheState::kUpdateInstance) {
      result = ObjectCacheState::kUpdateBVH;

      static uint32_t sLateStreamLogCount = 0;
      if (sLateStreamLogCount < 16u) {
        ++sLateStreamLogCount;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Geometry] re-interleaving for a late vertex stream: normal ",
          output.normalBuffer.defined() ? 1 : 0, "->",
          input.normalBuffer.defined() ? 1 : 0,
          " texcoord ", output.texcoordBuffer.defined() ? 1 : 0, "->",
          input.texcoordBuffer.defined() ? 1 : 0,
          " color ", output.color0Buffer.defined() ? 1 : 0, "->",
          input.color0Buffer.defined() ? 1 : 0,
          " vertices=", input.vertexCount));
      }
    }
    if (!needsSmoothNormals) {
      output.smoothNormalsApplied = false;
    }

    // If forceNormals is true, we can't use the fast "already interleaved" path since
    // we need to change the layout to include normal space.
    const bool preserveVertexLayout = input.isVertexDataInterleaved() && input.areFormatsGpuFriendly()
      && !forceNormals && !input.postVsPositionIsHomogeneousClip;
    const size_t vertexStride = preserveVertexLayout
      ? input.positionBuffer.stride()
      : RtxGeometryUtils::computeOptimalVertexStride(input, forceNormals);

    // Allocation sizes are cache-line rounded. Two different layouts can have
    // the same allocation size, especially on small props; history still uses
    // the old stride/offset. Validate the actual position layout and span.
    const uint32_t positionOffset = preserveVertexLayout
      ? input.positionBuffer.offsetFromSlice() : 0u;
    const bool historyLayoutChanged = !isNew &&
      (output.positionBuffer.stride() != vertexStride ||
       output.positionBuffer.offsetFromSlice() != positionOffset ||
       output.vertexCount != input.vertexCount);
    if (historyLayoutChanged && result == ObjectCacheState::kUpdateInstance)
      result = ObjectCacheState::kUpdateBVH;

    switch (result) {
      case ObjectCacheState::KBuildBVH: {
        // Set up the ideal vertex params, if input vertices are interleaved, it's safe to assume the positionBuffer stride is the vertex stride
        output.vertexCount = input.vertexCount;

        const size_t vertexBufferSize = output.vertexCount * vertexStride;

        // Set up the ideal index params
        output.indexCount = input.isTopologyRaytraceReady() ? input.indexCount : RtxGeometryUtils::getOptimalTriangleListSize(input);
        const VkIndexType indexBufferType = input.isTopologyRaytraceReady() ? input.indexBuffer.indexType() : RtxGeometryUtils::getOptimalIndexFormat(output.vertexCount);
        const size_t indexStride = (indexBufferType == VK_INDEX_TYPE_UINT16) ? 2 : 4;

        // Make sure we're not stomping something else...
        assert(output.indexCacheBuffer == nullptr && output.historyBuffer[0] == nullptr);

        // Create a index buffer and vertex buffer we can use for raytracing.
        DxvkBufferCreateInfo info;
        info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        // Index generation and post-VS vertex interleaving write these buffers
        // from compute shaders before the acceleration-structure build reads
        // them. Declare the complete producer/consumer set so DXVK can emit the
        // required barriers; transfer/ray-tracing-only metadata left the build
        // racing uninitialized index or vertex data and could lose the device.
        info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                    | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                    | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        info.access = VK_ACCESS_TRANSFER_WRITE_BIT
                    | VK_ACCESS_SHADER_READ_BIT
                    | VK_ACCESS_SHADER_WRITE_BIT
                    | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        info.size = align(output.indexCount * indexStride, CACHE_LINE_SIZE);
        output.indexCacheBuffer = m_device->createBuffer(info, memoryProperty, DxvkMemoryStats::Category::RTXAccelerationStructure, "Index Cache Buffer");

        // Allocation can fail under memory pressure. These buffers are written by
        // GPU passes through their device address, so a null here is not a dropped
        // draw - it is a write to address 0, which faults the device and takes the
        // whole process down with it.
        if (output.indexCacheBuffer == nullptr) {
          ONCE(Logger::err("processGeometryInfo: index cache buffer allocation failed; dropping geometry rather than writing to a null address."));
          return ObjectCacheState::kInvalid;
        }

        if (!RtxGeometryUtils::cacheIndexDataOnGPU(ctx, input, output)) {
          ONCE(Logger::err("processGeometryInfo: failed to cache index data on GPU"));
          return ObjectCacheState::kInvalid;
        }

        output.indexBuffer = RaytraceBuffer(DxvkBufferSlice(output.indexCacheBuffer), 0, indexStride, indexBufferType);

        info.size = align(vertexBufferSize, CACHE_LINE_SIZE);
        output.historyBuffer[0] = m_device->createBuffer(info, memoryProperty, DxvkMemoryStats::Category::RTXAccelerationStructure, "Geometry Buffer");

        // The interleaver writes vertex data straight through this buffer's device
        // address; a null one becomes a GPU write to 0x0 and a lost device.
        if (output.historyBuffer[0] == nullptr) {
          ONCE(Logger::err("processGeometryInfo: geometry buffer allocation failed; dropping geometry rather than interleaving into a null address."));
          return ObjectCacheState::kInvalid;
        }

        RtxGeometryUtils::cacheVertexDataOnGPU(ctx, input, output, forceNormals);

        if (geometryBufferAllocationOnly) {
          ONCE(KENSHI_DIAGNOSTIC_INFO("[D3D11SceneManager] Vertex-cache upload active; later RT work suppressed."));
        }

        break;
      }
      case ObjectCacheState::kUpdateBVH: {
        bool invalidateHistory = historyLayoutChanged;
        // V734: the copied vertex span follows this draw. dispatchSkinning uses
        // output.vertexCount; retaining an older, larger count can overrun the
        // newly sized input/output buffers after a range change.
        output.vertexCount = input.vertexCount;

        // Stride changed, so we must recreate the previous buffer and use identical data
        if (output.historyBuffer[0]->info().size != align(vertexStride * input.vertexCount, CACHE_LINE_SIZE)) {
          auto desc = output.historyBuffer[0]->info();
          desc.size = align(vertexStride * input.vertexCount, CACHE_LINE_SIZE);
          output.historyBuffer[0] = m_device->createBuffer(desc, memoryProperty, DxvkMemoryStats::Category::RTXAccelerationStructure, "Geometry Buffer");

          if (output.historyBuffer[0] == nullptr) {
            ONCE(Logger::err("processGeometryInfo: geometry buffer reallocation failed on stride change; dropping geometry rather than writing to a null address."));
            return ObjectCacheState::kInvalid;
          }

          // Invalidate the current buffer
          output.historyBuffer[1] = nullptr;

          // Mark this object for realignment
          invalidateHistory = true;
        }
        // DX11_V333_NO_HISTORY_PINGPONG (diagnostic): Kenshi routes ~48 of its
        // 126 recognised draws through kUpdateBVH EVERY frame, because their
        // captured vertices are stored in camera space and so genuinely
        // re-hash whenever the camera moves. That makes this ping-pong run
        // per-frame for the exact set of objects the user sees flickering, and
        // the symptom alternates per frame just as the swap does.
        //
        // Test the parity mechanism directly: interleave into a single buffer
        // and point the previous-position lookup at it. Motion vectors for
        // these objects collapse to zero (denoiser ghosting while the camera
        // moves), which is a visible cost but CANNOT hide geometry, so any
        // change in presence is attributable to the swap rather than to this
        // side effect.
        // DX11_V423_RESTORE_HISTORY_PINGPONG: the V333 diagnostic above was
        // ANSWERED - "History ping-pong (DX11_V333, suppressed): no change to
        // flicker. Not the mechanism." - but the diagnostic was never reverted,
        // so every dynamic object in the game has been rendering with
        // previous == current, i.e. an exactly zero motion vector, ever since.
        //
        // That is invisible for rigid geometry, whose motion vector comes from
        // the instance transform, which is why character WEAPONS track fine.
        // It is total for anything whose motion lives in its vertices, which is
        // exactly what V421's skinned characters are: objectToWorld is identity
        // by construction and all movement is baked into the skinned positions.
        // The reported symptom - body smearing into mush at high zoom while the
        // weapon resolves correctly - is this constant, not a skinning bug.
        static constexpr bool kUseHistoryPingPong = true;

        if (kUseHistoryPingPong) {
          // Use the previous updates vertex data for previous position lookup
          std::swap(output.historyBuffer[0], output.historyBuffer[1]);

          if (output.historyBuffer[0].ptr() == nullptr) {
            // First frame this object has been dynamic need to allocate a 2nd frame of data to preserve history.
            output.historyBuffer[0] = m_device->createBuffer(output.historyBuffer[1]->info(), memoryProperty, DxvkMemoryStats::Category::RTXAccelerationStructure, "Geometry Buffer");

            if (output.historyBuffer[0] == nullptr) {
              ONCE(Logger::err("processGeometryInfo: history buffer allocation failed; dropping geometry rather than writing to a null address."));
              return ObjectCacheState::kInvalid;
            }
          }
        }

        RtxGeometryUtils::cacheVertexDataOnGPU(ctx, input, output, forceNormals, geometryBufferAllocationOnly, geometryBufferAllocationOnly, geometryBufferAllocationOnly);

        // The dynamic diagnostic performs the complete GPU interleave while frontend state restoration protects the native draw.

        // V734: the interleaver has written bind-pose vertices here; skinning
        // runs later. Copying this buffer to history records bind pose as the
        // previous world position. A changed layout has no compatible history.
        // Leave the previous-position binding absent for this update instead.

        // Assign the previous buffer using the last slice (copy most params from the position, just change buffer).
        // Without the ping-pong there is only one buffer, so previous == current
        // and the motion vector is zero - never a null slice, which would be a
        // GPU read from address 0 rather than a missing history.
        if (!invalidateHistory) {
          const Rc<DxvkBuffer>& previousSource = kUseHistoryPingPong
            ? output.historyBuffer[1] : output.historyBuffer[0];
          output.previousPositionBuffer = RaytraceBuffer(DxvkBufferSlice(previousSource, 0, output.positionBuffer.length()), output.positionBuffer.offsetFromSlice(), output.positionBuffer.stride(), output.positionBuffer.vertexFormat());
        }

        if (geometryBufferAllocationOnly) {
          ONCE(KENSHI_DIAGNOSTIC_INFO("[D3D11SceneManager] Dynamic vertex history and buffer tracking active; later RT work suppressed."));
        }
        break;
      }
      default:
        break;
    }

    // Update color buffer in BVH with DrawCallState
    // The user can disable/enable color buffer for specific materials, so we manually sync the DrawCallState and BVH here to keep the color buffer in BVH updated.
    // Note, we don't setup kUpdateBVH because it's too waste to update all buffers if only the color buffer needs to be updated.
    if (output.color0Buffer.defined() && !drawCallState.geometryData.color0Buffer.defined()) {
      // Remove the color buffer in BVH if the color buffer from drawcall is removed by ignoreBakedLighting
      output.color0Buffer = RaytraceBuffer();
    } else if (!output.color0Buffer.defined() && drawCallState.geometryData.color0Buffer.defined()) {
      // Write the color buffer back to BVH if the color buffer is enabled again
      const DxvkBufferSlice slice = DxvkBufferSlice(output.historyBuffer[0]);
      const auto& colorBuffer = drawCallState.geometryData.color0Buffer;

      // DX11_V514_COLOR0_INTERLEAVED_LAYOUT: this RaytraceBuffer describes
      // `historyBuffer`, so it must carry the layout of THAT buffer, not the
      // layout of the draw call's source buffer. The two agree only when the
      // geometry took the already-interleaved fast copy; when it was
      // re-interleaved, color0 sits at the interleaved offset with the
      // interleaved stride. Copying the source geometry makes the hit shader
      // stride across the wrong floats entirely.
      //
      // Source and interleaved layouts can differ in both offset and stride,
      // so the retained buffer must describe the generated history layout.
      const bool sourceLayoutPreserved =
        preserveVertexLayout;

      uint32_t color0Offset = colorBuffer.offsetFromSlice();
      uint32_t color0Stride = colorBuffer.stride();
      if (!sourceLayoutPreserved) {
        // Mirrors the offset accumulation at the tail of
        // RtxGeometryUtils::interleaveGeometry.
        color0Stride = output.positionBuffer.stride();
        color0Offset = uint32_t(sizeof(float) * 3);
        if (output.normalBuffer.defined())
          color0Offset += uint32_t(sizeof(float) * 3);
        if (output.texcoordBuffer.defined())
          color0Offset += uint32_t(sizeof(float) * 2);
      }

      output.color0Buffer = RaytraceBuffer(slice, color0Offset, color0Stride, colorBuffer.vertexFormat());

      static uint32_t sColor0RelayoutLogCount = 0;
      if (sColor0RelayoutLogCount < 8u) {
        ++sColor0RelayoutLogCount;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Geometry][color0-relayout] fastCopy=", sourceLayoutPreserved ? 1 : 0,
          " src=", colorBuffer.offsetFromSlice(), "/", colorBuffer.stride(),
          " used=", color0Offset, "/", color0Stride,
          " hasNormal=", output.normalBuffer.defined() ? 1 : 0,
          " hasTexcoord=", output.texcoordBuffer.defined() ? 1 : 0,
          " posStride=", output.positionBuffer.stride(),
          " vertices=", output.vertexCount));
      }
    }

    // Update buffers in the cache
    updateBufferCache(output);

    // Finalize our modified geometry data to the output
    inOutGeometry = output;

    return result;
  }


  // DX11_V330_CENSUS_ON_DEMAND. A plain counter, never a Vulkan object, so it is
  // safe as a translation-unit static (objects holding device resources here
  // outlive the device and crash at unload).
  static std::atomic<uint32_t> s_vramCensusFramesRequested { 0u };

  // DX11_V332_IDENTITY_CHURN per-frame tallies, read and cleared by the census
  // in onFrameEnd. Plain counters, never Vulkan objects, so they are safe as
  // translation-unit statics.
  static std::atomic<uint32_t> s_frameCacheExisted { 0u };
  static std::atomic<uint32_t> s_frameCacheFresh { 0u };
  static std::atomic<uint32_t> s_frameBuildBvh { 0u };
  static std::atomic<uint32_t> s_frameUpdateBvh { 0u };
  static std::atomic<uint32_t> s_frameUpdateInstance { 0u };

  void SceneManager::requestVramCensus(uint32_t frames) {
    s_vramCensusFramesRequested.store(frames, std::memory_order_relaxed);
  }

  static std::atomic<uint32_t> s_submitWithoutCapture { 0u };

  void SceneManager::reportSubmitWithoutCapture(uint32_t count) {
    s_submitWithoutCapture.store(count, std::memory_order_relaxed);
  }

  static std::atomic<uint32_t> s_staleCaptureReuse { 0u };

  void SceneManager::reportStaleCaptureReuse(uint32_t count) {
    s_staleCaptureReuse.store(count, std::memory_order_relaxed);
  }

  static std::atomic<uint32_t> s_staleAgeSum { 0u };
  static std::atomic<uint32_t> s_staleAgeMax { 0u };
  static std::atomic<uint32_t> s_staleClamped { 0u };

  static std::atomic<uint32_t> s_capAttempts { 0u };
  static std::atomic<uint32_t> s_capCaptured { 0u };
  static std::atomic<uint32_t> s_capBudget { 0u };
  static std::atomic<uint32_t> s_capFailed { 0u };

  static std::atomic<uint32_t> s_drawTraceFramesRemaining { 0u };

  void SceneManager::requestDrawTrace(uint32_t frames) {
    s_drawTraceFramesRemaining.store(frames, std::memory_order_relaxed);
  }

  static std::atomic<uint32_t> s_injectSkipReason { 4u };

  void SceneManager::reportInjectSkipReason(uint32_t reason) {
    s_injectSkipReason.store(reason, std::memory_order_relaxed);
  }

  void SceneManager::reportCaptureStats(uint32_t attempts, uint32_t captured,
                                        uint32_t budgetRejected, uint32_t failed) {
    s_capAttempts.store(attempts, std::memory_order_relaxed);
    s_capCaptured.store(captured, std::memory_order_relaxed);
    s_capBudget.store(budgetRejected, std::memory_order_relaxed);
    s_capFailed.store(failed, std::memory_order_relaxed);
  }

  void SceneManager::reportStaleCaptureShape(uint32_t ageSum, uint32_t ageMax, uint32_t clamped) {
    s_staleAgeSum.store(ageSum, std::memory_order_relaxed);
    s_staleAgeMax.store(ageMax, std::memory_order_relaxed);
    s_staleClamped.store(clamped, std::memory_order_relaxed);
  }

  void SceneManager::onFrameEnd(Rc<DxvkContext> ctx, bool raytracedThisFrame) {
    ScopedCpuProfileZone();

    // DX11_V526. Ground blood decals are gathered by SubmitDraw during the game's
    // own deferred passes and consumed by every ray tracing pass afterwards, so
    // the list belongs to the frame - clearing it when the buffer is read left
    // eleven of the twelve passes reading an empty list.
    m_kenshiTerrainBloodDecals.clear();

    // DX11_V234_VRAM_LEAK_DIAG: periodic per-category resource census to locate the unbounded
    // VRAM growth reported across all GPUs. Logs the counts that, if climbing, pinpoint the leaking
    // subsystem (instances vs cached geometry) plus actual Vulkan device-local memory used. Cheap
    // (every 200 frames). If a number climbs monotonically over a session, that's the leak.
    if (kenshi_telemetry::enabled() && (terrain_profile::diagnosticsEnabled()
     || s_vramCensusFramesRequested.load(std::memory_order_relaxed) > 0u
     || (kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u))) {
      // V720: no automatic census/memory polling in ordinary play. Explicit
      // on-demand trace requests remain functional; the fault recorder is separate.
      // DX11_V267_LOG_CLEANUP: the census must not fill the log forever.
      // Dense while a session warms up (every 200 frames for the first 2000),
      // then a once-per-~5-minutes heartbeat (every 18000 frames at 60fps)
      // which still catches slow monotonic VRAM leaks.
      // DX11_V285_CENSUS_ON_GROWTH: the throttle went quiet exactly when the
      // Skyrim leak began (world load at frame ~2000+), so the census now also
      // fires whenever device-local usage grows >=256 MiB past the last line
      // (at most once per 100 frames) - a leak can no longer hide between
      // heartbeats. Also reports the per-frame unique-buffer table population
      // (kBufferCacheLimit overflow silently drops draws) so buffer-object
      // churn is visible directly.
      const uint32_t fid = m_device->getCurrentFrameId();
      // DX11_V332_IDENTITY_CHURN: read-and-clear every frame, not only on
      // census frames, so each reported figure covers exactly one frame rather
      // than everything since the last census.
      // DX11_V350_DRAW_TRACE: consume one frame of the trace window here, so
      // the scene-side rows cover the same frames as the census beside them.
      // DX11_V355_INSTANCE_DUMP: everything the raytracer knows about every
      // instance, once per frame, while the trace window is open.
      //
      // The per-draw trace established that the scene DESCRIPTION is constant
      // for the flickering geometry - identical submission, constant transform,
      // 100% cache reuse, flat instance and TLAS counts - while the output
      // still alternates. So the difference has to be in per-instance state the
      // tracer consumes, and aggregate counters cannot show it because both
      // classes sit inside the same totals.
      //
      // Intended for a view containing only a handful of objects: a stable mesh
      // and an unstable one side by side, dumped every frame, then diffed field
      // by field. With few objects the volume is small enough to log everything
      // rather than sample, which is what every previous attempt got wrong.
      if ((kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u)) {
        const uint32_t fidNow = m_device->getCurrentFrameId();

        // DX11_V357_SCREEN_MAP: project each instance's world position into
        // normalized screen space and log it, so a row in this dump can be tied
        // to an object the user can actually see. Every analysis so far has had
        // to guess which rows were "the flickering building" versus "the stable
        // prop" from family and world coordinates, which is unreliable and has
        // already produced one wrong reading. With sx/sy, a screenshot and this
        // log line up directly: "the bottle is centre-left" identifies a row.
        // sx/sy are 0..1 across the viewport, origin top-left; sz is the view
        // depth, and behind=1 marks an instance behind the camera.
        const RtCamera& mapCamera = getCamera();
        const Matrix4d worldToProj =
          mapCamera.getViewToProjection() * mapCamera.getWorldToView();

        // DX11_V379_BLAS_INPUT_DUMP: the actual vertex bytes the acceleration
        // structure was built from, decoded at the geometry's own stride and
        // position offset.
        //
        // Safe to read here: this runs at census time over geometry built on an
        // EARLIER frame, so the copy/interleave that filled it has completed. The
        // buffer is host-visible only because DEBUG_GEOMETRY_MEMORY is 1 in this
        // build; with it at 0 the mapPtr is null and this reports "unmapped" rather
        // than misreporting.
        //
        // What to look for: positions matching the object-space extents the mesh
        // actually has (Kenshi's wall segment is about +-50 x +-92 units, per
        // RenderDoc's mesh view), versus zeros, non-finite values, or every vertex
        // identical - any of which produce degenerate triangles that no ray can hit
        // while every count and pointer upstream still reads correct.
        uint32_t blasInputDumpBudget = 6u;

        for (const RtInstance* inst : m_instanceManager.getInstanceTable()) {
          if (inst == nullptr)
            continue;
          const BlasEntry* blas = inst->getBlas();
          const RaytraceGeometry* geo = blas != nullptr ? &blas->modifiedGeometryData : nullptr;
          const VkAccelerationStructureInstanceKHR& vk = inst->getVkInstance();

          if (blasInputDumpBudget > 0u && geo != nullptr && geo->vertexCount > 0
           && geo->positionBuffer.defined()) {
            --blasInputDumpBudget;
            const uint32_t stride = geo->positionBuffer.stride();
            const uint32_t attrOffset = geo->positionBuffer.offsetFromSlice();
            const DxvkBuffer* posBuf = geo->positionBuffer.buffer().ptr();
            const uint8_t* base = nullptr;
            size_t readable = 0;
            if (posBuf != nullptr && stride >= 12u) {
              const DxvkBufferSliceHandle slice = geo->positionBuffer.buffer()->getSliceHandle();
              if (slice.mapPtr != nullptr) {
                base = reinterpret_cast<const uint8_t*>(slice.mapPtr)
                     + geo->positionBuffer.offset() + attrOffset;
                readable = slice.length > (geo->positionBuffer.offset() + attrOffset)
                  ? slice.length - (geo->positionBuffer.offset() + attrOffset) : 0u;
              }
            }

            std::string samples;
            const uint32_t sampleIdx[6] = { 0u, 1u, 2u,
              geo->vertexCount > 3u ? geo->vertexCount - 3u : 0u,
              geo->vertexCount > 2u ? geo->vertexCount - 2u : 0u,
              geo->vertexCount - 1u };
            for (uint32_t s = 0; s < 6u; ++s) {
              const size_t byteOff = size_t(sampleIdx[s]) * stride;
              samples += " v" + std::to_string(sampleIdx[s]) + "=";
              if (base == nullptr) { samples += "unmapped"; continue; }
              if (byteOff + 12u > readable) { samples += "OOB"; continue; }
              float p[3];
              std::memcpy(p, base + byteOff, sizeof(p));
              samples += "[" + std::to_string(p[0]) + "," + std::to_string(p[1])
                       + "," + std::to_string(p[2]) + "]";
            }

            Logger::info(str::format(
              "[SceneManager][blas-input] verts=", geo->vertexCount,
              " idx=", geo->indexCount,
              " stride=", stride,
              " attrOffset=", attrOffset,
              " sliceOffset=", geo->positionBuffer.offset(),
              " readable=", readable,
              " needed=", size_t(geo->vertexCount) * stride,
              " fmt=", uint32_t(geo->positionBuffer.vertexFormat()),
              " xformT=[", vk.transform.matrix[0][3], ",", vk.transform.matrix[1][3],
              ",", vk.transform.matrix[2][3], "]",
              samples));
          }

          const Vector4d worldPos(double(vk.transform.matrix[0][3]),
                                  double(vk.transform.matrix[1][3]),
                                  double(vk.transform.matrix[2][3]), 1.0);
          const Vector4d clipPos = worldToProj * worldPos;
          const bool behindCamera = clipPos.w <= 0.0;
          const double invW = behindCamera ? 1.0 : 1.0 / clipPos.w;
          const double screenX = 0.5 + 0.5 * clipPos.x * invW;
          const double screenY = 0.5 - 0.5 * clipPos.y * invW;

          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[SceneManager][inst-dump] f=", fidNow,
            " id=", inst->getId(),
            " sx=", screenX, " sy=", screenY,
            " sz=", clipPos.w, " behind=", behindCamera ? 1 : 0,
            " surf=", inst->getSurfaceIndex(),
            " mask=", vk.mask,
            " customIdx=0x", std::hex, vk.instanceCustomIndex, std::dec,
            " blasRef=", vk.accelerationStructureReference != 0ull ? 1 : 0,
            " sbtOffset=", vk.instanceShaderBindingTableRecordOffset,
            " flags=0x", std::hex, uint32_t(vk.flags), std::dec,
            " hidden=", inst->isHidden() ? 1 : 0,
            " gc=", inst->isMarkedForGC() ? 1 : 0,
            " billboards=", inst->getBillboardCount(),
            " frameLastUpd=", inst->getFrameLastUpdated(),
            " age=", inst->getFrameAge(),
            " cat=0x", std::hex, inst->getCategoryFlags().raw(), std::dec,
            " matHash=0x", std::hex, inst->getMaterialDataHash(), std::dec,
            // DX11_V356_BUILD_GEOMETRY: the one link never measured. A bucket
            // builds its BLAS by concatenating each instance's
            // blasEntry->buildGeometries; an instance whose list is EMPTY
            // contributes no triangles at all while remaining present, masked,
            // transformed and correct in every field measured so far - which is
            // exactly the state the flickering buildings are in. The list is
            // cached across frames (fillGeometryInfoFromBlasEntry is skipped
            // when the geometry "hasn't changed"), so a stale or dropped cache
            // would be invisible anywhere else. bldGeo=0 on a frame where the
            // object is absent would close this out.
            " bldGeo=", blas != nullptr ? blas->buildGeometries.size() : size_t(0),
            " bldRanges=", blas != nullptr ? blas->buildRanges.size() : size_t(0),
            " blasFrameUpd=", blas != nullptr ? blas->frameLastUpdated : 0u,
            " blasFrameTouch=", blas != nullptr ? blas->frameLastTouched : 0u,
            " geoVerts=", geo != nullptr ? geo->vertexCount : 0u,
            " geoIdx=", geo != nullptr ? geo->indexCount : 0u,
            " posBufIdx=", geo != nullptr ? geo->positionBufferIndex : 0u,
            " idxBufIdx=", geo != nullptr ? geo->indexBufferIndex : 0u,
            " prevPosBufIdx=", geo != nullptr ? geo->previousPositionBufferIndex : 0u,
            " xformT=[", vk.transform.matrix[0][3], ",",
                         vk.transform.matrix[1][3], ",",
                         vk.transform.matrix[2][3], "]",
            // DX11_V366_XFORM_SHAPE: translation alone cannot tell a real
            // placement from an artifact - Kenshi reuses the same building
            // model all over the map, so identical geometry at two positions is
            // normal scene content, not evidence of duplication. The reported
            // giveaway is the ANGLE: duplicates sit at improbable orientations.
            //
            // A legitimate OGRE placement is a clean rigid transform:
            // orthonormal basis, unit axis lengths, determinant +1. Log the
            // basis lengths and the determinant so a sheared, scaled, mirrored
            // or otherwise degenerate transform identifies itself, and the full
            // rotation rows so the orientation can be inspected directly.
            " axisLen=[",
              std::sqrt(vk.transform.matrix[0][0]*vk.transform.matrix[0][0]
                      + vk.transform.matrix[1][0]*vk.transform.matrix[1][0]
                      + vk.transform.matrix[2][0]*vk.transform.matrix[2][0]), ",",
              std::sqrt(vk.transform.matrix[0][1]*vk.transform.matrix[0][1]
                      + vk.transform.matrix[1][1]*vk.transform.matrix[1][1]
                      + vk.transform.matrix[2][1]*vk.transform.matrix[2][1]), ",",
              std::sqrt(vk.transform.matrix[0][2]*vk.transform.matrix[0][2]
                      + vk.transform.matrix[1][2]*vk.transform.matrix[1][2]
                      + vk.transform.matrix[2][2]*vk.transform.matrix[2][2]), "]",
            " det=",
              vk.transform.matrix[0][0] * (vk.transform.matrix[1][1]*vk.transform.matrix[2][2]
                                         - vk.transform.matrix[1][2]*vk.transform.matrix[2][1])
            - vk.transform.matrix[0][1] * (vk.transform.matrix[1][0]*vk.transform.matrix[2][2]
                                         - vk.transform.matrix[1][2]*vk.transform.matrix[2][0])
            + vk.transform.matrix[0][2] * (vk.transform.matrix[1][0]*vk.transform.matrix[2][1]
                                         - vk.transform.matrix[1][1]*vk.transform.matrix[2][0]),
            // DX11_V368_GEOMETRY_EXTENT: the object-space bounding box of the
            // vertices actually handed to the BLAS.
            //
            // linearZ during an explosion shows ONE continuous surface covering
            // the whole frame at close range - a single triangle stretched
            // across and through the camera - while the composite is pure black
            // because it carries no usable material. Instance transforms are
            // measured rigid (axisLen=[1,1,1] det=1), so the wrongness is in the
            // vertex POSITIONS, not the placement. A mesh whose extent is
            // kilometres wide, or non-finite, is the exploding one; a real
            // Kenshi building is a few metres across. This names it directly
            // instead of inferring from screen position.
            " aabbMin=[", blas != nullptr ? blas->input.getGeometryData().boundingBox.minPos.x : 0.f, ",",
                          blas != nullptr ? blas->input.getGeometryData().boundingBox.minPos.y : 0.f, ",",
                          blas != nullptr ? blas->input.getGeometryData().boundingBox.minPos.z : 0.f, "]",
            " aabbMax=[", blas != nullptr ? blas->input.getGeometryData().boundingBox.maxPos.x : 0.f, ",",
                          blas != nullptr ? blas->input.getGeometryData().boundingBox.maxPos.y : 0.f, ",",
                          blas != nullptr ? blas->input.getGeometryData().boundingBox.maxPos.z : 0.f, "]",
            " rot0=[", vk.transform.matrix[0][0], ",", vk.transform.matrix[0][1], ",", vk.transform.matrix[0][2], "]",
            " rot1=[", vk.transform.matrix[1][0], ",", vk.transform.matrix[1][1], ",", vk.transform.matrix[1][2], "]",
            " rot2=[", vk.transform.matrix[2][0], ",", vk.transform.matrix[2][1], ",", vk.transform.matrix[2][2], "]"));
        }
      }

      if ((kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u))
        s_drawTraceFramesRemaining.fetch_sub(1u, std::memory_order_relaxed);
      const uint32_t churnExisted = s_frameCacheExisted.exchange(0u, std::memory_order_relaxed);
      const uint32_t churnFresh = s_frameCacheFresh.exchange(0u, std::memory_order_relaxed);
      const uint32_t churnBuildBvh = s_frameBuildBvh.exchange(0u, std::memory_order_relaxed);
      const uint32_t churnUpdateBvh = s_frameUpdateBvh.exchange(0u, std::memory_order_relaxed);
      const uint32_t churnUpdateInst = s_frameUpdateInstance.exchange(0u, std::memory_order_relaxed);
      uint32_t tlasHidden = 0, tlasGc = 0, tlasZeroMask = 0, tlasInTlas = 0;
      AccelManager::fetchAndResetTlasStats(tlasHidden, tlasGc, tlasZeroMask, tlasInTlas);
      uint32_t tlasNullBlas = 0, tlasZeroXform = 0;
      AccelManager::fetchTlasContentStats(tlasNullBlas, tlasZeroXform);
      // DX11_V381_CENSUS_FETCH_ORDER: these two are read-AND-RESET, so they must be
      // fetched only when the census actually prints. They were being fetched here,
      // every frame, outside the `if (censusDue)` guard below - which zeroed them on
      // every frame and left the printed value as just the sliver accumulated before
      // this point in the current frame. That is why `mergedInst`/`dynInst` read 0 on
      // most windows and why every `cacheSkip`/`cacheMembers` read 0 while the
      // per-bucket detail log proved restores were happening. Declared here, fetched
      // inside the guard.
      AccelManager::BlasRouteStats blasRoute;
      AccelManager::CacheRestoreStats cacheRestore;
      static VkDeviceSize s_lastCensusUsedBytes = 0;
      static uint32_t s_lastGrowthCensusFrame = 0;
      // DX11_V330_CENSUS_ON_DEMAND: the old steady-state heartbeat was every
      // 18000 frames - 5 minutes at 60fps, 10 at 30 - so in practice the census
      // only ever sampled the loading phase and every question about what the
      // settled RT scene contained was unanswerable. Two changes: the hotkey
      // arms a run of frames directly (below), and the background heartbeat
      // drops to 1800 frames (~30s at 60fps) so unattended runs still get a
      // usable time series.
      const bool censusRequested = s_vramCensusFramesRequested.load(std::memory_order_relaxed) > 0u;
      if (censusRequested)
        s_vramCensusFramesRequested.fetch_sub(1u, std::memory_order_relaxed);
      bool censusDue = censusRequested
        || ((fid != 0)
         && ((fid <= 2000u && (fid % 200) == 0) || (fid % 1800) == 0));
      VkDeviceSize usedBytes = 0, budgetBytes = 0;
      VkDeviceSize appBufferBytes = 0, appTextureBytes = 0;
      VkDeviceSize rtxBufferBytes = 0, rtxAsBytes = 0, rtxOmmBytes = 0;
      VkDeviceSize rtxMaterialBytes = 0, rtxTargetBytes = 0, rtxReplacementBytes = 0;
      {
        const DxvkAdapterMemoryInfo mem = m_device->adapter()->getMemoryHeapInfo();
        for (uint32_t i = 0; i < mem.heapCount; ++i) {
          if (mem.heaps[i].heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            usedBytes   += mem.heaps[i].memoryAllocated;
            budgetBytes += mem.heaps[i].memoryBudget;
            const DxvkMemoryStats& stats = m_device->getMemoryStats(i);
            appBufferBytes += stats.usedByCategory(DxvkMemoryStats::Category::AppBuffer);
            appTextureBytes += stats.usedByCategory(DxvkMemoryStats::Category::AppTexture);
            rtxBufferBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXBuffer);
            rtxAsBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXAccelerationStructure);
            rtxOmmBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXOpacityMicromap);
            rtxMaterialBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXMaterialTexture);
            rtxTargetBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXRenderTarget);
            rtxReplacementBytes += stats.usedByCategory(DxvkMemoryStats::Category::RTXReplacementGeometry);
          }
        }
      }
      if (!censusDue
       && usedBytes > s_lastCensusUsedBytes + (256ull << 20)
       && fid > s_lastGrowthCensusFrame + 100u) {
        censusDue = true;
        s_lastGrowthCensusFrame = fid;
        // DX11_V295_LEAK_NAMER: growth-triggered census means something is
        // climbing (the Skyrim leak shows as rtxBufMiB 110 -> 2000+). Name
        // the biggest RTXBuffer allocations so the leaking pool identifies
        // itself; requires rtx.profiler.memory.enable = True from launch.
        GpuMemoryTracker::logTopAllocationsByCategory(
          DxvkMemoryStats::Category::RTXBuffer, 10u);
      }
      if (censusDue) {
        s_lastCensusUsedBytes = usedBytes;
        // DX11_V381_CENSUS_FETCH_ORDER: read-and-reset only when printing, so the
        // reported value is "accumulated since the previous census".
        AccelManager::fetchAndResetBlasRouteStats(blasRoute);
        AccelManager::fetchAndResetCacheRestoreStats(cacheRestore);
        KENSHI_DIAGNOSTIC_INFO(str::format("[Remix-DX11][vram] frame=", fid,
          " src=", censusRequested ? "hotkey" : "auto",
          // DX11_V332_IDENTITY_CHURN. existed/fresh = did the draw's geometry
          // identity match the cache. buildBVH/updateBVH/updateInstance = what
          // that cost. Sustained fresh>0 in a static scene IS the flicker.
          " existed=", churnExisted,
          " fresh=", churnFresh,
          " buildBVH=", churnBuildBvh,
          " updateBVH=", churnUpdateBvh,
          " updateInst=", churnUpdateInst,
          // DX11_V334_TLAS_STATS: the last stage before a ray can hit anything.
          " inTlas=", tlasInTlas,
          " tlasHidden=", tlasHidden,
          " tlasGC=", tlasGc,
          " tlasZeroMask=", tlasZeroMask,
          // DX11_V344_TLAS_CONTENTS: present-but-inert entries.
          " tlasNullBlas=", tlasNullBlas,
          " tlasZeroXform=", tlasZeroXform,
          // DX11_V375_BLAS_ROUTE: instances split by BLAS route. `instances` and
          // `inTlas` are NOT comparable - a merged bucket is one TLAS entry for
          // many instances - so account for the split rather than inferring loss
          // from the difference (which was misread as 555 lost instances).
          // mergedInst + dynInst should equal the non-rejected instance count.
          " mergedInst=", blasRoute.mergedBucketInstances,
          " dynInst=", blasRoute.dynamicBlasInstances,
          " buckets=", blasRoute.mergedBuckets,
          " bucketsBuilt=", blasRoute.mergedBucketsBuilt,
          " bucketsSkipped=", blasRoute.mergedBucketsSkipped,
          " bucketsNoBlas=", blasRoute.mergedBucketNoBlas,
          " dynBlas=", blasRoute.dynamicBlasCount,
          // DX11_V380_CACHE_ACCOUNTING: the previously dark restore path.
          // cacheSkip should equal cacheMembers - a shortfall means instances were
          // skipped by the bucket cache that no restored bucket represents, i.e.
          // silently absent from the TLAS.
          " cacheSkip=", cacheRestore.skippedInstances,
          " cacheMembers=", cacheRestore.memberInstances,
          " cacheBuckets=", cacheRestore.bucketsRestored,
          " cacheEntries=", cacheRestore.tlasEntries,
          " cacheNoBlas=", cacheRestore.bucketsNoBlas,
          " noCapture=", s_submitWithoutCapture.load(std::memory_order_relaxed),
          // DX11_V343_CAPTURE_GATE: per-frame, so a frame where the whole
          // capture set collectively failed is visible as such.
          " injectSkip=", s_injectSkipReason.load(std::memory_order_relaxed),
          " capAtt=", s_capAttempts.load(std::memory_order_relaxed),
          " capOk=", s_capCaptured.load(std::memory_order_relaxed),
          " capBudget=", s_capBudget.load(std::memory_order_relaxed),
          " capFail=", s_capFailed.load(std::memory_order_relaxed),
          " staleReuse=", s_staleCaptureReuse.load(std::memory_order_relaxed),
          " staleAgeAvg=", s_staleCaptureReuse.load(std::memory_order_relaxed) > 0u
            ? s_staleAgeSum.load(std::memory_order_relaxed)
              / s_staleCaptureReuse.load(std::memory_order_relaxed) : 0u,
          " staleAgeMax=", s_staleAgeMax.load(std::memory_order_relaxed),
          " staleClamp=", s_staleClamped.load(std::memory_order_relaxed),
          " instances=", m_instanceManager.getActiveCount(),
          " geoEntries=", m_drawCallCache.getEntries().size(),
          " frameBuffers=", m_bufferCache.getActiveCount(), "/", m_bufferCache.getTotalCount(),
          // DX11_V286: light population - the DX11 layer captures no game
          // lights, so this is the fallback light + any Remix/USD lights.
          // lights=0 here while the scene renders black = the fallback light
          // is not reaching the scene (the exact bug being chased).
          " lights=", m_lightManager.getActiveCount(),
          " vramUsedMiB=", usedBytes / (1024ull * 1024ull),
          " vramBudgetMiB=", budgetBytes / (1024ull * 1024ull),
          " appBufMiB=", appBufferBytes >> 20,
          " appTexMiB=", appTextureBytes >> 20,
          " rtxBufMiB=", rtxBufferBytes >> 20,
          " asMiB=", rtxAsBytes >> 20,
          " ommMiB=", rtxOmmBytes >> 20,
          " matTexMiB=", rtxMaterialBytes >> 20,
          " rtTargetMiB=", rtxTargetBytes >> 20,
          " replGeoMiB=", rtxReplacementBytes >> 20));
      }
    }

    manageTextureVram();

    // A camera-history discontinuity does not change texture-space opacity.
    // Asset reloads still invalidate OMMs, including a reload on a cut frame.
    const bool assetsChanged = m_pReplacer->checkForChanges(ctx);
    const bool sceneCleared = m_enqueueDelayedClear || assetsChanged;
    if (sceneCleared) {
      clear(ctx, true, m_enqueueDelayedClear && !assetsChanged);
      m_enqueueDelayedClear = false;
    }

    m_cameraManager.onFrameEnd();
    m_instanceManager.onFrameEnd();
    // V663: clear() discarded this scene's BLAS owners; it cannot become history.
    m_previousFrameSceneAvailable = !sceneCleared && raytracedThisFrame && RtxOptions::enablePreviousTLAS();

    m_bufferCache.clear();
    if (raytracedThisFrame) {
      std::lock_guard lock { m_drawCallMeta.mutex };
      const uint8_t curTick = m_drawCallMeta.ticker;
      const uint8_t nextTick = (m_drawCallMeta.ticker + 1) % m_drawCallMeta.MaxTicks;

      m_drawCallMeta.ready[curTick] = true;

      m_drawCallMeta.infos[nextTick].clear();
      m_drawCallMeta.ready[nextTick] = false;
      m_drawCallMeta.ticker = nextTick;
    }

    m_terrainBaker->onFrameEnd(ctx);

    if (m_opacityMicromapManager) {
      m_opacityMicromapManager->onFrameEnd();
    }
    
    m_activePOMCount = 0;
    m_startInMediumMaterialIndex = SURFACE_INDEX_INVALID;
    m_fogStartInMediumMaterialIndex_inCache = UINT32_MAX;
    m_startInMediumMaterialIndex_inCache = UINT32_MAX;

    if (m_uniqueObjectSearchDistance != RtxOptions::uniqueObjectDistance()) {
      m_uniqueObjectSearchDistance = RtxOptions::uniqueObjectDistance();
      m_drawCallTracker.rebuildSpatialMaps(m_uniqueObjectSearchDistance * 2.f);
    }

    // Not currently safe to cache these across frames (due to texture indices and rtx options potentially changing)
    m_preCreationSurfaceMaterialMap.clear();

    m_thinOpaqueMaterialExist = false;
    m_sssMaterialExist = false;

    // execute graph updates after all garbage collection is complete (to avoid updating graphs that will just be deleted)
    // RtxOptions will still be pending, so any changes to them will apply next frame.
    if (raytracedThisFrame){
      m_graphManager.update(ctx);
    }

    // Clear replacement material hashes before the next frame.  These are used by components, so must clear after graphManager updates.
    clearFrameReplacementMaterialHashes();
    
    // Clear mesh hashes before the next frame.  These are used by components, so must clear after graphManager updates.
    clearFrameMeshHashes();
    
    // Reset the fog state to get it re-discovered on the next frame
    ImGUI::SetFogStates(m_fogStates, m_fog.getHash());
    m_fog = FogState();
    m_fogStates.clear();
  }

  void SceneManager::onKenshiOriginSnapshot(const kenshi_origin::Snapshot& snapshot) {
    const uint32_t frame = m_device->getCurrentFrameId();
    if (!kenshi_origin::receive(m_device, frame, snapshot)) return;
    auto& origin = kenshi_origin::state;
    const Vector3 shift(origin.shift[0], origin.shift[1], origin.shift[2]);
    m_drawCallTracker.rebaseKenshiTracking(shift);
  }

  // Submitted surfaces retain the usual previous-native -> current-native
  // history. Move only native surfaces that were retained without a new draw.
  static uint32_t repairRetainedNativeOrigin(uint32_t frame,
      const std::vector<RtInstance*>& instances,
      const std::vector<std::unique_ptr<ReplacementInstance>>& owners) {
    auto& origin = kenshi_origin::state;
    if (origin.retainedFrame != frame && !origin.retainedMotion.empty()) {
      for (auto* instance : instances) {
        if (instance && instance->getFrameLastUpdated() != frame && origin.retainedMotion.count(instance->getId())) {
          instance->surface.prevObjectToWorld = instance->surface.objectToWorld;
          instance->surface.isStatic = instance->surface.previousPositionBufferIndex == BINDING_INDEX_INVALID;
        }
      }
      origin.retainedMotion.clear();
    }
    if (origin.eventFrame != frame || origin.retainedFrame == frame) return 0;
    origin.retainedFrame = frame;
    const Vector3 shift(origin.shift[0], origin.shift[1], origin.shift[2]);
    for (const auto& owner : owners) {
      if (!origin.nativeTracking.count(owner.get())) continue;
      for (const auto& prim : owner->prims) {
        auto* instance = prim.getInstance();
        if (!instance || instance->isCreatedByRenderer() || instance->getFrameLastUpdated() == frame
            || !origin.retainedMotion.insert(instance->getId()).second) continue;
        const Matrix4 previous = instance->surface.objectToWorld;
        Matrix4 current = previous;
        current[3].xyz() += shift;
        instance->surface.isStatic = !instance->teleport(current, previous);
      }
    }
    return uint32_t(origin.retainedMotion.size());
  }

  std::unordered_set<XXH64_hash_t> uniqueHashes;


  void SceneManager::submitDrawState(Rc<DxvkContext> ctx, const DrawCallState& input, const MaterialData* overrideMaterialData, bool geometryCacheOnly) {
    ScopedCpuProfileZone();
    terrain_audit::draw(m_device->getCurrentFrameId(), 1, input, 0, 0, input.getTransformData().objectToWorld);
    if (m_bufferCache.getTotalCount() >= kBufferCacheLimit && m_bufferCache.getActiveCount() >= kBufferCacheLimit) {
      ONCE(Logger::info("[RTX-Compatibility-Info] This application is pushing more unique buffers than is currently supported - some objects may not raytrace."));
      return;
    }

    if (input.getFogState().mode != DX11_FOG_NONE) {
      XXH64_hash_t fogHash = input.getFogState().getHash();
      if (m_fogStates.find(fogHash) == m_fogStates.end()) {
        // Only do anything if we haven't seen this fog before.
        m_fogStates[fogHash] = input.getFogState();

        MaterialData* pFogReplacement = m_pReplacer->getReplacementMaterial(fogHash);
        if (pFogReplacement) {
          // Track this replacement material hash for hash checking
          trackReplacementMaterialHash(fogHash);
          // Fog has been replaced by a translucent material to start the camera in,
          // meaning that it was being used to indicate 'underwater' or something similar.
          if (pFogReplacement->getType() != MaterialDataType::Translucent) {
            Logger::warn(str::format("Fog replacement materials must be translucent.  Ignoring material for ", std::hex, m_fog.getHash()));
          } else {
            uint32_t id = UINT32_MAX;
            createSurfaceMaterial(*pFogReplacement, input, &id);
            assert(id != UINT32_MAX);
            m_fogStartInMediumMaterialIndex_inCache = id;
          }
        } else if (m_fog.mode == DX11_FOG_NONE) {
          // render the first unreplaced fog.
          m_fog = input.getFogState();
        }
      }
    }


    const XXH64_hash_t activeReplacementHash = input.getHash(RtxOptions::geometryAssetHashRule());
    
    // Track this mesh hash for mesh hash checking
    trackMeshHash(activeReplacementHash);
    
    std::vector<AssetReplacement>* pReplacements = m_pReplacer->getReplacementsForMesh(activeReplacementHash);

    // TODO (REMIX-656): Remove this once we can transition content to new hash
    if ((RtxOptions::geometryHashGenerationRule() & rules::LegacyAssetHash0) == rules::LegacyAssetHash0) {
      if (!pReplacements) {
        const XXH64_hash_t legacyHash = input.getHashLegacy(rules::LegacyAssetHash0);
        trackMeshHash(legacyHash);
        pReplacements = m_pReplacer->getReplacementsForMesh(legacyHash);
        if (RtxOptions::logLegacyHashReplacementMatches() && pReplacements && uniqueHashes.find(legacyHash) == uniqueHashes.end()) {
          uniqueHashes.insert(legacyHash);
          Logger::info(str::format("[Legacy-Hash-Replacement] Found a mesh referenced from legacyHash0: ", std::hex, legacyHash, ", new hash: ", std::hex, activeReplacementHash));
        }
      }
    }

    if ((RtxOptions::geometryHashGenerationRule() & rules::LegacyAssetHash1) == rules::LegacyAssetHash1) {
      if (!pReplacements) {
        const XXH64_hash_t legacyHash = input.getHashLegacy(rules::LegacyAssetHash1);
        trackMeshHash(legacyHash);
        pReplacements = m_pReplacer->getReplacementsForMesh(legacyHash);
        if (RtxOptions::logLegacyHashReplacementMatches() && pReplacements && uniqueHashes.find(legacyHash) == uniqueHashes.end()) {
          uniqueHashes.insert(legacyHash);
          Logger::info(str::format("[Legacy-Hash-Replacement] Found a mesh referenced from legacyHash1: ", std::hex, legacyHash, ", new hash: ", std::hex, activeReplacementHash));
        }
      }
    }

    MaterialData renderMaterialData = determineMaterialData(overrideMaterialData, input);

    ReplacementInstance* replacementInstance = m_drawCallTracker.findOrCreateReplacementInstance(input, renderMaterialData, m_rayPortalManager);
    terrain_profile::owner(replacementInstance);

    // DX11_V426_KENSHI_ANIMAL_INSTANCE_TRACE: V425 proved that native
    // skinning and BLAS reuse are healthy for the 8,634-index animal family,
    // but only one member of a visible pack reaches RT. Their game transform
    // is identity while bone 0 carries their world position, so record the
    // ReplacementInstance pairing alongside the existing trace-window
    // instance dump. This is deliberately diagnostic-only: it distinguishes
    // RI reassociation from later RtInstance/TLAS loss before the world-anchor
    // is allowed to alter matching behaviour.
    const bool traceKenshiAnimal =
      (kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u) &&
      input.getSkinningState().numBones > 0u &&
      input.getGeometryData().indexCount == 8634u;
    if (traceKenshiAnimal) {
      const auto& skinning = input.getSkinningState();
      const Matrix4& bone0 = skinning.pBoneMatrices[skinning.minBoneIndex];
      const Matrix4& objectToWorld = input.getTransformData().objectToWorld;
      const Vector3 trackedCentroid =
        input.getGeometryData().boundingBox.getTransformedCentroid(objectToWorld);
      const uint32_t frameId = m_device->getCurrentFrameId();
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[Kenshi][animal-ri] f=", frameId,
        " draw=", input.drawCallID,
        " ri=", replacementInstance->id,
        " riCreated=", replacementInstance->frameCreated,
        " riLastSeen=", replacementInstance->frameLastSeen,
        " sameFrame=", replacementInstance->frameLastSeen == frameId ? 1 : 0,
        " riPrims=", replacementInstance->prims.size(),
        " boneHash=0x", std::hex, skinning.boneHash, std::dec,
        " bone0T=[", bone0[3][0], ",", bone0[3][1], ",", bone0[3][2], "]",
        " o2wT=[", objectToWorld[3][0], ",", objectToWorld[3][1], ",", objectToWorld[3][2], "]",
        " trackedCentroid=[", trackedCentroid.x, ",", trackedCentroid.y, ",", trackedCentroid.z, "]"));
    }

    // The D3D11 bridge safety baseline exercises metadata and the normal
    // geometry-cache path, but never enters independent replacement submission.
    if (geometryCacheOnly && pReplacements != nullptr) {
      return;
    }

    if (pReplacements != nullptr) {
      drawReplacements(ctx, &input, pReplacements, renderMaterialData, replacementInstance);
    } else {
      // If there was a replacement last frame, clean it up.
      if (replacementInstance->activeReplacements != nullptr) {
        replacementInstance->clear();
      }

      // ExistingInstance will be nullptr the first frame a replacementInstance is used.
      // The actual instance creation still happens in instanceManager.processSceneObject().
      RtInstance* existingInstance = (replacementInstance->prims.size() > 0)
          ? replacementInstance->prims[0].getInstance() : nullptr;

      const uint64_t flickerPreviousId = existingInstance ? existingInstance->getId() : 0ull;
      RtInstance* instance = processDrawCallState(ctx, input, renderMaterialData,
          existingInstance, nullptr, geometryCacheOnly);
      terrain_origin::observe(this, m_device->getCurrentFrameId(), input, instance);
      terrain_audit::draw(m_device->getCurrentFrameId(), 2, input,
        replacementInstance->id, instance ? instance->getId() : 0, input.getTransformData().objectToWorld);
      KenshiFlickerTrace::resolved(input, replacementInstance->id, flickerPreviousId, instance);
      if (traceKenshiAnimal) {
        const BlasEntry* animalBlas = instance != nullptr ? instance->getBlas() : nullptr;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[Kenshi][animal-instance] f=", m_device->getCurrentFrameId(),
          " draw=", input.drawCallID,
          " ri=", replacementInstance->id,
          " oldInst=", existingInstance != nullptr ? existingInstance->getId() : 0ull,
          " inst=", instance != nullptr ? instance->getId() : 0ull,
          " blas=0x", std::hex, reinterpret_cast<uintptr_t>(animalBlas), std::dec,
          " hidden=", instance != nullptr && instance->isHidden() ? 1 : 0,
          " mask=", instance != nullptr ? uint32_t(instance->getVkInstance().mask) : 0u,
          " geoVerts=", animalBlas != nullptr ? animalBlas->modifiedGeometryData.vertexCount : 0u,
          " geoIdx=", animalBlas != nullptr ? animalBlas->modifiedGeometryData.indexCount : 0u));
      }
      if (instance != nullptr) {
        if (replacementInstance->root.getUntyped() == nullptr) {
          replacementInstance->setup(PrimInstance(instance, PrimInstance::Type::Instance), 1, nullptr);
        }
        if (replacementInstance->prims[0].getUntyped() != instance) {
          instance->getPrimInstanceOwner().setReplacementInstance(replacementInstance, 0, instance,
              PrimInstance::Type::Instance);
        }
      }
    }

    replacementInstance->frameLastSeen = m_device->getCurrentFrameId();
    replacementInstance->categoryFlags = input.getCategoryFlags().raw();
    replacementInstance->isSkinned = input.getSkinningState().numBones > 0;

    // For standalone draw calls, store the object-space bounding box for anti-culling.
    // For replacement draw calls, the aggregate AABB is computed inside drawReplacements.
    if (pReplacements == nullptr) {
      const auto& geoBBox = input.getGeometryData().boundingBox;
      if (geoBBox.isValid()) {
        replacementInstance->geometryBoundingBox = geoBBox;
        replacementInstance->objectToWorld = input.getTransformData().objectToWorld;
      }
    }
  }

  MaterialData SceneManager::determineMaterialData(const MaterialData* overrideMaterialData, const DrawCallState& input) {
    terrain_profile::Scope terrainCpuMaterial(terrain_profile::Stage::SceneMaterial);
    // First see if we have an explicit override
    if (overrideMaterialData != nullptr) {
      return *overrideMaterialData;
    } 

    // test if any direct material replacements exist
    //
    // Tiered material-instance lookup - every tier is a pure function of the current draw
    // (no session history), so the same surface always resolves to the same replacement:
    //   1. materialHash          - exact child identity (PS + material texture set + constants)
    //   2. lightmap-permutation alternates - exact identities this draw would have produced under
    //                              lightmap policy permutations that reference fewer material
    //                              symbols (see rtx.d3d11.lightmapPermutationBridgeLookup)
    //   3. textureSetShaderHash  - all material-instance siblings sharing the shader and texture
    //                              set (constants ignored; stable even for shaders with
    //                              frame-varying constants)
    //   4. textureHash           - parent-level tag on the primary color texture
    // Exact identities come before family/parent fallbacks so the same replacement wins
    // regardless of which permutation is running. For games without material-instance identity
    // the tiers collapse into the legacy single texture-hash lookup.
    const LegacyMaterialData& inputMaterial = input.getMaterialData();
    const XXH64_hash_t materialHash = inputMaterial.getHash();
    const XXH64_hash_t textureHash = inputMaterial.getColorTexture().getImageHash();
    const XXH64_hash_t textureSetShaderHash = inputMaterial.getTextureSetAndShaderHash();

    const char* matchedTier = "material";
    MaterialData* pReplacementMaterial = m_pReplacer->getReplacementMaterial(materialHash);

    if (pReplacementMaterial == nullptr && input.lightmapPermutationAlternateHashes != nullptr) {
      for (const XXH64_hash_t alternateHash : *input.lightmapPermutationAlternateHashes) {
        if (alternateHash == kEmptyHash || alternateHash == materialHash ||
            alternateHash == textureSetShaderHash || alternateHash == textureHash) {
          continue;
        }
        pReplacementMaterial = m_pReplacer->getReplacementMaterial(alternateHash);
        if (pReplacementMaterial != nullptr) {
          matchedTier = "lightmap-permutation bridge";
          static fast_unordered_set s_loggedBridgedMaterials;
          if (s_loggedBridgedMaterials.insert(materialHash).second) {
            KENSHI_DIAGNOSTIC_INFO(str::format(
              "[RTX-Compatibility] Lightmap-permutation bridge: materialHash=0x", std::hex, materialHash,
              " matched replacement keyed 0x", alternateHash, std::dec,
              " (authored under a simpler lightmap permutation)."));
          }
          break;
        }
      }
    }

    if (pReplacementMaterial == nullptr &&
        textureSetShaderHash != kEmptyHash && textureSetShaderHash != materialHash) {
      pReplacementMaterial = m_pReplacer->getReplacementMaterial(textureSetShaderHash);
      matchedTier = "textureSet+shader";
    }

    if (pReplacementMaterial == nullptr &&
        textureHash != kEmptyHash && textureHash != materialHash && textureHash != textureSetShaderHash) {
      pReplacementMaterial = m_pReplacer->getReplacementMaterial(textureHash);
      matchedTier = "texture";
    }

    if (pReplacementMaterial != nullptr) {
      if (Logger::logLevel() <= LogLevel::Debug && materialHash != textureHash) {
        static fast_unordered_set s_loggedReplacementTierMaterials;
        if (s_loggedReplacementTierMaterials.insert(materialHash).second) {
          Logger::debug(str::format(
            "[RTX-Compatibility] Replacement matched at tier '", matchedTier,
            "' for materialHash=0x", std::hex, materialHash,
            " (textureSetShader=0x", textureSetShaderHash, ", texture=0x", textureHash, ")", std::dec));
        }
      }

      // Make a copy - dont modify the replacement data.
      MaterialData renderMaterialData = *pReplacementMaterial;
      // merge in the input material from game
      renderMaterialData.mergeLegacyMaterial(input.getMaterialData());
      return renderMaterialData;
    }

    // Detect meshes that would have unstable hashes due to the vertex hash using vertex data from a shared vertex buffer.
    // TODO: Once the vertex hash only uses vertices referenced by the index buffer, this should be removed.
    const bool highlightUnsafeAnchor = RtxOptions::useHighlightUnsafeAnchorMode() && input.getGeometryData().indexBuffer.defined() && input.getGeometryData().vertexCount > input.getGeometryData().indexCount;
    if (highlightUnsafeAnchor) {
      const static MaterialData sHighlightMaterialData(OpaqueMaterialData(TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(),
                                                                          0.f, 1.f, Vector3(0.2f, 0.2f, 0.2f), 1.0f, 0.1f, 0.1f, Vector3(0.46f, 0.26f, 0.31f), true, 1, 1, 0, false, false, 200.f, true, false, BlendType::kAlpha, false, AlphaTestType::kAlways, 0, 0.0f, 0.0f, Vector3(), 0.0f, Vector3(), 0.0f, false, Vector3(), 0.0f, 0.0f,
                                                                          lss::Mdl::Filter::Nearest, lss::Mdl::WrapMode::Repeat, lss::Mdl::WrapMode::Repeat));
      return sHighlightMaterialData;
    }

    // Check if a Ray Portal override is needed
    size_t rayPortalTextureIndex;
    if (RtxOptions::getRayPortalTextureIndex(input.getMaterialData().getHash(), rayPortalTextureIndex)) {
      assert(rayPortalTextureIndex < maxRayPortalCount);
      assert(rayPortalTextureIndex < std::numeric_limits<uint8_t>::max());

      MaterialData renderMaterialData = input.getMaterialData().as<RayPortalMaterialData>();
      renderMaterialData.getRayPortalMaterialData().setRayPortalIndex(rayPortalTextureIndex);
      return renderMaterialData;
    }

    // DX11_V624/V627_KENSHI_WATER_TRANSLUCENT. Water becomes a TRANSLUCENT
    // material when transparency is asked for, unless simulated depth explicitly
    // keeps it on the opaque lighting/denoising path.
    //
    // This is the only material type that can transmit deterministically. On the
    // opaque path a sub-threshold `opacity` does not blend on a primary ray - it
    // DELETES the surface for that pixel (resolve.slangh, resolveVertexFinalContinue),
    // a per-pixel coin flip between "full water" and "no water". That mechanism
    // exists for particle soup and cannot represent one large coherent surface,
    // which is what made V621/V622 unusable.
    //
    // Remix agrees: its own rtx.translucentMaterial.animatedWaterEnable is
    // documented for "draw calls in the AnimatedWater category AND a translucent
    // material". The opaque route was always the bridge's deviation.
    //
    // Keeping the opaque path for transparency == 0 leaves V620 reachable live,
    // so the proven behaviour is one slider away and needs no rebuild.
    if (KenshiOptions::kenshiWaterTransparency() > 0.0f
     && !KenshiOptions::kenshiWaterSimulatedDepth()
     && input.testCategoryFlags(InstanceCategories::AnimatedWater)) {
      TranslucentMaterialData waterMaterial;

      // Real water. Fresnel comes from this - F0 = ((1-n)/(1+n))^2 ~= 0.02 -
      // which is exactly the reflectivity the opaque path was destroying.
      waterMaterial.setRefractiveIndex(1.33f);

      // THE DEPTH FALLOFF, and the reason this route was chosen. The
      // transmittance colour is applied over this distance of real path length
      // through the medium, so Beer-Lambert reproduces Kenshi's own curve
      // instead of us hand-rolling it: the game saturates to opaque at
      // 1 / invOpacity, measured 0.002 -> 500 units.
      float kenshiWetnessUnused = 0.0f;
      float kenshiWaterHeightUnused = 0.0f;
      getKenshiWetness(kenshiWetnessUnused, kenshiWaterHeightUnused);
      KenshiWaterParams waterParams;
      getKenshiWater(waterParams);
      const float waterInvOpacity = waterParams.invOpacity > 0.0f
        ? waterParams.invOpacity : 0.002f;
      const float waterDepthScale = std::max(KenshiOptions::kenshiWaterTransparency(), 0.01f);
      waterMaterial.setTransmittanceMeasurementDistance(
        std::clamp((1.0f / waterInvOpacity) * waterDepthScale, 0.001f, 65504.0f));

      // Slightly tinted rather than neutral: the colour map supplies the surface
      // tint through the diffuse layer, and this is the through-water colour.
      waterMaterial.setTransmittanceColor(Vector3(0.72f, 0.86f, 0.88f));

      // Thick, not thin-walled - thin-walled would skip the volumetric
      // absorption that produces the depth falloff. Flip this one bool if
      // refraction proves visually distracting; the fallback is that cheap.
      waterMaterial.setEnableThinWalled(false);

      // The diffuse layer carries the water's own albedo and its scum.
      waterMaterial.setEnableDiffuseLayer(true);
      waterMaterial.setTransmittanceTexture(input.getMaterialData().getColorTexture());
      waterMaterial.setNormalTexture(input.getMaterialData().kenshiNormalTexture);

      MaterialData renderWaterMaterialData(waterMaterial);
      if (input.getMaterialData().getSampler().ptr()) {
        renderWaterMaterialData.getTranslucentMaterialData().setSamplerOverride(
          input.getMaterialData().getSampler());
      }
      return renderWaterMaterialData;
    }

    // Standard legacy material conversion
    MaterialData renderMaterialData = opaque_preparation::prepare(this, input.getMaterialData(), m_device->getCurrentFrameId());
    renderMaterialData.setUseSecondaryTextureForOpacity(
      input.getMaterialData().useSecondaryTextureForOpacity);
    renderMaterialData.setKenshiTerrainBlend(
      input.getMaterialData().kenshiTerrainBlend);
    renderMaterialData.setKenshiCharacterHead(
      input.getMaterialData().kenshiCharacterHead);
    renderMaterialData.setKenshiCharacterBodyMaskTexture(
      input.getMaterialData().kenshiCharacterBodyMaskTexture);
    renderMaterialData.setKenshiCharacterHeadMaskTexture(
      input.getMaterialData().kenshiCharacterHeadMaskTexture);
    renderMaterialData.setKenshiCharacterHairTexture(
      input.getMaterialData().kenshiCharacterHairTexture);
    renderMaterialData.setKenshiCharacterBeardTexture(
      input.getMaterialData().kenshiCharacterBeardTexture);
    // DX11_V491: the head's own normal map (character.hlsl s6).
    renderMaterialData.setKenshiCharacterHeadNormalTexture(
      input.getMaterialData().kenshiCharacterHeadNormalTexture);
    // DX11_V766: the muscle-blend normal map (character.hlsl s4).
    renderMaterialData.setKenshiMuscleBlendTexture(
      input.getMaterialData().kenshiCharacterBlendNormalTexture);
    // DX11_V490: which encoding the game's normal map uses, so the shader does
    // not decode a tangent-space RGB texture as octahedral.
    renderMaterialData.setKenshiNormalEncoding(
      input.getMaterialData().kenshiNormalEncoding);
    // DX11_V533: only where the pixel shader declares `glossMult`, which is what
    // proves this material's diffuse alpha is gloss and not coverage.
    renderMaterialData.setKenshiGlossInAlpha(
      input.getMaterialData().kenshiGlossMult > 0.0f);
    // DX11_V550: the draw's second texture set. Set only where the pixel shader
    // declares a second diffuse map by name, which is what proves the secondary
    // texture is an albedo rather than a mask.
    renderMaterialData.setKenshiDualTextureSet(
      input.getMaterialData().kenshiDualTextureSet);
    renderMaterialData.setKenshiDualNormalTexture(
      input.getMaterialData().kenshiDualNormalTexture);
    renderMaterialData.setKenshiDualMetalTexture(
      input.getMaterialData().kenshiDualMetalTexture);
    renderMaterialData.setKenshiColorMask(
      input.getMaterialData().kenshiColorMask);
    renderMaterialData.setKenshiColor1(input.getMaterialData().kenshiColor1);
    renderMaterialData.setKenshiColor2(input.getMaterialData().kenshiColor2);
    renderMaterialData.setKenshiColorMaskTexture(
      input.getMaterialData().colorTextures[1]);
    renderMaterialData.setKenshiCharacterVest(
      input.getMaterialData().kenshiCharacterVest);
    renderMaterialData.setKenshiVestDiffuseTexture(
      input.getMaterialData().kenshiVestDiffuseTexture);
    renderMaterialData.setKenshiVestNormalTexture(
      input.getMaterialData().kenshiVestNormalTexture);
    renderMaterialData.setKenshiVestMaskTexture(
      input.getMaterialData().kenshiVestMaskTexture);
    renderMaterialData.setKenshiVestColor(
      input.getMaterialData().kenshiVestColor);
    // DX11_V760: the under-construction scaffold contract. Set only where the
    // pixel shader declares `constructionState` AND `scaffoldTiling` AND binds
    // a `grid_map`, which together are what prove this draw is objects.hlsl
    // compiled with CONSTRUCTION rather than an ordinary building.
    renderMaterialData.setKenshiConstruction(
      input.getMaterialData().kenshiConstruction);
    renderMaterialData.setKenshiConstructionGridTexture(
      input.getMaterialData().kenshiConstructionGridTexture);
    renderMaterialData.setKenshiConstructionParams(
      input.getMaterialData().kenshiConstructionParams);
    renderMaterialData.setKenshiDustNoiseTexture(
      input.getMaterialData().kenshiDustNoiseTexture);
    renderMaterialData.setKenshiDustColour(
      input.getMaterialData().kenshiDustColour);
    // DX11_V766: two more pieces of Kenshi normal state ride in the spare bits
    // of this word. The material `flags` field is FULL - offsets 0-13 occupy
    // bits 2-15 and V554 took the last one - while this one uses 9 of 16 bits
    // and is written unconditionally on every opaque material, so it costs
    // nothing and grows no struct. Bit 9 is the green flip, bits 10-15 the
    // muscle blend in 1/63 steps.
    const uint32_t kenshiMuscleSteps = uint32_t(std::lround(
      std::clamp(input.getMaterialData().kenshiMuscleBlend, 0.0f, 1.0f) * 63.0f));
    const uint16_t kenshiCharacterHairChannels = uint16_t(
        input.getMaterialData().kenshiCharacterHairColorChannel
      | (input.getMaterialData().kenshiCharacterHairAlphaChannel << 3u)
      | (input.getMaterialData().kenshiCharacterBeardAlphaChannel << 6u)
      | (input.getMaterialData().kenshiNormalFlipGreen ? (1u << 9u) : 0u)
      | (kenshiMuscleSteps << 10u));
    renderMaterialData.setKenshiCharacterHairChannels(kenshiCharacterHairChannels);
    if (input.getMaterialData().kenshiCharacterHead
     && (input.getMaterialData().kenshiCharacterHairTexture.isValid()
      || input.getMaterialData().kenshiCharacterBeardTexture.isValid())) {
      // Character emission is disabled. Its otherwise-unused colour field is
      // three half-floats already present in the 64-byte GPU material record.
      renderMaterialData.getOpaqueMaterialData().setEmissiveColorConstant(
        input.getMaterialData().kenshiCharacterHairColor);
    }
    if (input.getMaterialData().kenshiTerrainBlend) {
      renderMaterialData.setKenshiTerrainSetIndex(
        setKenshiTerrainArgs(input.getMaterialData(), input.hasTextureCoordinates()));
    }
    return renderMaterialData;
  }

  void SceneManager::createEffectLight(Rc<DxvkContext> ctx, const DrawCallState& input, const RtInstance* instance) {
    const float effectLightIntensity = RtxOptions::effectLightIntensity();
    if (effectLightIntensity <= 0.f)
      return;

    const RasterGeometry& geometryData = input.getGeometryData();

    const GeometryBufferData bufferData(geometryData);
    
    if (!bufferData.indexData && geometryData.indexCount > 0 || !bufferData.positionData)
      return;

    // Find centroid of point cloud.
    Vector3 centroid = Vector3();
    uint32_t counter = 0;
    if (geometryData.indexCount > 0) {
      for (uint32_t i = 0; i < geometryData.indexCount; i++) {
        const uint16_t index = bufferData.getIndex(i);
        centroid += bufferData.getPosition(index);
        ++counter;
      }
    } else {
      for (uint32_t i = 0; i < geometryData.vertexCount; i++) {
        centroid += bufferData.getPosition(i);
        ++counter;
      }
    }
    centroid /= (float) counter;
    
    const Vector4 renderingPos = input.getTransformData().objectToView * Vector4(centroid.x, centroid.y, centroid.z, 1.0f);
    // Note: False used in getViewToWorld since the renderingPos of the object is defined with respect to the game's object to view
    // matrix, not our freecam's, and as such we want to convert it back to world space using the matching matrix.
    const Vector4 worldPos{ getCamera().getViewToWorld(false) * Vector4d{ renderingPos } };

    RtLightShaping shaping{};

    float lightRadius = std::max(RtxOptions::effectLightRadius(), 1e-3f);
    const Vector3 lightPosition { worldPos.x, worldPos.y, worldPos.z };
    Vector3 lightRadiance;
    if (RtxOptions::effectLightPlasmaBall()) {
      // Todo: Make these options more configurable via config options.
      const double timeMilliseconds = static_cast<double>(GlobalTime::get().absoluteTimeMs());
      const double animationPhase = sin(timeMilliseconds * 0.006) * 0.5 + 0.5;
      lightRadiance = lerp(Vector3(1.f, 0.921f, 0.738f), Vector3(1.f, 0.521f, 0.238f), animationPhase);
    } else {
      const Dx11MaterialColor originalColor = input.getMaterialData().getLegacyMaterial().Diffuse;
      lightRadiance = Vector3(originalColor.r, originalColor.g, originalColor.b) * RtxOptions::effectLightColor();
    }
    const float surfaceArea = 4.f * kPi * lightRadius * lightRadius;
    const float radianceFactor = 1e5f * effectLightIntensity / surfaceArea;
    lightRadiance *= radianceFactor;

    RtLight rtLight(RtSphereLight(lightPosition, lightRadiance, lightRadius, shaping));
    rtLight.isDynamic = true;

    m_lightManager.addLight(rtLight, input, RtLightAntiCullingType::MeshReplacement);
  }

  void SceneManager::drawReplacements(Rc<DxvkContext> ctx, const DrawCallState* input, const std::vector<AssetReplacement>* pReplacements, MaterialData& renderMaterialData, ReplacementInstance* replacementInstance) {
    ScopedCpuProfileZone();

    // Reinitialize the RI if the replacement data changed (e.g., transitioning from
    // standalone to replacement when replacements load, or hot-reload of replacement assets).
    if (replacementInstance->activeReplacements != pReplacements &&
        replacementInstance->root.getUntyped() != nullptr) {
      replacementInstance->clear();
    }

    // Detect replacements of meshes that would have unstable hashes due to the vertex hash using vertex data from a shared vertex buffer.
    // TODO: Once the vertex hash only uses vertices referenced by the index buffer, this should be removed.
    const bool highlightUnsafeReplacement = RtxOptions::useHighlightUnsafeReplacementMode() &&
        input->getGeometryData().indexBuffer.defined() && input->getGeometryData().vertexCount > input->getGeometryData().indexCount;
        
    // If the index contains an RtInstance, get a pointer to it.
    auto getExistingInstance = [replacementInstance](size_t idx) -> RtInstance* {
      if (replacementInstance->prims.size() <= idx) {
        return nullptr;
      }
      return replacementInstance->prims[idx].getInstance();
    };

    for (size_t i = 0; i < pReplacements->size(); i++) {
      auto& replacement = (*pReplacements)[i];
      RtInstance* instance = nullptr;

      if (replacement.includeOriginal) {
        DrawCallState newDrawCallState(*input);
        newDrawCallState.categories = replacement.categories.applyCategoryFlags(newDrawCallState.categories);
        const RtxParticleSystemDesc* pParticleSystemDesc = replacement.particleSystem.has_value() ? &replacement.particleSystem.value() : nullptr;
        instance = processDrawCallState(ctx, newDrawCallState, renderMaterialData, getExistingInstance(i), pParticleSystemDesc);
      } else if (replacement.type == AssetReplacement::eMesh) {
        DrawCallTransforms transforms = input->getTransformData();
        
        transforms.objectToWorld = transforms.objectToWorld * replacement.replacementToObject;
        transforms.objectToView = transforms.objectToView * replacement.replacementToObject;

        if (replacement.instancesToObject && !replacement.instancesToObject->empty()) {
          transforms.instancesToObject = replacement.instancesToObject;
        } else {
          transforms.instancesToObject = nullptr;
        }
        
        // Mesh replacements dont support these.
        transforms.textureTransform = Matrix4();
        transforms.texgenMode = TexGenMode::None;

        DrawCallState newDrawCallState(*input);
        newDrawCallState.geometryData = replacement.geometry->data; // Note: Geometry Data replaced
        newDrawCallState.transformData = transforms;
        newDrawCallState.categories = replacement.categories.applyCategoryFlags(newDrawCallState.categories);

        // Note: Material Data replaced if a replacement is specified in the Mesh Replacement
        if (replacement.materialData != nullptr) {
          renderMaterialData = *replacement.materialData;
        }
        if (highlightUnsafeReplacement) {
          const static MaterialData sHighlightMaterialData(OpaqueMaterialData(TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(), TextureRef(),
              0.f, 1.f, Vector3(0.2f, 0.2f, 0.2f), 1.f, 0.1f, 0.1f, Vector3(1.f, 0.f, 0.f), true, 1, 1, 0, false, false, 200.f, true, false, BlendType::kAlpha, false, AlphaTestType::kAlways, 0, 0.0f, 0.0f, Vector3(), 0.0f, Vector3(), 0.0f, false, Vector3(), 0.0f, 0.0f,
              lss::Mdl::Filter::Nearest, lss::Mdl::WrapMode::Repeat, lss::Mdl::WrapMode::Repeat));
          if ((GlobalTime::get().absoluteTimeMs()) / 200 % 2 == 0) {
            renderMaterialData = sHighlightMaterialData;
          }
        }

        const RtxParticleSystemDesc* pParticleSystemDesc = replacement.particleSystem.has_value() ? &replacement.particleSystem.value() : nullptr;
        instance = processDrawCallState(ctx, newDrawCallState, renderMaterialData, getExistingInstance(i), pParticleSystemDesc);
      }
      
      if (instance != nullptr) {
        if (replacementInstance->root.getUntyped() == nullptr) {
          // This is the first time this replacementInstance is used, and the first mesh drawn
          //  as part of this replacementInstance, so invoke setup and set the root.
          replacementInstance->setup(PrimInstance(instance, PrimInstance::Type::Instance), pReplacements->size(), pReplacements);
          instance->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, instance, PrimInstance::Type::Instance);
        } else if (replacementInstance->prims[i].getUntyped() != instance) {
          instance->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, instance, PrimInstance::Type::Instance);
        }
      }
    }

    for (size_t i = 0; i < pReplacements->size(); i++) {
      auto&& replacement = (*pReplacements)[i];
      if (replacement.type == AssetReplacement::eLight) {
        if (replacementInstance->root.getUntyped() == nullptr) {
          Logger::err(str::format(
              "Light prims anchored to a mesh replacement must also include actual meshes.  mesh hash: ",
              std::hex, input->getHash(RtxOptions::geometryAssetHashRule())
          ));
          break;
        }
        if (replacement.lightData.has_value()) {
          RtLight objectSpaceLight = replacement.lightData->toRtLight();

          // Transform to world space for the actual light creation
          RtLight localLight = objectSpaceLight;
          localLight.applyTransform(input->getTransformData().objectToWorld);

          RtLight* existingLight = (replacementInstance->prims.size() > i)
              ? replacementInstance->prims[i].getLight() : nullptr;
          if (existingLight != nullptr) {
            if (existingLight->getPrimInstanceOwner().getReplacementInstance() != replacementInstance) {
              ONCE(assert(false && "light in a replacementInstance believes it is owned by a different replacementInstance."));
            }
            m_lightManager.updateExternallyTrackedLight(existingLight, localLight);
          } else {
            RtLight* newLight = m_lightManager.createExternallyTrackedLight(localLight);
            newLight->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, newLight, PrimInstance::Type::Light);
          }
        }
      }
    }

    for (size_t i = 0; i < pReplacements->size(); i++) {
      auto&& replacement = (*pReplacements)[i];
      if (replacement.type == AssetReplacement::eGraph) {
        bool hasGraph = (replacementInstance->prims.size() > i) &&
                        (replacementInstance->prims[i].getGraph() != nullptr);
        if (!hasGraph) {
          if (!replacement.graphState.has_value()) {
            Logger::err(str::format(
                "Graph prims missing graph state in mesh replacement.  mesh hash: ",
                std::hex, input->getHash(RtxOptions::geometryAssetHashRule())
            ));
            break;
          }
          GraphInstance* graphInstance = m_graphManager.addInstance(ctx, replacement.graphState.value());
          if (graphInstance) {
            graphInstance->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, graphInstance, PrimInstance::Type::Graph);
          }
        }
      }
    }

    replacementInstance->recalculateBoundingBox(
        input->getTransformData().objectToWorld, *pReplacements,
        &input->getGeometryData().boundingBox);
  }

  // DX11_V655_BINDLESS_SURFACE_AUDIT
  //
  // m_bufferCache is cleared every frame (onFrameEnd) and BufferRefTable::track
  // appends in submission order, deduplicating only against the entry directly
  // before it. A bindless buffer index is therefore meaningful for exactly one
  // frame. updateBufferCache has a single caller, inside processGeometryInfo,
  // which runs only for a BLAS touched this frame; processInstanceBuffers then
  // copies blas.modifiedGeometryData.*BufferIndex AND offsetFromSlice() into the
  // surface. So an instance that survives a frame without its BLAS being touched
  // - anti-culled, or simply not re-submitted - keeps last frame's index beside a
  // current offset. When the table then shrinks (a scene collapse drops realScene
  // from ~1030 to ~330) that index lands past descriptorCount, and
  // createDescriptorSet never writes those slots, so the shader reads a stale or
  // dummy descriptor at a real suballocation offset.
  //
  // Detection is free of side effects. The repair is opt-in.
  void SceneManager::auditAndRepairInstanceBufferIndices() {
    terrain_profile::Scope sceneCpuAuditInstances(terrain_profile::Stage::SceneAuditInstances);
    const bool audit = (kenshi_telemetry::enabled() && KenshiOptions::kenshiLogBindlessAudit());
    const bool repair = KenshiOptions::kenshiRetrackKeptInstanceBuffers();

    if (!audit && !repair) {
      return;
    }

    const uint32_t frameId = m_device->getCurrentFrameId();
    const uint32_t tableSizeBefore = m_bufferCache.getActiveCount();

    uint32_t instances = 0u;
    uint32_t stale = 0u;
    uint32_t staleOutOfRange = 0u;
    uint32_t repaired = 0u;
    uint32_t blasRetracked = 0u;
    uint32_t details = 0u;

    // One retrack per BLAS, not per instance: many instances share a mesh, and
    // track() would otherwise append a duplicate entry for every one of them.
    std::unordered_map<const BlasEntry*, std::array<uint32_t, 6>> retracked;

    for (RtInstance* instance : m_instanceManager.getInstanceTable()) {
      if (instance == nullptr) {
        continue;
      }

      ++instances;

      BlasEntry* blas = instance->getBlas();
      if (blas == nullptr) {
        continue;
      }

      // Touched this frame means processGeometryInfo already refreshed the
      // indices through updateBufferCache, so it is not part of the DEFECT being
      // measured. V655b still REPAIRS it: repairing only the untouched set left
      // the question of whether some other path also submits a bad index, and
      // re-tracking everything makes "every submitted surface is valid this
      // frame" true by construction, which is what the test needs to be
      // decisive. Cost is one table entry per distinct BLAS per frame.
      const bool touchedThisFrame = (blas->frameLastTouched == frameId);

      if (!touchedThisFrame) {
        ++stale;
      }

      const uint32_t before[6] = {
        instance->surface.positionBufferIndex,
        instance->surface.previousPositionBufferIndex,
        instance->surface.normalBufferIndex,
        instance->surface.texcoordBufferIndex,
        instance->surface.color0BufferIndex,
        instance->surface.indexBufferIndex,
      };

      bool outOfRange = false;
      for (uint32_t i = 0u; i < 6u; ++i) {
        if (before[i] != kSurfaceInvalidBufferIndex && before[i] >= tableSizeBefore) {
          outOfRange = true;
        }
      }

      if (outOfRange && !touchedThisFrame) {
        ++staleOutOfRange;
      }

      if (audit && outOfRange && !touchedThisFrame && details < 8u) {
        ++details;
        Logger::warn(str::format(
          "[D3D11Rtx][bindless-audit] out-of-range surface"
          " frame=", frameId,
          " tableSize=", tableSizeBefore,
          " blasLastTouched=", blas->frameLastTouched,
          " age=", (blas->frameLastTouched == kInvalidFrameIndex
                      ? 0u : frameId - blas->frameLastTouched),
          " idx=[", before[0], ",", before[1], ",", before[2],
              ",", before[3], ",", before[4], ",", before[5], "]",
          " posOffset=", instance->surface.positionOffset,
          " indices=", blas->modifiedGeometryData.indexCount,
          " vertices=", blas->modifiedGeometryData.vertexCount,
          " instanceId=", instance->getId()));
      }

      if (!repair) {
        continue;
      }

      auto it = retracked.find(blas);
      if (it == retracked.end()) {
        updateBufferCache(blas->modifiedGeometryData);
        ++blasRetracked;
        it = retracked.emplace(blas, std::array<uint32_t, 6> {
          blas->modifiedGeometryData.positionBufferIndex,
          blas->modifiedGeometryData.previousPositionBufferIndex,
          blas->modifiedGeometryData.normalBufferIndex,
          blas->modifiedGeometryData.texcoordBufferIndex,
          blas->modifiedGeometryData.color0BufferIndex,
          blas->modifiedGeometryData.indexBufferIndex,
        }).first;
      }

      const std::array<uint32_t, 6>& fresh = it->second;
      instance->surface.positionBufferIndex = fresh[0];
      instance->surface.previousPositionBufferIndex = fresh[1];
      instance->surface.normalBufferIndex = fresh[2];
      instance->surface.texcoordBufferIndex = fresh[3];
      instance->surface.color0BufferIndex = fresh[4];
      instance->surface.indexBufferIndex = fresh[5];
      ++repaired;
    }

    // Post-repair verification. With the repair on, every submitted surface must
    // now index inside the table that is about to be published. Anything counted
    // here is a surface this function does not reach, which is exactly what the
    // previous run could not distinguish.
    uint32_t postOutOfRange = 0u;
    if (repair) {
      const uint32_t tableSizeAfter = m_bufferCache.getActiveCount();
      for (const RtInstance* instance : m_instanceManager.getInstanceTable()) {
        if (instance == nullptr) {
          continue;
        }
        const uint32_t after[6] = {
          instance->surface.positionBufferIndex,
          instance->surface.previousPositionBufferIndex,
          instance->surface.normalBufferIndex,
          instance->surface.texcoordBufferIndex,
          instance->surface.color0BufferIndex,
          instance->surface.indexBufferIndex,
        };
        for (uint32_t i = 0u; i < 6u; ++i) {
          if (after[i] != kSurfaceInvalidBufferIndex && after[i] >= tableSizeAfter) {
            ++postOutOfRange;
            break;
          }
        }
      }
    }

    if (!audit) {
      return;
    }

    // Silent while nothing is stale. Otherwise one line, throttled to once a
    // second, plus every frame that actually produced an out-of-range index -
    // that is the event worth catching and it is expected to be rare.
    static uint32_t s_lastSummaryFrame = 0u;
    const bool interesting = (staleOutOfRange > 0u || postOutOfRange > 0u);

    if ((stale > 0u || postOutOfRange > 0u) && (interesting || frameId - s_lastSummaryFrame >= 60u)) {
      s_lastSummaryFrame = frameId;
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[D3D11Rtx][bindless-audit] frame=", frameId,
        " instances=", instances,
        " staleIdx=", stale,
        " outOfRange=", staleOutOfRange,
        " tableBefore=", tableSizeBefore,
        " tableAfter=", m_bufferCache.getActiveCount(),
        " blasRetracked=", blasRetracked,
        " repaired=", repaired,
        " postOutOfRange=", postOutOfRange,
        " repairOn=", repair ? 1 : 0));
    }
  }

  // DX11_V656_BUFFER_TABLE_AUDIT
  //
  // V655b settled that every surface's bindless index is IN RANGE. It did not
  // establish that the table entry the index names still refers to a live
  // buffer. createDescriptorSet writes engineObject.getDescriptor().buffer
  // whenever engineObject.defined() is true, and defined() only tests that the
  // slice has a buffer pointer - a released DxvkBuffer still passes it, and the
  // descriptor then carries a dead or null VkBuffer beside a real suballocation
  // offset. That is the shape of every fake-OOM address this session: a read
  // below every tracked allocation, at 72-101 MB.
  //
  // Detection only. Runs immediately before the table is published.
  void SceneManager::auditBufferTableEntries() {
    terrain_profile::Scope sceneCpuAuditBuffers(terrain_profile::Stage::SceneAuditBuffers);
    if (!(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogBufferTableAudit())) {
      return;
    }

    const std::vector<RaytraceBuffer>& table = getBufferTable();

    uint32_t undefinedEntries = 0u;
    uint32_t nullHandles = 0u;
    uint32_t rangeOverruns = 0u;
    uint32_t zeroLength = 0u;
    VkDeviceSize maxDescriptorOffset = 0u;
    uint32_t details = 0u;

    for (uint32_t i = 0u; i < table.size(); ++i) {
      const RaytraceBuffer& entry = table[i];

      // Not defined is safe: createDescriptorSet leaves the dummy descriptor.
      if (!entry.defined()) {
        ++undefinedEntries;
        continue;
      }

      const DxvkBufferSliceHandle sliceHandle = entry.getSliceHandle();
      const VkDescriptorBufferInfo desc = entry.getDescriptor().buffer;

      if (desc.offset > maxDescriptorOffset) {
        maxDescriptorOffset = desc.offset;
      }

      const bool nullHandle = (sliceHandle.handle == VK_NULL_HANDLE)
                           || (desc.buffer == VK_NULL_HANDLE);
      const VkDeviceSize bufferSize = entry.buffer() != nullptr
        ? entry.buffer()->info().size : 0u;
      const bool overrun = (bufferSize != 0u)
        && (desc.range != VK_WHOLE_SIZE)
        && (desc.offset + desc.range > bufferSize);
      const bool empty = (desc.range == 0u);

      if (nullHandle) {
        ++nullHandles;
      }
      if (overrun) {
        ++rangeOverruns;
      }
      if (empty) {
        ++zeroLength;
      }

      if ((nullHandle || overrun) && details < 8u) {
        ++details;
        Logger::warn(str::format(
          "[D3D11Rtx][buffer-table] bad entry idx=", i,
          " of ", table.size(),
          nullHandle ? " NULL_HANDLE" : "",
          overrun ? " RANGE_OVERRUN" : "",
          " descOffset=", desc.offset,
          " descRange=", desc.range,
          " bufferSize=", bufferSize,
          " sliceOffset=", sliceHandle.offset,
          " sliceLength=", sliceHandle.length,
          " offsetFromSlice=", entry.offsetFromSlice(),
          " stride=", entry.stride()));
      }
    }

    // The band that matters is 72-101 MB: if the largest live descriptor offset
    // sits there, the faulting resource is in this table.
    static uint32_t s_lastFrame = 0u;
    const uint32_t frameId = m_device->getCurrentFrameId();
    const bool interesting = (nullHandles > 0u) || (rangeOverruns > 0u);

    if (interesting || frameId - s_lastFrame >= 120u) {
      s_lastFrame = frameId;
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[D3D11Rtx][buffer-table] frame=", frameId,
        " entries=", table.size(),
        " undefined=", undefinedEntries,
        " nullHandle=", nullHandles,
        " rangeOverrun=", rangeOverruns,
        " zeroLength=", zeroLength,
        " maxDescOffset=", maxDescriptorOffset,
        " maxDescOffsetMiB=", maxDescriptorOffset >> 20));
    }
  }

  void SceneManager::updateBufferCache(RaytraceGeometry& newGeoData) {
    ScopedCpuProfileZone();
    if (newGeoData.indexBuffer.defined()) {
      newGeoData.indexBufferIndex = m_bufferCache.track(newGeoData.indexBuffer);
    } else {
      newGeoData.indexBufferIndex = kSurfaceInvalidBufferIndex;
    }

    if (newGeoData.normalBuffer.defined()) {
      newGeoData.normalBufferIndex = m_bufferCache.track(newGeoData.normalBuffer);
    } else {
      newGeoData.normalBufferIndex = kSurfaceInvalidBufferIndex;
    }

    if (newGeoData.color0Buffer.defined()) {
      newGeoData.color0BufferIndex = m_bufferCache.track(newGeoData.color0Buffer);
    } else {
      newGeoData.color0BufferIndex = kSurfaceInvalidBufferIndex;
    }

    if (newGeoData.texcoordBuffer.defined()) {
      newGeoData.texcoordBufferIndex = m_bufferCache.track(newGeoData.texcoordBuffer);
    } else {
      newGeoData.texcoordBufferIndex = kSurfaceInvalidBufferIndex;
    }

    if (newGeoData.positionBuffer.defined()) {
      newGeoData.positionBufferIndex = m_bufferCache.track(newGeoData.positionBuffer);
    } else {
      newGeoData.positionBufferIndex = kSurfaceInvalidBufferIndex;
    }

    if (newGeoData.previousPositionBuffer.defined()) {
      newGeoData.previousPositionBufferIndex = m_bufferCache.track(newGeoData.previousPositionBuffer);
    } else {
      newGeoData.previousPositionBufferIndex = kSurfaceInvalidBufferIndex;
    }
  }

  void SceneManager::registerKenshiTerrainNormals(
      LegacyMaterialData& materialData,
      const std::array<TextureRef, LegacyMaterialData::kKenshiTerrainLayerCount>& normalTextures,
      const Vector4& textureFade) {
    KenshiTerrainNormalData data;
    data.textures = normalTextures;
    data.textureFade = textureFade;
    std::lock_guard<std::mutex> lock(s_kenshiTerrainNormalMutex);
    const auto key = getKenshiTerrainBaseIdentity(materialData);
    auto& lookup = s_kenshiTerrainNormalSets[key];
    auto previous = lookup.lock();
    if (previous) {
      bool changed = previous->textureFade != textureFade;
      for (uint32_t i = 0; i < normalTextures.size(); ++i) {
        const auto& old = previous->textures[i];
        const auto& current = normalTextures[i];
        changed |= old.isValid() != current.isValid()
          || (old.isValid() && current.isValid() && old.getUniqueKey() != current.getUniqueKey());
      }
      // This side-table identity omits normals/fade. Preserve last-writer semantics
      // if another draw replaces those values under an existing base material.
      if (changed) prepared_terrain::invalidate(prepared_terrain::Invalidation::NormalTable);
    }
    if (!previous) previous = std::make_shared<KenshiTerrainNormalData>();
    *previous = std::move(data);
    lookup = previous;
    materialData.kenshiTerrainNormals = std::move(previous);
  }

  void SceneManager::registerKenshiInteriorVolume(const KenshiInteriorVolume& volume) {
    std::lock_guard<std::mutex> lock(s_kenshiInteriorVolumeMutex);

    // A newer frame replaces the set rather than adding to it, so a building
    // that stopped being drawn stops being culled on the very next frame.
    if (volume.publishFrame != s_kenshiInteriorVolumeFrame) {
      s_kenshiInteriorVolumeFrame = volume.publishFrame;
      s_kenshiInteriorVolumes.clear();
    }

    // Each shell is drawn twice, once per mask pass, so the same box arrives
    // twice a frame. Slots are scarce - drop the duplicate rather than spend one.
    for (const KenshiInteriorVolume& existing : s_kenshiInteriorVolumes) {
      bool same = existing.vertexCount == volume.vertexCount;
      for (uint32_t c = 0; c < 3u && same; ++c) {
        same = existing.objMin[c] == volume.objMin[c]
            && existing.objMax[c] == volume.objMax[c];
      }
      // V641: the TRANSFORM is part of the identity. Comparing only geometry
      // merged two instances of the same house into one entry, so the second
      // building silently kept its intruding terrain.
      for (uint32_t col = 0; col < 4u && same; ++col) {
        for (uint32_t row = 0; row < 4u && same; ++row) {
          same = existing.worldViewProj[col][row] == volume.worldViewProj[col][row];
        }
      }
      if (same)
        return;
    }

    if (s_kenshiInteriorVolumes.size() < 8u)
      s_kenshiInteriorVolumes.push_back(volume);
  }

  void SceneManager::clearKenshiInteriorVolumes() {
    std::lock_guard<std::mutex> lock(s_kenshiInteriorVolumeMutex);
    s_kenshiInteriorVolumes.clear();
  }

  std::vector<SceneManager::KenshiInteriorVolume> SceneManager::getKenshiInteriorVolumes() {
    std::lock_guard<std::mutex> lock(s_kenshiInteriorVolumeMutex);
    return s_kenshiInteriorVolumes;
  }

  void SceneManager::registerKenshiWater(const KenshiWaterParams& params) {
    // DX11_V601: the first 13 only. Rain (13) and the scum trio (14-16) are
    // carried by the near-water shaders alone, and the distant one reports zero
    // for all of them - publishing them here let it erase them every frame.
    const float* source = &params.tileScaleX;
    for (uint32_t i = 0; i < 13u; ++i)
      s_kenshiWaterParams[i].store(source[i], std::memory_order_relaxed);
  }

  void SceneManager::registerKenshiWaterGlobalTextures(const TextureRef& colour,
                                                      const TextureRef& flow,
                                                      const TextureRef& blend) {
    std::lock_guard<std::mutex> lock(s_kenshiWaterZoneMutex);
    // Same keep-what-you-have rule as the biome records: a frustum-culled draw
    // must not clear a map that is already known good.
    if (!colour.isImageEmpty()) s_kenshiWaterGlobalTextures.scum = colour;
    if (!flow.isImageEmpty())   s_kenshiWaterGlobalTextures.scumNormal = flow;
    if (!blend.isImageEmpty())  s_kenshiWaterGlobalTextures.rain = blend;
  }

  void SceneManager::registerKenshiWaterBiome(uint32_t channel,
                                             const KenshiWaterParams& params,
                                             const KenshiWaterBiomeTextures& textures) {
    if (channel >= kKenshiWaterBiomeCount)
      return;

    std::lock_guard<std::mutex> lock(s_kenshiWaterZoneMutex);
    auto& record = s_kenshiWaterBiomes[channel];
    const bool isNew = !record.valid;
    record.params = params;
    // DX11_V619: keep a texture only while the draw actually supplies one. A
    // near patch is frustum culled the moment you look away, and a biome that
    // loses its maps to a null must not lose its look with them.
    if (!textures.scum.isImageEmpty())         record.textures.scum = textures.scum;
    if (!textures.scumNormal.isImageEmpty())   record.textures.scumNormal = textures.scumNormal;
    if (!textures.rain.isImageEmpty())         record.textures.rain = textures.rain;
    if (!textures.turbulence.isImageEmpty())   record.textures.turbulence = textures.turbulence;
    if (!textures.rippleNormal.isImageEmpty()) record.textures.rippleNormal = textures.rippleNormal;
    record.valid = true;

    if (isNew) {
      KENSHI_DIAGNOSTIC_INFO(str::format(
        "[SceneManager][kenshi-water] biome ", channel,
        " tileScale=", params.tileScaleX,
        " speed=", params.speedX,
        " distortion=", params.distortion,
        " scumScale=", params.scumScaleX,
        " rain=", params.rainAmount));
    }
  }

  Rc<DxvkBuffer> SceneManager::getKenshiWaterZoneBuffer(Rc<DxvkContext> ctx) {
    // Never null: the hit shader declares the buffer unconditionally, and a
    // header-only allocation reads as zero zones.
    if (m_kenshiWaterZoneBuffer == nullptr) {
      const uint32_t emptyHeader[4] = {};
      DxvkBufferCreateInfo info;
      info.size = align(sizeof(emptyHeader), kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiWaterZoneBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi water zones");
      ctx->writeToBuffer(m_kenshiWaterZoneBuffer, 0, sizeof(emptyHeader), emptyHeader);
    }
    return m_kenshiWaterZoneBuffer;
  }

  Rc<DxvkBuffer> SceneManager::getKenshiInteriorBuffer(Rc<DxvkContext> ctx) {
    // Never null. The hit shader declares kenshiInteriorVolumeBuffer
    // unconditionally, and a null descriptor faults inside the driver on submit -
    // the same failure getKenshiBloodBuffer documents. A header-only allocation
    // reads as zero volumes and clips nothing.
    if (m_kenshiInteriorBuffer == nullptr) {
      const uint32_t emptyHeader[8] = {};
      DxvkBufferCreateInfo info;
      info.size = align(sizeof(emptyHeader), kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiInteriorBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi interior volumes");
      ctx->writeToBuffer(m_kenshiInteriorBuffer, 0, sizeof(emptyHeader), emptyHeader);
    }
    return m_kenshiInteriorBuffer;
  }

  void SceneManager::buildKenshiInteriorBuffer(Rc<DxvkContext> ctx) {
    // DX11_V650_KENSHI_INTERIOR_SHELL. Concatenate the active shells' own
    // triangles into one buffer. This replaces the bounding box as the cull
    // volume - the box survives only as the broad-phase reject inside each
    // record, because measurement showed the shells are neither convex (2 of 32)
    // nor vertical-walled (median wall 3.6 degrees off vertical), so no box or
    // sampled field can stand in for them.
    //
    // Sole producer of the volume set: the accepted count published here is what
    // RtxContext copies into the constants, so the buffer and the count cannot
    // disagree about which volume is in which slot.
    static constexpr uint32_t kMaxVolumes = 4u;
    static constexpr uint32_t kHeaderVec4 = 2u;   // count, then per-volume offsets

    const std::vector<KenshiInteriorVolume> volumes = getKenshiInteriorVolumes();

    std::vector<uint32_t> data;
    data.assign(size_t(kHeaderVec4) * 4u, 0u);

    uint32_t accepted = 0u;
    for (const KenshiInteriorVolume& volume : volumes) {
      if (accepted >= kMaxVolumes)
        break;
      if (!volume.valid || volume.blob == nullptr || volume.blob->empty())
        continue;

      // The affine guard, unchanged in intent from V641: WVP = VP * world with
      // world affine, so dividing VP out must return an affine matrix. A wrong
      // convention, a mismatched camera or a projective result all break the
      // bottom row, and are rejected rather than published.
      const Matrix4& o2w = volume.objectToWorld;
      const float affineError = std::abs(o2w[0][3]) + std::abs(o2w[1][3])
                              + std::abs(o2w[2][3]) + std::abs(o2w[3][3] - 1.0f);
      if (!(affineError < 1e-2f))
        continue;

      const Matrix4 worldToObject = inverse(o2w);
      bool finite = true;
      for (uint32_t col = 0; col < 4u && finite; ++col)
        for (uint32_t row = 0; row < 4u && finite; ++row)
          finite = std::isfinite(worldToObject[col][row]);
      if (!finite)
        continue;

      const uint32_t recordVec4 = uint32_t(data.size() / 4u);
      data.insert(data.end(), volume.blob->begin(), volume.blob->end());

      // Words 0-2 of the record: the world-to-object rows the blob left blank.
      float* const record = reinterpret_cast<float*>(data.data() + size_t(recordVec4) * 4u);
      for (uint32_t row = 0; row < 3u; ++row) {
        record[row * 4u + 0u] = worldToObject[0][row];
        record[row * 4u + 1u] = worldToObject[1][row];
        record[row * 4u + 2u] = worldToObject[2][row];
        record[row * 4u + 3u] = worldToObject[3][row];
      }

      // The bias inflates or shrinks the broad-phase box only. It cannot move the
      // triangles, so a non-zero bias now widens the SEARCH, never the cull.
      const float bias = KenshiOptions::kenshiInteriorClipBias();
      if (bias != 0.0f) {
        for (uint32_t c = 0; c < 3u; ++c) {
          record[3u * 4u + c] -= bias;
          record[4u * 4u + c] += bias;
        }
      }

      data[4u + accepted] = recordVec4;
      ++accepted;
    }

    data[0] = accepted;
    m_kenshiInteriorVolumeCount = accepted;

    Rc<DxvkBuffer> buffer = getKenshiInteriorBuffer(ctx);
    const VkDeviceSize required = align(data.size() * sizeof(uint32_t), kBufferAlignment);
    if (buffer->info().size < required) {
      DxvkBufferCreateInfo info = buffer->info();
      info.size = required;
      m_kenshiInteriorBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi interior volumes");
      buffer = m_kenshiInteriorBuffer;
    }
    ctx->writeToBuffer(buffer, 0, data.size() * sizeof(uint32_t), data.data());
  }

  void SceneManager::buildKenshiWaterZoneBuffer(Rc<DxvkContext> ctx) {
    // DX11_V618. Five fixed records addressed by blendChannel, not a growing
    // list of rects. Slot 4 is the base biome - the unblended `water` material,
    // which raster weights by `1 - sum(blendMap)`.
    KenshiWaterBiomeRecord biomes[kKenshiWaterBiomeCount];
    {
      std::lock_guard<std::mutex> lock(s_kenshiWaterZoneMutex);
      for (uint32_t i = 0; i < kKenshiWaterBiomeCount; ++i)
        biomes[i] = s_kenshiWaterBiomes[i];
    }

    // 4 uint header + 20 uints (5 vec4s) per biome, matching the stride the hit
    // shader indexes with.
    //
    // DX11_V756: grown from 4 vec4s to 5 for the zone's own `invOpacity`. The
    // shader indexes this in vec4 units (`1 + biome * 5`), so BOTH water paths -
    // the opaque block and kenshiWaterShade - and the validity probe inside each
    // biome-selection loop have to move together. Five sites; there is no
    // derived constant to change on the shader side.
    constexpr uint32_t kRecordUints = 20u;
    std::vector<uint32_t> data;
    data.resize(4u + size_t(kKenshiWaterBiomeCount) * kRecordUints, 0u);
    data[0] = kKenshiWaterBiomeCount;

    // DX11_V623: header words 1-3 carry the whole-map colour, flow and blend
    // indices, two 16-bit indices per uint32. The opaque water path reaches
    // these through material slots; the translucent one has no spare slots, so
    // it reads them here instead and both paths stay in sync by construction.
    {
      KenshiWaterBiomeTextures globals;
      {
        std::lock_guard<std::mutex> lock(s_kenshiWaterZoneMutex);
        globals = s_kenshiWaterGlobalTextures;
      }
      auto index = [&](const TextureRef& texture) -> uint32_t {
        if (texture.isImageEmpty())
          return kSurfaceMaterialInvalidTextureIndex;
        uint32_t out = kSurfaceMaterialInvalidTextureIndex;
        trackTexture(texture, out, true);
        return out <= 0xffffu ? out : kSurfaceMaterialInvalidTextureIndex;
      };
      const uint32_t colourIndex = index(globals.scum);
      const uint32_t flowIndex = index(globals.scumNormal);
      const uint32_t blendIndex = index(globals.rain);
      data[1] = (colourIndex & 0xffffu) | ((flowIndex & 0xffffu) << 16u);
      data[2] = (blendIndex & 0xffffu)
              | ((uint32_t(kSurfaceMaterialInvalidTextureIndex) & 0xffffu) << 16u);
    }

    for (uint32_t i = 0; i < kKenshiWaterBiomeCount; ++i) {
      const auto& biome = biomes[i];
      float* dst = reinterpret_cast<float*>(data.data() + 4u + i * kRecordUints);
      dst[0] = biome.params.tileScaleX;
      dst[1] = biome.params.tileScaleY;
      dst[2] = biome.params.speedX;
      dst[3] = biome.params.speedY;
      dst[4] = biome.params.distortion;
      dst[5] = biome.params.invStrength;
      dst[6] = biome.params.scumScaleX;
      dst[7] = biome.params.scumScaleY;
      dst[8] = biome.params.scumDistortion;
      dst[9] = biome.params.rainAmount;
      dst[10] = biome.params.tileOffsetX;
      dst[11] = biome.params.tileOffsetY;
      // A record the game has never drawn must never be selected: the shader
      // folds its blend weight into the base biome instead.
      dst[12] = biome.valid ? 1.0f : 0.0f;

      // DX11_V756_KENSHI_WATER_ZONE_VISIBILITY. The zone's own depth range,
      // `1 / "water visibility"` as authored in FCS. Zero means this zone never
      // reported one and the shader keeps the frame global, which is the same
      // rule every other per-biome field here follows.
      dst[16] = biome.params.invOpacity;
      dst[17] = 0.0f;
      dst[18] = 0.0f;
      dst[19] = 0.0f;

      // DX11_V619: this biome's OWN maps, as packed bindless indices.
      //
      // trackTexture only RESERVES an index; the descriptor table is published
      // by m_bindlessResourceManager.prepareSceneData, which runs a few lines
      // after this function inside prepareSceneData. An index reserved after
      // that publish points past the end of what the GPU can see and loses the
      // device (V526). Verified for this call site: buildKenshiWaterZoneBuffer
      // is called immediately before that publish, so these are safe to sample
      // this frame. Do not move either call without re-checking the other.
      //
      // Two 16-bit indices per uint32, exactly as KenshiTerrainArgs packs its
      // per-layer normal maps.
      auto waterTextureIndex = [&](const TextureRef& texture) -> uint32_t {
        if (texture.isImageEmpty())
          return kSurfaceMaterialInvalidTextureIndex;
        uint32_t index = kSurfaceMaterialInvalidTextureIndex;
        trackTexture(texture, index, true);
        return index <= 0xffffu ? index : kSurfaceMaterialInvalidTextureIndex;
      };
      auto packWaterPair = [](uint32_t lo, uint32_t hi) {
        return (lo & 0xffffu) | ((hi & 0xffffu) << 16u);
      };

      const uint32_t scumIndex = waterTextureIndex(biome.textures.scum);
      const uint32_t scumNormalIndex = waterTextureIndex(biome.textures.scumNormal);
      const uint32_t rainIndex = waterTextureIndex(biome.textures.rain);
      const uint32_t turbulenceIndex = waterTextureIndex(biome.textures.turbulence);
      const uint32_t rippleIndex = waterTextureIndex(biome.textures.rippleNormal);

      uint32_t* packed = data.data() + 4u + i * kRecordUints;
      packed[13] = packWaterPair(scumIndex, scumNormalIndex);
      packed[14] = packWaterPair(rainIndex, turbulenceIndex);
      packed[15] = packWaterPair(rippleIndex, kSurfaceMaterialInvalidTextureIndex);
    }

    const VkDeviceSize requiredSize = VkDeviceSize(data.size()) * sizeof(uint32_t);
    if (m_kenshiWaterZoneBuffer == nullptr
     || m_kenshiWaterZoneBuffer->info().size < requiredSize) {
      DxvkBufferCreateInfo info;
      info.size = align(requiredSize, kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiWaterZoneBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi water biomes");
    }

    ctx->writeToBuffer(m_kenshiWaterZoneBuffer, 0, requiredSize, data.data());
  }

  void SceneManager::registerKenshiWaterRain(float rainAmount) {
    s_kenshiWaterParams[13].store(rainAmount, std::memory_order_relaxed);
  }

  // DX11_V621: `invOpacity` exists only on the near shaders - the distant one
  // has no depth and no alpha at all - so it is published on its own rather
  // than being zeroed by every distant draw, exactly like rain and scum.
  void SceneManager::registerKenshiWaterInvOpacity(float invOpacity) {
    if (invOpacity > 0.0f)
      s_kenshiWaterParams[17].store(invOpacity, std::memory_order_relaxed);
  }

  // DX11_V628: `glow` is another near-water-only constant. Preserve it across
  // distant draws so simulated depth can reproduce the raster surface term.
  void SceneManager::registerKenshiWaterGlow(float glow) {
    if (glow >= 0.0f)
      s_kenshiWaterParams[18].store(glow, std::memory_order_relaxed);
  }

  void SceneManager::registerKenshiWaterScum(float scaleX, float scaleY, float distortion) {
    s_kenshiWaterParams[14].store(scaleX, std::memory_order_relaxed);
    s_kenshiWaterParams[15].store(scaleY, std::memory_order_relaxed);
    s_kenshiWaterParams[16].store(distortion, std::memory_order_relaxed);
  }

  void SceneManager::getKenshiWater(KenshiWaterParams& params) {
    float* destination = &params.tileScaleX;
    for (uint32_t i = 0; i < 19u; ++i)
      destination[i] = s_kenshiWaterParams[i].load(std::memory_order_relaxed);
  }

  void SceneManager::registerKenshiWetness(float wetness, float waterHeight) {
    s_kenshiWetness.store(wetness, std::memory_order_relaxed);
    s_kenshiWaterHeight.store(waterHeight, std::memory_order_relaxed);
  }

  void SceneManager::registerKenshiDust(const Vector3& colour, float amountX, float amountY, float amountZ) {
    s_kenshiDustColourR.store(colour.x, std::memory_order_relaxed);
    s_kenshiDustColourG.store(colour.y, std::memory_order_relaxed);
    s_kenshiDustColourB.store(colour.z, std::memory_order_relaxed);
    s_kenshiDustAmountX.store(amountX, std::memory_order_relaxed);
    s_kenshiDustAmountY.store(amountY, std::memory_order_relaxed);
    s_kenshiDustAmountZ.store(amountZ, std::memory_order_relaxed);
  }

  void SceneManager::getKenshiDust(Vector3& colour, float& amountX, float& amountY, float& amountZ) {
    colour.x = s_kenshiDustColourR.load(std::memory_order_relaxed);
    colour.y = s_kenshiDustColourG.load(std::memory_order_relaxed);
    colour.z = s_kenshiDustColourB.load(std::memory_order_relaxed);
    amountX = s_kenshiDustAmountX.load(std::memory_order_relaxed);
    amountY = s_kenshiDustAmountY.load(std::memory_order_relaxed);
    amountZ = s_kenshiDustAmountZ.load(std::memory_order_relaxed);
  }

  void SceneManager::getKenshiWetness(float& wetness, float& waterHeight) {
    wetness = s_kenshiWetness.load(std::memory_order_relaxed);
    waterHeight = s_kenshiWaterHeight.load(std::memory_order_relaxed);
  }

  void SceneManager::registerKenshiTerrainBiomeSets(
      LegacyMaterialData& materialData, uint64_t key,
      const std::array<LegacyMaterialData::KenshiTerrainBiomeSet,
                       LegacyMaterialData::kKenshiTerrainBiomeSetCount>& sets) {
    if (key == 0u)
      return;
    std::lock_guard<std::mutex> lock(s_kenshiTerrainBiomeMutex);
    auto& lookup = s_kenshiTerrainBiomeSets[key];
    auto data = lookup.lock();
    if (!data) data = std::make_shared<KenshiTerrainBiomeData>();
    *data = sets;
    lookup = data;
    materialData.kenshiTerrainBiomes = std::move(data);
  }

  uint32_t SceneManager::setKenshiTerrainArgs(const LegacyMaterialData& materialData,
                                             bool hasTexcoords) {
    terrain_profile::Scope terrainCpuArgs(terrain_profile::Stage::TerrainArgs);
    // DX11_V398_KENSHI_TERRAIN_PER_MATERIAL: content-address one complete
    // six-layer set so cached surface materials can retain its stable index.
    //
    // DX11_V528_KENSHI_TERRAIN_BIOME_BLEND: a boundary tile registers one record
    // per biome, all through the same packer, and the primary record carries the
    // blend map plus the neighbours' record indices. Every texture referenced by
    // every record is tracked HERE - inside determineMaterialData, therefore
    // during scene building and before m_bindlessResourceManager.prepareSceneData
    // publishes the descriptor table. That ordering is the V527 rule and it is
    // the reason these indices are safe to sample.
    auto toFloat4 = [](const Vector4& v) {
      return float4 { v.x, v.y, v.z, v.w };
    };

    static constexpr uint32_t kLayerCount =
      LegacyMaterialData::kKenshiTerrainLayerCount;

    uint32_t overlayIndex = kSurfaceMaterialInvalidTextureIndex;
    trackTexture(materialData.kenshiTerrainOverlay, overlayIndex, hasTexcoords);
    auto& textureManager = m_device->getCommon()->getTextureManager();
    textureManager.markTerrainTexture(overlayIndex);

    // V745: regional maps keep the material's native sampler. Track the detail
    // sampler before descriptor publication, with the same filtering policy.
    Rc<DxvkSampler> detailSampler = materialData.getSampler2();
    if (detailSampler == nullptr) detailSampler = materialData.getSampler();
    if (detailSampler != nullptr) {
      const auto info = detailSampler->info();
      detailSampler = patchSampler(info.magFilter, info.addressModeU,
        info.addressModeV, info.addressModeW, info.borderColor);
    }
    const uint32_t detailSamplerIndex = trackSampler(detailSampler);
    const XXH64_hash_t detailSamplerHash = detailSampler != nullptr ? detailSampler->hash() : 0u;
    const uint32_t featureFlag = materialData.kenshiTerrainBlend == 2u ? 1u : 0u;

    // One complete six-layer biome: the parts that differ per set. Everything
    // else on the record - detail rect, overlay map, height offset - is a
    // property of the CHUNK and is shared by all of them.
    struct TerrainSetSource {
      const TextureRef* layers;
      const TextureRef* normals;
      const Vector2* layerScales;
      Vector4 slopeMin;
      Vector4 slopeMax;
      Vector4 slopeBlend;
      Vector4 overlayMult;
      float brightnessFix;
      Vector4 textureFade;
    };

    std::array<uint32_t, kLayerCount> loggedNormalIndices = {};

    auto packSet = [&](const TerrainSetSource& source,
                       KenshiTerrainArgs& args,
                       XXH64_hash_t& identityOut) {
      args = {};
      args.detailScale = float2 { materialData.kenshiTerrainDetailScale.x,
                                  materialData.kenshiTerrainDetailScale.y };
      args.detailOffset = float2 { materialData.kenshiTerrainDetailOffset.x,
                                   materialData.kenshiTerrainDetailOffset.y };

      float2* const layerScales[kLayerCount] = {
        &args.layerScale0, &args.layerScale1, &args.layerScale2,
        &args.layerScale3, &args.layerScale4, &args.layerScale5,
      };
      uint32_t* const layerTextures[kLayerCount] = {
        &args.layerTexture0, &args.layerTexture1, &args.layerTexture2,
        &args.layerTexture3, &args.layerTexture4, &args.layerTexture5,
      };
      std::array<uint32_t, kLayerCount> normalTextureIndices = {};

      for (uint32_t layer = 0u; layer < kLayerCount; ++layer) {
        *layerScales[layer] = float2 { source.layerScales[layer].x,
                                       source.layerScales[layer].y };
        uint32_t textureIndex = kSurfaceMaterialInvalidTextureIndex;
        trackTexture(source.layers[layer], textureIndex, hasTexcoords);
        textureManager.markTerrainTexture(textureIndex);
        *layerTextures[layer] = textureIndex;
        uint32_t normalIndex = kSurfaceMaterialInvalidTextureIndex;
        trackTexture(source.normals[layer], normalIndex, hasTexcoords);
        textureManager.markTerrainTexture(normalIndex);
        normalTextureIndices[layer] = normalIndex;
      }

      args.overlayTexture = overlayIndex;
      args.slopeMin = toFloat4(source.slopeMin);
      args.slopeMax = toFloat4(source.slopeMax);
      args.slopeBlend = toFloat4(source.slopeBlend);
      args.overlayMult = toFloat4(source.overlayMult);
      args.brightnessFix = source.brightnessFix;
      args.objectHeightOffset = materialData.kenshiTerrainHeightOffset;
      // Existing word: bit 0 remains the feature flag; bits 1..16 hold the
      // detail sampler. No terrain-buffer stride or field-offset change.
      args.pad0 = featureFlag | (detailSamplerIndex << 1u);
      auto packTexturePair = [&normalTextureIndices](uint32_t first) {
        const uint32_t lo = normalTextureIndices[first] <= 0xffffu
          ? normalTextureIndices[first] : 0xffffu;
        const uint32_t hi = normalTextureIndices[first + 1u] <= 0xffffu
          ? normalTextureIndices[first + 1u] : 0xffffu;
        return lo | (hi << 16u);
      };
      const uint32_t packedNormal01 = packTexturePair(0u);
      const uint32_t packedNormal23 = packTexturePair(2u);
      const uint32_t packedNormal45 = packTexturePair(4u);
      std::memcpy(&args.slopeMin.z, &packedNormal01, sizeof(uint32_t));
      std::memcpy(&args.slopeMin.w, &packedNormal23, sizeof(uint32_t));
      std::memcpy(&args.slopeMax.z, &packedNormal45, sizeof(uint32_t));
      args.textureFade = toFloat4(source.textureFade);
      // DX11_V530: the composite reads only .xy of each slope vector, so .zw
      // are spare and already carry packed extras elsewhere in this record.
      args.slopeBlend.z = materialData.kenshiTerrainHeightWarp.x;
      args.slopeBlend.w = materialData.kenshiTerrainHeightWarp.y;
      args.set = 1u;
      // A neighbour record must never itself open a selection: zero here is
      // what makes the selector one level deep by construction.
      args.blendMask = 0u;
      args.blendTexture = kSurfaceMaterialInvalidTextureIndex;

      // Identity is CONTENT, deliberately excluding the tracked bindless
      // indices - those are per-frame, the record is not.
      XXH64_hash_t identity = XXH64(
        &materialData.kenshiTerrainDetailScale, sizeof(Vector2), 0);
      identity = XXH64(
        &materialData.kenshiTerrainDetailOffset, sizeof(Vector2), identity);
      for (uint32_t layer = 0u; layer < kLayerCount; ++layer)
        identity = XXH64(&source.layerScales[layer], sizeof(Vector2), identity);
      identity = XXH64(&source.slopeMin, sizeof(Vector4), identity);
      identity = XXH64(&source.slopeMax, sizeof(Vector4), identity);
      identity = XXH64(&source.slopeBlend, sizeof(Vector4), identity);
      identity = XXH64(
        &materialData.kenshiTerrainHeightWarp, sizeof(Vector2), identity);
      identity = XXH64(&source.overlayMult, sizeof(Vector4), identity);
      identity = XXH64(&source.brightnessFix, sizeof(float), identity);
      identity = XXH64(&source.textureFade, sizeof(Vector4), identity);
      identity = XXH64(&featureFlag, sizeof(featureFlag), identity);
      // Hash sampler content, not its tracked descriptor-table index.
      identity = XXH64(&detailSamplerHash, sizeof(detailSamplerHash), identity);
      identity = XXH64(
        &materialData.kenshiTerrainHeightOffset, sizeof(float), identity);
      for (uint32_t layer = 0u; layer < kLayerCount; ++layer) {
        const uint64_t layerKey = source.layers[layer].getUniqueKey();
        identity = XXH64(&layerKey, sizeof(layerKey), identity);
        const uint64_t normalKey = source.normals[layer].getUniqueKey();
        identity = XXH64(&normalKey, sizeof(normalKey), identity);
      }
      const uint64_t overlayKey = materialData.kenshiTerrainOverlay.getUniqueKey();
      identity = XXH64(&overlayKey, sizeof(overlayKey), identity);
      identityOut = identity;
      loggedNormalIndices = normalTextureIndices;
    };

    auto registerSet = [&](XXH64_hash_t identity,
                           const KenshiTerrainArgs& args,
                           bool& isNew) {
      uint32_t setIndex = 0u;
      isNew = false;
      const auto existing = m_kenshiTerrainSetIndices.find(identity);
      if (existing != m_kenshiTerrainSetIndices.end()) {
        setIndex = existing->second;
      } else {
        setIndex = static_cast<uint32_t>(m_kenshiTerrainArgs.size());
        m_kenshiTerrainSetIndices.emplace(identity, setIndex);
        m_kenshiTerrainArgs.emplace_back();
        isNew = true;
      }
      m_kenshiTerrainArgs[setIndex] = args;
      return setIndex;
    };

    // The primary set's normal maps arrive through a side table keyed on the
    // base identity, because the D3D11 side registers them separately.
    KenshiTerrainNormalData normalData;
    {
      std::lock_guard<std::mutex> lock(s_kenshiTerrainNormalMutex);
      if (materialData.kenshiTerrainNormals)
        normalData = *materialData.kenshiTerrainNormals;
    }

    uint32_t blendMask = materialData.kenshiTerrainBlendChannelMask & 0xfu;
    uint32_t activeCount = 0u;
    for (uint32_t bits = blendMask; bits != 0u; bits >>= 1u)
      activeCount += bits & 1u;
    if (activeCount > LegacyMaterialData::kKenshiTerrainBiomeSetCount) {
      blendMask = 0u;
      activeCount = 0u;
    }

    KenshiTerrainBiomeData biomeData;
    if (blendMask != 0u) {
      std::lock_guard<std::mutex> lock(s_kenshiTerrainBiomeMutex);
      if (materialData.kenshiTerrainBiomes) {
        biomeData = *materialData.kenshiTerrainBiomes;
      } else {
        // The material named biome sets that are not registered. Fall back to
        // the hard boundary rather than to three empty sets.
        blendMask = 0u;
        activeCount = 0u;
      }
    }

    std::array<uint32_t, LegacyMaterialData::kKenshiTerrainBiomeSetCount>
      blendSetIndices = { 0u, 0u, 0u };
    std::array<XXH64_hash_t, LegacyMaterialData::kKenshiTerrainBiomeSetCount>
      blendSetIdentities = { 0u, 0u, 0u };
    for (uint32_t set = 0u; set < activeCount; ++set) {
      const auto& biomeSet = biomeData[set];
      const TerrainSetSource source {
        biomeSet.layers.data(), biomeSet.normals.data(),
        biomeSet.layerScales.data(),
        biomeSet.slopeMin, biomeSet.slopeMax, biomeSet.slopeBlend,
        biomeSet.overlayMult, biomeSet.brightnessFix, biomeSet.textureFade,
      };
      KenshiTerrainArgs blendArgs = {};
      XXH64_hash_t blendIdentity = 0u;
      packSet(source, blendArgs, blendIdentity);
      bool isNew = false;
      blendSetIndices[set] = registerSet(blendIdentity, blendArgs, isNew);
      blendSetIdentities[set] = blendIdentity;
    }

    uint32_t blendIndex = kSurfaceMaterialInvalidTextureIndex;
    if (blendMask != 0u)
      trackTexture(materialData.kenshiTerrainBiomeBlend, blendIndex, hasTexcoords);
    textureManager.markTerrainTexture(blendIndex);
    if (blendIndex == kSurfaceMaterialInvalidTextureIndex)
      blendMask = 0u;

    const TerrainSetSource primarySource {
      materialData.kenshiTerrainLayers.data(), normalData.textures.data(),
      materialData.kenshiTerrainLayerScales.data(),
      materialData.kenshiTerrainSlopeMin, materialData.kenshiTerrainSlopeMax,
      materialData.kenshiTerrainSlopeBlend, materialData.kenshiTerrainOverlayMult,
      materialData.kenshiTerrainBrightnessFix, normalData.textureFade,
    };
    KenshiTerrainArgs args = {};
    XXH64_hash_t identity = 0u;
    packSet(primarySource, args, identity);

    if (blendMask != 0u) {
      args.blendMask = blendMask;
      args.blendTexture = blendIndex;
      args.biomeScale = float2 { materialData.kenshiTerrainBiomeScale.x,
                                 materialData.kenshiTerrainBiomeScale.y };
      args.biomeOffset = float2 { materialData.kenshiTerrainBiomeOffset.x,
                                  materialData.kenshiTerrainBiomeOffset.y };
      args.blendSet0 = blendSetIndices[0];
      args.blendSet1 = blendSetIndices[1];
      args.blendSet2 = blendSetIndices[2];

      // A plain tile and a boundary tile can share every primary-set field.
      // Without this they would collapse onto one record and whichever
      // registered last would decide blending for both.
      identity = XXH64(&blendMask, sizeof(blendMask), identity);
      identity = XXH64(&materialData.kenshiTerrainBiomeScale, sizeof(Vector2), identity);
      identity = XXH64(&materialData.kenshiTerrainBiomeOffset, sizeof(Vector2), identity);
      const uint64_t blendKey = materialData.kenshiTerrainBiomeBlend.getUniqueKey();
      identity = XXH64(&blendKey, sizeof(blendKey), identity);
      for (uint32_t set = 0u; set < activeCount; ++set)
        identity = XXH64(&blendSetIdentities[set], sizeof(XXH64_hash_t), identity);
    }

    bool newTerrainSet = false;
    const uint32_t setIndex = registerSet(identity, args, newTerrainSet);

    if (newTerrainSet) {
      static uint32_t sSetLogCount = 0u;
      if (sSetLogCount < 32u) {
        ++sSetLogCount;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[RTX Kenshi Terrain][set] ", setIndex,
          " layerTex=[", args.layerTexture0, ",", args.layerTexture1, ",",
          args.layerTexture2, ",", args.layerTexture3, ",",
          args.layerTexture4, ",", args.layerTexture5, "]",
          " normalTex=[", loggedNormalIndices[0], ",", loggedNormalIndices[1], ",",
          loggedNormalIndices[2], ",", loggedNormalIndices[3], ",",
          loggedNormalIndices[4], ",", loggedNormalIndices[5], "]",
          " overlayTex=", args.overlayTexture,
          " invalidIs=", uint32_t(kSurfaceMaterialInvalidTextureIndex),
          " hasTexcoords=", hasTexcoords ? 1 : 0,
          " detail=[", args.detailScale.x, ",", args.detailScale.y, "]",
          " layer0Scale=[", args.layerScale0.x, ",", args.layerScale0.y, "]",
          " heightOffset=", args.objectHeightOffset,
          " blendMask=0x", std::hex, args.blendMask, std::dec,
          " blendTex=", args.blendTexture,
          " blendSets=[", args.blendSet0, ",", args.blendSet1, ",",
          args.blendSet2, "]",
          " records=", m_kenshiTerrainArgs.size()));
      }
    }
    return setIndex;
  }

  void SceneManager::addKenshiTerrainBloodDecal(const Vector4& rect,
                                               const TextureRef& texture,
                                               const Rc<DxvkSampler>& sampler) {
    // Bounded: the hit shader reads at most 64, and an unbounded list would grow
    // with every splat the camera can see.
    static constexpr size_t kMaxKenshiTerrainBloodDecals = 64;
    if (m_kenshiTerrainBloodDecals.size() >= kMaxKenshiTerrainBloodDecals) {
      ONCE(Logger::warn(str::format(
        "[SceneManager][terrain-blood] decal budget of ",
        kMaxKenshiTerrainBloodDecals, " reached; extra ground blood ignored")));
      return;
    }
    m_kenshiTerrainBloodDecals.push_back({ rect, texture, sampler });
  }

  Rc<DxvkBuffer> SceneManager::getKenshiTerrainBloodBuffer(Rc<DxvkContext> ctx) {
    // Pure accessor: runs once per ray tracing pass and must NOT track textures.
    // buildKenshiTerrainBloodBuffer does that during prepareSceneData, before the
    // bindless table is finalized.
    //
    // Still guarantees non-null, for the frames where the scene is never prepared
    // (menus, loading) but a pass still binds: a header-only buffer reads as zero
    // decals. Allocating here is safe; reserving a texture index would not be.
    if (m_kenshiTerrainBloodBuffer == nullptr) {
      const uint32_t emptyHeader[4] = {};
      DxvkBufferCreateInfo info;
      info.size = align(sizeof(emptyHeader), kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiTerrainBloodBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi terrain blood decals");
      ctx->writeToBuffer(m_kenshiTerrainBloodBuffer, 0, sizeof(emptyHeader), emptyHeader);
    }
    return m_kenshiTerrainBloodBuffer;
  }

  void SceneManager::buildKenshiTerrainBloodBuffer(Rc<DxvkContext> ctx) {

    // Layout: element 0 is a header (.x = decal count), then two uint4 per decal -
    // the rect as floats, then (textureIndex, samplerIndex, 0, 0). Keeping the
    // count in the buffer avoids touching RaytraceArgs for it.
    std::vector<uint32_t> data;
    data.resize(4u + m_kenshiTerrainBloodDecals.size() * 8u, 0u);
    data[0] = static_cast<uint32_t>(m_kenshiTerrainBloodDecals.size());

    for (size_t i = 0; i < m_kenshiTerrainBloodDecals.size(); ++i) {
      auto& decal = m_kenshiTerrainBloodDecals[i];
      uint32_t textureIndex = kSurfaceMaterialInvalidTextureIndex;
      trackTexture(decal.texture, textureIndex, true);
      const uint32_t samplerIndex = trackSampler(decal.sampler);

      uint32_t* dst = data.data() + 4u + i * 8u;
      std::memcpy(dst, &decal.rect, sizeof(float) * 4);
      dst[4] = textureIndex;
      dst[5] = samplerIndex;
    }

    const VkDeviceSize requiredSize = VkDeviceSize(data.size()) * sizeof(uint32_t);
    if (m_kenshiTerrainBloodBuffer == nullptr
     || m_kenshiTerrainBloodBuffer->info().size < requiredSize) {
      DxvkBufferCreateInfo info;
      info.size = align(requiredSize, kBufferAlignment);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                  | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      m_kenshiTerrainBloodBuffer = m_device->createBuffer(
        info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        DxvkMemoryStats::Category::RTXBuffer, "kenshi terrain blood decals");
    }

    ctx->writeToBuffer(m_kenshiTerrainBloodBuffer, 0, requiredSize, data.data());

    static std::atomic<uint32_t> s_terrainBloodUploadLogs = { 0u };
    if (kenshi_telemetry::enabled() && s_terrainBloodUploadLogs.fetch_add(1u, std::memory_order_relaxed) < 12u) {
      std::string indices;
      for (size_t i = 0; i < m_kenshiTerrainBloodDecals.size() && i < 8u; ++i)
        indices += str::format(" [", i, "]tex=", data[4u + i * 8u + 4u],
                               ",smp=", data[4u + i * 8u + 5u]);
      Logger::info(str::format(
        "[SceneManager][terrain-blood] uploaded decals=", data[0], indices));
    }
  }

  void SceneManager::ensureKenshiTerrainBuffer(size_t recordCount) {
    static_assert(sizeof(KenshiTerrainArgs) == 224u,
      "Kenshi terrain record ABI changed; update the shader's matching struct");
    const VkDeviceSize requiredSize = VkDeviceSize(
      std::max<size_t>(recordCount, size_t(1))) * sizeof(KenshiTerrainArgs);

    if (m_kenshiTerrainBuffer != nullptr
     && m_kenshiTerrainBuffer->info().size >= requiredSize) {
      return;
    }

    DxvkBufferCreateInfo info;
    info.size = align(requiredSize, kBufferAlignment);
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    m_kenshiTerrainBuffer = m_device->createBuffer(
      info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      DxvkMemoryStats::Category::RTXBuffer, "kenshi terrain parameters");
  }

  void SceneManager::buildKenshiTerrainBuffer(Rc<DxvkContext> ctx) {
    // DX11_V530: once per frame, from prepareSceneData. This used to run inside
    // getKenshiTerrainBuffer, which bindCommonRayTracingResources calls once per
    // ray tracing PASS - so the whole record array was re-uploaded twelve times
    // a frame to no purpose. Records only ever change during scene building,
    // which has already finished by the time this runs.
    ensureKenshiTerrainBuffer(m_kenshiTerrainArgs.size());

    if (!m_kenshiTerrainArgs.empty()) {
      ctx->writeToBuffer(m_kenshiTerrainBuffer, 0,
                         m_kenshiTerrainArgs.size() * sizeof(KenshiTerrainArgs),
                         m_kenshiTerrainArgs.data());
    } else {
      const KenshiTerrainArgs empty = {};
      ctx->writeToBuffer(m_kenshiTerrainBuffer, 0, sizeof(empty), &empty);
    }
  }

  Rc<DxvkBuffer> SceneManager::getKenshiTerrainBuffer(Rc<DxvkContext> ctx) {
    // Pure accessor - no upload, no growth, no per-frame state. Menus and
    // loading screens bind without ever preparing a scene, and the shader
    // declares this slot unconditionally, so a first-use allocation with one
    // inert record keeps the never-null contract.
    if (m_kenshiTerrainBuffer == nullptr) {
      ensureKenshiTerrainBuffer(1u);
      const KenshiTerrainArgs empty = {};
      ctx->writeToBuffer(m_kenshiTerrainBuffer, 0, sizeof(empty), &empty);
    }

    return m_kenshiTerrainBuffer;
  }
  SceneManager::ObjectCacheState SceneManager::onSceneObjectAdded(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, BlasEntry* pBlas, bool geometryBufferAllocationOnly) {
    // This is a new object.
    ObjectCacheState result = processGeometryInfo<true>(ctx, drawCallState, pBlas->modifiedGeometryData, geometryBufferAllocationOnly);
    
    assert(result == ObjectCacheState::KBuildBVH);

    pBlas->frameLastUpdated = m_device->getCurrentFrameId();
    m_instanceManager.notifySceneChanged();

    return result;
  }
  
  SceneManager::ObjectCacheState SceneManager::onSceneObjectUpdated(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, BlasEntry* pBlas, bool geometryBufferAllocationOnly) {
    if (pBlas->frameLastTouched == m_device->getCurrentFrameId()) {
      pBlas->cacheMaterial(drawCallState.getMaterialData());
      return SceneManager::ObjectCacheState::kUpdateInstance;
    }

    // TODO: If mesh is static, no need to do any of the below, just use the existing modifiedGeometryData and set result to kInstanceUpdate.
    ObjectCacheState result = processGeometryInfo<false>(ctx, drawCallState, pBlas->modifiedGeometryData, geometryBufferAllocationOnly);

    // We dont expect to hit the rebuild path here - since this would indicate an index buffer or other topological change, and that *should* trigger a new scene object (since the hash would change)
    assert(result != ObjectCacheState::KBuildBVH);

    if (result == ObjectCacheState::kUpdateBVH) {
      pBlas->frameLastUpdated = m_device->getCurrentFrameId();
      m_instanceManager.notifySceneChanged();
    }
    
    pBlas->clearMaterialCache();
    pBlas->input = drawCallState; // cache the draw state for the next time.
    return result;
  }
  
  void SceneManager::onInstanceAdded(RtInstance& instance) {
    BlasEntry* pBlas = instance.getBlas();
    if (pBlas != nullptr) {
      pBlas->linkInstance(&instance);
    }
  }

  void SceneManager::onInstanceUpdated(RtInstance& instance, const DrawCallState& drawCall, const MaterialData& material, const bool hasTransformChanged, const bool hasVerticesChanged, const bool isFirstUpdateThisFrame) {
    auto capturer = m_device->getCommon()->capturer();
    if (hasTransformChanged) {
      capturer->setInstanceUpdateFlag(instance, GameCapturer::InstFlag::XformUpdate);
    }

    if (hasVerticesChanged) {
      capturer->setInstanceUpdateFlag(instance, GameCapturer::InstFlag::PositionsUpdate);
      capturer->setInstanceUpdateFlag(instance, GameCapturer::InstFlag::NormalsUpdate);
    }

    // Blood amounts and appearance are per-draw instance state, not material
    // identity. The projection buffer itself is immutable bind-pose data.
    {
      RtSurface& surface = instance.surface;
      const LegacyMaterialData& legacy = drawCall.getMaterialData();
      const RasterBuffer& projection = drawCall.getGeometryData().kenshiBloodProjectionBuffer;

      // DX11_V538: the roughness is packed for EVERY surface, not only those
      // carrying character blood. Terrain blood writes the same
      // surfaceInteraction.kenshiBlood and runs the same downstream roughness
      // blend, but a terrain draw has no character blood mode - so it was
      // reading roughness bits of ZERO, i.e. a perfect mirror, which is both why
      // the slider did nothing for ground blood and why it threw stark white
      // specular highlights where decals overlapped.
      //
      // Safe against the `kenshiBloodMode != 0` character test because the
      // shader-side property already masks to the low 8 bits (surface.h).
      const float bloodRoughness = std::min(1.0f, std::max(0.0f,
        KenshiOptions::kenshiBloodRoughness()));
      const uint32_t bloodRoughnessBits = uint32_t(bloodRoughness * 255.0f + 0.5f);

      surface.kenshiBloodMode = bloodRoughnessBits << 8u;
      surface.kenshiBloodProjectionBufferIndex = kSurfaceInvalidBufferIndex;
      surface.kenshiBloodTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      surface.kenshiBloodSamplerIndex = kSurfaceMaterialInvalidTextureIndex;
      surface.kenshiBloodAmounts0 = 0u;
      surface.kenshiBloodAmounts1 = 0u;
      surface.kenshiBloodScale = 0u;
      surface.kenshiBloodColor = 0u;

      if (legacy.kenshiBloodMode != 0u && projection.defined()
       && legacy.kenshiBloodTexture.isValid()) {
        const RaytraceBuffer rtProjection(
          DxvkBufferSlice(projection), projection.offsetFromSlice(),
          projection.stride(), projection.vertexFormat());
        surface.kenshiBloodProjectionBufferIndex = m_bufferCache.track(rtProjection);
        trackTexture(legacy.kenshiBloodTexture, surface.kenshiBloodTextureIndex, true);
        surface.kenshiBloodSamplerIndex = trackSampler(legacy.kenshiBloodSampler);

        auto packAmounts = [&](uint32_t first) {
          uint32_t packed = 0u;
          for (uint32_t i = 0; i < 4u; ++i) {
            const float value = std::clamp(legacy.kenshiBloodAmounts[first + i], 0.0f, 1.0f);
            packed |= uint32_t(value * 255.0f + 0.5f) << (i * 8u);
          }
          return packed;
        };
        auto packColor = [](const Vector4& color) {
          uint32_t packed = 0u;
          for (uint32_t i = 0; i < 4u; ++i) {
            const float value = std::clamp(color[i], 0.0f, 1.0f);
            packed |= uint32_t(value * 255.0f + 0.5f) << (i * 8u);
          }
          return packed;
        };

        surface.kenshiBloodAmounts0 = packAmounts(0u);
        surface.kenshiBloodAmounts1 = packAmounts(4u);
        surface.kenshiBloodScale = glm::packHalf2x16(glm::vec2(
          legacy.kenshiBloodScale.x, legacy.kenshiBloodScale.y));
        surface.kenshiBloodColor = packColor(legacy.kenshiBloodColor);
        // Roughness rides in bits 8-15 of the mode word rather than in
        // RaytraceArgs: no constant-buffer layout change, so no chance of the
        // C++ and shader views disagreeing. Already packed above for every
        // surface; this only adds the character mode into the low byte.
        surface.kenshiBloodMode |= legacy.kenshiBloodMode;
      }
    }

    const bool traceParticleMaterial =
      (kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u) &&
      drawCall.categories.test(InstanceCategories::Particle);
    const auto oldSurfaceMaterialIndex = instance.surface.surfaceMaterialIndex;
    const uint32_t oldAlbedoTextureIndex = instance.getAlbedoOpacityTextureIndex();

    uint64_t textureKey = 0u;
    XXH64_hash_t textureHash = 0u;
    VkExtent3D textureExtent { 0u, 0u, 0u };
    if (traceParticleMaterial && material.getType() == MaterialDataType::Opaque) {
      const TextureRef& texture = material.getOpaqueMaterialData().getAlbedoOpacityTexture();
      if (texture.isValid())
        textureKey = static_cast<uint64_t>(texture.getUniqueKey());
      textureHash = texture.getImageHash();
      if (const DxvkImageView* imageView = texture.getImageView())
        textureExtent = imageView->imageInfo().extent;
    }

    // Create and bind the RT material
    const RtSurfaceMaterial& surfaceMaterial = createSurfaceMaterial(material, drawCall);
    const uint32_t candidateAlbedoTextureIndex =
      surfaceMaterial.getType() == RtSurfaceMaterialType::Opaque
        ? surfaceMaterial.getOpaqueSurfaceMaterial().getAlbedoOpacityTextureIndex()
        : kSurfaceMaterialInvalidTextureIndex;

    if(isFirstUpdateThisFrame) {
      m_instanceManager.bindMaterial(instance, surfaceMaterial);
    }

    if (traceParticleMaterial) {
      Logger::info(str::format(
        "[SceneManager][particle-material-bind] f=", m_device->getCurrentFrameId(),
        " draw=", drawCall.drawCallID,
        " inst=", instance.getId(),
        " first=", isFirstUpdateThisFrame ? 1 : 0,
        " matHash=0x", std::hex, material.getHash(),
        " texKey=0x", textureKey,
        " texHash=0x", textureHash, std::dec,
        " extent=", textureExtent.width, "x", textureExtent.height,
        " oldSurfMat=", oldSurfaceMaterialIndex,
        " oldAlbedo=", oldAlbedoTextureIndex,
        " candidateAlbedo=", candidateAlbedoTextureIndex,
        " boundSurfMat=", instance.surface.surfaceMaterialIndex,
        " boundAlbedo=", instance.getAlbedoOpacityTextureIndex()));
    }

    // Update portal
    if (surfaceMaterial.getType() == RtSurfaceMaterialType::RayPortal) {
      m_rayPortalManager.processRayPortalData(instance, surfaceMaterial);
    }
  }

  void SceneManager::onInstanceDestroyed(RtInstance& instance) {
    // Evict from the AccelManager bucket cache to prevent stale pointer ABA issues.
    m_accelManager.removeInstanceFromBucketCache(&instance);

    BlasEntry* pBlas = instance.getBlas();
    if (pBlas != nullptr) {
      pBlas->unlinkInstance(&instance);
    }
  }

  // Helper to populate the texture cache with this resource (and patch sampler if required for texture)
  void SceneManager::trackTexture(const TextureRef &inputTexture,
                                  uint32_t& textureIndex,
                                  bool hasTexcoords,
                                  bool async,
                                  uint16_t samplerFeedbackStamp) {
    terrain_profile::Scope terrainCpuTexture(terrain_profile::Stage::Texture);
    // If no texcoords, no need to bind the texture
    if (!hasTexcoords) {
      ONCE(Logger::info(str::format("[RTX-Compatibility-Info] Trying to bind a texture to a mesh without UVs.  Was this intended?")));
      return;
    }

    auto& textureManager = m_device->getCommon()->getTextureManager();
    textureManager.addTexture(inputTexture, samplerFeedbackStamp, async, textureIndex);
  }

  RtInstance* SceneManager::processDrawCallState(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, MaterialData& renderMaterialData, RtInstance* existingInstance, const RtxParticleSystemDesc* pParticleSystemDesc, bool geometryCacheOnly) {
    terrain_profile::Scope terrainCpuInstance(terrain_profile::Stage::Instance);
    ScopedCpuProfileZone();

    if (renderMaterialData.getIgnored()) {
      return nullptr;
    }

    ObjectCacheState result = ObjectCacheState::kInvalid;
    BlasEntry* pBlas = nullptr;
    const bool cacheExisted =
      m_drawCallCache.get(drawCallState, &pBlas,
        existingInstance != nullptr ? existingInstance->getBlas() : nullptr) == DrawCallCache::CacheState::kExisted;
    terrain_profile::count(cacheExisted ? terrain_profile::CacheHit : terrain_profile::CacheMiss);
    if (cacheExisted) {
      result = onSceneObjectUpdated(ctx, drawCallState, pBlas, geometryCacheOnly);
    } else {
      result = onSceneObjectAdded(ctx, drawCallState, pBlas, geometryCacheOnly);
    }

    // DX11_V332_IDENTITY_CHURN: this is the one place that decides whether a
    // draw is RECOGNISED as an object seen before, and it is the question the
    // building flicker turns on. Counting instances or cache entries cannot
    // answer it - a population of 125 is equally consistent with 125 objects
    // being reused and 125 objects being replaced wholesale every frame, and
    // that ambiguity has already produced one wrong conclusion in this journal.
    //
    // `existed` counts draws whose geometry identity matched something already
    // in the cache; `fresh` counts draws that minted a new identity. In a
    // static outdoor scene viewed from a moving camera, a healthy frame is
    // almost entirely `existed` + `updateInstance`. Sustained non-zero `fresh`
    // means identities are churning, and `buildBVH` then says that churn is
    // paying for a full BLAS rebuild each time.
    // DX11_V350_DRAW_TRACE: the scene-side half of the per-draw row. This is
    // where the cache decision is made, and the D3D11 bridge cannot observe it.
    // Joined to the bridge's row by drawCallID.
    if ((kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u)) {
      const char* stateName =
        result == ObjectCacheState::KBuildBVH ? "buildBVH"
        : result == ObjectCacheState::kUpdateBVH ? "updateBVH"
        : result == ObjectCacheState::kUpdateInstance ? "updateInst" : "invalid";
      Logger::info(str::format(
        "[SceneManager][trace-scene] f=", m_device->getCurrentFrameId(),
        " id=", drawCallState.drawCallID,
        " existed=", cacheExisted ? 1 : 0,
        " state=", stateName,
        " vtxHash=0x", std::hex,
        drawCallState.getGeometryData().hashes[HashComponents::VertexPosition],
        " idxHash=0x",
        drawCallState.getGeometryData().hashes[HashComponents::Indices], std::dec,
        " blas=", pBlas != nullptr ? 1 : 0,
        " smoothNormals=",
        drawCallState.categories.test(InstanceCategories::SmoothNormals) ? 1 : 0));
    }

    if (kenshi_telemetry::enabled()) {
    if (cacheExisted)
      s_frameCacheExisted.fetch_add(1u, std::memory_order_relaxed);
    else
      s_frameCacheFresh.fetch_add(1u, std::memory_order_relaxed);
    switch (result) {
      case ObjectCacheState::KBuildBVH:
        s_frameBuildBvh.fetch_add(1u, std::memory_order_relaxed); break;
      case ObjectCacheState::kUpdateBVH:
        s_frameUpdateBvh.fetch_add(1u, std::memory_order_relaxed); break;
      case ObjectCacheState::kUpdateInstance:
        s_frameUpdateInstance.fetch_add(1u, std::memory_order_relaxed); break;
      default: break;
    }
    }
    
    assert(pBlas != nullptr);
    terrain_profile::count(result == ObjectCacheState::KBuildBVH ? terrain_profile::MeshBuild
      : result == ObjectCacheState::kUpdateBVH ? terrain_profile::MeshUpdate : terrain_profile::InstanceOnly);
    if (terrain_profile::enabled()) {
      const bool skinned = drawCallState.getSkinningState().numBones > 0;
      const bool particle = drawCallState.getCategoryFlags().test(InstanceCategories::Particle);
      terrain_profile::geometryReason(skinned, particle, terrain_profile::GeometryReason::Draw);
      if (result == ObjectCacheState::kUpdateBVH)
        terrain_profile::geometryReason(skinned, particle, terrain_profile::GeometryReason::Update);
      else if (result == ObjectCacheState::kUpdateInstance)
        terrain_profile::geometryReason(skinned, particle, terrain_profile::GeometryReason::Instance);
    }
    assert(result != ObjectCacheState::kInvalid);

    // Update the input state, so we always have a reference to the original draw call state
    pBlas->frameLastTouched = m_device->getCurrentFrameId();

    const CategoryFlags categories = drawCallState.getCategoryFlags();
    const bool rigidWorldInstanceOnly = geometryCacheOnly
      && result != ObjectCacheState::kUpdateBVH
      && renderMaterialData.getType() == MaterialDataType::Opaque
      && !drawCallState.getMaterialData().blendMode.enableBlending
      && drawCallState.getSkinningState().numBones == 0
      && !drawCallState.getTransformData().cameraRelativeView
      && !categories.test(InstanceCategories::WorldUI)
      && !categories.test(InstanceCategories::WorldMatte)
      && !categories.test(InstanceCategories::Sky)
      && !categories.test(InstanceCategories::Particle)
      && !categories.test(InstanceCategories::Beam)
      && !categories.test(InstanceCategories::DecalStatic)
      && !categories.test(InstanceCategories::DecalDynamic)
      && !categories.test(InstanceCategories::DecalSingleOffset)
      && !categories.test(InstanceCategories::DecalNoOffset)
      && !categories.test(InstanceCategories::AnimatedWater)
      && !categories.test(InstanceCategories::ThirdPersonPlayerModel)
      && !categories.test(InstanceCategories::ThirdPersonPlayerBody)
      && !categories.test(InstanceCategories::ParticleEmitter)
      && !categories.test(InstanceCategories::SmoothNormals);

    if (geometryCacheOnly && !rigidWorldInstanceOnly) {
      return nullptr;
    }

    if (rigidWorldInstanceOnly) {
      ONCE(KENSHI_DIAGNOSTIC_INFO("[D3D11SceneManager] Rigid opaque world-instance submission active; RTX composite suppressed."));
    }

    // Generate smooth normals for geometry that is flagged via the SmoothNormals texture category.
    // This is useful for older D3D11 games where geometry may lack smooth normals, especially
    // when using the VertexShader Capture mechanism. The smooth normals are computed on the GPU
    // from the triangle mesh (area-weighted) and written into the normal buffer.
    // Only dispatch on BVH build/update — for static geometry, positions don't change so
    // the normals computed on the first pass remain valid for subsequent frames.
    if (!rigidWorldInstanceOnly && drawCallState.categories.test(InstanceCategories::SmoothNormals) &&
        (result == ObjectCacheState::KBuildBVH || result == ObjectCacheState::kUpdateBVH)) {
      m_device->getCommon()->metaGeometryUtils().dispatchSmoothNormals(ctx, drawCallState.getGeometryData(), pBlas->modifiedGeometryData);
      pBlas->modifiedGeometryData.smoothNormalsApplied = true;
      pBlas->frameLastUpdated = pBlas->frameLastTouched;
      m_instanceManager.notifySceneChanged();
    }

    if (!rigidWorldInstanceOnly && drawCallState.getSkinningState().numBones > 0 &&
        drawCallState.getGeometryData().numBonesPerVertex > 0 &&
        (result == ObjectCacheState::KBuildBVH || result == ObjectCacheState::kUpdateBVH)) {
      m_device->getCommon()->metaGeometryUtils().dispatchSkinning(ctx, drawCallState, pBlas->modifiedGeometryData);
      pBlas->frameLastUpdated = pBlas->frameLastTouched;
      m_instanceManager.notifySceneChanged();
    }

    // Note: The material data can be modified in instance manager
    RtInstance* instance = m_instanceManager.processSceneObject(m_cameraManager, m_rayPortalManager, *pBlas, drawCallState, renderMaterialData, existingInstance);

    // DX11_V324_CAPTURED_GEOMETRY_TRACE: post-VS captured geometry from the
    // world-space path reaches this point with verified-valid world vertices
    // and a camera that exactly matches the game's, yet produces no ray hits.
    // Capture and camera are therefore exonerated and the failure lies at or
    // below this boundary, where nothing was previously observable. Record
    // what the scene manager actually did with such a draw: whether the BLAS
    // was built/updated or merely reused, and whether an RtInstance was
    // produced at all. Bounded and logging-only.
    // Restricted to the WORLD-space capture path. The first attempt logged any
    // captured draw and its whole budget was spent on SV_Position captures at
    // startup, yielding zero records for the path under investigation.
    if (drawCallState.getGeometryData().postVsCaptureIdentity != kEmptyHash
     && !drawCallState.getGeometryData().postVsPositionIsHomogeneousClip) {
      static uint32_t s_capturedGeometryTraceCount = 0;
      if (s_capturedGeometryTraceCount < 24u) {
        ++s_capturedGeometryTraceCount;
        const auto& traceGeo = drawCallState.getGeometryData();
        const auto& traceTransform = drawCallState.getTransformData();
        const char* cacheStateName = "unknown";
        switch (result) {
          case ObjectCacheState::KBuildBVH:     cacheStateName = "buildBVH"; break;
          case ObjectCacheState::kUpdateBVH:    cacheStateName = "updateBVH"; break;
          case ObjectCacheState::kUpdateInstance: cacheStateName = "updateInstance"; break;
          case ObjectCacheState::kInvalid:      cacheStateName = "invalid"; break;
        }
        // An instance existing is not the same as an instance being traced:
        // a hidden instance takes mask 0 and produces no surface, which looks
        // exactly like missing geometry. Sky classification alone forces that
        // (rtx_instance_manager.cpp:1018), and with skyMode=PhysicalAtmosphere
        // sky draws are dropped entirely, so record the camera type too.
        const char* cameraTypeName = "other";
        switch (drawCallState.cameraType) {
          case CameraType::Main:            cameraTypeName = "Main"; break;
          case CameraType::Sky:             cameraTypeName = "Sky"; break;
          case CameraType::ViewModel:       cameraTypeName = "ViewModel"; break;
          case CameraType::RenderToTexture: cameraTypeName = "RenderToTexture"; break;
          case CameraType::Unknown:         cameraTypeName = "Unknown"; break;
          default: break;
        }

        Logger::warn(str::format(
          "[RTX][captured-geometry] state=", cacheStateName,
          " instance=", instance != nullptr ? 1 : 0,
          " hidden=", instance != nullptr && instance->isHidden() ? 1 : 0,
          " mask=", instance != nullptr
            ? uint32_t(instance->getVkInstance().mask) : 0u,
          " cameraType=", cameraTypeName,
          " rigidWorldOnly=", rigidWorldInstanceOnly ? 1 : 0,
          " homogeneousClip=", traceGeo.postVsPositionIsHomogeneousClip ? 1 : 0,
          " dynamicPositions=", traceGeo.postVsCapturedPositionsDynamic ? 1 : 0,
          " vertexCount=", traceGeo.vertexCount,
          " indexCount=", traceGeo.indexCount,
          " blasVertexCount=", pBlas->modifiedGeometryData.vertexCount,
          " blasIndexCount=", pBlas->modifiedGeometryData.indexCount,
          " blasPositionDefined=", pBlas->modifiedGeometryData.positionBuffer.defined() ? 1 : 0,
          " o2wT=[", traceTransform.objectToWorld[3][0], ",",
                     traceTransform.objectToWorld[3][1], ",",
                     traceTransform.objectToWorld[3][2], "]",
          " materialIgnored=", renderMaterialData.getIgnored() ? 1 : 0));

        // The BLAS interleave probe that lived here has served its purpose and
        // was REMOVED: it read back the interleaved positions and found them
        // identical to the capture buffer (valid world coordinates, stride 32
        // = position + normal + UV). The interleaver is therefore correct.
        //
        // It was also actively harmful as a permanent fixture. It called
        // flushCommandList() and blocked mid-frame inside scene processing -
        // the same class of mid-frame flush that previously made ALL captured
        // geometry disappear when the capture submission throttle engaged.
        // Any probe that disturbs frame submission cannot be trusted to
        // measure a rendering symptom. Diagnostics of this kind must be
        // temporary and must never be the state a visual result is judged in.
      }
    }

    // Check if a light should be created for this Material
    if (!rigidWorldInstanceOnly && instance && RtxOptions::shouldConvertToLight(drawCallState.getMaterialData().getHash())) {
      createEffectLight(ctx, drawCallState, instance);
    }

    const bool objectPickingActive = m_device->getCommon()->getResources().getRaytracingOutput()
      .m_primaryObjectPicking.isValid();
    if (kenshi_material_probe::active(m_device->getCurrentFrameId()) && instance) {
      if (kenshi_material_probe::sceneRows++ < 8192u) {
        const auto& material = drawCallState.getMaterialData();
        KENSHI_DIAGNOSTIC_INFO(str::format("[MaterialProbe] scene ticket=", kenshi_material_probe::ticket,
          " frame=", m_device->getCurrentFrameId(), " draw=", drawCallState.drawCallID,
          " picking=", instance->surface.objectPickingValue,
          " albedoKey=", material.getColorTexture().getUniqueKey(),
          " materialHash=", material.getHash(), " textured=", material.usesTexture(),
          " texgen=", uint32_t(drawCallState.getTransformData().texgenMode),
          " uvBuffer=", drawCallState.getGeometryData().texcoordBuffer.defined()));
      } else ++kenshi_material_probe::sceneSuppressed;
    }

    if (objectPickingActive && instance && g_allowMappingLegacyHashToObjectPickingValue) {
      auto meta = DrawCallMetaInfo {};
      {
        XXH64_hash_t h;
        h = drawCallState.getMaterialData().getColorTexture().getImageHash();
        // Texture-less (constant-color) materials have no albedo texture; fall back to
        // the material hash so clicking the surface resolves to its texture-UI entry.
        if (h == kEmptyHash) {
          h = drawCallState.getMaterialData().getHash();
        }
        if (h != kEmptyHash) {
          meta.legacyTextureHash = h;
        }
        h = drawCallState.getMaterialData().getColorTexture2().getImageHash();
        if (h != kEmptyHash) {
          meta.legacyTextureHash2 = h;
        }
      }

      {
        std::lock_guard lock { m_drawCallMeta.mutex };
        auto [iter, isNew] = m_drawCallMeta.infos[m_drawCallMeta.ticker].emplace(instance->surface.objectPickingValue, meta);
        ONCE_IF_FALSE(isNew, Logger::warn(
          "Found multiple draw calls with the same \'objectPickingValue\'. "
          "Ignoring further MetaInfo-s, some objects might be not be available through object picking"));
      }
    }

    // Priority ordering for particle system descriptors is: Mesh, Material, Texture.  This matches the implementation in toolkit.
    // By this point, pParticleSystemDesc will contain the information from a mesh replacement (if one exists), so we just handle
    // materials replacements, and texture taggin categories below.
    RtxParticleSystemDesc globalParticleDesc; // Storage for global desc if needed
    if (!pParticleSystemDesc) {
      pParticleSystemDesc = renderMaterialData.getParticleSystemDesc();
    }
    if (!pParticleSystemDesc && drawCallState.categories.test(InstanceCategories::ParticleEmitter)) {
      globalParticleDesc = RtxParticleSystemManager::createGlobalParticleSystemDesc();
      pParticleSystemDesc = &globalParticleDesc;
    }
    if (!rigidWorldInstanceOnly && instance && pParticleSystemDesc) {
      RtxParticleSystemManager& particleSystem = device()->getCommon()->metaParticleSystem();
      particleSystem.spawnParticles(ctx.ptr(), *pParticleSystemDesc, instance->getVectorIdx(), drawCallState, renderMaterialData);

      if (pParticleSystemDesc->hideEmitter) {
        instance->setHidden(true);
      }
    }

    return instance; 
  }

  const RtSurfaceMaterial& SceneManager::createSurfaceMaterial(const MaterialData& renderMaterialData,
                                                               const DrawCallState& drawCallState,
                                                               uint32_t* out_indexInCache) {
    ScopedCpuProfileZone();
    const bool hasTexcoords = drawCallState.hasTextureCoordinates();
    const auto renderMaterialDataType = renderMaterialData.getType();

    // We're going to use this to create a modified sampler for replacement textures.
    // Legacy and replacement materials should follow same filtering but due to lack of override capability per texture
    // legacy textures use original sampler to stay true to the original intent while replacements use more advanced filtering
    // for better quality by default.
    const Rc<DxvkSampler>& samplerOverride = renderMaterialData.getSamplerOverride();
    Rc<DxvkSampler> sampler = samplerOverride;
    // If the original sampler if valid and there isnt an override sampler
    // go ahead with patching and maybe merging the sampler states
    if (samplerOverride == nullptr && drawCallState.getMaterialData().getSampler().ptr() != nullptr) {
      DxvkSamplerCreateInfo samplerInfo = drawCallState.getMaterialData().getSampler()->info(); // Use sampler create info struct as convenience
      renderMaterialData.populateSamplerInfo(samplerInfo);

      sampler = patchSampler(samplerInfo.magFilter,
                             samplerInfo.addressModeU, samplerInfo.addressModeV, samplerInfo.addressModeW,
                             samplerInfo.borderColor);
    }
    if (drawCallState.isEye()) {
      // force eye whites and iris to not repeat
      sampler = patchSampler(
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        {}
      );
    }
    uint32_t samplerIndex = trackSampler(sampler);
    uint32_t samplerIndex2 = UINT32_MAX;
    if (renderMaterialDataType == MaterialDataType::RayPortal) {
      samplerIndex2 = trackSampler(drawCallState.getMaterialData().getSampler2());
    }

    // DX11_V763: carried to the cache-tracking site at the end of this function,
    // which is the only place that can say whether a changed progress produced a
    // NEW surface material or was collapsed onto an existing one. Declared here
    // because that site is shared by every material type.
    float kenshiConstructionLogState = -1.0f;
    uint32_t kenshiConstructionLogGrid = kSurfaceMaterialInvalidTextureIndex;

    XXH64_hash_t preCreationHash = renderMaterialData.getHash();
    const bool secondaryOpacity = renderMaterialData.getUseSecondaryTextureForOpacity();
    preCreationHash = XXH64(&secondaryOpacity, sizeof(secondaryOpacity), preCreationHash);
    const bool kenshiTerrain = renderMaterialData.getKenshiTerrainBlend();
    preCreationHash = XXH64(&kenshiTerrain, sizeof(kenshiTerrain), preCreationHash);
    const bool kenshiCharacterHead = renderMaterialData.getKenshiCharacterHead();
    preCreationHash = XXH64(&kenshiCharacterHead, sizeof(kenshiCharacterHead), preCreationHash);
    const auto hashKenshiCharacterTexture = [&](const TextureRef& texture) {
      const XXH64_hash_t imageHash = texture.getImageHash();
      preCreationHash = XXH64(&imageHash, sizeof(imageHash), preCreationHash);
      if (texture.isValid() && imageHash == 0u) {
        const uint64_t runtimeTextureKey = static_cast<uint64_t>(texture.getUniqueKey());
        preCreationHash = XXH64(&runtimeTextureKey, sizeof(runtimeTextureKey), preCreationHash);
      }
    };
    hashKenshiCharacterTexture(renderMaterialData.getKenshiCharacterBodyMaskTexture());
    hashKenshiCharacterTexture(renderMaterialData.getKenshiCharacterHeadMaskTexture());
    hashKenshiCharacterTexture(renderMaterialData.getKenshiCharacterHairTexture());
    hashKenshiCharacterTexture(renderMaterialData.getKenshiCharacterBeardTexture());
    // V724: armor recolor constants also live outside the generic variant.
    // The usual mask is already the secondary texture, but include the actual
    // extension texture here so the key follows what surface creation consumes.
    const bool kenshiColorMask = renderMaterialData.getKenshiColorMask();
    preCreationHash = XXH64(&kenshiColorMask, sizeof(kenshiColorMask), preCreationHash);
    if (kenshiColorMask) {
      hashKenshiCharacterTexture(renderMaterialData.getKenshiColorMaskTexture());
      const Vector4& color1 = renderMaterialData.getKenshiColor1();
      const Vector4& color2 = renderMaterialData.getKenshiColor2();
      const float colors[] = { color1.x, color1.y, color1.z, color1.w,
                               color2.x, color2.y, color2.z, color2.w };
      preCreationHash = XXH64(colors, sizeof(colors), preCreationHash);
    }
    // V723: clothing is stored outside the generic material variant, so neither
    // getHash() nor forEachTexture() includes it. Separate clothing before this
    // cache can return another character's already-created surface material.
    const bool kenshiCharacterVest = renderMaterialData.getKenshiCharacterVest();
    preCreationHash = XXH64(&kenshiCharacterVest, sizeof(kenshiCharacterVest), preCreationHash);
    if (kenshiCharacterVest) {
      hashKenshiCharacterTexture(renderMaterialData.getKenshiVestDiffuseTexture());
      hashKenshiCharacterTexture(renderMaterialData.getKenshiVestNormalTexture());
      hashKenshiCharacterTexture(renderMaterialData.getKenshiVestMaskTexture());
      const Vector4& vestColor = renderMaterialData.getKenshiVestColor();
      const float colorComponents[] = { vestColor.x, vestColor.y, vestColor.z, vestColor.w };
      preCreationHash = XXH64(colorComponents, sizeof(colorComponents), preCreationHash);
    }
    const uint16_t kenshiCharacterHairChannels =
      renderMaterialData.getKenshiCharacterHairChannels();
    preCreationHash = XXH64(&kenshiCharacterHairChannels,
      sizeof(kenshiCharacterHairChannels), preCreationHash);
    // DX11_V763: build progress, for exactly the reason V723 gives for clothing
    // and V724 for the armour recolour - the construction contract is stored
    // OUTSIDE the generic material variant, so neither getHash() nor
    // forEachTexture() includes it, and without it this cache hands one
    // building's already-created surface material to another. Two buildings of
    // the same type at different progress share every other term of this key,
    // which is why placing a second one showed it at the first one's progress.
    const bool kenshiConstructionMaterial = renderMaterialData.getKenshiConstruction();
    preCreationHash = XXH64(&kenshiConstructionMaterial,
      sizeof(kenshiConstructionMaterial), preCreationHash);
    if (kenshiConstructionMaterial) {
      hashKenshiCharacterTexture(renderMaterialData.getKenshiConstructionGridTexture());
      const Vector4& constructionKey = renderMaterialData.getKenshiConstructionParams();
      const float constructionComponents[] = {
        constructionKey.x, constructionKey.y, constructionKey.z, constructionKey.w };
      preCreationHash = XXH64(constructionComponents,
        sizeof(constructionComponents), preCreationHash);
    }
    // Two terrain materials can share every texture the material itself holds
    // and still belong to different parameter sets, so the set must separate
    // them or one would be cached under the other's identity.
    const uint32_t kenshiTerrainSet = renderMaterialData.getKenshiTerrainSetIndex();
    preCreationHash = XXH64(&kenshiTerrainSet, sizeof(kenshiTerrainSet), preCreationHash);
    preCreationHash = XXH64(&samplerIndex, sizeof(samplerIndex), preCreationHash);
    preCreationHash = XXH64(&samplerIndex2, sizeof(samplerIndex2), preCreationHash);
    preCreationHash = XXH64(&hasTexcoords, sizeof(hasTexcoords), preCreationHash);
    preCreationHash = XXH64(&drawCallState.isUsingRaytracedRenderTarget, sizeof(drawCallState.isUsingRaytracedRenderTarget), preCreationHash);

    // Runtime-created textures can have no content hash. In that case
    // MaterialData cannot distinguish them, and this cache may return another
    // draw's surface material before trackTexture sees the actual TextureRef.
    // This cache is session-local, so runtime keys are the correct fallback
    // identity for every hashless material, without requiring a Particle tag.
    uint32_t textureSlot = 0u;
    renderMaterialData.forEachTexture([&](const TextureRef& texture) {
      if (texture.isValid() && texture.getImageHash() == 0u) {
        const uint64_t runtimeTextureKey = static_cast<uint64_t>(texture.getUniqueKey());
        preCreationHash = XXH64(&textureSlot, sizeof(textureSlot), preCreationHash);
        preCreationHash = XXH64(&runtimeTextureKey, sizeof(runtimeTextureKey), preCreationHash);
      }
      ++textureSlot;
    });

    auto iter = m_preCreationSurfaceMaterialMap.find(preCreationHash);
    if (iter != m_preCreationSurfaceMaterialMap.end()) {
      if (out_indexInCache) {
        *out_indexInCache = iter->second;
      }
      return m_surfaceMaterialCache.at(iter->second);
    }

    std::optional<RtSurfaceMaterial> surfaceMaterial;

    if (renderMaterialDataType == MaterialDataType::Opaque || drawCallState.isUsingRaytracedRenderTarget) {
      uint32_t albedoOpacityTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t secondaryTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t normalTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t tangentTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t heightTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t roughnessTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t metallicTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t emissiveColorTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t kenshiCharacterBodyMaskTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t kenshiCharacterHeadMaskTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t kenshiCharacterHairTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t kenshiCharacterBeardTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t subsurfaceMaterialIndex = SURFACE_INDEX_INVALID;
      uint32_t subsurfaceTransmittanceTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t subsurfaceThicknessTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      uint32_t subsurfaceSingleScatteringAlbedoTextureIndex = kSurfaceMaterialInvalidTextureIndex;

      float anisotropy;
      float emissiveIntensity;
      Vector4 albedoOpacityConstant;
      float roughnessConstant;
      float metallicConstant;
      Vector3 emissiveColorConstant;
      bool enableEmissive;
      bool thinFilmEnable = false;
      bool alphaIsThinFilmThickness = false;
      float thinFilmThicknessConstant = 0.0f;
      float displaceIn = 0.0f;
      float displaceOut = 0.0f;
      bool isUsingRaytracedRenderTarget = drawCallState.isUsingRaytracedRenderTarget;
      uint16_t samplerFeedbackStamp = SAMPLER_FEEDBACK_INVALID;

      Vector3 subsurfaceTransmittanceColor(0.0f, 0.0f, 0.0f);
      float subsurfaceMeasurementDistance = 0.0f;
      Vector3 subsurfaceSingleScatteringAlbedo(0.0f, 0.0f, 0.0f);
      float subsurfaceVolumetricAnisotropy = 0.0f;

      float subsurfaceRadiusScale = 0.0f;
      float subsurfaceMaxSampleRadius = 0.0f;

      bool ignoreAlphaChannel = false;
      bool useSecondaryTextureForOpacity = false;
      bool kenshiTerrainBlend = false;
      bool kenshiCharacterHead = false;
      constexpr Vector4 kWhiteModeAlbedo = Vector4(0.7f, 0.7f, 0.7f, 1.0f);

      const auto& opaqueMaterialData = renderMaterialData.getOpaqueMaterialData();

      if (RtxOptions::useWhiteMaterialMode()) {
        albedoOpacityConstant = kWhiteModeAlbedo;
        metallicConstant = 0.f;
        roughnessConstant = 1.f;
      } else {
        if (opaqueMaterialData.getAlbedoOpacityTexture().getManagedTexture() != nullptr) {
          samplerFeedbackStamp = opaqueMaterialData.getAlbedoOpacityTexture().getManagedTexture()->m_samplerFeedbackStamp;
        }

        trackTexture(opaqueMaterialData.getAlbedoOpacityTexture(), albedoOpacityTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(opaqueMaterialData.getRoughnessTexture(), roughnessTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(opaqueMaterialData.getMetallicTexture(), metallicTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(opaqueMaterialData.getSecondaryTexture(), secondaryTextureIndex, hasTexcoords, true, samplerFeedbackStamp);

        albedoOpacityConstant.xyz() = opaqueMaterialData.getAlbedoConstant();
        albedoOpacityConstant.w = opaqueMaterialData.getOpacityConstant();
        metallicConstant = opaqueMaterialData.getMetallicConstant();
        roughnessConstant = opaqueMaterialData.getRoughnessConstant();
      }

      trackTexture(opaqueMaterialData.getNormalTexture(), normalTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      trackTexture(opaqueMaterialData.getTangentTexture(), tangentTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      // DX11_V491_KENSHI_HEAD_NORMAL: a Kenshi character's head sits in NEGATIVE
      // V of the same draw and has its own normal map, so the body's map reads
      // unrelated texels there. Carry the head map in the TANGENT slot: the GPU
      // material struct is exactly full at kSurfaceMaterialGPUSize, and the
      // bridge never supplies a tangent texture for any Kenshi material (Remix
      // derives its tangent frame from UVs). Only for head materials, so a USD
      // replacement with a real tangent map keeps it. The shader reads it back
      // under the same flag and does not treat it as a tangent map.
      if (renderMaterialData.getKenshiCharacterHead()
       && renderMaterialData.getKenshiCharacterHeadNormalTexture().isValid()) {
        trackTexture(renderMaterialData.getKenshiCharacterHeadNormalTexture(),
                     tangentTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      }
      trackTexture(opaqueMaterialData.getHeightTexture(), heightTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      // DX11_V766_KENSHI_MUSCLE_NORMAL: character.hlsl's second body normal map,
      // lerped with the first by `muscleBlend` on the raw DXT5nm pair. It takes
      // heightTextureIndex, dead on every Kenshi material because this bridge
      // supplies no height map and never sets HAS_DISPLACEMENT - and the GPU
      // record is exactly full at kSurfaceMaterialGPUSize. The muscle amount in
      // the hair-channel word is what proves this is the Kenshi case rather than
      // a replacement's real height map; writeGPUData is exempted on the same
      // test so the POM-disable branch cannot wipe the index.
      if ((renderMaterialData.getKenshiCharacterHairChannels() >> 10u) != 0u
       && renderMaterialData.getKenshiMuscleBlendTexture().isValid()) {
        trackTexture(renderMaterialData.getKenshiMuscleBlendTexture(),
                     heightTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      }
      trackTexture(opaqueMaterialData.getEmissiveColorTexture(), emissiveColorTextureIndex, hasTexcoords, true, samplerFeedbackStamp);

      emissiveIntensity = opaqueMaterialData.getEmissiveIntensity() * RtxOptions::emissiveIntensity();
      emissiveColorConstant = opaqueMaterialData.getEmissiveColorConstant();
      enableEmissive = opaqueMaterialData.getEnableEmission();
      anisotropy = opaqueMaterialData.getAnisotropyConstant();
        
      thinFilmEnable = opaqueMaterialData.getEnableThinFilm();
      alphaIsThinFilmThickness = opaqueMaterialData.getAlphaIsThinFilmThickness();
      thinFilmThicknessConstant = opaqueMaterialData.getThinFilmThicknessConstant();
      displaceIn = opaqueMaterialData.getDisplaceIn();
      displaceOut = opaqueMaterialData.getDisplaceOut();

      ignoreAlphaChannel = opaqueMaterialData.getIgnoreAlphaChannel();
      useSecondaryTextureForOpacity = renderMaterialData.getUseSecondaryTextureForOpacity();
      kenshiTerrainBlend = renderMaterialData.getKenshiTerrainBlend();
      kenshiCharacterHead = renderMaterialData.getKenshiCharacterHead();

      // DX11_V552_KENSHI_COLOR_MASK: the two recolour colours ride in constants
      // that are dead on a textured, non-emissive material - albedoOpacityConstant
      // is only the fallback when no albedo texture loads, and the emissive pair
      // does nothing while emission is off. Emission is forced off here so the
      // aliased fields cannot also be read as light.
      // DX11_V554_KENSHI_CHARACTER_VEST: three indices a character material
      // never uses. Kenshi's character shader binds no metal, roughness or
      // emissive map at all, so on a legacy-captured character these are all
      // BINDING_INDEX_INVALID and free; the clothing colour goes in
      // albedoOpacityConstant, dead once an albedo texture loads.
      // DX11_V557: the dust noise, in tangentTextureIndex. Tracked HERE, inside
      // determineMaterialData - during scene building and before
      // m_bindlessResourceManager.prepareSceneData publishes the descriptor
      // table. That ordering is the V527 rule and is what makes the index safe
      // to sample; reserving one at binding time is what produces the
      // out-of-VRAM box that is really a device loss.
      // DX11_V571: write the COLOUR whenever the material HAS one, and bind the
      // noise separately. These two were gated on the same condition and must
      // not be - that mismatch is what made dust paint WHITE.
      //
      // V568 made the capture record a dust colour whenever the constants read,
      // including the many load-time draws where the noise texture is not bound
      // yet (measured: 250 consecutive draws with noise=0). Those materials were
      // hashed as dust materials by V565 but skipped this write, so they kept
      // `albedoOpacityConstant` at its DEFAULT - opaque WHITE. Dust then
      // accumulates over the following seconds, the noise binds, the hash still
      // matches, and the cached WHITE material is reused: dust renders white
      // however correct the colour on the capture side is.
      //
      // The colour is a property of the material; the noise is a binding. Gate
      // each on its own condition. DX11_V573 makes W an explicit presence plus
      // amount-component selector rather than a dynamic amount, which may
      // legitimately be zero during loading.
      const bool kenshiHasDustColour =
           renderMaterialData.getKenshiDustColour().w > 0.0f
        && !renderMaterialData.getKenshiCharacterVest()
        && !RtxOptions::useWhiteMaterialMode();
      if (kenshiHasDustColour) {
        albedoOpacityConstant = renderMaterialData.getKenshiDustColour();
      }

      if (renderMaterialData.getKenshiDustNoiseTexture().isValid()
       && !renderMaterialData.getKenshiCharacterVest()
       && !RtxOptions::useWhiteMaterialMode()) {
        trackTexture(renderMaterialData.getKenshiDustNoiseTexture(),
          tangentTextureIndex, hasTexcoords, true, samplerFeedbackStamp);

        // DX11_V570: the last unverified link. The capture log proves the right
        // colour reaches the MATERIAL; this proves what reaches the SURFACE
        // MATERIAL the shader reads, which is the only step never measured.
        // One line per distinct surface-material colour, so it cannot flood.
        {
          static std::mutex sDustSurfMutex;
          static std::unordered_set<uint64_t> sDustSurfSeen;
          const Vector4& dc = albedoOpacityConstant;
          struct { float r, g, b, a; } k = { dc.x, dc.y, dc.z, dc.w };
          const uint64_t kh = XXH3_64bits(&k, sizeof(k));
          std::lock_guard<std::mutex> lock(sDustSurfMutex);
          if (sDustSurfSeen.size() < 40u && sDustSurfSeen.insert(kh).second) {
            char line[200];
            std::snprintf(line, sizeof(line),
              "[RTX Kenshi][dust-surface] albedoOpacityConstant=%.4f,%.4f,%.4f,%.4f "
              "tangentIdx=%u",
              dc.x, dc.y, dc.z, dc.w, tangentTextureIndex);
            KENSHI_DIAGNOSTIC_INFO(line);
          }
        }
      }

      if (renderMaterialData.getKenshiCharacterVest()
       && !RtxOptions::useWhiteMaterialMode()) {
        trackTexture(renderMaterialData.getKenshiVestDiffuseTexture(),
          metallicTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiVestNormalTexture(),
          roughnessTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiVestMaskTexture(),
          emissiveColorTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        albedoOpacityConstant = renderMaterialData.getKenshiVestColor();
        // DX11_V555: these three slots no longer hold what their names say, so
        // the ordinary consumers must be off on both sides. The shader gates its
        // texture reads on the same flag; this stops emission independently, the
        // way V552 does for the armour mask.
        enableEmissive = false;
      }

      // DX11_V760_KENSHI_CONSTRUCTION: a building that is still being built.
      // The scaffold lattice takes kenshiCharacterHairTextureIndex and the three
      // scalars take emissiveColorConstant - all four dead on a textured,
      // non-emissive building material, and the GPU record is exactly full at
      // kSurfaceMaterialGPUSize. See the flag in shared_constants.h.
      //
      // Emission is forced off for the same reason V552 forces it off: the
      // constant no longer means a colour, so letting the ordinary emissive path
      // read it would make every unfinished building glow with its own progress.
      const bool kenshiConstruction =
           renderMaterialData.getKenshiConstruction()
        && renderMaterialData.getKenshiConstructionGridTexture().isValid()
        && !RtxOptions::useWhiteMaterialMode();
      if (kenshiConstruction) {
        const Vector4& constructionParams = renderMaterialData.getKenshiConstructionParams();
        trackTexture(renderMaterialData.getKenshiConstructionGridTexture(),
          kenshiCharacterHairTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        emissiveColorConstant = Vector3(
          constructionParams.x, constructionParams.y, constructionParams.z);
        emissiveIntensity = 0.0f;
        enableEmissive = false;

        kenshiConstructionLogState = constructionParams.x;
        kenshiConstructionLogGrid = kenshiCharacterHairTextureIndex;
      }

      if (renderMaterialData.getKenshiColorMask()) {
        const Vector4& kenshiColor1 = renderMaterialData.getKenshiColor1();
        const Vector4& kenshiColor2 = renderMaterialData.getKenshiColor2();
        albedoOpacityConstant = kenshiColor1;
        emissiveColorConstant = Vector3(kenshiColor2.x, kenshiColor2.y, kenshiColor2.z);
        emissiveIntensity = kenshiColor2.w;
        enableEmissive = false;
        trackTexture(renderMaterialData.getKenshiColorMaskTexture(),
          secondaryTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      }
      // DX11_V551: a dual-set material reuses the first two character-mask
      // indices for its SECOND normal and metal maps. The two cases are
      // mutually exclusive - a character material never sets the dual flag -
      // so the same two slots serve both without a size increase.
      if (renderMaterialData.getKenshiDualTextureSet() && !kenshiCharacterHead
       && !RtxOptions::useWhiteMaterialMode()) {
        trackTexture(renderMaterialData.getKenshiDualNormalTexture(),
          kenshiCharacterBodyMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiDualMetalTexture(),
          kenshiCharacterHeadMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      }
      // DX11_V599_KENSHI_WATER_SCUM: water is the third mutually-exclusive user
      // of these three slots, on exactly the argument V551 makes above - a
      // character material is never water, and the GPU surface material has no
      // room to grow. Scum albedo, scum normal and the rain map ride here.
      if (drawCallState.testCategoryFlags(InstanceCategories::AnimatedWater)
       && !kenshiCharacterHead && !renderMaterialData.getKenshiDualTextureSet()
       && !RtxOptions::useWhiteMaterialMode()) {
        trackTexture(renderMaterialData.getKenshiCharacterBodyMaskTexture(),
          kenshiCharacterBodyMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiCharacterHeadMaskTexture(),
          kenshiCharacterHeadMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiCharacterHairTexture(),
          kenshiCharacterHairTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        // DX11_V620: the BEARD slot too, which V618 put the water blend map in
        // and then never tracked. The index stayed invalid, so the blend sample
        // never loaded, `waterBlendLoaded` was always false, and every hit fell
        // through to the BASE biome record. Only non-blended water materials
        // register into that record, so it raced last-draw-wins between them and
        // the whole surface flipped character with camera angle - the exact
        // symptom the biome table was built to remove, reintroduced by a missing
        // trackTexture call.
        trackTexture(renderMaterialData.getKenshiCharacterBeardTexture(),
          kenshiCharacterBeardTextureIndex, hasTexcoords, true, samplerFeedbackStamp);

        static uint32_t sWaterBlendTrackLogs = 0;
        if (sWaterBlendTrackLogs < 4u) {
          ++sWaterBlendTrackLogs;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[SceneManager][kenshi-water] blend=", kenshiCharacterBeardTextureIndex,
            kenshiCharacterBeardTextureIndex == kSurfaceMaterialInvalidTextureIndex
              ? " (INVALID - biome selection inert)" : " (valid)",
            " scum=", kenshiCharacterBodyMaskTextureIndex,
            " scumNormal=", kenshiCharacterHeadMaskTextureIndex,
            " rain=", kenshiCharacterHairTextureIndex));
        }
      }
      if (kenshiCharacterHead && !RtxOptions::useWhiteMaterialMode()) {
        trackTexture(renderMaterialData.getKenshiCharacterBodyMaskTexture(),
          kenshiCharacterBodyMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiCharacterHeadMaskTexture(),
          kenshiCharacterHeadMaskTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiCharacterHairTexture(),
          kenshiCharacterHairTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
        trackTexture(renderMaterialData.getKenshiCharacterBeardTexture(),
          kenshiCharacterBeardTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
      }
      if (renderMaterialData.getKenshiGlossInAlpha()) {
        // Bounded per SUBJECT: the roughness/metal indices actually written to
        // the surface material, which is the half V533 could not see.
        static uint32_t sGlossMaterialLogCount = 0;
        if (sGlossMaterialLogCount < 16u) {
          ++sGlossMaterialLogCount;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX Kenshi][gloss-material] glossMult=", roughnessConstant,
            " metalTex=", metallicTextureIndex,
            " metalConst=", metallicConstant,
            " albedoTex=", albedoOpacityTextureIndex,
            " hasTexcoords=", hasTexcoords ? 1 : 0));
        }
      }

      if (kenshiTerrainBlend) {
        static uint32_t sTerrainMaterialLogCount = 0;
        if (sTerrainMaterialLogCount < 24u) {
          ++sTerrainMaterialLogCount;
          KENSHI_DIAGNOSTIC_INFO(str::format(
            "[RTX Kenshi Terrain][material] set=",
            renderMaterialData.getKenshiTerrainSetIndex(),
            " albedoTex=", albedoOpacityTextureIndex,
            " priorSecondary=", secondaryTextureIndex,
            " hasTexcoords=", hasTexcoords ? 1 : 0));
        }
        // secondaryTextureIndex is free for terrain (the layer stack travels in
        // the parameter set, not in the material), and the flag makes the
        // reinterpretation unambiguous on the shader side.
        secondaryTextureIndex = renderMaterialData.getKenshiTerrainSetIndex();
      }

      subsurfaceMeasurementDistance = opaqueMaterialData.getSubsurfaceMeasurementDistance() * RtxOptions::SubsurfaceScattering::surfaceThicknessScale();

      const bool isSubsurfaceScatteringDiffusionProfile = opaqueMaterialData.getSubsurfaceDiffusionProfile();

      if ((RtxOptions::SubsurfaceScattering::enableThinOpaque()       && subsurfaceMeasurementDistance > 0.0f) ||
          (RtxOptions::SubsurfaceScattering::enableDiffusionProfile() && isSubsurfaceScatteringDiffusionProfile)) {

        subsurfaceTransmittanceColor = opaqueMaterialData.getSubsurfaceTransmittanceColor();
        subsurfaceVolumetricAnisotropy = opaqueMaterialData.getSubsurfaceVolumetricAnisotropy();

        if (isSubsurfaceScatteringDiffusionProfile) {
          // NOTE: reuse of the variable!
          subsurfaceSingleScatteringAlbedo = opaqueMaterialData.getSubsurfaceRadius(); 
          subsurfaceMaxSampleRadius = std::max(0.F, opaqueMaterialData.getSubsurfaceMaxSampleRadius());
          subsurfaceRadiusScale = std::max(opaqueMaterialData.getSubsurfaceRadiusScale(), 1e-5f);
          assert(subsurfaceRadiusScale > 0);

          m_sssMaterialExist = true;
        } else /* if thin opaque */ {
          assert(subsurfaceMeasurementDistance > 0);

          subsurfaceSingleScatteringAlbedo = opaqueMaterialData.getSubsurfaceSingleScatteringAlbedo();
          subsurfaceMaxSampleRadius = 0;
          subsurfaceRadiusScale = -1;
          assert(subsurfaceRadiusScale < 0);  // if < 0, then shaders assume that
                                              // this material is not SubsurfaceScatter, but just SingleScatter
                                              // same here, but <0.F

          m_thinOpaqueMaterialExist = true;
        }

        if (RtxOptions::SubsurfaceScattering::enableTextureMaps()) {
          trackTexture(opaqueMaterialData.getSubsurfaceTransmittanceTexture(), subsurfaceTransmittanceTextureIndex, hasTexcoords, true, samplerFeedbackStamp);

          if (isSubsurfaceScatteringDiffusionProfile) {
            // NOTE: reuse of 'subsurfaceSingleScatteringAlbedoTextureIndex' variable!
            trackTexture(opaqueMaterialData.getSubsurfaceRadiusTexture(), subsurfaceSingleScatteringAlbedoTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
          } else {
            trackTexture(opaqueMaterialData.getSubsurfaceSingleScatteringAlbedoTexture(), subsurfaceSingleScatteringAlbedoTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
            trackTexture(opaqueMaterialData.getSubsurfaceThicknessTexture(), subsurfaceThicknessTextureIndex, hasTexcoords, true, samplerFeedbackStamp);
          }
        }

        const auto subsurfaceMaterial = RtSubsurfaceMaterial{
          subsurfaceTransmittanceTextureIndex,
          subsurfaceThicknessTextureIndex,
          subsurfaceSingleScatteringAlbedoTextureIndex,
          subsurfaceTransmittanceColor,
          subsurfaceMeasurementDistance,
          subsurfaceSingleScatteringAlbedo,
          subsurfaceVolumetricAnisotropy,
          subsurfaceRadiusScale,
          subsurfaceMaxSampleRadius,
        };
        subsurfaceMaterialIndex = m_surfaceMaterialExtensionCache.track(subsurfaceMaterial);
      }

      const RtOpaqueSurfaceMaterial opaqueSurfaceMaterial{
        albedoOpacityTextureIndex, normalTextureIndex,
        tangentTextureIndex, heightTextureIndex, roughnessTextureIndex,
        metallicTextureIndex, emissiveColorTextureIndex,
        anisotropy, emissiveIntensity,
        albedoOpacityConstant,
        roughnessConstant, metallicConstant,
        emissiveColorConstant, enableEmissive,
        ignoreAlphaChannel, thinFilmEnable, alphaIsThinFilmThickness,
        thinFilmThicknessConstant, samplerIndex, displaceIn, displaceOut, 
        subsurfaceMaterialIndex, isUsingRaytracedRenderTarget,
        samplerFeedbackStamp,
        secondaryTextureIndex,
        useSecondaryTextureForOpacity,
        kenshiTerrainBlend,
        kenshiCharacterHead,
        kenshiCharacterBodyMaskTextureIndex,
        kenshiCharacterHeadMaskTextureIndex,
        kenshiCharacterHairTextureIndex,
        kenshiCharacterBeardTextureIndex,
        renderMaterialData.getKenshiCharacterHairChannels(),
        // DX11_V490: only meaningful when a normal texture is present; 0 keeps
        // Remix's octahedral decode for USD replacements.
        renderMaterialData.getKenshiNormalEncoding(),
        // DX11_V534: this was MISSING in V533. The constructor parameter has a
        // default, so the omission compiled silently and the flag was never set
        // on any material - the entire object-gloss shader path was unreachable.
        // Any new argument here needs adding at BOTH ends.
        renderMaterialData.getKenshiGlossInAlpha(),
        // DX11_V550. Added at BOTH ends in the same edit - see the V534 note
        // immediately above, which is exactly the trap this comment guards.
        renderMaterialData.getKenshiDualTextureSet(),
        // DX11_V552. Both ends, same edit - see the V534 note above.
        renderMaterialData.getKenshiColorMask(),
        // DX11_V554. Both ends, same edit - see the V534 note above.
        renderMaterialData.getKenshiCharacterVest()
      };

      if (opaqueSurfaceMaterial.hasValidDisplacement()) {
        ++m_activePOMCount;
      }

      surfaceMaterial.emplace(opaqueSurfaceMaterial);
    } else if (renderMaterialDataType == MaterialDataType::Translucent) {
      surfaceMaterial.emplace(createTranslucentSurfaceMaterial(renderMaterialData.getTranslucentMaterialData(), samplerIndex, hasTexcoords));
    } else if (renderMaterialDataType == MaterialDataType::RayPortal) {
      const auto& rayPortalMaterialData = renderMaterialData.getRayPortalMaterialData();

      uint32_t maskTextureIndex = kSurfaceMaterialInvalidTextureIndex;
      trackTexture(rayPortalMaterialData.getMaskTexture(), maskTextureIndex, hasTexcoords, false);
      uint32_t maskTextureIndex2 = kSurfaceMaterialInvalidTextureIndex;
      trackTexture(rayPortalMaterialData.getMaskTexture2(), maskTextureIndex2, hasTexcoords, false);

      uint8_t rayPortalIndex = rayPortalMaterialData.getRayPortalIndex();
      float rotationSpeed = rayPortalMaterialData.getRotationSpeed();
      bool enableEmissive = rayPortalMaterialData.getEnableEmission();
      float emissiveIntensity = rayPortalMaterialData.getEmissiveIntensity() * RtxOptions::emissiveIntensity();

      const RtRayPortalSurfaceMaterial rayPortalSurfaceMaterial{
        maskTextureIndex, maskTextureIndex2, rayPortalIndex,
        rotationSpeed, enableEmissive, emissiveIntensity, samplerIndex, samplerIndex2
      };

      surfaceMaterial.emplace(rayPortalSurfaceMaterial);
    }

    assert(surfaceMaterial.has_value());
    assert(surfaceMaterial->validate());

    // V776: include the primary terrain splat mask and any other ordinary
    // material slots, in addition to the arrays registered by setKenshiTerrainArgs.
    if (kenshiTerrain) {
      auto& textures = m_device->getCommon()->getTextureManager();
      surfaceMaterial->forEachTextureIndex([&](uint32_t slot) { textures.markTerrainTexture(slot); });
    }

    // Cache this
    const uint32_t index = m_surfaceMaterialCache.track(*surfaceMaterial);
    m_preCreationSurfaceMaterialMap[preCreationHash] = index;

    // DX11_V763: the decisive line. The bridge is proven to re-read progress
    // every step (the [D3D11Rtx][construction] log ramps), so the remaining
    // question is whether a changed progress reaches the GPU as a NEW material.
    // Logs on CHANGE, not first-N: V761's cap of 6 was consumed within 85ms of
    // placement and could therefore say nothing about any later frame.
    // A ramping `index` means the GPU is being fed new materials and any
    // remaining fault is in the shader; a frozen one means it is still cached.
    if (kenshiConstructionLogState >= 0.0f) {
      static dxvk::mutex sConstructionMutex;
      static float sLastState = -1.0f;
      static uint32_t sLines = 0;
      std::lock_guard<dxvk::mutex> lock(sConstructionMutex);
      // V764: trigger on the STATE alone. V763 also triggered on a changed
      // index, and a construction building has two sub-meshes whose surface
      // material indices alternate (238/239) - that alternation consumed the
      // whole 64-line cap at state=0 and the log could say nothing about the
      // ramp it was added to observe.
      if (sLines < 64u && kenshiConstructionLogState != sLastState) {
        ++sLines;
        sLastState = kenshiConstructionLogState;
        KENSHI_DIAGNOSTIC_INFO(str::format(
          "[SceneManager][kenshi-construction] state=", kenshiConstructionLogState,
          " grid=", kenshiConstructionLogGrid,
          " surfaceMaterialIndex=", index));
      }
    }
    if (out_indexInCache) {
      *out_indexInCache = index;
    }
    return m_surfaceMaterialCache.at(index);
  }

  RtTranslucentSurfaceMaterial SceneManager::createTranslucentSurfaceMaterial(const TranslucentMaterialData& translucentMaterialData,
                                                                              uint32_t samplerIndex,
                                                                              bool hasTexcoords) {
    uint32_t normalTextureIndex = kSurfaceMaterialInvalidTextureIndex;
    uint32_t transmittanceTextureIndex = kSurfaceMaterialInvalidTextureIndex;
    uint32_t emissiveColorTextureIndex = kSurfaceMaterialInvalidTextureIndex;

    trackTexture(translucentMaterialData.getNormalTexture(), normalTextureIndex, hasTexcoords);
    trackTexture(translucentMaterialData.getTransmittanceTexture(), transmittanceTextureIndex, hasTexcoords);
    trackTexture(translucentMaterialData.getEmissiveColorTexture(), emissiveColorTextureIndex, hasTexcoords);

    return RtTranslucentSurfaceMaterial{
      normalTextureIndex,
      transmittanceTextureIndex,
      emissiveColorTextureIndex,
      translucentMaterialData.getRefractiveIndex() * std::clamp(TranslucentMaterialOptions::refractiveIndexScale(), 0.0f, 3.0f),
      translucentMaterialData.getTransmittanceMeasurementDistance(),
      translucentMaterialData.getTransmittanceColor(),
      translucentMaterialData.getEnableEmission(),
      translucentMaterialData.getEmissiveIntensity() * RtxOptions::emissiveIntensity(),
      translucentMaterialData.getEmissiveColorConstant(),
      translucentMaterialData.getEnableThinWalled(),
      translucentMaterialData.getThinWallThickness(),
      translucentMaterialData.getEnableDiffuseLayer(),
      samplerIndex
    };
  }

  Rc<DxvkSampler> SceneManager::getOrCreateExternalSampler() {
    if (m_externalSampler == nullptr) {
      auto s = DxvkSamplerCreateInfo {};
      {
        s.magFilter = VK_FILTER_LINEAR;
        s.minFilter = VK_FILTER_LINEAR;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        s.mipmapLodBias = 0.f;
        s.mipmapLodMin = 0.f;
        s.mipmapLodMax = 0.f;
        s.useAnisotropy = VK_FALSE;
        s.maxAnisotropy = 1.f;
        s.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        s.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        s.compareToDepth = VK_FALSE;
        s.compareOp = VK_COMPARE_OP_NEVER;
        s.borderColor = VkClearColorValue {};
        s.usePixelCoord = VK_FALSE;
      }
      m_externalSampler = m_device->createSampler(s);
    }

    return m_externalSampler;
  }

  void SceneManager::setExternalStartInMediumMaterial(const MaterialData& translucentMaterial) {
    assert(translucentMaterial.getType() == MaterialDataType::Translucent);

    const auto samplerIndex = trackSampler(getOrCreateExternalSampler());
    const auto surfaceMaterial = RtSurfaceMaterial(
      createTranslucentSurfaceMaterial(translucentMaterial.getTranslucentMaterialData(), samplerIndex, true));

    m_externalStartInMediumMaterialIndex_inCache = m_surfaceMaterialCache.track(surfaceMaterial);
  }

  void SceneManager::clearExternalStartInMediumMaterial() {
    m_externalStartInMediumMaterialIndex_inCache = UINT32_MAX;
  }

  std::optional<XXH64_hash_t> SceneManager::findLegacyTextureHashByObjectPickingValue(uint32_t objectPickingValue) {
    std::lock_guard lock { m_drawCallMeta.mutex };

    auto tryFindIn = [](const std::unordered_map<ObjectPickingValue, DrawCallMetaInfo>& table, ObjectPickingValue toFind)
      -> std::optional<XXH64_hash_t> {
      auto found = table.find(toFind);
      if (found != table.end()) {
        const DrawCallMetaInfo& meta = found->second;
        if (meta.legacyTextureHash != kEmptyHash) {
          return meta.legacyTextureHash;
        }
      }
      return std::nullopt;
    };

    const int ticksToCheck[] = {
      m_drawCallMeta.ticker, // current tick
      (m_drawCallMeta.ticker + m_drawCallMeta.MaxTicks - 1) % m_drawCallMeta.MaxTicks, // prev tick
    };
    for (int tick : ticksToCheck) {
      if (m_drawCallMeta.ready[tick]) {
        if (auto h = tryFindIn(m_drawCallMeta.infos[tick], objectPickingValue)) {
          return h;
        }
      }
    }
    return std::nullopt;
  }

  std::vector<ObjectPickingValue> SceneManager::gatherObjectPickingValuesByTextureHash(XXH64_hash_t texHash) {
    std::lock_guard lock { m_drawCallMeta.mutex };
    assert(texHash != kEmptyHash);

    const int ticksToCheck[] = {
      m_drawCallMeta.ticker, // current tick
      (m_drawCallMeta.ticker + m_drawCallMeta.MaxTicks - 1) % m_drawCallMeta.MaxTicks, // prev tick
    };

    auto correspondingValues = std::vector<ObjectPickingValue> {};
    for (int tick : ticksToCheck) {
      if (m_drawCallMeta.ready[tick]) {
        for (const auto& [pickingValue, meta] : m_drawCallMeta.infos[tick]) {
          if (texHash == meta.legacyTextureHash) {
            correspondingValues.push_back(pickingValue);
          } else if (texHash == meta.legacyTextureHash2) {
            correspondingValues.push_back(pickingValue);
          }
        }
        break;
      }
    }
    return correspondingValues;
  }

  SceneManager::SamplerIndex SceneManager::trackSampler(Rc<DxvkSampler> sampler) {
    if (sampler == nullptr) {
      ONCE(Logger::warn("Found a null sampler. Fallback to linear-repeat"));
      sampler = patchSampler(
        VK_FILTER_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VkClearColorValue {});
    }
    return m_samplerCache.track(sampler);
  }

  Rc<DxvkSampler> SceneManager::patchSampler( const VkFilter filterMode,
                                              const VkSamplerAddressMode addressModeU,
                                              const VkSamplerAddressMode addressModeV,
                                              const VkSamplerAddressMode addressModeW,
                                              const VkClearColorValue borderColor) {
    auto& resourceManager = m_device->getCommon()->getResources();
    // Create a sampler to account for DLSS lod bias and any custom filtering overrides the user has set
    return resourceManager.getSampler(
      filterMode,
      VK_SAMPLER_MIPMAP_MODE_LINEAR,
      addressModeU,
      addressModeV,
      addressModeW,
      borderColor,
      getTotalMipBias(),
      RtxOptions::useAnisotropicFiltering());
  }

  void SceneManager::addLight(const Dx11LightDesc& light) {
    ScopedCpuProfileZone();
    // Attempt to convert the D3D11 light to RT

    std::optional<LightData> lightData = LightData::tryCreate(light);

    // Note: Skip adding this light if it is somehow malformed such that it could not be created.
    if (!lightData.has_value()) {
      return;
    }

    const RtLight rtLight = lightData->toRtLight();
    const std::vector<AssetReplacement>* pReplacements = m_pReplacer->getReplacementsForLight(rtLight.getInitialHash());

    // Build identity hash from the light's stable hash + position. Used by
    // both the replacement and the toggle-off cleanup paths below; must stay
    // in sync between them so they target the same RI.
    const XXH64_hash_t lightAssetHash = rtLight.getInitialHash();
    const Vector3 lightPos = rtLight.getPosition();
    const XXH64_hash_t lightIdHash = XXH64(&lightPos, sizeof(Vector3), lightAssetHash);

    if (pReplacements) {
      const Matrix4 lightTransform = LightUtils::getLightTransform(light);

      const ReplacementInstance::LookupKey lightKey { lightIdHash, lightAssetHash, kEmptyHash, kEmptyHash, lightPos, lightTransform };
      ReplacementInstance* replacementInstance = m_drawCallTracker.findOrCreateReplacementInstance(lightKey);

      // Reinitialize the RI if the prim count doesn't match the replacement count.
      // This handles the transition from unreplaced (1 prim) to replaced (N prims)
      // when replacements finish loading asynchronously.
      if (replacementInstance->root.getUntyped() != nullptr &&
          replacementInstance->prims.size() != pReplacements->size()) {
        replacementInstance->clear();
      }

      // All lights in a light replacement are externally tracked, with their
      // lifecycle managed by the ReplacementInstance. This unifies root and sub-light
      // handling: create on first frame, update on subsequent frames.
      // TODO(TREX-1091) to implement meshes as light replacements, replace the below loop with a call to drawReplacements.
      const bool needsBBoxUpdate = replacementInstance->boundingBoxDirty;
      AxisAlignedBoundingBox litBBox;
      for (size_t i = 0; i < pReplacements->size(); i++) {
        const auto& replacement = (*pReplacements)[i];
        if (replacement.type == AssetReplacement::eLight && replacement.lightData.has_value()) {
          LightData replacementLight = replacement.lightData.value();

          // Merge the d3d11 light into replacements based on overrides.
          // Must happen before AABB extraction: some entries (e.g. the translated
          // original game light) have Unknown lightType and zero position/radius
          // until merged with the d3d11 light.
          replacementLight.merge(light);

          // Convert to runtime light
          RtLight rtReplacementLight = replacementLight.toRtLight();

          if (needsBBoxUpdate) {
            const Vector3 pos = rtReplacementLight.getPosition();
            float lightRadius = 0.f;
            if (rtReplacementLight.getType() == RtLightType::Sphere) {
              lightRadius = rtReplacementLight.getSphereLight().getRadius();
            }
            for (uint32_t j = 0; j < 3; j++) {
              litBBox.minPos[j] = std::min(litBBox.minPos[j], pos[j] - lightRadius);
              litBBox.maxPos[j] = std::max(litBBox.maxPos[j], pos[j] + lightRadius);
            }
          }

          // Transform the replacement light by the legacy light
          if (replacementLight.relativeTransform()) {
            rtReplacementLight.applyTransform(lightTransform);
          }

          RtLight* existingLight = (replacementInstance->prims.size() > i)
              ? replacementInstance->prims[i].getLight() : nullptr;
          if (existingLight != nullptr) {
            m_lightManager.updateExternallyTrackedLight(existingLight, rtReplacementLight);
          } else {
            RtLight* newLight = m_lightManager.createExternallyTrackedLight(rtReplacementLight);
            if (newLight != nullptr) {
              if (replacementInstance->prims.empty()) {
                replacementInstance->setup(PrimInstance(newLight, PrimInstance::Type::Light), pReplacements->size(), pReplacements);
              }
              newLight->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, newLight, PrimInstance::Type::Light);
              if (replacementInstance->root.getUntyped() == nullptr) {
                replacementInstance->root = PrimInstance(newLight, PrimInstance::Type::Light);
              }
            }
          }
        } else {
          assert(false); // We don't support meshes as children of lights yet.
        }
      }

      replacementInstance->frameLastSeen = m_device->getCurrentFrameId();
      replacementInstance->objectToWorld = lightTransform;
      if (needsBBoxUpdate) {
        if (litBBox.isValid()) {
          replacementInstance->lightBoundingBox = litBBox;
        }
        replacementInstance->boundingBoxDirty = false;
      }
    } else {
      // If this light previously had a replacement (e.g. enableReplacementLights
      // was just toggled off), the externally-tracked replacement lights are
      // still alive in LightManager -- they have no frame-age GC; their
      // lifecycle is owned by the RI. Tear down the RI so its prims get marked
      // for GC; otherwise the user sees the replacement lights and the original
      // game light rendering simultaneously until DrawCallTracker collects the
      // RI ~numFramesToKeepInstances frames later.
      if (ReplacementInstance* stale = m_drawCallTracker.findReplacementInstanceByIdentity(lightIdHash)) {
        if (stale->activeReplacements != nullptr) {
          stale->clear();
        }
      }

      // This is a light coming from the game directly, so use the appropriate API for filter rules
      m_lightManager.addGameLight(light.Type, rtLight, light.KenshiCullRadius);
    }
  }

  void SceneManager::collectKenshiTerrainResourceLookups(uint32_t frame) {
    if (frame % 600u != 0u) return;
    size_t normals = 0, biomes = 0, expired = 0;
    {
      std::lock_guard<std::mutex> lock(s_kenshiTerrainNormalMutex);
      for (auto i = s_kenshiTerrainNormalSets.begin(); i != s_kenshiTerrainNormalSets.end();) {
        if (i->second.expired()) { i = s_kenshiTerrainNormalSets.erase(i); ++expired; }
        else ++i;
      }
      normals = s_kenshiTerrainNormalSets.size();
    }
    {
      std::lock_guard<std::mutex> lock(s_kenshiTerrainBiomeMutex);
      for (auto i = s_kenshiTerrainBiomeSets.begin(); i != s_kenshiTerrainBiomeSets.end();) {
        if (i->second.expired()) { i = s_kenshiTerrainBiomeSets.erase(i); ++expired; }
        else ++i;
      }
      biomes = s_kenshiTerrainBiomeSets.size();
    }
    KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainMemory V776] frame=", frame,
      " normalRecords=", normals, " biomeRecords=", biomes, " expiredLookups=", expired));
  }

  void SceneManager::collectUnusedTerrainTextures() {
    const uint32_t frame = m_device->getCurrentFrameId();
    // V780: cache-only releases are staggered away from texture/root marking and
    // the bridge's 60-frame slice pass. No live-instance/geometry GC is deferred.
    if (frame % 30u == 15u) opaque_preparation::collect(this, frame);
    if (frame % 30u != 0u) return;
    const auto protectionStart = std::chrono::steady_clock::now();
    auto& textures = m_device->getCommon()->getTextureManager();
    const auto& table = textures.getTextureTable();
    const auto keep = [&](uint32_t index) {
      // This patch must not change managed/replacement-texture demotion policy.
      if (index != kSurfaceMaterialInvalidTextureIndex && index < table.size()
          && table[index].getManagedTexture() == nullptr)
        textures.keepTextureAlive(index);
    };
    static thread_local MaintenanceMarks seenMaterials, seenTerrain, seenExtensions;
    seenMaterials.begin(m_surfaceMaterialCache.getTotalCount());
    seenTerrain.begin(m_kenshiTerrainArgs.size());
    seenExtensions.begin(m_surfaceMaterialExtensionCache.getTotalCount());
    const auto keepTerrainRecord = [&](uint32_t index) {
      if (index >= m_kenshiTerrainArgs.size() || !seenTerrain.first(index)) return;
      forEachKenshiTerrainTexture(m_kenshiTerrainArgs[index], keep);
    };
    const auto keepMaterial = [&](uint32_t index) {
      if (index >= m_surfaceMaterialCache.getTotalCount() || !seenMaterials.first(index)) return;
      const auto& material = m_surfaceMaterialCache.at(index);
      material.forEachTextureIndex(keep);
      if (material.getType() != RtSurfaceMaterialType::Opaque) return;
      const auto& opaque = material.getOpaqueSurfaceMaterial();
      const uint32_t extension = opaque.getSubsurfaceMaterialIndex();
      if (extension < m_surfaceMaterialExtensionCache.getTotalCount() && seenExtensions.first(extension))
        m_surfaceMaterialExtensionCache.at(extension).forEachTextureIndex(keep);
      if (!opaque.isKenshiTerrain()) return;
      const uint32_t set = opaque.getKenshiTerrainSetIndex();
      keepTerrainRecord(set);
      if (set >= m_kenshiTerrainArgs.size()) return;
      const auto& args = m_kenshiTerrainArgs[set];
      // The shader's selection is one level deep. Only active neighbor slots
      // are valid; zero in an unused field is NOT a reference to record zero.
      uint32_t neighbors = 0;
      for (uint32_t mask = args.blendMask; mask; mask >>= 1u) neighbors += mask & 1u;
      if (neighbors > 0u) keepTerrainRecord(args.blendSet0);
      if (neighbors > 1u) keepTerrainRecord(args.blendSet1);
      if (neighbors > 2u) keepTerrainRecord(args.blendSet2);
    };
    // Include off-screen/anti-culled and currently hidden instances. TLAS-only
    // enumeration would free slots which an existing instance can use again.
    for (const auto* instance : m_instanceManager.getInstanceTable()) {
      keepMaterial(instance->surface.surfaceMaterialIndex);
      // V780: blood overlays live on the surface, outside the material record.
      keep(instance->surface.kenshiBloodTextureIndex);
      keep(instance->getOmmOpacityTextureIndex());
      keep(instance->getSecondaryOpacityTextureIndex());
    }
    keepMaterial(m_startInMediumMaterialIndex_inCache);
    keepMaterial(m_fogStartInMediumMaterialIndex_inCache);
    keepMaterial(m_externalStartInMediumMaterialIndex_inCache);
    const auto protectionUs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - protectionStart).count();
    textures.collectUnusedTerrainTextures(uint32_t(std::min<int64_t>(protectionUs, UINT32_MAX)));
    if (m_device->getCurrentFrameId() % 600u == 0u)
      opaque_preparation::reportTextureOwners(this, m_device->getCurrentFrameId());
  }

  void SceneManager::prepareSceneData(Rc<RtxContext> ctx, DxvkBarrierSet& execBarriers) {
    terrain_profile::Scope terrainCpuScene(terrain_profile::Stage::Scene, 2);
    ScopedGpuProfileZone(ctx, "Build Scene");

  #ifdef REMIX_DEVELOPMENT
    if (m_device->getCurrentFrameId() == RtxOptions::dumpAllInstancesOnFrame()) {
      // Print all RtInstances for debugging
      printAllRtInstances();
    }
  #endif

    terrain_profile::Scope sceneCpuMaintenance(terrain_profile::Stage::SceneMaintenance);
    // Needs to happen before garbageCollection to avoid destroying dynamic lights
    m_lightManager.dynamicLightMatching();

    garbageCollection();

    m_graphManager.applySceneOverrides(ctx);

    m_terrainBaker->prepareSceneData(ctx);
    sceneCpuMaintenance.finish();
    terrain_profile::Scope sceneCpuResources(terrain_profile::Stage::SceneResources);

    // DX11_V527. Ground blood decals MUST be packed here, before the bindless
    // table below is finalized: trackTexture only reserves an index, and an index
    // reserved after the table has been uploaded points past its end. Doing this
    // from bindCommonRayTracingResources instead cost a GPU page fault and a
    // VK_ERROR_DEVICE_LOST that the game reports as an out-of-video-memory box.
    buildKenshiTerrainBuffer(ctx);
    buildKenshiTerrainBloodBuffer(ctx);
    buildKenshiWaterZoneBuffer(ctx);
    buildKenshiInteriorBuffer(ctx);

    auto& textureManager = m_device->getCommon()->getTextureManager();
    // DX11_V655: must run BEFORE the publish below - the repair reserves new
    // bindless indices, and an index reserved after the publish points past the
    // end of what the GPU can see (the V526/V527 rule).
    auditAndRepairInstanceBufferIndices();
    auditBufferTableEntries();

    // Globals/baker have registered their textures; all retained material roots
    // are protected before reclaiming slots and publishing this frame's table.
    collectUnusedTerrainTextures();

    m_bindlessResourceManager.prepareSceneData(ctx, textureManager.getTextureTable(), getBufferTable(), getSamplerTable());
    sceneCpuResources.finish();

    // If there are no instances, we should do nothing!
    if (m_instanceManager.getActiveCount() == 0) {
      m_accelManager.logMemoryOwnership(m_drawCallCache, "sample");
      // Clear the ray portal data before the next frame
      m_rayPortalManager.clear();
      return;
    }

    m_rayPortalManager.prepareSceneData(ctx);
    // Note: only main camera needs to be teleportation corrected as only that one is used for ray tracing & denoising
    m_rayPortalManager.fixCameraInBetweenPortals(m_cameraManager.getCamera(CameraType::Main));
    m_rayPortalManager.fixCameraInBetweenPortals(m_cameraManager.getCamera(CameraType::ViewModel));
    m_rayPortalManager.createVirtualCameras(m_cameraManager);
    const bool didTeleport = m_rayPortalManager.detectTeleportationAndCorrectCameraHistory(
      m_cameraManager.getCamera(CameraType::Main),
      m_cameraManager.isCameraValid(CameraType::ViewModel) ? &m_cameraManager.getCamera(CameraType::ViewModel) : nullptr);

    m_startInMediumMaterialIndex_inCache = m_externalStartInMediumMaterialIndex_inCache != kInvalidMaterialCacheIndex
      ? m_externalStartInMediumMaterialIndex_inCache
      : m_fogStartInMediumMaterialIndex_inCache;

    if (m_cameraManager.isCameraCutThisFrame()) {
      // Ignore camera cut events on teleportation so we don't flush the caches
      if (!didTeleport) {
        Logger::info(str::format("Camera cut detected on frame ", m_device->getCurrentFrameId()));
        kenshi_fault::add(kenshi_fault::CameraCut,m_device->getCurrentFrameId(),didTeleport);
        m_enqueueDelayedClear = true;
      }
    }

    // Initialize/remove opacity micromap manager
    if (RtxOptions::getEnableOpacityMicromap()) {
      if (!m_opacityMicromapManager.get()) {
        kenshi_fault::add(kenshi_fault::OmmReset,m_device->getCurrentFrameId(),m_opacityMicromapManager?1:0,m_enqueueDelayedClear);
        m_opacityMicromapManager = std::make_unique<OpacityMicromapManager>(m_device);
        m_instanceManager.addEventHandler(m_opacityMicromapManager->getInstanceEventHandler());
        // Seed candidates with instances that were added before the event handler was registered
        m_opacityMicromapManager->seedCandidates(m_instanceManager.getInstanceTable());
        KENSHI_DIAGNOSTIC_INFO("[RTX] Opacity Micromap: enabled");
      }
    } else if (m_opacityMicromapManager.get()) {
      m_accelManager.invalidateOpacityMicromapBindings();
      m_instanceManager.notifySceneChanged();
      m_instanceManager.removeEventHandler(m_opacityMicromapManager.get());
      m_opacityMicromapManager = nullptr;
      KENSHI_DIAGNOSTIC_INFO("[RTX] Opacity Micromap: disabled");
    }

    RtxParticleSystemManager& particles = m_device->getCommon()->metaParticleSystem();
    terrain_profile::Scope sceneCpuParticles(terrain_profile::Stage::SceneParticles);
    particles.simulate(ctx.ptr());
    sceneCpuParticles.finish();

    terrain_profile::Scope sceneCpuVirtualInstances(terrain_profile::Stage::SceneVirtualInstances);
    m_instanceManager.findPortalForVirtualInstances(m_cameraManager, m_rayPortalManager);
    m_instanceManager.createViewModelInstances(ctx, m_cameraManager, m_rayPortalManager);
    m_instanceManager.createPlayerModelVirtualInstances(ctx, m_cameraManager, m_rayPortalManager);
    sceneCpuVirtualInstances.finish();

    terrain_origin::repair(this, m_device->getCurrentFrameId(),
      m_instanceManager.getInstanceTable(), m_cameraManager.getCamera(CameraType::Main));
    const auto retainedOriginMoved = repairRetainedNativeOrigin(m_device->getCurrentFrameId(),
      m_instanceManager.getInstanceTable(), m_drawCallTracker.getReplacementInstances());
    if (retainedOriginMoved) m_instanceManager.notifySceneChanged();
    m_accelManager.mergeInstancesIntoBlas(ctx, execBarriers, textureManager.getTextureTable(), m_cameraManager, m_instanceManager, m_opacityMicromapManager.get());

    // Call on the other managers to prepare their GPU data for the current scene
    m_accelManager.prepareSceneData(ctx, execBarriers, m_instanceManager);
    if (terrain_audit::active(m_device->getCurrentFrameId())) {
      const auto& textures = textureManager.getTextureTable();
      for (const auto* instance : m_accelManager.getOrderedInstances()) {
        const auto* geometry = instance->getBlas();
        if (!geometry || !terrain_audit::family(geometry->input.programmableVertexShaderBytecodeHash)) continue;
        // Retained instances can move with an origin rebase after their source draw.
        terrain_audit::draw(m_device->getCurrentFrameId(), 3, geometry->input, 0, instance->getId(),
          instance->surface.objectToWorld);
        if (terrain_audit::state.counts[3] > 512) continue;
        const uint32_t albedo = instance->getAlbedoOpacityTextureIndex();
        KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] rendered frame=", m_device->getCurrentFrameId(),
          " instance=", instance->getId(), " updated=", instance->getFrameLastUpdated(),
          " hidden=", instance->isHidden(), " mask=", uint32_t(instance->getVkInstance().mask),
          " flags=", uint32_t(instance->getVkInstance().flags),
          " opaque=", bool(instance->surface.alphaState.isFullyOpaque),
          " albedo=", albedo, " albedoValid=", albedo < textures.size() && textures[albedo].isValid()));
      }
      const auto& c = terrain_audit::state.counts;
      KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] totals frame=", m_device->getCurrentFrameId(),
        " commit=", c[0], " scene=", c[1], " resolved=", c[2], " ordered=", c[3],
        " cap=512 camera=", getCamera().getPosition(false).x, ",", getCamera().getPosition(false).y,
        ",", getCamera().getPosition(false).z));
    }
    terrain_profile::Scope sceneCpuLights(terrain_profile::Stage::SceneLights);
    m_lightManager.prepareSceneData(ctx, m_cameraManager);
    sceneCpuLights.finish();

    // Upload surface material buffer BEFORE the GPU culling dispatch so the
    // compute shader can copy template material entries to per-instance slots.
    // For PointInstancer duplicate entries we skip writeGPUData and advance past
    // the gap — the GPU shader will fill those slots.
    //
    // When the scene is unchanged (fast-skip path in mergeInstancesIntoBlas),
    // the surface order and material data are normally identical to last frame.
    // Baked terrain materials are updated independently of acceleration-structure
    // scene generation, so keep their surface-material upload live.
    const bool updateSurfaceMaterials =
      !m_accelManager.wasSceneUnchangedThisFrame() ||
      TerrainBaker::needsTerrainBaking();
    if ((kenshi_telemetry::enabled() && s_drawTraceFramesRemaining.load(std::memory_order_relaxed) > 0u)) {
      Logger::info(str::format(
        "[SceneManager][particle-material-upload] f=", m_device->getCurrentFrameId(),
        " sceneUnchanged=", m_accelManager.wasSceneUnchangedThisFrame() ? 1 : 0,
        " upload=", updateSurfaceMaterials ? 1 : 0));
    }
    if (updateSurfaceMaterials) {
      terrain_profile::Scope sceneCpuMaterials(terrain_profile::Stage::SceneMaterials);
      DxvkBufferCreateInfo matInfo;
      matInfo.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                    | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                    | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      matInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                     | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      matInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;

      if (m_surfaceMaterialCache.getTotalCount() > 0) {
        ScopedGpuProfileZone(ctx, "updateSurfaceMaterials");
        // Note: We duplicate the materials in the buffer so we don't have to do pointer chasing on the GPU
        size_t surfaceMaterialsGPUSize = m_accelManager.getSurfaceCount() * kSurfaceMaterialGPUSize;
        if (m_startInMediumMaterialIndex_inCache != kInvalidMaterialCacheIndex) {
          surfaceMaterialsGPUSize += kSurfaceMaterialGPUSize;
        }

        matInfo.size = align(surfaceMaterialsGPUSize, kBufferAlignment);
        if (m_surfaceMaterialBuffer == nullptr || matInfo.size > m_surfaceMaterialBuffer->info().size) {
          m_surfaceMaterialBuffer = m_device->createBuffer(matInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Surface Material Buffer");
        }

        std::size_t dataOffset = 0;
        uint32_t surfaceIndex = 0;
        std::vector<unsigned char> surfaceMaterialsGPUData(surfaceMaterialsGPUSize);
        for (auto&& pInstance : m_accelManager.getOrderedInstances()) {
          // For PointInstancer duplicates (entries beyond the template), skip
          // writeGPUData — the GPU culling shader copies the template material.
          const auto& surf = pInstance->surface;
          if (surf.instancesToObject != nullptr &&
              surf.surfaceIndexOfFirstInstance != SIZE_MAX &&
              surfaceIndex > surf.surfaceIndexOfFirstInstance) {
            dataOffset += kSurfaceMaterialGPUSize;
          } else {
            auto&& surfaceMaterial = m_surfaceMaterialCache.getObjectTable()[surf.surfaceMaterialIndex];
            surfaceMaterial.writeGPUData(surfaceMaterialsGPUData.data(), dataOffset, surfaceIndex);
          }
          surfaceIndex++;
        }

        if (m_startInMediumMaterialIndex_inCache != kInvalidMaterialCacheIndex) {
          auto&& surfaceMaterial = m_surfaceMaterialCache.getObjectTable()[m_startInMediumMaterialIndex_inCache];
          surfaceMaterial.writeGPUData(surfaceMaterialsGPUData.data(), dataOffset, surfaceIndex);
          m_startInMediumMaterialIndex = surfaceIndex;
          surfaceIndex++;
        }

        assert(dataOffset == surfaceMaterialsGPUSize);
        assert(surfaceMaterialsGPUData.size() == surfaceMaterialsGPUSize);

        ctx->writeToBuffer(m_surfaceMaterialBuffer, 0, surfaceMaterialsGPUData.size(), surfaceMaterialsGPUData.data());
      }
    }

    // GPU-driven PointInstancer culling: overwrites visible instance placeholders
    // in m_vkInstanceBuffer with proper transforms and masks, copies per-instance
    // surface and material data from templates. Must run after prepareSceneData
    // (which uploads placeholders) and before buildTlas.
    m_accelManager.dispatchPointInstancerCulling(ctx, m_cameraManager, m_surfaceMaterialBuffer);

    // Build the TLAS
    m_accelManager.buildTlas(ctx);

    // Todo: These updates require a lot of temporary buffer allocations and memcopies, ideally we should memcpy directly into a mapped pointer provided by Vulkan,
    // but we have to create a buffer to pass to DXVK's updateBuffer for now.
    // Skip when scene is unchanged — buffers from last frame are still valid.
    if (!m_accelManager.wasSceneUnchangedThisFrame()) {
      terrain_profile::Scope sceneCpuExtensions(terrain_profile::Stage::SceneExtensions);
      // Allocate the instance buffer and copy its contents from host to device memory
      DxvkBufferCreateInfo info;
      info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT;

      // Surface Material Extension Buffer
      if (m_surfaceMaterialExtensionCache.getTotalCount() > 0) {
        ScopedGpuProfileZone(ctx, "updateSurfaceMaterialExtensions");
        const auto surfaceMaterialExtensionsGPUSize = m_surfaceMaterialExtensionCache.getTotalCount() * kSurfaceMaterialGPUSize;

        info.size = align(surfaceMaterialExtensionsGPUSize, kBufferAlignment);
        info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (m_surfaceMaterialExtensionBuffer == nullptr || info.size > m_surfaceMaterialExtensionBuffer->info().size) {
          m_surfaceMaterialExtensionBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Surface Material Extension Buffer");
        }

        std::size_t dataOffset = 0;
        std::vector<unsigned char> surfaceMaterialExtensionsGPUData(surfaceMaterialExtensionsGPUSize);

        uint32_t surfaceIndex = 0;
        for (auto&& surfaceMaterialExtension : m_surfaceMaterialExtensionCache.getObjectTable()) {
          surfaceMaterialExtension.writeGPUData(surfaceMaterialExtensionsGPUData.data(), dataOffset, surfaceIndex);
          surfaceIndex++;
        }

        assert(dataOffset == surfaceMaterialExtensionsGPUSize);
        assert(surfaceMaterialExtensionsGPUData.size() == surfaceMaterialExtensionsGPUSize);

        ctx->writeToBuffer(m_surfaceMaterialExtensionBuffer, 0, surfaceMaterialExtensionsGPUData.size(), surfaceMaterialExtensionsGPUData.data());
      }

      // Volume Material buffer
      if (m_volumeMaterialCache.getTotalCount() > 0) {
        ScopedGpuProfileZone(ctx, "updateVolumeMaterials");
        const auto volumeMaterialsGPUSize = m_volumeMaterialCache.getTotalCount() * kVolumeMaterialGPUSize;

        info.size = align(volumeMaterialsGPUSize, kBufferAlignment);
        info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (m_volumeMaterialBuffer == nullptr || info.size > m_volumeMaterialBuffer->info().size) {
          m_volumeMaterialBuffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "Volume Material Buffer");
        }

        std::size_t dataOffset = 0;
        std::vector<unsigned char> volumeMaterialsGPUData(volumeMaterialsGPUSize);

        for (auto&& volumeMaterial : m_volumeMaterialCache.getObjectTable()) {
          volumeMaterial.writeGPUData(volumeMaterialsGPUData.data(), dataOffset);
        }

        assert(dataOffset == volumeMaterialsGPUSize);
        assert(volumeMaterialsGPUData.size() == volumeMaterialsGPUSize);

        ctx->writeToBuffer(m_volumeMaterialBuffer, 0, volumeMaterialsGPUData.size(), volumeMaterialsGPUData.data());
      }
    }

    ctx->emitMemoryBarrier(0,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);

    // Update stats
    m_device->statCounters().setCtr(DxvkStatCounter::RtxBlasCount, AccelManager::getBlasCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxBufferCount, m_bufferCache.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxTextureCount, textureManager.getTextureTable().size());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxInstanceCount, m_instanceManager.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxSurfaceMaterialCount, m_surfaceMaterialCache.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxSurfaceMaterialExtensionCount, m_surfaceMaterialExtensionCache.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxVolumeMaterialCount, m_volumeMaterialCache.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxLightCount, m_lightManager.getActiveCount());
    m_device->statCounters().setCtr(DxvkStatCounter::RtxSamplers, m_samplerCache.getActiveCount());

    m_accelManager.logMemoryOwnership(m_drawCallCache, "sample");

    auto capturer = m_device->getCommon()->capturer();
    if (m_device->getCurrentFrameId() == m_beginUsdExportFrameNum) {
      capturer->triggerNewCapture();
    }
    capturer->step(ctx, ctx->getCommonObjects()->getLastKnownWindowHandle());

    // Clear the ray portal data before the next frame
    m_rayPortalManager.clear();

    // Check Anti-Culling Support:
    // When the game doesn't set up the View Matrix, we must disable Anti-Culling to prevent visual corruption.
    m_isAntiCullingSupported = (getCamera().getViewToWorld() != Matrix4d());
  }

  static_assert(std::is_same_v< decltype(RtSurface::objectPickingValue), ObjectPickingValue>);

  void SceneManager::submitExternalDraw(Rc<DxvkContext> ctx, ExternalDrawState&& state) {
    Rc<DxvkSampler> externalSampler = getOrCreateExternalSampler();

    {
      state.drawCall.materialData.samplers[0] = externalSampler;
      state.drawCall.materialData.samplers[1] = externalSampler;
    }
    {
      const RtCamera& rtCamera = ctx->getCommonObjects()->getSceneManager().getCameraManager()
        .getCamera(state.cameraType);
      state.drawCall.transformData.worldToView = Matrix4 { rtCamera.getWorldToView() };
      state.drawCall.transformData.viewToProjection = Matrix4 { rtCamera.getViewToProjection() };
      state.drawCall.transformData.objectToView = state.drawCall.transformData.worldToView * state.drawCall.transformData.objectToWorld;
    }

    if (!state.gpuInstancingTransforms.empty()) {
      state.drawCall.transformData.instancesToObject =
        std::make_shared<const std::vector<Matrix4>>(std::move(state.gpuInstancingTransforms));
    }

    const auto& submeshes = m_pReplacer->accessExternalMesh(state.mesh);

    const XXH64_hash_t identityHash = state.computeExternalDrawIdentityHash();
    const XXH64_hash_t spatialMapHash = spatialMapHashForExternalDrawMesh(state.mesh);
    const Matrix4& xform = state.drawCall.transformData.objectToWorld;
    const XXH64_hash_t matHash = state.drawCall.materialData.getHash();
    const Vector3 worldPos = xform[3].xyz();

    // NOTE: disallow a search for matching instances by 'materialHash' and other indirect ways,
    //       as it's expected that the API user controls the instances precisely with remixapi_MeshHandle
    //       (external draw calls set 'spatialMapHash' to remixapi_MeshHandle and remixapi_MeshHandle must be immutable on instances,
    //       so setting 'allowCrossTopologyMatching' to 'true' will overwrite 'spatialMapHash')
    constexpr bool allowCrossTopologyMatching = false;

    const ReplacementInstance::LookupKey externalKey { identityHash, spatialMapHash, matHash, kEmptyHash, worldPos, xform };
    ReplacementInstance* replacementInstance = m_drawCallTracker.findOrCreateReplacementInstance(externalKey, allowCrossTopologyMatching);

    AxisAlignedBoundingBox geometryBBox;

    for (size_t i = 0; i < submeshes.size(); i++) {
      state.drawCall.geometryData = submeshes[i];
      state.drawCall.geometryData.cullMode = state.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;

      const MaterialData* material = m_pReplacer->accessExternalMaterial(submeshes[i].externalMaterial);
      if (material != nullptr) {
        state.drawCall.materialData.setHashOverride(material->getHash());
      } 

      const RtxParticleSystemDesc* pParticles = nullptr;
      if (state.optionalParticleDesc.has_value()) {
        pParticles = &state.optionalParticleDesc.value();
      }

      RtInstance* existingInstance = (replacementInstance->prims.size() > i)
          ? replacementInstance->prims[i].getInstance() : nullptr;

      RtInstance* instance = processDrawCallState(ctx, state.drawCall,
          material != nullptr ? MaterialData(*material) : LegacyMaterialData().as<OpaqueMaterialData>(),
          existingInstance, pParticles);

      if (instance != nullptr) {
        if (replacementInstance->root.getUntyped() == nullptr) {
          replacementInstance->setup(PrimInstance(instance, PrimInstance::Type::Instance), submeshes.size(), nullptr);
        }
        if (replacementInstance->prims.size() > i &&
            replacementInstance->prims[i].getUntyped() != instance) {
          instance->getPrimInstanceOwner().setReplacementInstance(replacementInstance, i, instance,
              PrimInstance::Type::Instance);
        }
      }

      geometryBBox.unionWith(submeshes[i].boundingBox);
    }

    replacementInstance->frameLastSeen = m_device->getCurrentFrameId();

    if (geometryBBox.isValid()) {
      replacementInstance->geometryBoundingBox = geometryBBox;
      replacementInstance->objectToWorld = xform;
    }
  }

  void SceneManager::destroyExternalMesh(remixapi_MeshHandle handle) {
    removeInstancesWithExternalMesh(handle);
    m_pReplacer->destroyExternalMesh(handle);
  }

  void SceneManager::removeInstancesWithExternalMesh(remixapi_MeshHandle handle) {
    if (handle) {
      m_drawCallTracker.removeReplacementInstancesWithSpatialMapHash(
          spatialMapHashForExternalDrawMesh(handle));
    }
  }

  namespace {
    bool ifTrue_andThenSetFalse(std::atomic_bool& atomicBool) {
      bool expected = true;
      if (atomicBool.compare_exchange_strong(expected, false)) {
        return true;
      }
      return false;
    }
  } // unnamed

  void SceneManager::requestTextureVramFree() {
    m_forceFreeTextureMemory.store(true);
  }

  void SceneManager::requestVramCompaction() {
    m_forceFreeUnusedDxvkAllocatorChunks.store(true);
  }

  void SceneManager::manageTextureVram() {
    bool freeUnused = false;
    bool freeTextures = false;
    {
      if (ifTrue_andThenSetFalse(m_forceFreeTextureMemory)) {
        freeTextures = true;
        freeUnused = true;
      }
      if (ifTrue_andThenSetFalse(m_forceFreeUnusedDxvkAllocatorChunks)) {
        freeUnused = true;
      }
    }

    if (freeTextures) {
      m_device->getCommon()->getTextureManager().clear();

      if (m_opacityMicromapManager) {
        m_opacityMicromapManager->clear();
      }
    }

    if (freeUnused) {
      // DXVK doesnt free chunks for us by default (its high water mark) so force release some memory back to the system here.
      m_device->getCommon()->memoryManager().freeUnusedChunks();
    }
  }

  void SceneManager::printAllRtInstances() {
  #ifdef REMIX_DEVELOPMENT
    
    const auto& instances = m_instanceManager.getInstanceTable();
    Logger::info(str::format("=== Printing all RtInstances (", instances.size(), " total) ==="));
    
    for (size_t i = 0; i < instances.size(); ++i) {
      const RtInstance* instance = instances[i];
      if (instance != nullptr) {
        Logger::info(str::format("Instance ", i, ":"));
        instance->printDebugInfo();
      } else {
        Logger::warn(str::format("Instance ", i, ": nullptr"));
      }
    }
    
    Logger::info("=== End RtInstances Print ===");
  #endif
  }

  void SceneManager::trackReplacementMaterialHash(XXH64_hash_t materialHash) {
    if (materialHash != kEmptyHash) {
      m_currentFrameReplacementMaterialHashes[materialHash]++;
    }
  }

  bool SceneManager::isReplacementMaterialHashUsedThisFrame(XXH64_hash_t materialHash) const {
    return m_currentFrameReplacementMaterialHashes.find(materialHash) != m_currentFrameReplacementMaterialHashes.end();
  }

  uint32_t SceneManager::getReplacementMaterialHashUsageCount(XXH64_hash_t materialHash) const {
    auto it = m_currentFrameReplacementMaterialHashes.find(materialHash);
    return (it != m_currentFrameReplacementMaterialHashes.end()) ? it->second : 0;
  }

  void SceneManager::clearFrameReplacementMaterialHashes() {
    m_currentFrameReplacementMaterialHashes.clear();
  }

  void SceneManager::trackMeshHash(XXH64_hash_t meshHash) {
    if (meshHash != kEmptyHash) {
      m_currentFrameMeshHashes[meshHash]++;
    }
  }

  bool SceneManager::isMeshHashUsedThisFrame(XXH64_hash_t meshHash) const {
    return m_currentFrameMeshHashes.find(meshHash) != m_currentFrameMeshHashes.end();
  }

  uint32_t SceneManager::getMeshHashUsageCount(XXH64_hash_t meshHash) const {
    auto it = m_currentFrameMeshHashes.find(meshHash);
    return (it != m_currentFrameMeshHashes.end()) ? it->second : 0;
  }

  void SceneManager::clearFrameMeshHashes() {
    m_currentFrameMeshHashes.clear();
  }

}  // namespace dxvk
