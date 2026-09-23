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
#include "rtx_draw_call_cache.h"
#include "../d3d11/d3d11_state.h"
#include "rtx_kenshi_options.h"

#include <atomic>

namespace dxvk 
{

namespace {
  bool postVsCaptureIdentityMatch(const DrawCallState& drawCall, const BlasEntry& blas) {
    const XXH64_hash_t drawIdentity =
      drawCall.getGeometryData().postVsCaptureIdentity;
    return drawIdentity != kEmptyHash
        && drawIdentity == blas.input.getGeometryData().postVsCaptureIdentity;
  }

  bool postVsCaptureInvolved(const DrawCallState& drawCall, const BlasEntry& blas) {
    return drawCall.getGeometryData().postVsCaptureIdentity != kEmptyHash
        || blas.input.getGeometryData().postVsCaptureIdentity != kEmptyHash;
  }

  void logPreventedMaterialOnlyReuse(
      const DrawCallState& drawCall, const BlasEntry& blas,
      bool materialHashesMatch) {
    if (!kenshi_telemetry::enabled()) return;
    const bool vertexDataMatches =
      drawCall.getGeometryData().getHashForRule<rules::VertexDataHash>()
        == blas.input.getGeometryData().getHashForRule<rules::VertexDataHash>();
    const bool boneHashesMatch =
      drawCall.getSkinningState().boneHash == blas.input.getSkinningState().boneHash;
    if (!materialHashesMatch
     || (vertexDataMatches && boneHashesMatch)
     || postVsCaptureIdentityMatch(drawCall, blas))
      return;

    static uint32_t s_logCount = 0;
    if (s_logCount++ >= 32)
      return;

    Logger::warn(str::format(
      "[RTX Geometry Identity] prevented material-only BLAS reuse: material=0x",
      std::hex, drawCall.getMaterialData().getHash(),
      " postVs=", postVsCaptureInvolved(drawCall, blas) ? 1 : 0,
      " drawCapture=0x", drawCall.getGeometryData().postVsCaptureIdentity,
      " cachedCapture=0x", blas.input.getGeometryData().postVsCaptureIdentity,
      " drawVertex=0x",
      drawCall.getGeometryData().hashes[HashComponents::VertexPosition],
      " cachedVertex=0x",
      blas.input.getGeometryData().hashes[HashComponents::VertexPosition],
      std::dec));
  }

