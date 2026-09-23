/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
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
#pragma once
#include "rtx/dx11/dx11_light_state.h"
#include <mutex>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <variant>

#include "../dxvk_buffer.h"
#include "../dxvk_image.h"
#include "../dxvk_staging.h"
#include "../dxvk_bind_mask.h"
#include "../util/util_hashtable.h"

#include "rtx_globals.h"
#include "rtx_types.h"
#include "rtx_common_object.h"
#include "rtx_camera_manager.h"
#include "rtx_draw_call_cache.h"
#include "rtx_draw_call_tracker.h"
#include "rtx_sparse_unique_cache.h"
#include "rtx_light_manager.h"
#include "rtx_instance_manager.h"
#include "rtx_accel_manager.h"
#include "rtx_ray_portal_manager.h"
#include "rtx_bindless_resource_manager.h"
#include "rtx_objectpicking.h"
#include "rtx_mod_manager.h"
#include "graph/rtx_graph_manager.h"
#include "rtx_particle_system.h"
#include <d3d11.h>

namespace dxvk 
{
namespace kenshi_origin { struct Snapshot; }
class DxvkContext;
class DxvkDevice;
struct AssetReplacement;
struct AssetReplacer;
class OpacityMicromapManager;
class TerrainBaker;

// The resource cache can be *searched* by other users
class ResourceCache {
public:
  bool find(const RtSurfaceMaterial& surf, uint32_t& outIdx) const { return m_surfaceMaterialCache.find(surf, outIdx); }
  const RtSurfaceMaterial& get(const uint32_t index) const { return m_surfaceMaterialCache.getObjectTable()[index]; }

protected:
  BufferRefTable<RaytraceBuffer> m_bufferCache;

  // Every distinct terrain parameter set in the scene, content-addressed so a set keeps its index across
  // frames and a cached surface material can store it. See setKenshiTerrainArgs.
  std::vector<KenshiTerrainArgs> m_kenshiTerrainArgs;

  // Kenshi's ground blood decals for this frame. Global rather than per surface: a terrain chunk can
  // carry several, and the hit shader's rect test does the association.
  struct KenshiTerrainBloodDecal {
    Vector4 rect;              // originX, originZ, 1/sizeX, 1/sizeZ
    TextureRef texture;
    Rc<DxvkSampler> sampler;
  };
  std::vector<KenshiTerrainBloodDecal> m_kenshiTerrainBloodDecals;
  Rc<DxvkBuffer> m_kenshiTerrainBloodBuffer;
  std::unordered_map<XXH64_hash_t, uint32_t> m_kenshiTerrainSetIndices;
  Rc<DxvkBuffer> m_kenshiTerrainBuffer;
  BufferRefTable<Rc<DxvkSampler>> m_materialSamplerCache;

  struct SurfaceMaterialHashFn {
    size_t operator() (const RtSurfaceMaterial& mat) const {
      return (size_t)mat.getHash();
    }
  };
  SparseUniqueCache<RtSurfaceMaterial, SurfaceMaterialHashFn> m_surfaceMaterialCache;
  SparseUniqueCache<RtSurfaceMaterial, SurfaceMaterialHashFn> m_surfaceMaterialExtensionCache;
  fast_unordered_cache<uint32_t> m_preCreationSurfaceMaterialMap;

  struct VolumeMaterialHashFn {
    size_t operator() (const RtVolumeMaterial& mat) const {
      return (size_t)mat.getHash();
    }
  };
  SparseUniqueCache<RtVolumeMaterial, VolumeMaterialHashFn> m_volumeMaterialCache;

  struct SamplerHashFn {
    size_t operator() (const Rc<DxvkSampler>& sampler) const {
      return (size_t) sampler->hash();
    }
  };

  struct SamplerKeyEqual {
    bool operator()(const Rc<DxvkSampler>& lhs, const Rc<DxvkSampler>& rhs) const {
      return lhs->info() == rhs->info();
    }
  };

