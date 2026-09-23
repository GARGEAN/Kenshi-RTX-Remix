#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace dxvk::kenshi_culling {
  // Candidate radius verified by the live class-wide test, in game units.
  constexpr float kOffscreenRadius = 1000.0f;
  constexpr std::size_t kMaxSlots = 16384;

  // Published by the D3D11 frame boundary; OGRE workers never read mutable UI
  // option storage. Zero bypasses the policy without modifying installed code.
  inline std::atomic<float> runtimeRadius { kOffscreenRadius };
  inline std::atomic<float> runtimeFeatureRadius { kOffscreenRadius };
  static_assert(std::atomic<float>::is_always_lock_free);
  inline float enabledRadius(bool enabled, float radius) {
    const float validRadius = std::isfinite(radius)
      ? std::clamp(radius, 100.0f, 10000.0f) : kOffscreenRadius;
    return enabled ? validRadius : 0.0f;
  }
  inline bool setRuntimeSettings(bool enabled, float radius, bool featuresEnabled, float featureRadius) {
    const float value = enabledRadius(enabled, radius);
    const float featureValue = enabledRadius(featuresEnabled, featureRadius);
    const bool changed = runtimeRadius.exchange(value, std::memory_order_relaxed) != value;
    const bool featuresChanged = runtimeFeatureRadius.exchange(featureValue, std::memory_order_relaxed) != featureValue;
    return changed || featuresChanged;
  }

  struct alignas(16) PackedAabb {
    float center[3][4];
    float half[3][4];
  };
  struct ObjectData {
    std::size_t index;
    void* parents;
    void** owners;
    void* localAabbs;
    PackedAabb* worldAabbs;
    void* localRadii;
    float* worldRadii;
    float* upperDistances;
    std::uint32_t* visibilityFlags;
    void* remainingPools[2];
  };
  static_assert(sizeof(PackedAabb) == 96);
  static_assert(sizeof(ObjectData) == 0x58);
  static_assert(offsetof(ObjectData, owners) == 0x10);
  static_assert(offsetof(ObjectData, worldAabbs) == 0x20);
  static_assert(offsetof(ObjectData, visibilityFlags) == 0x40);

  struct Counts {
    std::uint64_t nearLanes = 0, farLanes = 0;
    std::uint32_t moonMask = 0;
  };

  inline bool withinRadius(const PackedAabb& box, std::size_t lane, const float* camera, float radius) {
    double distanceSquared = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const float center = box.center[axis][lane], half = box.half[axis][lane];
      if (!std::isfinite(center) || !std::isfinite(half) || half < 0.0f || !std::isfinite(camera[axis]))
        return false;
      const double gap = std::max(std::abs(double(center) - camera[axis]) - half, 0.0);
      distanceSquared += gap * gap;
    }
    return distanceSquared <= double(radius) * radius;
  }

  // No game-owned array is modified. Scratch grows only to a thread's high-water
  // mark. Nonselected outer lanes and distant lanes retain their original boxes.
  inline bool prepare(std::size_t count, const ObjectData& source, const float* camera,
      bool innerHwInstances, const void* hwCullMethod, ObjectData& copy,
      std::vector<PackedAabb>& scratch, Counts& counts, float radius = kOffscreenRadius,
      const void* staticFeatureEntityVtable = nullptr, float featureRadius = kOffscreenRadius,
      const void* rigidFeatureVtable = nullptr,
      const void* moon0 = nullptr, const void* moon1 = nullptr) {
    if (count == 0 || count > kMaxSlots || !source.owners || !source.worldAabbs || !source.visibilityFlags)
      return false;
    bool changed = false;
    for (std::size_t i = 0; i < count; ++i) {
      const void* owner = source.owners[i];
      if (!owner || source.visibilityFlags[i] == 0)
        continue;
      float selectedRadius = radius;
      std::uint32_t moonBit = 0;
      if (!innerHwInstances) {
        const auto* vtable = *reinterpret_cast<const void* const* const*>(owner);
        // Only the two verified native moon Entities, in their own queue. Their transforms follow the
        // camera; keep submitting current draws instead of freezing a last-seen Remix instance. No radius.
        if (staticFeatureEntityVtable && vtable == staticFeatureEntityVtable
            && static_cast<const std::uint8_t*>(owner)[0x30] == 6) {
          moonBit = (owner == moon0 ? 1u : 0u) | (owner == moon1 ? 2u : 0u);
        }
        if (!moonBit && vtable[0x48 / sizeof(void*)] != hwCullMethod) {
          // Exact verified classes: static Entity scenery and rigid Forests
          // batches. Wind batches and broad-mask grass Entities stay native.
          const auto category = source.visibilityFlags[i] & 0x1fffffffu;
          const bool entityFeature = staticFeatureEntityVtable && vtable == staticFeatureEntityVtable
            && (category == 0x80u || category == 0x20u);
          const bool rigidFeature = rigidFeatureVtable && vtable == rigidFeatureVtable;
          if (!entityFeature && !rigidFeature)
            continue;
          const auto* entity = static_cast<const std::uint8_t*>(owner);
          if (entity[0x30] != 24)
            continue;
          const auto* manager = *reinterpret_cast<const std::uint8_t* const*>(entity + 0x128);
          if (!manager || *reinterpret_cast<const std::uint32_t*>(manager + 0xa0) != 1)
            continue;
          selectedRadius = featureRadius;
        }
      }
      if (!moonBit && selectedRadius <= 0.0f)
        continue;
      const std::size_t pack = i / 4, lane = i % 4;
      if (!moonBit && !withinRadius(source.worldAabbs[pack], lane, camera, selectedRadius)) {
        ++counts.farLanes;
        continue;
      }
      if (!moonBit) ++counts.nearLanes;
      if (!changed) {
        const std::size_t packs = (count + 3) / 4;
        if (scratch.size() < packs)
          scratch.resize(packs);
        std::memcpy(scratch.data(), source.worldAabbs, packs * sizeof(PackedAabb));
        copy = source;
        copy.worldAabbs = scratch.data();
        changed = true;
      }
      for (std::size_t axis = 0; axis < 3; ++axis)
        scratch[pack].half[axis][lane] = std::numeric_limits<float>::infinity();
      counts.moonMask |= moonBit;
    }
    return changed;
  }
}
