#include "util_kenshi_telemetry.h"
#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dxvk::prepared_terrain {
  inline std::atomic<bool> enabled { true };
  inline std::atomic<uint32_t> verifyRemaining { 0 };
  inline std::atomic<uint64_t> epoch { 1 };
  // Raw VS attributes do not depend on Remix material options or the normal
  // texture side table. Only scene/resource lifetime changes invalidate both.
  inline std::atomic<uint64_t> attributeEpoch { 1 };
  enum class Invalidation { Manual, Scene, CommandList, Option, NormalTable, Count };
  inline std::array<std::atomic<uint64_t>, size_t(Invalidation::Count)> invalidations {};
  inline void invalidate(Invalidation reason = Invalidation::Manual) {
    invalidations[size_t(reason)].fetch_add(1, std::memory_order_relaxed);
    if (reason == Invalidation::Manual || reason == Invalidation::Scene
     || reason == Invalidation::CommandList)
      attributeEpoch.fetch_add(1, std::memory_order_relaxed);
    epoch.fetch_add(1, std::memory_order_relaxed);
  }
  inline bool optionAffectsPreparation(std::string_view name) {
    // Harvested lighting is consumed by the backend atmosphere/light passes,
    // not by the cached bridge material/geometry products. Keep unknown options
    // conservative; neither unchanged writes nor these changing values expire tiles.
    return name != "rtx.atmosphere.sunElevation"
      && name != "rtx.atmosphere.sunRotation"
      && name != "rtx.atmosphere.sunRadianceTint"
      && name != "rtx.dx11.kenshiSkyXCameraPos"
      && name != "rtx.dx11.kenshiSkyXInvWaveLength"
      && name != "rtx.dx11.kenshiSkyXScatter"
      && name != "rtx.dx11.kenshiSkyXScaleParams"
      && name != "rtx.dx11.kenshiSkyXGeometry"
      && name != "rtx.dx11.kenshiFogSunDir"
      && name != "rtx.dx11.kenshiAmbientTint"
      && name != "rtx.dx11.kenshiFogColour"
      && name != "rtx.dx11.kenshiFogParams"
      && name != "rtx.dx11.kenshiFogAtmoEnd"
      && name != "rtx.dx11.kenshiFogHorizon"
      && name != "rtx.dx11.kenshiFogExtra";
  }
  inline bool claimVerification() {
    if (!kenshi_telemetry::enabled()) return false;
    auto remaining = verifyRemaining.load(std::memory_order_relaxed);
    while (remaining && !verifyRemaining.compare_exchange_weak(
      remaining, remaining - 1, std::memory_order_relaxed)) { }
    return remaining != 0;
  }

  // Cached products retain resources, not registry entries. Retire only at actual native/capture-buffer
  // destruction. Occupancy follows live resources rather than every buffer address seen since launch;
  // unique tokens also stop a recycled address from matching an old resource's version.
  struct RevisionStats {
    uint64_t live = 0, peak = 0, registered = 0, retired = 0;
  };
  struct RevisionRegistry {
    std::mutex mutex;
    std::unordered_map<const void*, uint64_t> versions;
    uint64_t serial = 0;
    RevisionStats stats;
  };
  inline RevisionRegistry& revisionRegistry() {
    // Survive static/TLS teardown: buffer destructors may run after other statics.
    // Buffer records are still retired normally; the registry shell is process-lived.
    static auto* value = new RevisionRegistry;
    return *value;
  }
  inline uint64_t revision(const void* resource) {
    auto& r = revisionRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto i = r.versions.find(resource);
    if (i != r.versions.end()) return i->second;
    const auto value = ++r.serial;
    r.versions.emplace(resource, value);
    ++r.stats.registered;
    r.stats.live = r.versions.size();
    if (r.stats.live > r.stats.peak) r.stats.peak = r.stats.live;
    return value;
  }
  inline void written(const void* resource) {
    auto& r = revisionRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto i = r.versions.find(resource);
    if (i != r.versions.end()) i->second = ++r.serial;
  }
  inline void retired(const void* resource) {
    auto& r = revisionRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.stats.retired += r.versions.erase(resource);
    r.stats.live = r.versions.size();
  }
  inline RevisionStats revisionStats() {
    auto& r = revisionRegistry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.stats;
  }
  struct Key {
    std::vector<uint8_t> bytes;
    void data(const void* p, size_t n) {
      const auto* b = static_cast<const uint8_t*>(p);
      bytes.insert(bytes.end(), b, b + n);
    }
    template<class T> void add(const T& v) {
      static_assert(std::is_trivially_copyable_v<T>);
      data(&v, sizeof(v));
    }
    bool operator==(const Key& b) const { return bytes == b.bytes; }
  };
}