  SparseUniqueCache<Rc<DxvkSampler>, SamplerHashFn, SamplerKeyEqual> m_samplerCache;
};

struct ExternalDrawState {
  DrawCallState drawCall {};
  remixapi_MeshHandle mesh {};
  CameraType::Enum cameraType {};
  CategoryFlags categories {};
  bool doubleSided {};
  const std::optional<RtxParticleSystemDesc> optionalParticleDesc {};
  std::vector<Matrix4> gpuInstancingTransforms {};

  // Draw-instance identity for ReplacementInstance lookup. Excludes per-frame camera matrices
  // (worldToView / viewToProjection / objectToView) filled in submitExternalDraw after hashing.
  XXH64_hash_t computeExternalDrawIdentityHash() const;
};

// Scene manager is a super manager, it's the interface between rendering and world state
// along with managing the operation of other caches, scene manager also manages the cache
// directly for "SceneObject"'s - which are "unique meshes/geometry", which map 1-to-1 with
// BLAS entries in raytracing terminology.
class SceneManager : public CommonDeviceObject, public ResourceCache {
public:
  SceneManager(SceneManager const&) = delete;
  SceneManager& operator=(SceneManager const&) = delete;

  explicit SceneManager(DxvkDevice* device);
  ~SceneManager();

  void initialize(Rc<DxvkContext> ctx);
  void logStatistics();

  void onDestroy();

  void submitDrawState(Rc<DxvkContext> ctx, const DrawCallState& input, const MaterialData* overrideMaterialData, bool geometryCacheOnly = false);
  void submitExternalDraw(Rc<DxvkContext> ctx, ExternalDrawState&& state);

  // Remove an externally created mesh and all associated replacement instances.
  void destroyExternalMesh(remixapi_MeshHandle handle);
  void removeInstancesWithExternalMesh(remixapi_MeshHandle handle);
  
  void setExternalStartInMediumMaterial(const MaterialData& translucentMaterial);
  void clearExternalStartInMediumMaterial();
  
  bool areAllReplacementsLoaded() const;
  std::vector<Mod::State> getReplacementStates() const;

  RtxGlobals& getGlobals() { return m_globals; }

  Rc<DxvkBuffer> getSurfaceMaterialBuffer() { return m_surfaceMaterialBuffer; }
  Rc<DxvkBuffer> getSurfaceMaterialExtensionBuffer() { return m_surfaceMaterialExtensionBuffer; }
  Rc<DxvkBuffer> getVolumeMaterialBuffer() { return m_volumeMaterialBuffer; }
  Rc<DxvkBuffer> getSurfaceBuffer() const { return m_accelManager.getSurfaceBuffer(); }
  Rc<DxvkBuffer> getKenshiBloodBuffer(Rc<DxvkContext> ctx) { return m_accelManager.getKenshiBloodBuffer(ctx); }
  Rc<DxvkBuffer> getSurfaceMappingBuffer() const { return m_accelManager.getSurfaceMappingBuffer(); }
  Rc<DxvkBuffer> getCurrentFramePrimitiveIDPrefixSumBuffer() const { return m_accelManager.getCurrentFramePrimitiveIDPrefixSumBuffer(); }
  Rc<DxvkBuffer> getLastFramePrimitiveIDPrefixSumBuffer() const { return m_accelManager.getLastFramePrimitiveIDPrefixSumBuffer(); }
  Rc<DxvkBuffer> getBillboardsBuffer() const { return m_accelManager.getBillboardsBuffer(); }
  bool isPreviousFrameSceneAvailable() const { return m_previousFrameSceneAvailable && getSurfaceMappingBuffer().ptr() != nullptr; }

  // Arm the VRAM/scene census for the next `frames` frame ends, from the D3D11 bridge's Ctrl+Alt+O
  // hotkey, so consecutive frames around a chosen moment are recorded (frame-to-frame churn needs a
  // run). Static and atomic-backed, so the bridge can arm it from the submit thread and it adds no data
  // member.
  static void requestVramCensus(uint32_t frames);

  // Pushed from the D3D11 bridge each frame: how many draws reached the RT scene without a position
  // capture (a different geometry representation than on frames where capture succeeded). Reported on
  // the census line.
  static void reportSubmitWithoutCapture(uint32_t count);

