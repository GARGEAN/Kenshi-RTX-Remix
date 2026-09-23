#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace dxvk::ommretention {
  constexpr uint64_t MiB = 1024 * 1024;
  constexpr uint64_t abandonedGraceMs = 3000;

  inline bool abandonedExpired(uint64_t since, uint64_t now) {
    return since && now >= since && now - since >= abandonedGraceMs;
  }

  // Non-owning snapshot. References owned by other cache entries are counted
  // explicitly; any additional BLAS/command/retirement owner protects the group.
  struct Entry {
    uint64_t hash = 0, resource = 0, parent = 0, references = 0, bytes = 0;
    uint32_t age = 0;
    bool diskBacked = false;
  };
}
