#include "../../util/util_kenshi_telemetry.h"
/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_draw_call_tracker.h"
#include "../../util/util_kenshi_origin.h"
#include "../../util/util_kenshi_terrain_profile.h"
#include "rtx_kenshi_options.h"
#include "rtx_kenshi_terrain_options.h"
#include "rtx_kenshi_retention.h"
#include "../../util/util_kenshi_terrain_audit.h"
#include "rtx_instance_manager.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include "rtx_options.h"
#include "rtx_ray_portal_manager.h"
#include "rtx_intersection_test.h"
#include "dxvk_device.h"


namespace dxvk {

  // Native ground vertices carry tile placement. Keep V737 source ownership
  // discrimination, while V739 lets a native origin reset preserve that owner.
  static XXH64_hash_t groundTrackingBucket(XXH64_hash_t topology,
      XXH64_hash_t vertices, const Matrix4& transform, XXH64_hash_t shader,
      uint32_t bones) {
    // XXH3 of the shipped DXBC, matching D3D11CommonShader::GetBytecodeHash:
    // VS_7507b1ef (detailed) and VS_96854ea3 (simple).
    if (bones != 0u || (shader != 0xdf5c7e0b230c7f2eull
                    && shader != 0x3a2f0355b844fbf6ull))
      return topology;
    struct GroundIdentity {
      XXH64_hash_t topology;
      XXH64_hash_t vertices;
      Matrix4 transform;
    } identity {};
    identity.topology = topology;
    identity.vertices = vertices;
    identity.transform = transform;
    // V739: native translation is an origin, not part of ground ownership.
    // Tracking positions are shifted by the authoritative native reset event.
    identity.transform[3].xyz() = Vector3(0.f);
    return XXH3_64bits_withSeed(&identity, sizeof(identity), 0x4b656e7368694772ull);
  }

  // A skeletal pose changes the palette, not the source bind-pose vertices.
  // Material + proximity cannot identify a part: the V731 capture shows one
  // instance changing from 17109 upper-body vertices to 2136 lower-body vertices.
  // Within a topology bucket, require the same source vertices as well.
  static bool sameSkinnedSourceVertices(const ReplacementInstance* candidate,
                                       const ReplacementInstance::LookupKey& key) {
    return !candidate->isSkinned || (key.vertexPositionHash != kEmptyHash &&
      candidate->vertexPositionHash == key.vertexPositionHash);
  }

  // V729: Kenshi's skinned object transform is identity; placement lives in
  // the palette. Equal transforms must not shortcut spatial character tracking.
  static bool sameTrackingPosition(const ReplacementInstance* candidate,
                                  const ReplacementInstance::LookupKey& key) {
    return !candidate->isSkinned ||
      (candidate->centroid.x == key.worldPos.x &&
       candidate->centroid.y == key.worldPos.y &&
       candidate->centroid.z == key.worldPos.z);
  }

  // V730: the global search radius also serves camera travel and can span many
  // copies of a small rigid mesh. Proximity alone at that scale is not ownership.
  // Limit rigid fallback travel to the mesh's world-space bounding diameter;
  // exact identities and skeletal tracking retain their existing behavior.
  static bool withinRigidTrackingExtent(const ReplacementInstance* candidate,
                                       const ReplacementInstance::LookupKey& key) {
    if (candidate->isSkinned || !candidate->geometryBoundingBox.isValid())
      return true;
    const Vector3 size = candidate->geometryBoundingBox.maxPos - candidate->geometryBoundingBox.minPos;
    Vector3 worldSize(0.f);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      worldSize += abs(candidate->objectToWorld[axis].xyz()) * size[axis];
    const float diameterSqr = lengthSqr(worldSize);
    return !std::isfinite(diameterSqr)
      || lengthSqr(candidate->centroid - key.worldPos) <= diameterSqr;
  }

  // Compute which of the lookup-key fields differ from the values currently cached
  // on the supplied ReplacementInstance. Used to populate ri->dirtyFlags after a
  // match/reassociate, so downstream consumers can gate work on what actually changed.
  // Caller is expected to invoke this BEFORE overwriting the RI's cached fields.
  static ReplacementInstance::DirtyFlags computeDirtyFlags(
      const ReplacementInstance* ri, const ReplacementInstance::LookupKey& key) {
    ReplacementInstance::DirtyFlags flags;
    // This function is only called when the full identity hash doesn't match, so something must have changed.
    flags.set(ReplacementInstance::DirtyFlag::Any);
    if (std::memcmp(&ri->objectToWorld, &key.transform, sizeof(Matrix4)) != 0) {
      flags.set(ReplacementInstance::DirtyFlag::Transform);
    }
    if (ri->vertexPositionHash != key.vertexPositionHash) {
      flags.set(ReplacementInstance::DirtyFlag::VertexPosHash);
    }
    if (ri->materialHash != key.materialHash) {
      flags.set(ReplacementInstance::DirtyFlag::MaterialHash);
    }
    return flags;
  }

  // Frustum check for an AABB in the space defined by objectToWorld.
  // Uses SAT (high precision) or fast check based on the RtxOption.
  static bool aabbIntersectsFrustum(
      RtCamera& camera,
      const AxisAlignedBoundingBox& aabb,
      const Matrix4& objectToWorld) {
    const Matrix4 objectToView = camera.getWorldToView(false) * objectToWorld;
    if (RtxOptions::AntiCulling::Object::enableHighPrecisionAntiCulling()) {
      return boundingBoxIntersectsFrustumSAT(
          camera, aabb.minPos, aabb.maxPos, objectToView,
          RtxOptions::AntiCulling::Object::enableInfinityFarFrustum());
    }
    return boundingBoxIntersectsFrustum(camera.getFrustum(), aabb.minPos, aabb.maxPos, objectToView);
  }

