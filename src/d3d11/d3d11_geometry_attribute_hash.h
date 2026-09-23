#pragma once

#include "../util/xxHash/xxhash.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxvk {
  // Hash the actual attribute, not neighbouring interleaved positions/padding.
  // The caller supplies bytes beginning at the attribute within its draw slice.
  // No buffer addresses or rename-dependent slice offsets enter the identity.
  inline XXH64_hash_t hashGeometryAttribute(const void* data, size_t length,
      uint32_t stride, uint32_t elementBytes, uint32_t first, uint32_t count,
      uint32_t totalCount, uint32_t format, uint64_t domain) {
    if (!data || !stride || !elementBytes || elementBytes > stride ||
        elementBytes > 16 || !count || first > totalCount || count > totalCount - first)
      return 0;
    const uint64_t start = uint64_t(first) * stride;
    if (start > length || elementBytes > length - start ||
        uint64_t(count - 1) > (length - start - elementBytes) / stride)
      return 0; // Never hash a truncated or out-of-range attribute stream.

    const uint64_t metadata[] = { domain, format, first, count, totalCount };
    XXH64_hash_t hash = XXH3_64bits(metadata, sizeof(metadata));
    const auto* source = static_cast<const uint8_t*>(data) + start;
    std::array<uint8_t, 1024> packed;
    const uint32_t perChunk = static_cast<uint32_t>(packed.size()) / elementBytes;
    for (uint32_t done = 0; done < count;) {
      const uint32_t n = std::min(perChunk, count - done);
      const uint8_t* bytes = source + uint64_t(done) * stride;
      // Contiguous streams already hold exactly the bytes to hash. Preserve chunk boundaries and the
      // seeded chain so identities stay exact.
      if (stride != elementBytes) {
        // Constant sizes avoid one out-of-line memcpy per interleaved element.
        switch (elementBytes) {
          case 4:
            for (uint32_t i = 0; i < n; ++i)
              std::memcpy(packed.data() + size_t(i) * 4, bytes + uint64_t(i) * stride, 4);
            break;
          case 8:
            for (uint32_t i = 0; i < n; ++i)
              std::memcpy(packed.data() + size_t(i) * 8, bytes + uint64_t(i) * stride, 8);
            break;
          case 12:
            for (uint32_t i = 0; i < n; ++i)
              std::memcpy(packed.data() + size_t(i) * 12, bytes + uint64_t(i) * stride, 12);
            break;
          case 16:
            for (uint32_t i = 0; i < n; ++i)
              std::memcpy(packed.data() + size_t(i) * 16, bytes + uint64_t(i) * stride, 16);
            break;
          default:
            for (uint32_t i = 0; i < n; ++i)
              std::memcpy(packed.data() + size_t(i) * elementBytes,
                          bytes + uint64_t(i) * stride, elementBytes);
        }
        bytes = packed.data();
      }
      hash = XXH3_64bits_withSeed(bytes, size_t(n) * elementBytes, hash);
      done += n;
    }
    return hash ? hash : 1; // Zero denotes unavailable identity to Remix.
  }
}
