#pragma once

#include <cstdint>
#include <unordered_map>

namespace dxvk::vram_ownership {
  // Sample-local raw identities only: diagnostics must never prolong resource life.
  // Each object contributes bytes once; masks expose overlapping owning collections.
  enum Owner : uint32_t {
    SeenGeometry = 1, RetainedGeometry = 2, UnlinkedGeometry = 4,
    PoolRecent = 8, PoolIdle = 16, CachedBucket = 32, ActiveDynamic = 64,
    Intersection = 128, CurrentTlas = 256, PreviousTlas = 512,
  };
  struct Entry { uint64_t bytes = 0; uint32_t owners = 0; };
  struct Census {
    std::unordered_map<const void*, Entry> objects;
    void add(const void* identity, uint64_t bytes, uint32_t owners) {
      if (!identity) return;
      auto& entry = objects[identity];
      entry.bytes = bytes;
      entry.owners |= owners;
    }
  };
  inline uint32_t age(uint32_t frame, uint32_t last) {
    return last <= frame ? frame - last : 0;
  }
}
