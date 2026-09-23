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

  // DX11_V398_KENSHI_TERRAIN_PER_MATERIAL: every distinct terrain parameter set
  // in the scene, content-addressed so a set keeps its index across frames and
  // a cached surface material can store it. See setKenshiTerrainArgs.
  std::vector<KenshiTerrainArgs> m_kenshiTerrainArgs;

  // DX11_V525. Kenshi's ground blood decals for this frame. Global rather than
  // per-surface: a terrain chunk can carry several, and the raster pass draws one
  // per (chunk x splat), so associating each draw with a chunk buys nothing the
  // hit shader's rect test does not already do.
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

  // DX11_V330_CENSUS_ON_DEMAND: arm the VRAM/scene census for the next `frames`
  // frame ends. The census reports the only numbers that say what the RT scene
  // actually CONTAINS (instances, geoEntries, frameBuffers, asMiB), and its
  // periodic schedule spends itself during loading and then goes quiet for
  // ~18000 frames - so every sample ever taken has come from the load, never
  // from the gameplay frame being looked at. Driven from the D3D11 bridge's
  // Ctrl+Alt+O hotkey so a run of consecutive frames is recorded around the
  // chosen moment; a run rather than one frame because the question is
  // frame-to-frame churn, which a single sample cannot show.
  //
  // Static (no instance needed, adds no data member) and backed by an atomic
  // counter, so the bridge can arm it from the submit thread without reaching
  // through a SceneManager it does not own.
  static void requestVramCensus(uint32_t frames);

  // DX11_V335_REPRESENTATION_SWITCH: pushed from the D3D11 bridge each frame -
  // how many draws reached the RT scene without a position capture, and so
  // carry a different geometry representation than on frames where the capture
  // succeeded. Reported on the census line beside the stable counters.
  static void reportSubmitWithoutCapture(uint32_t count);

  // DX11_V337_STALE_REUSE: draws served from an earlier frame's captured bytes.
  static void reportStaleCaptureReuse(uint32_t count);

  // DX11_V338_STALE_SHAPE: mean/max age in frames of reused capture bytes, and
  // the number of draws truncated to fit a shorter older capture.
  static void reportStaleCaptureShape(uint32_t ageSum, uint32_t ageMax, uint32_t clamped);

  // DX11_V343_CAPTURE_GATE: per-frame capture outcome. The unstable geometry
  // blinks as a CLASS and in sync, which is a frame-level gate; these counters
  // exist only in the submit summary, which prints every few seconds and so can
  // never show whether a single frame's captures collectively failed.
  static void reportCaptureStats(uint32_t attempts, uint32_t captured,
                                 uint32_t budgetRejected, uint32_t failed);

  // DX11_V349_INJECT_GATE: 0 = RTX injected, 1 = no real scene draw preceded
  // the UI draw, 2 = UI target was not the full output, 3 = both, 4 = the UI
  // path was never reached. Non-zero means the frame reached the screen as
  // pure game raster.
  static void reportInjectSkipReason(uint32_t reason);

  // DX11_V350_DRAW_TRACE: arm per-draw scene-side tracing for `frames` frames.
  // Emits the cache decision the D3D11 bridge cannot see, keyed by drawCallID
  // so the two halves join into one row per draw per frame.
  static void requestDrawTrace(uint32_t frames);

  const std::vector<Rc<DxvkSampler>>& getSamplerTable() const { return m_samplerCache.getObjectTable(); }
  const std::vector<RaytraceBuffer>& getBufferTable() const { return m_bufferCache.getObjectTable(); }

  static void registerKenshiTerrainNormals(
    LegacyMaterialData& materialData,
    const std::array<TextureRef, LegacyMaterialData::kKenshiTerrainLayerCount>& normalTextures,
    const Vector4& textureFade);
  // DX11_V528: the neighbouring biome sets for one boundary material, keyed by
  // their own content so two tiles that meet the same biomes share one entry.
  // DX11_V540: the game's weather wetness and water level, published by
  // whichever draw last declared them. Global by nature, so a static pair
  // rather than anything per-material - the same shape as the terrain normal
  // side table above.
  // DX11_V591: Kenshi's water constants, same last-draw-wins transport as
  // wetness. One water material exists in the game; its values are per-zone.
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
  // DX11_V636_KENSHI_INTERIOR_CLIP: the building-interior mask shell that is
  // active right now, published by the two InteriorMask compositor draws that
  // SubmitDraw rejects. Same last-draw-wins static transport as the water
  // params below, because it is global by nature - at most a handful of shells
  // are active and only while a character is inside.
  //
  // The shell's OBJECT-space bounds and the raw worldViewProjMatrix Kenshi fed
  // its vertex shader are carried separately: the D3D11 layer must not run
  // ExtractTransforms on these draws (they would claim the same-frame OGRE
  // camera and force the whole interior to raster - chapter 03), so the world
  // placement is reconstructed later, in RtxContext, where a resolved camera
  // exists to divide the view-projection back out.
  struct KenshiInteriorVolume {
    float objMin[3] = { 0.0f, 0.0f, 0.0f };
    float objMax[3] = { 0.0f, 0.0f, 0.0f };
    // V642: the RESOLVED placement, not the raw WVP. The D3D11 layer now divides
    // out Kenshi's own view-projection from the same frame, so what arrives here
    // is already a world transform and depends on no camera at all. Buildings are
    // static, so once resolved this matrix is correct indefinitely - which is
    // what stops the cull volume drifting when the camera moves.
    Matrix4 objectToWorld;
    // V650: the shell's own triangles, binned. This IS the cull volume; the box
    // below is only a broad-phase reject.
    std::shared_ptr<const std::vector<uint32_t>> blob;
    // Kept only as part of the instance identity, never used to place anything.
    Matrix4 worldViewProj;
    uint32_t publishFrame = 0u;
    uint32_t vertexCount = 0u;
    bool valid = false;
  };
  // V638: a LIST, not one. Kenshi renders an interior for every building that
  // has a character inside it, and V637 measured two shells alternating frame to
  // frame in one town. Entries accumulate for a frame and the set is replaced
  // whenever a draw arrives stamped with a newer frame.
  static void registerKenshiInteriorVolume(const KenshiInteriorVolume& volume);
  static std::vector<KenshiInteriorVolume> getKenshiInteriorVolumes();
  // Explicit deactivation, so a building stops being culled the frame its shell
  // stops being drawn. V641 relied on frame-staleness for this, which forced a
  // slack window that in turn let a stale camera place the box.
  static void clearKenshiInteriorVolumes();

  // V650: assemble the active shells into the buffer the hit shader reads, and
  // report how many were accepted. Never returns null - the shader declares the
  // binding unconditionally and a null descriptor faults the driver on submit.
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

  // DX11_V618_KENSHI_WATER_BIOMES. Replaces V603's rect table.
  //
  // Kenshi's water is not divided by rectangles - that was a reconstruction of
  // something the game publishes directly. `WaterFPBlend` samples a whole-map
  // 4-channel BLEND MAP at the colour map's own UV and multiplies its alpha by
  // one channel of it, chosen by the material's `blendChannel` constant
  // (disassembly of FS_d77f3f48: `dp4 r0.z, blendSample, icb[blendChannel]`,
  // with `1 - dot(blendSample, 1)` for the unblended base material). So biome
  // coverage is a continuous function of world position and there are at most
  // five biomes, addressed by a small integer.
  //
  // Keying on that integer instead of on a rect removes, in one change: the
  // drift of rects stored in the game's moving render frame, the duplicate
  // entries a drift produced, the first-match-wins staleness, the 64-slot
  // budget, and the contamination that survived a save load.
  static constexpr uint32_t kKenshiWaterBiomeCount = 5u;

  // DX11_V619: the textures that belong to THIS biome, not to whichever water
  // draw happened to be submitted last. Scum, rain and turbulence exist only on
  // the near shaders, and the traced surface is the distant one, so they have to
  // be harvested per biome and carried in the record - the same shape terrain
  // uses for its per-layer maps.
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

  // DX11_V623: the three WHOLE-MAP water textures. They ride the biome buffer's
  // otherwise-unused header words rather than material slots, so the translucent
  // water material needs no texture fields of its own.
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
  // DX11_V557/V573: Kenshi's biome dust - colour plus the complete dustAmount
  // vector. The shader family selects either .x or .y as strength; .z is the
  // slope bias. Global, same transport as wetness.
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
  // DX11_V530: packed and uploaded ONCE per frame from prepareSceneData. The
  // getter is a pure accessor for the twelve per-pass binds - see the V527 rule
  // that anything reached from bindCommonRayTracingResources must not mutate
  // per-frame state.
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

  // DX11_V655_BINDLESS_SURFACE_AUDIT. Reports, and optionally repairs, surfaces
  // whose bindless buffer indices were reserved in an earlier frame. Must be
  // called during prepareSceneData BEFORE the bindless descriptor table is
  // published, because the repair reserves new indices.
  void auditAndRepairInstanceBufferIndices();

  // DX11_V656_BUFFER_TABLE_AUDIT. Validates the bindless buffer table's ENTRIES
  // (are the buffers they name still alive?) rather than the indices into it.
  // Call immediately before the table is published.
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