  DrawCallTracker::DrawCallTracker(DxvkDevice* device)
    : m_device(device) {
  }

  void DrawCallTracker::recordSkinnedTrackingClaim(const ReplacementInstance* instance) {
    if (!instance->isSkinned || instance->frameLastSeen == m_skinnedTrackingClaimsFrame)
      return;
    m_skinnedTrackingClaims[instance->spatialMapHash].push_back({
      instance->vertexPositionHash, instance->materialHash, instance->centroid });
  }

  XXH64_hash_t DrawCallTracker::computeIdentityHash(const DrawCallState& drawCallState) {
    struct IdentityHashData{
      XXH64_hash_t geoHash;
      XXH64_hash_t matHash;
      XXH64_hash_t boneHash;
      CameraType::Enum cameraType;
      uint32_t categories;
      Matrix4 xform;
    };

    // 0 initialize to avoid any problems caused by padding
    IdentityHashData data{};

    data.geoHash = drawCallState.getGeometryData().getHashForRule(rules::FullGeometryHash);
    data.matHash = drawCallState.getMaterialData().getHash();
    data.xform = drawCallState.getTransformData().objectToWorld;
    data.categories = drawCallState.getCategoryFlags().raw();
    data.boneHash = drawCallState.getSkinningState().boneHash;
    data.cameraType = drawCallState.cameraType;

    return XXH3_64bits(&data, sizeof(data));
  }

  void DrawCallTracker::eraseFromSpatialMap(
      std::unordered_map<XXH64_hash_t, ReplacementSpatialMap>& mapOfMaps,
      XXH64_hash_t bucketKey,
      XXH64_hash_t transformHash,
      const ReplacementInstance* data) {
    auto iter = mapOfMaps.find(bucketKey);
    if (iter != mapOfMaps.end()) {
      iter->second.erase(transformHash, data);
      if (iter->second.size() == 0) {
        mapOfMaps.erase(iter);
      }
    }
  }

  ReplacementInstance* DrawCallTracker::reassociateMatch(
      ReplacementInstance* match,
      const ReplacementInstance::LookupKey& key,
      ReplacementSpatialMap* moveInAssetMap) {
    const XXH64_hash_t prevMat = match->materialHash;
    m_identityHashMap.erase(match->identityHash);
    match->identityHash = key.identityHash;
    match->vertexPositionHash = key.vertexPositionHash;
    match->centroid = key.worldPos;

    if (moveInAssetMap) {
      match->spatialCacheTransformHash = moveInAssetMap->move(
          match->spatialCacheTransformHash, key.worldPos, key.transform, match);
    }

    const float spatialMapCellSize = RtxOptions::uniqueObjectDistance() * 2.f;

    if (prevMat != key.materialHash) {
      eraseFromSpatialMap(m_materialSpatialMaps, prevMat,
          match->materialSpatialCacheTransformHash, match);
      auto matEmplace = m_materialSpatialMaps.try_emplace(key.materialHash, spatialMapCellSize);
      match->materialSpatialCacheTransformHash =
          matEmplace.first->second.insert(key.worldPos, key.transform, match);
      match->materialHash = key.materialHash;
    } else {
      auto matMapIter = m_materialSpatialMaps.find(prevMat);
      if (matMapIter != m_materialSpatialMaps.end()) {
        match->materialSpatialCacheTransformHash = matMapIter->second.move(
            match->materialSpatialCacheTransformHash, key.worldPos, key.transform, match);
      }
      match->materialHash = key.materialHash;
    }

    m_identityHashMap[key.identityHash] = match;
    return match;
  }

  void DrawCallTracker::removeReplacementInstancesWithSpatialMapHash(XXH64_hash_t spatialMapKey) {
    for (size_t i = 0; i < m_replacementInstances.size();) {
      ReplacementInstance* replacementInstance = m_replacementInstances[i].get();
      if (replacementInstance->spatialMapHash == spatialMapKey) {
        destroyReplacementInstance(replacementInstance);
        std::swap(m_replacementInstances[i], m_replacementInstances.back());
        m_replacementInstances.pop_back();
        continue;
      }
      ++i;
    }
  }

  namespace {
    // DX11_V652_IDENTITY_PROBE. Diagnostic only: nothing here changes which
    // ReplacementInstance a draw resolves to.
    //
    // This is the layer that actually decides object identity across frames. The
    // V651 probe measured the layer below it - DrawCallCache - and found ~144
    // "steals" every single frame, but a BlasEntry is one GEOMETRY and N copies
    // of a mesh sharing one BLAS is exactly what instancing is for, so that
    // number was normal and uncorrelated with the flicker. A ReplacementInstance
    // is one OBJECT, so two of them trading places is a real defect.
    //
    // Levels, in the order findOrCreateReplacementInstance tries them:
    //   L1  exact identity hash   - geometry + material + transform all unchanged
    //   L2x exact transform       - same place, vertex positions match
    //   L2s spatial nearest       - DIFFERENT transform, nearest RI within
    //                               rtx.uniqueObjectDistance wins
    //   L25 cross-topology        - as L2s, across topology buckets
    //   L3  new instance
    //
    // L2s and L25 are where two identical objects can swap: the winner is
    // whichever is nearest, and that can change as the scene reorders. Static
    // geometry should be resolving on L1 every frame and never reaching them.
    enum class IdentityLevel : uint32_t { L1 = 0u, L2Exact, L2Spatial, L25Cross, L3New, Count };

