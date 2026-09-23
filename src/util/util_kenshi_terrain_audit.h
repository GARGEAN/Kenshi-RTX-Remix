#include "util_kenshi_telemetry.h"
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include "log/log.h"
#include "util_string.h"

namespace dxvk::terrain_audit {
  inline std::atomic<bool> requested {false};
  inline uint32_t family(uint64_t shader) {
    return shader == 0xdf5c7e0b230c7f2eull ? 1u : shader == 0x3a2f0355b844fbf6ull ? 2u : 0u;
  }
  struct Removal { uint32_t frame = 0, lod = 0, age = 0; uint64_t owner = 0;
    float x = 0, y = 0, z = 0; bool range = false; };
  inline thread_local bool removalIsRange = false;
  struct State {
    uint32_t frame = ~0u, frames = 0;
    std::array<uint32_t, 4> counts {};
    std::array<Removal, 64> removals {};
    uint64_t removed = 0;
  };
  inline thread_local State state;
  inline bool active(uint32_t frame) {
    if (!kenshi_telemetry::enabled()) { state.frames = 0; requested = false; return false; }
    if (state.frame == frame) return state.frames != 0;
    state.frame = frame; state.counts = {};
    if (requested.exchange(false, std::memory_order_relaxed)) {
      state.frames = 4;
      KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] begin frame=", frame,
        " frames=4 rowCapPerStage=512 recentRemovalTotal=", state.removed));
      const auto count = std::min<uint64_t>(state.removed, state.removals.size());
      for (uint64_t i = state.removed - count; i < state.removed; ++i) {
        const auto& r = state.removals[i % state.removals.size()];
        KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] removed frame=", r.frame,
          " lod=", r.lod, " owner=", r.owner, " age=", r.age, " range=", r.range,
          " center=", r.x, ",", r.y, ",", r.z));
      }
    } else if (state.frames && --state.frames == 0) {
      KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] complete frame=", frame));
    }
    return state.frames != 0;
  }
  inline bool row(uint32_t frame, uint32_t stage) {
    return active(frame) && state.counts[stage]++ < 512;
  }
  template<typename Vec>
  void removal(uint32_t frame, uint32_t lod, uint64_t owner, uint32_t age, const Vec& center) {
    if (!kenshi_telemetry::enabled() || !lod) return;
    state.removals[state.removed++ % state.removals.size()] =
      {frame, lod, age, owner, center.x, center.y, center.z, removalIsRange};
  }
  template<typename Draw, typename Transform>
  void draw(uint32_t frame, uint32_t stage, const Draw& draw, uint64_t owner, uint64_t instance,
            const Transform& t) {
    const uint32_t lod = family(draw.programmableVertexShaderBytecodeHash);
    if (!lod || !row(frame, stage)) return;
    const auto& g = draw.getGeometryData();
    const auto c = g.boundingBox.getTransformedCentroid(t);
    KENSHI_DIAGNOSTIC_INFO(str::format("[TerrainAudit V781] frame=", frame, " stage=", stage,
      " draw=", draw.drawCallID, " lod=", lod, " owner=", owner, " instance=", instance,
      " indices=", g.indexCount, " vertices=", g.vertexCount,
      " center=", c.x, ",", c.y, ",", c.z,
      " min=", g.boundingBox.minPos.x, ",", g.boundingBox.minPos.y, ",", g.boundingBox.minPos.z,
      " max=", g.boundingBox.maxPos.x, ",", g.boundingBox.maxPos.y, ",", g.boundingBox.maxPos.z,
      " translate=", t[3][0], ",", t[3][1], ",", t[3][2],
      " basis=", t[0][0], ",", t[0][1], ",", t[0][2], ";",
      t[1][0], ",", t[1][1], ",", t[1][2], ";", t[2][0], ",", t[2][1], ",", t[2][2]));
  }
}