  // Entry-steal probe, diagnostic only. Every reuse test here (exactMatch, the vertex-data fallback, the
  // post-VS identity match) compares material, geometry and bone hashes but never the transform, so
  // identical copies of a mesh are interchangeable and which draw wins depends on submission order.
  // Records whether and how often that happens; silent while the pairing is stable.
  void noteEntryReuse(const DrawCallState& drawCall, const BlasEntry& blas,
                      const char* branch, uint32_t frameId) {
    if (!(kenshi_telemetry::enabled() && KenshiOptions::kenshiLogEntrySteal()))
      return;

    const Vector4& from = blas.input.getTransformData().objectToWorld[3];
    const Vector4& to = drawCall.getTransformData().objectToWorld[3];
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const float distanceSq = dx * dx + dy * dy + dz * dz;

    const float threshold = KenshiOptions::kenshiEntryStealDistance();
    if (!(distanceSq > threshold * threshold))
      return;   // same object, or ordinary per-frame motion

    static std::atomic<uint32_t> s_lines { 0u };
    static std::atomic<uint32_t> s_total { 0u };
    static std::atomic<uint32_t> s_frame { 0xFFFFFFFFu };
    static std::atomic<uint32_t> s_thisFrame { 0u };
    static std::atomic<uint32_t> s_framesWithSteals { 0u };
    static std::atomic<uint32_t> s_maxPerFrame { 0u };
    static std::atomic<uint32_t> s_lastSummaryFrame { 0u };

    const uint32_t total = s_total.fetch_add(1u, std::memory_order_relaxed) + 1u;

    // Fold the previous frame's tally into the running maximum when the frame
    // advances, so the summary can report bursts rather than just a rate.
    const uint32_t previousFrame = s_frame.exchange(frameId, std::memory_order_relaxed);
    if (previousFrame != frameId) {
      const uint32_t closed = s_thisFrame.exchange(1u, std::memory_order_relaxed);
      if (closed > 0u) {
        s_framesWithSteals.fetch_add(1u, std::memory_order_relaxed);
        uint32_t prevMax = s_maxPerFrame.load(std::memory_order_relaxed);
        while (closed > prevMax
            && !s_maxPerFrame.compare_exchange_weak(prevMax, closed, std::memory_order_relaxed)) {
        }
      }
    } else {
      s_thisFrame.fetch_add(1u, std::memory_order_relaxed);
    }

    const float distance = std::sqrt(distanceSq);

    if (s_lines.fetch_add(1u, std::memory_order_relaxed) < 24u) {
      Logger::info(str::format(
        "[DrawCallCache][entry-steal] frame=", frameId,
        " branch=", branch,
        " dist=", distance,
        " from=(", from.x, ",", from.y, ",", from.z, ")",
        " to=(", to.x, ",", to.y, ",", to.z, ")",
        " material=0x", std::hex, drawCall.getMaterialData().getHash(), std::dec,
        " thisFrame=", s_thisFrame.load(std::memory_order_relaxed)));
      return;
    }

    // After the burst, one bounded summary every 600 frames. The shape that
    // would confirm the hypothesis is bursts that start and stop with camera
    // movement, not a constant background rate.
    const uint32_t lastSummary = s_lastSummaryFrame.load(std::memory_order_relaxed);
    if (frameId - lastSummary >= 600u
     && s_lastSummaryFrame.compare_exchange_strong(
          const_cast<uint32_t&>(lastSummary), frameId, std::memory_order_relaxed)) {
      Logger::info(str::format(
        "[DrawCallCache][entry-steal] summary: frame=", frameId,
        " total=", total,
        " framesWithSteals=", s_framesWithSteals.load(std::memory_order_relaxed),
        " maxPerFrame=", s_maxPerFrame.load(std::memory_order_relaxed),
        " lastDist=", distance));
    }
  }