    std::atomic<uint32_t> g_identityCounts[uint32_t(IdentityLevel::Count)] = {};
    std::atomic<uint32_t> g_identityFarMatches { 0u };
    std::atomic<uint32_t> g_identityLines { 0u };
    std::atomic<uint32_t> g_identityFrame { 0xFFFFFFFFu };
    std::atomic<uint32_t> g_identityLastSummary { 0u };
    std::atomic<uint32_t> g_identityFarThisFrame { 0u };
    std::atomic<uint32_t> g_identityMaxFarPerFrame { 0u };

    void noteIdentityMatch(IdentityLevel level, uint32_t frameId,
                           float distance, const char* levelName) {
      if (!(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal()))
        return;

      g_identityCounts[uint32_t(level)].fetch_add(1u, std::memory_order_relaxed);

      // Close the previous frame's tally when the frame advances.
      const uint32_t previousFrame = g_identityFrame.exchange(frameId, std::memory_order_relaxed);
      if (previousFrame != frameId) {
        const uint32_t closed = g_identityFarThisFrame.exchange(0u, std::memory_order_relaxed);
        uint32_t prevMax = g_identityMaxFarPerFrame.load(std::memory_order_relaxed);
        while (closed > prevMax
            && !g_identityMaxFarPerFrame.compare_exchange_weak(prevMax, closed, std::memory_order_relaxed)) {
        }
      }

      // A spatial match at a real distance means this object inherited another
      // object's identity, and with it that object's motion vectors and
      // temporal history.
      const float threshold = KenshiOptions::kenshiEntryStealDistance();
      // NB: not named `far` - windows.h still defines that as a legacy macro.
      const bool isFarMatch = (distance > threshold)
                    && (level == IdentityLevel::L2Spatial || level == IdentityLevel::L25Cross);
      if (isFarMatch) {
        g_identityFarMatches.fetch_add(1u, std::memory_order_relaxed);
        g_identityFarThisFrame.fetch_add(1u, std::memory_order_relaxed);

        if (g_identityLines.fetch_add(1u, std::memory_order_relaxed) < 24u) {
          Logger::info(str::format(
            "[DrawCallTracker][identity-migrate] frame=", frameId,
            " level=", levelName,
            " dist=", distance,
            " farThisFrame=", g_identityFarThisFrame.load(std::memory_order_relaxed)));
        }
      }

      // One bounded summary per 600 frames. The shape that confirms the
      // hypothesis is L2s/L25 bursting when the camera moves while L1 drops,
      // not a constant background rate.
      uint32_t lastSummary = g_identityLastSummary.load(std::memory_order_relaxed);
      if (frameId - lastSummary >= 600u
       && g_identityLastSummary.compare_exchange_strong(lastSummary, frameId, std::memory_order_relaxed)) {
        Logger::info(str::format(
          "[DrawCallTracker][identity-migrate] summary: frame=", frameId,
          " L1=", g_identityCounts[0].load(std::memory_order_relaxed),
          " L2exact=", g_identityCounts[1].load(std::memory_order_relaxed),
          " L2spatial=", g_identityCounts[2].load(std::memory_order_relaxed),
          " L25cross=", g_identityCounts[3].load(std::memory_order_relaxed),
          " L3new=", g_identityCounts[4].load(std::memory_order_relaxed),
          " farMatches=", g_identityFarMatches.load(std::memory_order_relaxed),
          " maxFarPerFrame=", g_identityMaxFarPerFrame.load(std::memory_order_relaxed)));
      }
    }
  }