  // Draws served from an earlier frame's captured bytes.
  static void reportStaleCaptureReuse(uint32_t count);

  // Mean/max age in frames of reused capture bytes, and the number of draws truncated to fit a shorter
  // older capture.
  static void reportStaleCaptureShape(uint32_t ageSum, uint32_t ageMax, uint32_t clamped);

  // Per-frame capture outcome, so a frame whose captures collectively failed is visible (the submit
  // summary only prints every few seconds).
  static void reportCaptureStats(uint32_t attempts, uint32_t captured,
                                 uint32_t budgetRejected, uint32_t failed);

  // 0 = RTX injected, 1 = no real scene draw preceded the UI draw, 2 = UI target was not the full
  // output, 3 = both, 4 = the UI path was never reached. Non-zero means the frame reached the screen as
  // pure game raster.
  static void reportInjectSkipReason(uint32_t reason);

  // Arm per-draw scene-side tracing for `frames` frames: the cache decision the D3D11 bridge cannot see,
  // keyed by drawCallID so both halves join into one row per draw per frame.
  static void requestDrawTrace(uint32_t frames);

  const std::vector<Rc<DxvkSampler>>& getSamplerTable() const { return m_samplerCache.getObjectTable(); }
  const std::vector<RaytraceBuffer>& getBufferTable() const { return m_bufferCache.getObjectTable(); }

  static void registerKenshiTerrainNormals(
    LegacyMaterialData& materialData,
    const std::array<TextureRef, LegacyMaterialData::kKenshiTerrainLayerCount>& normalTextures,
    const Vector4& textureFade);
  // Neighbouring biome sets for one boundary material, keyed by their own content so two tiles that meet
  // the same biomes share one entry.
  // The game's weather wetness and water level, published by whichever draw last declared them (global,
  // so a static pair).
  // Kenshi's water constants, same last-draw-wins transport; values are per zone.
  struct KenshiWaterParams {
    float tileScaleX = 0.0f;
    float tileScaleY = 0.0f;
    float tileOffsetX = 0.0f;
    float tileOffsetY = 0.0f;
    float mapScaleX = 0.0f;
    float mapScaleY = 0.0f;
    float mapOffsetX = 0.0f;
    float mapOffsetY = 0.0f;
    float speedX = 0.0f;
    float speedY = 0.0f;
    float distortion = 1.0f;
    float invStrength = 1.0f;
    float time = 0.0f;
    float rainAmount = 0.0f;
    float scumScaleX = 0.0f;
    float scumScaleY = 0.0f;
    float scumDistortion = 0.0f;
    float invOpacity = 0.0f;
    float glow = 0.0f;
  };
  // The building-interior mask shell active right now, published by the two InteriorMask compositor
  // draws SubmitDraw rejects. Global by nature (a handful of shells, only while a character is inside),
  // so a static transport like the water params. The D3D11 layer must not run ExtractTransforms on these
  // draws (they would claim the same-frame OGRE camera and force the interior to raster), so the shell's
  // object-space bounds and raw worldViewProjMatrix are carried and the placement resolved separately.
  struct KenshiInteriorVolume {
    float objMin[3] = { 0.0f, 0.0f, 0.0f };
    float objMax[3] = { 0.0f, 0.0f, 0.0f };
    // The resolved placement, not the raw WVP: the D3D11 layer divides out Kenshi's own same-frame
    // view-projection, so this is a world transform that depends on no camera. Buildings are static, so it
    // stays correct and the cull volume does not drift.
    Matrix4 objectToWorld;
    // The shell's own triangles, binned. This is the cull volume; the box below is only a broad-phase
    // reject.
    std::shared_ptr<const std::vector<uint32_t>> blob;
    // Kept only as part of the instance identity, never used to place anything.
    Matrix4 worldViewProj;
    uint32_t publishFrame = 0u;
    uint32_t vertexCount = 0u;
    bool valid = false;
  };
  // A list: Kenshi renders an interior for every building with a character inside, and several can be
  // active in one town. Entries accumulate for a frame; the set is replaced when a draw arrives stamped
  // with a newer frame.
  static void registerKenshiInteriorVolume(const KenshiInteriorVolume& volume);
  static std::vector<KenshiInteriorVolume> getKenshiInteriorVolumes();
  // Explicit deactivation, so a building stops being culled on the frame its shell stops being drawn (a
  // staleness window would let a stale camera place the box).
  static void clearKenshiInteriorVolumes();

