#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dxvk {
// Reused per-thread scratch, no scene/resource ownership. A new generation also
// makes reuse safe when material/terrain table indices are recycled after a clear.
struct MaintenanceMarks {
  std::vector<uint32_t> marks;
  uint32_t generation = 0;
  void begin(size_t count) {
    if (++generation == 0) {
      std::fill(marks.begin(), marks.end(), 0);
      generation = 1;
    }
    if (marks.size() < count) marks.resize(count, 0);
  }
  bool first(uint32_t index) {
    if (index >= marks.size() || marks[index] == generation) return false;
    marks[index] = generation;
    return true;
  }
};

// Stable numeric slots for an unordered cache: no iterators survive insertion /
// rehash. The caller updates the moved entry's slot after remove().
struct MaintenanceKeys {
  std::vector<uint64_t> keys;
  uint32_t cursor = 0;
  void clear() { keys.clear(); cursor = 0; }
  uint32_t add(uint64_t key) { keys.push_back(key); return uint32_t(keys.size() - 1); }
  uint32_t next() {
    if (cursor >= keys.size()) cursor = 0;
    return cursor++;
  }
  uint64_t remove(uint32_t index) {
    const uint64_t moved = keys.back();
    keys[index] = moved;
    keys.pop_back();
    if (index < cursor) cursor = index; // visit the swapped-in entry next
    return moved;
  }
};
}