  ReplacementInstance* DrawCallTracker::findOrCreateReplacementInstance(
      const ReplacementInstance::LookupKey& key, bool allowCrossTopologyMatching) {
    const uint32_t currentFrameId = m_device->getCurrentFrameId();
    if (m_skinnedTrackingClaimsFrame != currentFrameId) {
      m_skinnedTrackingClaims.clear();
      m_skinnedTrackingClaimsFrame = currentFrameId;
    }

    // Level 1: exact identity match.
    // Draw calls with the same identity hash (same geometry, material, transform) return
    // the same ReplacementInstance even if already seen this frame. This handles two-pass rendering where
    // the game draws the same mesh twice (e.g., base pass + overlay). The second pass
    // merges into the same instance via mergeInstanceHeuristics in updateInstance.
    auto exactMatchIter = m_identityHashMap.find(key.identityHash);
    if (exactMatchIter != m_identityHashMap.end()) {
      // identityHash includes transform/material/vertex hashes, so all key fields
      // match this RI's cached values by construction. Nothing changed since last
      // submission of this identity.
      //
      // Only clear dirtyFlags on the first lookup of a new frame. A second lookup
      // within the same frame (two-pass rendering) must not clobber flags an earlier
      // path (L2/L2.5) set when first matching this RI for the current frame.
      ReplacementInstance* match = exactMatchIter->second;
      recordSkinnedTrackingClaim(match);
      if (match->frameLastSeen != currentFrameId) {
        match->dirtyFlags = ReplacementInstance::DirtyFlags();
      }
      noteIdentityMatch(IdentityLevel::L1, currentFrameId, 0.0f, "L1identity");
      return match;
    }

    // Level 2: tracking hash (topological, stable for animated geometry) + spatial proximity
    const float uniqueObjectDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();
    const float spatialMapCellSize = RtxOptions::uniqueObjectDistance() * 2.f;

    auto l2Filter = [&](const ReplacementInstance* candidate) {
      return candidate->frameLastSeen != currentFrameId &&
             candidate->materialHash == key.materialHash;
    };

    auto spatialMapIter = m_assetSpatialMaps.find(key.spatialMapHash);
    if (spatialMapIter != m_assetSpatialMaps.end()) {
      // Try exact transform + vertex position hash match first
      ReplacementInstance* exactTransformMatch = nullptr;
      spatialMapIter->second.forEachAtTransform(key.transform, [&](const ReplacementInstance* candidate) {
        if (candidate->vertexPositionHash == key.vertexPositionHash && l2Filter(candidate)
         && sameTrackingPosition(candidate, key)) {
          exactTransformMatch = const_cast<ReplacementInstance*>(candidate);
          return true;
        }
        return false;
      });
      if (exactTransformMatch != nullptr) {
        recordSkinnedTrackingClaim(exactTransformMatch);
        // Compute diff before reassociation. Transform/vertex/spatialMap match
        // by construction here; only material can diverge (l2Filter requires
        // material match, so usually nothing differs at this point).
        exactTransformMatch->dirtyFlags = computeDirtyFlags(exactTransformMatch, key);
        m_identityHashMap.erase(exactTransformMatch->identityHash);
        exactTransformMatch->identityHash = key.identityHash;
        m_identityHashMap[key.identityHash] = exactTransformMatch;
        noteIdentityMatch(IdentityLevel::L2Exact, currentFrameId, 0.0f, "L2exactTransform");
        return exactTransformMatch;
      }

      // Spatial nearest-neighbor search
      float nearestDistSqr = FLT_MAX;
      const ReplacementInstance* nearestMatch = spatialMapIter->second.getNearestData(
        key.worldPos, uniqueObjectDistanceSqr, nearestDistSqr,
        [&](const ReplacementInstance* candidate) {
          if (!l2Filter(candidate) || !withinRigidTrackingExtent(candidate, key)) return false;
          if (!sameSkinnedSourceVertices(candidate, key)) {
            return false;
          }
          return true;
        });

      // V733: filtering claimed owners before the nearest search changes
      // "nearest owner" into "nearest unused owner". The V732 capture contains
      // chains where a draw lands within 0.01 units of an already-consumed
      // owner's old position, then inherits a different owner's distant history.
      // Preserve the old claim through the frame; ambiguity gets fresh history.
      if (nearestMatch != nullptr && nearestMatch->isSkinned) {
        auto claims = m_skinnedTrackingClaims.find(key.spatialMapHash);
        if (claims != m_skinnedTrackingClaims.end()) {
          for (const auto& claim : claims->second) {
            if (claim.vertexPositionHash == key.vertexPositionHash &&
                claim.materialHash == key.materialHash &&
                lengthSqr(claim.previousPosition - key.worldPos) <= nearestDistSqr) {
              nearestMatch = nullptr;
              break;
            }
          }
        }
      }

      if (nearestMatch != nullptr) {
        // Compute diff before reassociation overwrites the cached fields. The
        // transform differs (otherwise we would have hit the exact-transform
        // branch above); other fields may also have changed.
        ReplacementInstance* match = const_cast<ReplacementInstance*>(nearestMatch);
        recordSkinnedTrackingClaim(match);
        match->dirtyFlags = computeDirtyFlags(match, key);
        noteIdentityMatch(IdentityLevel::L2Spatial, currentFrameId,
                          std::sqrt(nearestDistSqr), "L2spatial");
        return reassociateMatch(match, key, &spatialMapIter->second);
      }
    }

    // Level 2.5: cross-topology fallback via material spatial index.
    // When animated geometry uses different index buffers per frame, both components of
    // the tracking hash (Indices + GeometryDescriptor) change, so the RI lives in a
    // different asset spatial map bucket. The material spatial map indexes all RIs by
    // material hash regardless of topology, allowing O(1) bucket lookup + spatial search.
    // Skipped for non-skinned game draws: identical static modular meshes share material +
    // topology class but must not steal each other's ReplacementInstance across buckets.
    if (allowCrossTopologyMatching) {
      auto frameFilter = [&](const ReplacementInstance* candidate) {
        return candidate->frameLastSeen != currentFrameId;
      };

      auto matMapIter = m_materialSpatialMaps.find(key.materialHash);
      if (matMapIter != m_materialSpatialMaps.end()) {
        float nearestDistSqr = FLT_MAX;
        const ReplacementInstance* crossMatch = matMapIter->second.getNearestData(
          key.worldPos, uniqueObjectDistanceSqr, nearestDistSqr, frameFilter);

        if (crossMatch != nullptr) {
          ReplacementInstance* bestMatch = const_cast<ReplacementInstance*>(crossMatch);
          recordSkinnedTrackingClaim(bestMatch);

          // Compute diff before any cached field is overwritten. SpatialMapHash
          // typically differs here (cross-topology entry point), and the
          // transform usually differs too.
          bestMatch->dirtyFlags = computeDirtyFlags(bestMatch, key);

          // Migrate the RI from the old spatial map to the new one.
          eraseFromSpatialMap(m_assetSpatialMaps, bestMatch->spatialMapHash,
              bestMatch->spatialCacheTransformHash, bestMatch);

          // NOTE: overriding existing 'spatialMapHash',
          //       so extra care required if 'spatialMapHash' should be immutable, mainly for remixapi_MeshHandle
          bestMatch->spatialMapHash = key.spatialMapHash;
          auto [newMapIter, inserted] = m_assetSpatialMaps.try_emplace(key.spatialMapHash, spatialMapCellSize);
          bestMatch->spatialCacheTransformHash =
              newMapIter->second.insert(key.worldPos, key.transform, bestMatch);

          noteIdentityMatch(IdentityLevel::L25Cross, currentFrameId,
                            std::sqrt(nearestDistSqr), "L25crossTopology");
          return reassociateMatch(bestMatch, key, nullptr);
        }
      }
    }

    // Level 3: no match — create a new ReplacementInstance.
    auto newReplacementInstance = std::make_unique<ReplacementInstance>(
        key, m_nextReplacementInstanceId++, currentFrameId);
    ReplacementInstance* replacementInstance = newReplacementInstance.get();

    m_identityHashMap[key.identityHash] = replacementInstance;

    auto [mapIter, inserted] = m_assetSpatialMaps.try_emplace(key.spatialMapHash, spatialMapCellSize);
    replacementInstance->spatialCacheTransformHash =
        mapIter->second.insert(key.worldPos, key.transform, replacementInstance);

    auto [matMapIter, matInserted] = m_materialSpatialMaps.try_emplace(key.materialHash, spatialMapCellSize);
    replacementInstance->materialSpatialCacheTransformHash =
        matMapIter->second.insert(key.worldPos, key.transform, replacementInstance);

    m_replacementInstances.push_back(std::move(newReplacementInstance));

    noteIdentityMatch(IdentityLevel::L3New, currentFrameId, 0.0f, "L3new");
    return replacementInstance;
  }