  // Assemble the active shells into the buffer the hit shader reads and report how many were accepted.
  // Never returns null: the shader declares the binding unconditionally and a null descriptor faults the
  // driver on submit.
  Rc<DxvkBuffer> getKenshiInteriorBuffer(Rc<DxvkContext> ctx);
  void buildKenshiInteriorBuffer(Rc<DxvkContext> ctx);
  uint32_t getKenshiInteriorVolumeCount() const { return m_kenshiInteriorVolumeCount; }

  static void registerKenshiWater(const KenshiWaterParams& params);
  // Scum lives only on the near-water shaders, so it is published separately
  // rather than being overwritten to zero by every distant draw.
  static void registerKenshiWaterScum(float scaleX, float scaleY, float distortion);
  // Rain likewise: only the near-water shaders carry a rainAmount, and the
  // distant one reports zero. Published separately so it cannot zero itself.
  static void registerKenshiWaterRain(float rainAmount);
  static void registerKenshiWaterInvOpacity(float invOpacity);
  static void registerKenshiWaterGlow(float glow);

  // Water biomes. `WaterFPBlend` samples a whole-map 4-channel blend map at the colour map's UV and
  // multiplies its alpha by one channel, chosen by the material's `blendChannel` constant (FS_d77f3f48:
  // `dp4 r0.z, blendSample, icb[blendChannel]`, with `1 - dot(blendSample, 1)` for the unblended base
  // material). Coverage is a continuous function of world position and there are at most five biomes,
  // addressed by that small integer.
  static constexpr uint32_t kKenshiWaterBiomeCount = 5u;

  // The textures that belong to this biome, not to whichever water draw was submitted last. Scum, rain
  // and turbulence exist only on the near shaders while the traced surface is the distant one, so they
  // are harvested per biome and carried in the record, as terrain does for its per-layer maps.
  struct KenshiWaterBiomeTextures {
    TextureRef scum = {};
    TextureRef scumNormal = {};
    TextureRef rain = {};
    TextureRef turbulence = {};
    TextureRef rippleNormal = {};
  };
  static void registerKenshiWaterBiome(uint32_t channel,
                                       const KenshiWaterParams& params,
                                       const KenshiWaterBiomeTextures& textures);

  // The three whole-map water textures. They ride the biome buffer's otherwise unused header words
  // rather than material slots, so the translucent water material needs no texture fields of its own.
  static void registerKenshiWaterGlobalTextures(const TextureRef& colour,
                                                const TextureRef& flow,
                                                const TextureRef& blend);
  Rc<DxvkBuffer> getKenshiWaterZoneBuffer(Rc<DxvkContext> ctx);
  void buildKenshiWaterZoneBuffer(Rc<DxvkContext> ctx);
  Rc<DxvkBuffer> m_kenshiInteriorBuffer;
  uint32_t m_kenshiInteriorVolumeCount = 0u;
  static void getKenshiWater(KenshiWaterParams& params);

