#pragma once
#include "util_kenshi_telemetry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace dxvk::shadow_geometry {
  enum Kind : uint32_t { IndexRange, IndexHash, TexcoordHash, KindCount };
  struct Key {
    // Kind, source offset/length, then every argument of the original calculation.
    std::array<uint64_t, 10> fields {};
    bool operator==(const Key& other) const { return fields == other.fields; }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      uint64_t h = 0;
      for (const auto v : key.fields)
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return size_t(h);
    }
  };
  struct Value {
    uint64_t result = 0;
    bool valid = true;
    bool operator==(const Value& b) const { return result == b.result && valid == b.valid; }
  };
  // Owned by the existing lazy buffer cache. Only scalars, no resource ownership.
  class Cache {
    struct Entry { Value value; uint64_t access; };
    std::unordered_map<Key, Entry, KeyHash> m_entries;
    uint64_t m_revision = 0, m_access = 0;
    void revision(uint64_t value) {
      if (value != m_revision) { m_entries.clear(); m_revision = value; m_access = 0; }
    }
  public:
    bool find(uint64_t version, const Key& key, Value& value) {
      revision(version);
      const auto i = m_entries.find(key);
      if (i == m_entries.end()) return false;
      i->second.access = ++m_access; value = i->second.value; return true;
    }
    void store(uint64_t version, const Key& key, Value value) {
      revision(version);
      if (m_entries.size() >= 512 && m_entries.find(key) == m_entries.end()) {
        auto oldest = m_entries.begin();
        for (auto i = m_entries.begin(); i != m_entries.end(); ++i)
          if (i->second.access < oldest->second.access) oldest = i;
        m_entries.erase(oldest);
      }
      m_entries.insert_or_assign(key, Entry { value, ++m_access });
    }
    size_t size() const { return m_entries.size(); }
  };
  struct Counts { uint64_t calls = 0, hits = 0, misses = 0, fallback = 0, avoided = 0; };
  struct State {
    std::array<Counts, KindCount> counts {};
    std::array<uint32_t, KindCount> remaining {}, verified {}, mismatches {};
    uint32_t frames = 0;
    bool enabled = true;
  };
  inline thread_local State state;
  inline void arm() { state.remaining.fill(64); state.verified.fill(0); state.mismatches.fill(0); }
  inline void stop() { state.remaining.fill(0); }
  template<class Compute>
  Value evaluate(Cache* cache, uint64_t revision, Kind kind, const Key& key,
                 uint64_t units, Compute&& compute) {
    auto& c = state.counts[kind]; ++c.calls;
    if (!state.enabled || cache == nullptr) { ++c.fallback; return compute(); }
    Value value;
    if (cache->find(revision, key, value)) {
      ++c.hits;
      if (kenshi_telemetry::enabled() && state.remaining[kind]) {
        --state.remaining[kind]; ++state.verified[kind];
        const auto reference = compute();
        if (!(reference == value)) { ++state.mismatches[kind]; state.enabled = false; stop(); }
        return reference;
      }
      c.avoided += units; return value;
    }
    ++c.misses; value = compute(); cache->store(revision, key, value); return value;
  }
}