  ReplacementInstance* DrawCallTracker::findOrCreateReplacementInstance(
      const DrawCallState& drawCallState,
      const MaterialData& materialData,
      const RayPortalManager& rayPortalManager) {
    // Tracking hash uses topology only (indices + geometry descriptor), matching how the
    // baseline grouped instances by BlasEntry (topological hash). The material hash is used
    // as a filter in the spatial search, not as part of the key — this allows matching
    // even when the material changes between frames (LOD, texture animation).
    const auto& hashes = drawCallState.getGeometryData().hashes;
    const Matrix4& objectToWorld = drawCallState.getTransformData().objectToWorld;
    Vector3 trackingPosition = drawCallState.getGeometryData().boundingBox.getTransformedCentroid(objectToWorld);
    const auto& skin = drawCallState.getSkinningState();
    if (skin.numBones > 0u && skin.minBoneIndex < skin.pBoneMatrices.size()) {
      const Vector4 bonePosition = objectToWorld * skin.pBoneMatrices[skin.minBoneIndex][3];
      trackingPosition = Vector3(bonePosition.x, bonePosition.y, bonePosition.z);
    }

    const ReplacementInstance::LookupKey key {
      computeIdentityHash(drawCallState),
      groundTrackingBucket(hashes.getHashForRule<rules::TopologicalHash>(),
        hashes[HashComponents::VertexPosition], objectToWorld,
        drawCallState.programmableVertexShaderBytecodeHash, skin.numBones),
      materialData.getHash(),
      hashes[HashComponents::VertexPosition],
      trackingPosition,
      objectToWorld
    };

    // V732: a material bucket is not a character-part identity. Multiple parts
    // share the same material and bone anchor, so L2.5 can steal a torso for a
    // leg draw even without camera/character travel. Changed topology already
    // requires fresh geometry in DrawCallCache; give it a fresh instance too.
    // The separate light/external callers retain their explicit matching policy.
    const bool allowCrossTopology = false;
    ReplacementInstance* result = findOrCreateReplacementInstance(key, allowCrossTopology);
    if (kenshi_origin::state.owner == m_device && kenshi_origin::finite(kenshi_origin::state.snapshot)
        && drawCallState.programmableVertexShaderBytecodeHash != 0) {
      kenshi_origin::state.nativeTracking.insert(result);
      if (kenshi_origin::state.eventFrame == m_device->getCurrentFrameId()) {
        result->dirtyFlags.set(ReplacementInstance::DirtyFlag::Transform);
        result->dirtyFlags.set(ReplacementInstance::DirtyFlag::Any);
      }
    }

    // Portal-aware matching for ViewModel draw calls: if the normal lookup created a
    // brand-new ReplacementInstance (no existing prims), try matching through portals.
    if (result->prims.empty() &&
        drawCallState.cameraType == CameraType::ViewModel &&
        RtxOptions::useRayPortalVirtualInstanceMatching()) {
      ReplacementInstance* portalResult = tryPortalMatch(result, key, rayPortalManager);
      if (portalResult != nullptr) {
        return portalResult;
      }
    }

    return result;
  }

  ReplacementInstance* DrawCallTracker::findReplacementInstanceByIdentity(XXH64_hash_t identityHash) {
    auto it = m_identityHashMap.find(identityHash);
    return (it != m_identityHashMap.end()) ? it->second : nullptr;
  }