  static void registerKenshiWetness(float wetness, float waterHeight);
  static void getKenshiWetness(float& wetness, float& waterHeight);
  // Kenshi's biome dust: colour plus the complete dustAmount vector. The shader family selects .x or .y
  // as strength; .z is the slope bias. Global, same transport as wetness.
  static void registerKenshiDust(const Vector3& colour, float amountX, float amountY, float amountZ);
  static void getKenshiDust(Vector3& colour, float& amountX, float& amountY, float& amountZ);
  static void registerKenshiTerrainBiomeSets(
    LegacyMaterialData& materialData, uint64_t key,
    const std::array<LegacyMaterialData::KenshiTerrainBiomeSet,
                     LegacyMaterialData::kKenshiTerrainBiomeSetCount>& sets);
  uint32_t setKenshiTerrainArgs(const LegacyMaterialData& materialData, bool hasTexcoords);
  static void collectKenshiTerrainResourceLookups(uint32_t frame);
  void collectUnusedTerrainTextures();
  const std::vector<KenshiTerrainArgs>& getKenshiTerrainArgs() const {
    return m_kenshiTerrainArgs;
  }
  // Packed and uploaded once per frame from prepareSceneData. The getter is a pure accessor for the
  // per-pass binds: anything reached from bindCommonRayTracingResources must not mutate per-frame state.
  void buildKenshiTerrainBuffer(Rc<DxvkContext> ctx);
  void ensureKenshiTerrainBuffer(size_t recordCount);
  // Always a valid buffer with at least one entry, so the binding is never
  // left empty even on frames where no scene was ever prepared.
  Rc<DxvkBuffer> getKenshiTerrainBuffer(Rc<DxvkContext> ctx);
  Rc<DxvkBuffer> getKenshiTerrainBloodBuffer(Rc<DxvkContext> ctx);
  void buildKenshiTerrainBloodBuffer(Rc<DxvkContext> ctx);
  Rc<DxvkBuffer> m_kenshiWaterZoneBuffer = nullptr;
  void addKenshiTerrainBloodDecal(const Vector4& rect,
                                  const TextureRef& texture,
                                  const Rc<DxvkSampler>& sampler);
  const std::vector<RtInstance*>& getInstanceTable() const { return m_instanceManager.getInstanceTable(); }
  void onKenshiOriginSnapshot(const kenshi_origin::Snapshot& snapshot);
  
  const InstanceManager& getInstanceManager() const { return m_instanceManager; }
  const AccelManager& getAccelManager() const { return m_accelManager; }
  const LightManager& getLightManager() const { return m_lightManager; }
  const GraphManager& getGraphManager() const { return m_graphManager; }
  const RayPortalManager& getRayPortalManager() const { return m_rayPortalManager; }
  const BindlessResourceManager& getBindlessResourceManager() const { return m_bindlessResourceManager; }
  OpacityMicromapManager* getOpacityMicromapManager() const { return m_opacityMicromapManager.get(); }
  LightManager& getLightManager() { return m_lightManager; }
  GraphManager& getGraphManager() { return m_graphManager; }
  std::unique_ptr<AssetReplacer>& getAssetReplacer() { return m_pReplacer; }
  TerrainBaker& getTerrainBaker() { return *m_terrainBaker.get(); }

  // Scene utility functions
  static Vector3 getSceneUp();
  static Vector3 getSceneForward();
  static Vector3 calculateSceneRight();

  // Reswizzles input vector to an output that has xy coordinates on scene's horizontal axes and z coordinate to be on the scene's vertical axis
  static Vector3 worldToSceneOrientedVector(const Vector3& worldVector); 

  static Vector3 sceneToWorldOrientedVector(const Vector3& sceneVector);

  void addLight(const Dx11LightDesc& light);

  const CameraManager& getCameraManager() const { return m_cameraManager; }
  CameraManager& getCameraManager() { return m_cameraManager; }
  const RtCamera& getCamera() const { return m_cameraManager.getMainCamera(); }
  RtCamera& getCamera() { return m_cameraManager.getMainCamera(); }

  const FogState& getFogState() const { return m_fog; }
  FogState& getFogState() { return m_fog; }
  const fast_unordered_cache<FogState>& getFogStates() const { return m_fogStates; }

  uint32_t getStartInMediumMaterialIndex() { return m_startInMediumMaterialIndex; }
  
  uint32_t getActivePOMCount() {return m_activePOMCount;}

  float getTotalMipBias();
  float getCalculatedUpscalingMipBias();

  // ISceneManager but not really
  void clear(Rc<DxvkContext> ctx, bool needWfi, bool preserveBuiltOmms = false);
  void garbageCollection();
  void prepareSceneData(Rc<RtxContext> ctx, class DxvkBarrierSet& execBarriers);

  void onFrameEnd(Rc<DxvkContext> ctx, bool raytracedThisFrame);

  // GameCapturer
  void triggerUsdCapture() const;
  bool isGameCapturerIdle() const;

