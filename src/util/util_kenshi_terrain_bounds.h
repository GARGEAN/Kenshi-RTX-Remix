#pragma once
#include "util_kenshi_telemetry.h"

// V698: cache only the existing CPU-shadow bounds calculation. This revision
// describes shadow bytes, not arbitrary GPU or mapped-buffer writes.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include "util_kenshi_shadow_geometry.h"

namespace dxvk::terrain_bounds {
  struct Key {
    uint64_t offset = 0;
    uint32_t vertices = 0, stride = 0, format = 0;
    bool operator==(const Key& other) const {
      return offset == other.offset && vertices == other.vertices
        && stride == other.stride && format == other.format;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      uint64_t h = key.offset;
      for (uint32_t v : { key.vertices, key.stride, key.format })
        h ^= uint64_t(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return size_t(h);
    }
  };
  using Bounds = std::array<float, 6>; // padded min xyz, max xyz
  inline bool identical(const Bounds& a, const Bounds& b) {
    return std::memcmp(a.data(), b.data(), sizeof(Bounds)) == 0;
  }
  class Cache {
  public:
    shadow_geometry::Cache geometry;
    static constexpr size_t capacity = 512;
    bool find(uint64_t revision, const Key& key, Bounds& result) {
      setRevision(revision);
      const auto entry = m_entries.find(key);
      if (entry == m_entries.end()) return false;
      result = entry->second;
      return true;
    }
    void store(uint64_t revision, const Key& key, const Bounds& bounds) {
      setRevision(revision);
      const auto entry = m_entries.find(key);
      if (entry != m_entries.end()) {
        entry->second = bounds;
        return;
      }
      if (m_entries.size() >= capacity) m_entries.erase(m_entries.begin());
      m_entries.emplace(key, bounds);
    }
    size_t size() const { return m_entries.size(); }
  private:
    void setRevision(uint64_t revision) {
      if (revision != m_revision) {
        m_entries.clear();
        m_revision = revision;
      }
    }
    uint64_t m_revision = 0;
    std::unordered_map<Key, Bounds, KeyHash> m_entries;
  };

  // V706: the cached result depends on shadow bytes and layout, not the draw's
  // terrain classification or current camera/object transform.
  inline bool eligible(bool immediate, bool fromShadow, bool originalSource) {
    return immediate && fromShadow && originalSource;
  }
  inline std::atomic<bool> requestedEnabled { true };
  inline std::atomic<uint32_t> requestedVerification { 0 };
  inline bool enabled() { return requestedEnabled.load(std::memory_order_relaxed); }
  inline bool takeVerification() {
    if (!kenshi_telemetry::enabled()) return false;
    uint32_t n = requestedVerification.load(std::memory_order_relaxed);
    while (n != 0) {
      if (requestedVerification.compare_exchange_weak(n, n - 1, std::memory_order_relaxed))
        return true;
    }
    return false;
  }
  // Cheap integer coverage counts, no clocks or resampling in normal operation.
  // Emitted at most once per 600 frames by the immediate submit thread.
  struct Coverage {
    uint64_t considered = 0, eligible = 0, hits = 0, misses = 0;
    uint64_t samples = 0, avoided = 0, verified = 0, mismatches = 0;
    uint32_t frames = 0, modeChanges = 0;
    bool lastEnabled = true;
  };
  inline thread_local std::array<Coverage, 2> coverage; // terrain, nonterrain
}