  ReplacementInstance* DrawCallTracker::tryPortalMatch(
      ReplacementInstance* newInstance,
      const ReplacementInstance::LookupKey& key,
      const RayPortalManager& rayPortalManager) {
    const uint32_t currentFrameId = m_device->getCurrentFrameId();
    const float uniqueObjectDistanceSqr = RtxOptions::getUniqueObjectDistanceSqr();

    auto spatialMapIter = m_assetSpatialMaps.find(key.spatialMapHash);
    if (spatialMapIter == m_assetSpatialMaps.end()) {
      return nullptr;
    }

    auto portalFilter = [&](const ReplacementInstance* candidate) {
      return candidate != newInstance &&
             candidate->frameLastSeen != currentFrameId &&
             candidate->materialHash == key.materialHash;
    };

    for (auto& rayPortalPair : rayPortalManager.getRayPortalPairInfos()) {
      if (!rayPortalPair.has_value()) {
        continue;
      }
      for (uint32_t portalIdx = 0; portalIdx < 2; portalIdx++) {
        const auto& rayPortal = rayPortalPair->pairInfos[portalIdx];
        const Vector3 virtualPos = rayPortalManager.getVirtualPosition(
            key.worldPos, rayPortal.portalToOpposingPortalDirection);

        float nearestDistSqr = FLT_MAX;
        const ReplacementInstance* portalMatch = spatialMapIter->second.getNearestData(
            virtualPos, uniqueObjectDistanceSqr, nearestDistSqr, portalFilter);

        if (portalMatch != nullptr) {
          destroyReplacementInstance(newInstance);
          // When this code was written, newInstance would always be the last element
          // of m_replacementInstances. If this assert fires, that assumption is no
          // longer true. Either restore that assumption, or change this code to search
          // for the correct ReplacementInstance to remove.
          assert(m_replacementInstances.back().get() == newInstance &&
                 "tryPortalMatch: newInstance must be the last element");
          m_replacementInstances.pop_back();

          return reassociateMatch(
              const_cast<ReplacementInstance*>(portalMatch),
              key, &spatialMapIter->second);
        }
      }
    }

    return nullptr;
  }

  void DrawCallTracker::destroyReplacementInstance(ReplacementInstance* replacementInstance) {
    kenshi_origin::state.nativeTracking.erase(replacementInstance);
    terrain_profile::forgetOwner(replacementInstance);
    if (!replacementInstance) {
      return;
    }

    // Fixed-size scalar history only, so a capture can inspect removals that
    // preceded an already-visible hole. No views, buffers or instance pointers.
    for (const auto& prim : replacementInstance->prims) {
      const auto* instance = prim.getInstance();
      const auto* geometry = instance ? instance->getBlas() : nullptr;
      if (!geometry) continue;
      const uint32_t lod = terrain_audit::family(geometry->input.programmableVertexShaderBytecodeHash);
      if (lod) terrain_audit::removal(m_device->getCurrentFrameId(), lod, replacementInstance->id,
        m_device->getCurrentFrameId() - std::min(m_device->getCurrentFrameId(), replacementInstance->frameLastSeen),
        replacementInstance->geometryBoundingBox.getTransformedCentroid(replacementInstance->objectToWorld));
    }

    m_identityHashMap.erase(replacementInstance->identityHash);

    eraseFromSpatialMap(m_assetSpatialMaps, replacementInstance->spatialMapHash,
        replacementInstance->spatialCacheTransformHash, replacementInstance);
    eraseFromSpatialMap(m_materialSpatialMaps, replacementInstance->materialHash,
        replacementInstance->materialSpatialCacheTransformHash, replacementInstance);

    replacementInstance->clear();
  }