  using SamplerIndex = uint32_t;

  void trackTexture(const TextureRef& inputTexture,
                    uint32_t& textureIndex,
                    bool hasTexcoords,
                    bool async = true,
                    uint16_t samplerFeedbackStamp = SAMPLER_FEEDBACK_INVALID);
  [[nodiscard]] SamplerIndex trackSampler(Rc<DxvkSampler> sampler);

  std::optional<XXH64_hash_t> findLegacyTextureHashByObjectPickingValue(uint32_t objectPickingValue);
  std::vector<ObjectPickingValue> gatherObjectPickingValuesByTextureHash(XXH64_hash_t texHash);

  // Replacement material hash tracking
  void trackReplacementMaterialHash(XXH64_hash_t materialHash);
  bool isReplacementMaterialHashUsedThisFrame(XXH64_hash_t materialHash) const;
  uint32_t getReplacementMaterialHashUsageCount(XXH64_hash_t materialHash) const;
  void clearFrameReplacementMaterialHashes();

  // Mesh hash tracking
  void trackMeshHash(XXH64_hash_t meshHash);
  bool isMeshHashUsedThisFrame(XXH64_hash_t meshHash) const;
  uint32_t getMeshHashUsageCount(XXH64_hash_t meshHash) const;
  void clearFrameMeshHashes();

  Rc<DxvkSampler> patchSampler( const VkFilter filterMode,
                                const VkSamplerAddressMode addressModeU,
                                const VkSamplerAddressMode addressModeV,
                                const VkSamplerAddressMode addressModeW,
                                const VkClearColorValue borderColor);

  void requestTextureVramFree();
  void requestVramCompaction();
  void manageTextureVram();

  bool isThinOpaqueMaterialExist() const { return m_thinOpaqueMaterialExist; }
  bool isSssMaterialExist() const { return m_sssMaterialExist; }

  bool isAntiCullingSupported() const { return m_isAntiCullingSupported; }

private:
  enum class ObjectCacheState
  {
    kUpdateInstance = 0,
    kUpdateBVH = 1,
    KBuildBVH = 2,
    kInvalid = -1
  };
  // Handles conversion of geometry data coming from a draw call, to the data used by the raytracing backend
  template<bool isNew>
  ObjectCacheState processGeometryInfo(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, RaytraceGeometry& modifiedGeometryData, bool geometryBufferAllocationOnly = false);

  // Consumes a draw call state and updates the scene state accordingly
  RtInstance* processDrawCallState(Rc<DxvkContext> ctx, 
                                   const DrawCallState& blasInput, 
                                   MaterialData& materialData, 
                                   RtInstance* existingInstance = nullptr,
                                   const RtxParticleSystemDesc* pParticleSystemDesc = nullptr, bool geometryCacheOnly = false);

  const RtSurfaceMaterial& createSurfaceMaterial(const MaterialData& renderMaterialData,
                                                 const DrawCallState& drawCallState,
                                                 uint32_t* out_indexInCache = nullptr);
  RtTranslucentSurfaceMaterial createTranslucentSurfaceMaterial(const TranslucentMaterialData& translucentMaterialData,
                                                                uint32_t samplerIndex,
                                                                bool hasTexcoords);
  Rc<DxvkSampler> getOrCreateExternalSampler();

  // Updates ref counts for new buffers
  void updateBufferCache(RaytraceGeometry& newGeoData);

  // Reports, and optionally repairs, surfaces whose bindless buffer indices were reserved in an earlier
  // frame. Must be called during prepareSceneData before the bindless descriptor table is published,
  // because the repair reserves new indices.
  void auditAndRepairInstanceBufferIndices();

  // Validates the bindless buffer table's entries (are the buffers they name still alive?) rather than
  // the indices into it. Call immediately before the table is published.
  void auditBufferTableEntries();