  bool exactMatch(const DrawCallState& drawCall, BlasEntry& blas) {
    auto isSky = [](CameraType::Enum t) {
      return t == CameraType::Sky;
    };

    if (isSky(drawCall.cameraType) != isSky(blas.input.cameraType)) {
      return false;
    }

    return drawCall.getMaterialData().getHash() == blas.input.getMaterialData().getHash()
        && drawCall.getGeometryData().getHashForRule<rules::FullGeometryHash>() == blas.input.getGeometryData().getHashForRule<rules::FullGeometryHash>()
        && drawCall.getSkinningState().boneHash == blas.input.getSkinningState().boneHash;
  }
}

DrawCallCache::DrawCallCache(DxvkDevice* device) : CommonDeviceObject(device) {
  m_entries.reserve(1024);
}
DrawCallCache::~DrawCallCache() {}

DrawCallCache::CacheState DrawCallCache::get(const DrawCallState& drawCall, BlasEntry** out, BlasEntry* ownedEntry) {
  // First, find the right bucket:
  XXH64_hash_t hash = drawCall.getGeometryData().getHashForRule<rules::TopologicalHash>();
  // The source topology is identical for intact and amputated bodies, but their generated BLAS index
  // streams are not. Partition the runtime cache without changing the authored asset hash used for
  // replacement matching.
  const uint32_t kenshiHiddenMask =
    drawCall.getGeometryData().kenshiHiddenMask;
  if (kenshiHiddenMask != 0u) {
    hash = XXH3_64bits_withSeed(
      &kenshiHiddenMask, sizeof(kenshiHiddenMask), hash);
  }
  const XXH64_hash_t postVsCaptureIdentity =
    drawCall.getGeometryData().postVsCaptureIdentity;
  if (postVsCaptureIdentity != kEmptyHash) {
    // DX11 post-VS captures from modern engines frequently share their index
    // topology and material while containing completely different transformed
    // meshes.  Keeping all of them in the topology-only bucket makes lookup
    // quadratic and repeatedly compares unrelated Unreal meshes.  The capture
    // identity is stable across frames and describes the exact replay contract,
    // so use it to partition captured geometry without changing legacy draws.
    hash = XXH3_64bits_withSeed(
      &postVsCaptureIdentity, sizeof(postVsCaptureIdentity), hash);
  }
  auto range = m_entries.equal_range(hash);
  // Skinned history belongs to the tracked character, not to the first unclaimed copy of a shared mesh;
  // otherwise reordered draws take another character's skinned vertices as their previous positions.
  if (drawCall.getSkinningState().numBones > 0u) {
    const uint32_t frame = m_device->getCurrentFrameId();
    for (auto candidate = range.first; candidate != range.second; ++candidate) {
      BlasEntry& entry = candidate->second;
      if (&entry != ownedEntry)
        continue;
      const auto& oldSkin = entry.input.getSkinningState();
      const bool compatible = oldSkin.numBones == drawCall.getSkinningState().numBones
        && entry.input.cameraType == drawCall.cameraType
        && entry.input.getGeometryData().getHashForRule<rules::VertexDataHash>()
          == drawCall.getGeometryData().getHashForRule<rules::VertexDataHash>()
        && !postVsCaptureInvolved(drawCall, entry);
      const bool previousFrame = frame > 0u && entry.frameLastTouched == frame - 1u;
      const bool sameFrameSamePose = entry.frameLastTouched == frame
        && oldSkin.boneHash == drawCall.getSkinningState().boneHash;
      if (compatible && (previousFrame || sameFrameSamePose)) {
        *out = &entry;
        return CacheState::kExisted;
      }
      break;
    }
    // New owner, changed topology/streams, conflicting same-frame pose or a
    // visibility gap: build fresh geometry instead of borrowing stale history.
    *out = allocateEntry(hash, drawCall);
    return CacheState::kNew;
  }
  if (range.first == m_entries.end()) {
    // New bucket
    *out = allocateEntry(hash, drawCall);
    return CacheState::kNew;
  }
  // Handle buckets with 1 entry:
  auto iter = range.first;
  iter++;
  if (iter == range.second) {
    // Only 1 element
    BlasEntry& entry = range.first->second;
    if (entry.input.getSkinningState().numBones > 0u) {
      *out = allocateEntry(hash, drawCall);
      return CacheState::kNew;
    }

    const bool updatedThisFrame = entry.frameLastTouched == m_device->getCurrentFrameId();
    const bool vertexDataMatches = entry.input.getGeometryData().getHashForRule<rules::VertexDataHash>() == drawCall.getGeometryData().getHashForRule<rules::VertexDataHash>();
    const bool materialHashesMatch = entry.input.getMaterialData().getHash() == drawCall.getMaterialData().getHash();

    logPreventedMaterialOnlyReuse(drawCall, entry, materialHashesMatch);

    const bool reusableCapturedOutput =
      postVsCaptureIdentityMatch(drawCall, entry)
      && (!updatedThisFrame
       || !drawCall.getGeometryData().postVsCapturedPositionsDynamic);
    // An animating skinned mesh must still be recognised as the object it was last frame. Its bone hash
    // changes every frame, so requiring it here would allocate a fresh BlasEntry per frame (kBuildBVH
    // clears previousPositionBuffer) and skinned motion vectors would be zero. The bone hash keeps its real
    // jobs: exactMatch above still compares it (separating simultaneous instances of one skeletal mesh,
    // e.g. a pack of animals), and processGeometryInfo still uses it to choose kUpdateBVH, so a changed
    // pose refits the BLAS. `!updatedThisFrame` keeps this safe: an entry claimed this frame is never
    // handed out twice.
    const bool reusableLegacyOutput =
      !postVsCaptureInvolved(drawCall, entry)
      && !updatedThisFrame
      && vertexDataMatches;

    if (exactMatch(drawCall, entry) || reusableCapturedOutput || reusableLegacyOutput) {
      // Exact vertex match that is reusable for the current draw call,
      // or something that hasn't been updated this frame and is similar enough.
      // Matching the logic in the multi-element loop below.
      noteEntryReuse(drawCall, entry,
        exactMatch(drawCall, entry) ? "single:exactMatch"
          : (reusableCapturedOutput ? "single:capturedOutput" : "single:legacyVertex"),
        m_device->getCurrentFrameId());
      *out = &entry;
      return CacheState::kExisted;
    } else {
      // First frame of having two mismatching instances, and the first instance has already 
      // been paired with the existing BlasEntry.
      *out = allocateEntry(hash, drawCall);
      return CacheState::kNew;
    }
  }

  // Bucket has multiple BlasEntries. Geometry may be reused only when its
  // vertex/bone identity (or exact post-VS capture identity) matches. A material
  // hash is shading state, never geometry identity: accepting it here attached
  // faces and other textures to unrelated sky/terrain meshes in DX11 games.
  const bool drawIsPostVsCaptured =
    drawCall.getGeometryData().postVsCaptureIdentity != kEmptyHash;

  for (auto bucketIter = range.first; bucketIter != range.second; bucketIter++) {
    BlasEntry& blas  = bucketIter->second;
    if (blas.input.getSkinningState().numBones > 0u)
      continue;
    const bool cachedIsPostVsCaptured =
      blas.input.getGeometryData().postVsCaptureIdentity != kEmptyHash;
    const bool materialHashesMatch =
      blas.input.getMaterialData().getHash() == drawCall.getMaterialData().getHash();
    logPreventedMaterialOnlyReuse(drawCall, blas, materialHashesMatch);

    if (exactMatch(drawCall, blas)) {
      noteEntryReuse(drawCall, blas, "bucket:exactMatch", m_device->getCurrentFrameId());
      *out = &blas;
      return CacheState::kExisted;
    }
    if (blas.frameLastTouched == m_device->getCurrentFrameId()
     && (drawIsPostVsCaptured
       ? drawCall.getGeometryData().postVsCapturedPositionsDynamic
       : true)) {
      continue;
    }
    if (drawIsPostVsCaptured != cachedIsPostVsCaptured)
      continue;

    if (drawIsPostVsCaptured) {
      if (postVsCaptureIdentityMatch(drawCall, blas)) {
        noteEntryReuse(drawCall, blas, "bucket:capturedIdentity", m_device->getCurrentFrameId());
        *out = &blas;
        return CacheState::kExisted;
      }
      continue;
    }

    const bool vertexDataMatches =
      blas.input.getGeometryData().getHashForRule<rules::VertexDataHash>()
        == drawCall.getGeometryData().getHashForRule<rules::VertexDataHash>();
    // Same reasoning as the single-entry path above: entries touched this frame were skipped further up,
    // so dropping the bone-hash requirement lets a moving skinned mesh rematch its own entry without
    // letting two simultaneous instances share one.
    if (vertexDataMatches) {
      noteEntryReuse(drawCall, blas, "bucket:legacyVertex", m_device->getCurrentFrameId());
      *out = &blas;
      return CacheState::kExisted;
    }
  }

  // No exact geometry identity in this topology bucket.
  *out = allocateEntry(hash, drawCall);
  return CacheState::kNew;

}

BlasEntry* DrawCallCache::allocateEntry(XXH64_hash_t hash, const DrawCallState& drawCall) {
  auto iter = m_entries.emplace(hash, drawCall);
  BlasEntry* result = &iter->second;
  result->frameCreated = m_device->getCurrentFrameId();
  return result;
}

}  // namespace nvvk