  void DrawCallTracker::garbageCollectReplacementInstances(RtCamera& camera, bool isAntiCullingSupported) {
    const uint32_t currentFrame = m_device->getCurrentFrameId();
    const uint32_t numFramesToKeepObjects = RtxOptions::numFramesToKeepInstances();
    const uint32_t numFramesToKeepLights = RtxOptions::AntiCulling::Light::numFramesToExtendLightLifetime();

    const bool objectAntiCullingEnabled = RtxOptions::AntiCulling::isObjectAntiCullingEnabled();
    const bool lightAntiCullingEnabled = RtxOptions::AntiCulling::isLightAntiCullingEnabled();
    const bool isCameraCut = camera.isCameraCut();
    const bool forceGC = (m_replacementInstances.size() >=
        RtxOptions::AntiCulling::Object::numObjectsToKeep());

    // Native hooks submit nearby off-screen objects continuously. The Remix
    // fallback must not retain missing objects outside those radii indefinitely.
    // Keep this state off the device/bridge layout and on the owning CS thread.
    using RetentionClock = std::chrono::steady_clock;
    struct RetentionState {
      const DxvkDevice* device = nullptr;
      kenshi_retention::Pressure pressure;
      RetentionClock::time_point budgetCheck{}, logTime{};
      uint64_t used = 0, budget = 0, rangeReleased = 0, pressureReleased = 0;
    };
    static thread_local RetentionState retention;
    if (retention.device != m_device) { retention = {}; retention.device = m_device; }
    const auto now = RetentionClock::now();
    const auto& origin = kenshi_origin::state;
    const bool rangeReady = isAntiCullingSupported && !isCameraCut
      && origin.owner == m_device && kenshi_origin::finite(origin.snapshot)
      && origin.snapshotFrame == currentFrame && origin.eventFrame != currentFrame
      && camera.getLastUpdateFrame() == currentFrame;
    if (rangeReady && now - retention.budgetCheck >= std::chrono::seconds(1)) {
      retention.budgetCheck = now;
      const auto props = m_device->adapter()->memoryProperties();
      const auto memory = m_device->adapter()->getMemoryHeapInfo();
      retention.used = retention.budget = 0;
      for (uint32_t heap = 0; heap < props.memoryHeapCount; ++heap) {
        if (!(props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            || !memory.heaps[heap].memoryBudget) continue;
        retention.used += memory.heaps[heap].memoryAllocated;
        retention.budget += memory.heaps[heap].memoryBudget;
      }
      retention.pressure.update(retention.used, retention.budget);
    }
    const float groundRadius = kenshi_retention::radius(KenshiTerrainOptions::antiCulling(),
      KenshiTerrainOptions::antiCullingRadius(), true);
    // The tracker does not carry the native OGRE class (prop versus landscape
    // decoration). Use the larger enabled non-ground radius, never the smaller.
    const float objectRadius = std::max(
      kenshi_retention::radius(KenshiOptions::kenshiInstanceAntiCulling(), KenshiOptions::kenshiInstanceAntiCullingRadius(), false),
      kenshi_retention::radius(KenshiOptions::kenshiTerrainFeatureAntiCulling(), KenshiOptions::kenshiTerrainFeatureAntiCullingRadius(), false));
    const Vector3 cameraPosition = camera.getPosition(false);
    struct Candidate { ReplacementInstance* owner; uint32_t unseen; double distanceSqr; };
    std::vector<Candidate> candidates;
    uint32_t nearProtected = 0, invalidBounds = 0, unknownGeometry = 0;

    for (size_t i = 0; i < m_replacementInstances.size();) {
      ReplacementInstance* replacementInstance = m_replacementInstances[i].get();

      const bool hasLights = replacementInstance->lightBoundingBox.isValid();
      if (terrain_profile::purgeOwner(replacementInstance)) {
        terrain_profile::count(terrain_profile::TerrainPurged);
        destroyReplacementInstance(replacementInstance);
        std::swap(m_replacementInstances[i], m_replacementInstances.back());
        m_replacementInstances.pop_back();
        continue;
      }
      const bool hasMeshes = replacementInstance->geometryBoundingBox.isValid();

      if (replacementInstance->frameLastSeen + numFramesToKeepObjects <= currentFrame) {
        bool keepAlive = false;

        // Only anti-cull RIs that have been matched at least once after creation.
        const bool isStable = (replacementInstance->frameLastSeen > replacementInstance->frameCreated);

        if (!isCameraCut && isStable) {
          // Skinned and player model objects should always be GC'd when stale —
          // anti-culling them produces frozen poses or wrong positions.
          // Objects tagged IgnoreAntiCulling by the game config are also exempt.
          // Translation-animated objects (transform changed in their last update)
          // are also exempt: anti-culling them freezes them mid-motion at the
          // last-seen position, which is rarely the right answer for a moving
          // entity that the game has finished drawing.
          // Note: the old system also exempted spritesheet-animated objects, but that
          // was likely a conservative safeguard — spritesheet state isn't broken by
          // anti-culling the way skeletal pose is.
          const CategoryFlags categories(replacementInstance->categoryFlags);
          const bool wasMovingWhenLastSeen =
              replacementInstance->dirtyFlags.test(ReplacementInstance::DirtyFlag::Transform);
          const bool exemptFromAntiCulling =
              replacementInstance->isSkinned ||
              categories.test(InstanceCategories::ThirdPersonPlayerModel) ||
              categories.test(InstanceCategories::IgnoreAntiCulling) ||
              wasMovingWhenLastSeen;

          // Object anti-culling: keep objects that are OUTSIDE the camera frustum.
          // If the game stopped submitting an object that's clearly visible, it
          // likely had a valid reason (destruction, LOD swap, etc.) — allow GC.
          // Objects outside the frustum may have been wrongly culled by the game's
          // own frustum (which doesn't match ours) and should be preserved.
          if (!keepAlive && objectAntiCullingEnabled && !forceGC
              && isAntiCullingSupported && hasMeshes && !exemptFromAntiCulling) {
            keepAlive = !aabbIntersectsFrustum(
                camera,
                replacementInstance->geometryBoundingBox,
                replacementInstance->objectToWorld);
          }

          // Light anti-culling for mesh replacement lights. Mirrors the old
          // LightManager MeshReplacement behavior: isDynamic lights inside
          // the frustum were GC'd immediately — only lights OUTSIDE the
          // camera frustum were protected, and only within the extended
          // lifetime window. Uses the camera frustum (not the wider light
          // anti-culling frustum) since the check is position-based like
          // object anti-culling.
          if (!keepAlive && lightAntiCullingEnabled && hasLights
              && isAntiCullingSupported && !exemptFromAntiCulling && !forceGC) {
            const bool withinExtendedLifetime =
                replacementInstance->frameLastSeen + numFramesToKeepLights > currentFrame;
            if (withinExtendedLifetime) {
              keepAlive = !aabbIntersectsFrustum(
                  camera,
                  replacementInstance->lightBoundingBox,
                  replacementInstance->objectToWorld);
            }
          }
        }

        if (keepAlive && rangeReady && hasMeshes && !hasLights
            && origin.nativeTracking.count(replacementInstance)
            && replacementInstance->frameLastSeen <= currentFrame
            && currentFrame - replacementInstance->frameLastSeen >= kenshi_retention::pressureGraceFrames) {
          bool known = false;
          float radius = 0.f;
          for (const auto& prim : replacementInstance->prims) {
            const auto* instance = prim.getInstance();
            const auto* geometry = instance ? instance->getBlas() : nullptr;
            if (!geometry) continue;
            known = true;
            const auto shader = geometry->input.programmableVertexShaderBytecodeHash;
            const bool ground = shader == 0xdf5c7e0b230c7f2eull || shader == 0x3a2f0355b844fbf6ull;
            radius = std::max(radius, ground ? groundRadius : objectRadius);
          }
          if (!known) ++unknownGeometry;
          else {
            const double distanceSqr = kenshi_retention::distanceSquared(
              replacementInstance->geometryBoundingBox.minPos, replacementInstance->geometryBoundingBox.maxPos,
              replacementInstance->objectToWorld, cameraPosition);
            if (distanceSqr < 0) ++invalidBounds;
            else if (distanceSqr <= double(radius) * radius) ++nearProtected;
            else if (kenshi_retention::eligible(distanceSqr, radius, currentFrame,
                       replacementInstance->frameLastSeen, retention.pressure.active)) {
              candidates.push_back({replacementInstance, currentFrame - replacementInstance->frameLastSeen, distanceSqr});
            }
          }
        }

        if (!keepAlive) {
          destroyReplacementInstance(replacementInstance);
          std::swap(m_replacementInstances[i], m_replacementInstances.back());
          m_replacementInstances.pop_back();
          continue;
        }
      }
      ++i;
    }

    const size_t releaseCount = std::min<size_t>(candidates.size(), retention.pressure.active
      ? kenshi_retention::pressureReleaseLimit : kenshi_retention::normalReleaseLimit);
    if (releaseCount) {
      // Submission recency, not original admission age. All candidates are outside
      // the protected range; use distance to break equally old candidates' ties.
      std::partial_sort(candidates.begin(), candidates.begin() + releaseCount, candidates.end(),
        [](const Candidate& a, const Candidate& b) {
          return a.unseen != b.unseen ? a.unseen > b.unseen : a.distanceSqr > b.distanceSqr;
        });
      std::unordered_set<ReplacementInstance*> selected;
      for (size_t i = 0; i < releaseCount; ++i) selected.insert(candidates[i].owner);
      for (size_t i = 0; i < m_replacementInstances.size();) {
        auto* owner = m_replacementInstances[i].get();
        if (selected.count(owner)) {
          // Existing path clears spatial maps/back-pointers and marks instances
          // for GC. Geometry follows when unlinked; GPU command owners stay alive.
          terrain_audit::removalIsRange = true;
          destroyReplacementInstance(owner);
          terrain_audit::removalIsRange = false;
          std::swap(m_replacementInstances[i], m_replacementInstances.back());
          m_replacementInstances.pop_back();
        } else ++i;
      }
      if (retention.pressure.active) retention.pressureReleased += releaseCount;
      else retention.rangeReleased += releaseCount;
    }
    if (now - retention.logTime >= std::chrono::seconds(5)) {
      retention.logTime = now;
      KENSHI_DIAGNOSTIC_INFO(str::format("[RetainedGeometry V778] frame=", currentFrame,
        " rangeReady=", rangeReady, " pressure=", retention.pressure.active,
        " groundRadius=", groundRadius, " objectRadius=", objectRadius,
        " rangeReleased=", retention.rangeReleased, " pressureReleased=", retention.pressureReleased,
        " farPending=", candidates.size() - releaseCount, " nearProtected=", nearProtected,
        " invalidBounds=", invalidBounds, " unknownGeometry=", unknownGeometry,
        " heapUsedMiB=", retention.used >> 20, " heapBudgetMiB=", retention.budget >> 20));
    }
  }

  void DrawCallTracker::clear() {
    if (kenshi_origin::state.owner == m_device) {
      kenshi_origin::state.nativeTracking.clear();
      kenshi_origin::state.retainedMotion.clear();
    }
    m_skinnedTrackingClaims.clear();
    m_skinnedTrackingClaimsFrame = ~0u;
    if (terrain_profile::enabled())
      for (const auto& instance : m_replacementInstances) terrain_profile::forgetOwner(instance.get());
    m_identityHashMap.clear();
    m_assetSpatialMaps.clear();
    m_materialSpatialMaps.clear();
    m_replacementInstances.clear();
  }

  void DrawCallTracker::rebuildSpatialMaps(float cellSize) {
    for (auto& [hash, spatialMap] : m_assetSpatialMaps) {
      spatialMap.rebuild(cellSize);
    }
    for (auto& [hash, spatialMap] : m_materialSpatialMaps) {
      spatialMap.rebuild(cellSize);
    }
  }

  void DrawCallTracker::rebaseKenshiTracking(const Vector3& shift) {
    // Move only owners observed through native shader draws. Geometry buffers
    // and surface history remain untouched; current submissions update normally.
    for (const auto& entry : m_replacementInstances) {
      auto* ri = entry.get();
      if (!kenshi_origin::state.nativeTracking.count(ri)) continue;
      m_identityHashMap.erase(ri->identityHash); // Raw transform hash is now stale.
      ri->identityHash = kEmptyHash; // Later GC must not erase another owner's new raw key.
      ri->centroid += shift;
      if (!ri->isSkinned) ri->objectToWorld[3].xyz() += shift;
      auto asset = m_assetSpatialMaps.find(ri->spatialMapHash);
      if (asset != m_assetSpatialMaps.end())
        ri->spatialCacheTransformHash = asset->second.move(ri->spatialCacheTransformHash, ri->centroid, ri->objectToWorld, ri);
      auto material = m_materialSpatialMaps.find(ri->materialHash);
      if (material != m_materialSpatialMaps.end())
        ri->materialSpatialCacheTransformHash = material->second.move(ri->materialSpatialCacheTransformHash, ri->centroid, ri->objectToWorld, ri);
    }
    m_skinnedTrackingClaims.clear();
  }

}  // namespace dxvk