  // Called whenever a new BLAS scene object is added to the cache
  ObjectCacheState onSceneObjectAdded(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, BlasEntry* pBlas, bool geometryBufferAllocationOnly);
  // Called whenever a BLAS scene object is updated
  ObjectCacheState onSceneObjectUpdated(Rc<DxvkContext> ctx, const DrawCallState& drawCallState, BlasEntry* pBlas, bool geometryBufferAllocationOnly);
  // Called whenever a new instance has been added to the database
  void onInstanceAdded(RtInstance& instance);
  // Called whenever instance metadata is updated
  void onInstanceUpdated(RtInstance& instance, const DrawCallState& drawCall, const MaterialData& material, const bool hasTransformChanged, const bool hasVerticesdChanged, const bool isFirstUpdateThisFrame);
  // Called whenever an instance has been removed from the database
  void onInstanceDestroyed(RtInstance& instance);

  void drawReplacements(Rc<DxvkContext> ctx, const DrawCallState* input, const std::vector<AssetReplacement>* pReplacements, MaterialData& renderMaterialData, ReplacementInstance* replacementInstance);

  void createEffectLight(Rc<DxvkContext> ctx, const DrawCallState& input, const RtInstance* instance);

  // Print all RtInstances for debugging
  void printAllRtInstances();
  
  MaterialData determineMaterialData(const MaterialData* overrideMaterialData, const DrawCallState& input);
  
  const uint32_t kInvalidMaterialCacheIndex = UINT32_MAX;
  uint32_t m_beginUsdExportFrameNum = -1;
  bool m_enqueueDelayedClear = false;
  bool m_previousFrameSceneAvailable = false;

  RtxGlobals m_globals;

  // Hash/Cache's
  InstanceManager m_instanceManager;
  AccelManager m_accelManager;
  LightManager m_lightManager;
  GraphManager m_graphManager;
  RayPortalManager m_rayPortalManager;
  BindlessResourceManager m_bindlessResourceManager;
  std::unique_ptr<OpacityMicromapManager> m_opacityMicromapManager;

  DrawCallCache m_drawCallCache;

  CameraManager m_cameraManager;

  std::unique_ptr<AssetReplacer> m_pReplacer;

  std::unique_ptr<TerrainBaker> m_terrainBaker;

  FogState m_fog;
  fast_unordered_cache<FogState> m_fogStates;
  uint32_t m_startInMediumMaterialIndex = SURFACE_INDEX_INVALID;
  uint32_t m_fogStartInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;
  uint32_t m_externalStartInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;
  uint32_t m_startInMediumMaterialIndex_inCache = kInvalidMaterialCacheIndex;

  // TODO: Move the following resources and getters to RtResources class
  Rc<DxvkBuffer> m_surfaceMaterialBuffer;
  Rc<DxvkBuffer> m_surfaceMaterialExtensionBuffer;
  Rc<DxvkBuffer> m_volumeMaterialBuffer;

  uint32_t m_activePOMCount = 0;
  
  float m_uniqueObjectSearchDistance = 1.f;

  struct DrawCallMetaInfo {
    XXH64_hash_t legacyTextureHash { kEmptyHash };
    XXH64_hash_t legacyTextureHash2 { kEmptyHash };
  };
  struct DrawCallMeta {
    constexpr static inline uint8_t MaxTicks = 2;
    std::unordered_map<ObjectPickingValue, DrawCallMetaInfo> infos[MaxTicks] {};
    bool ready[MaxTicks] {};
    uint8_t ticker {};
    dxvk::mutex mutex {};
  } m_drawCallMeta {};

  // TODO: expand to many different
  Rc<DxvkSampler> m_externalSampler = nullptr;

  std::atomic_bool m_forceFreeTextureMemory = false;
  std::atomic_bool m_forceFreeUnusedDxvkAllocatorChunks = false;

  bool m_thinOpaqueMaterialExist = false;
  bool m_sssMaterialExist = false;

  bool m_isAntiCullingSupported = true;

  // Replacement material hash tracking for current frame (hash -> count)
  std::unordered_map<XXH64_hash_t, uint32_t> m_currentFrameReplacementMaterialHashes;

  // Mesh hash tracking for current frame (hash -> count)
  std::unordered_map<XXH64_hash_t, uint32_t> m_currentFrameMeshHashes;

  DrawCallTracker m_drawCallTracker;
};

}  // namespace nvvk
