#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

namespace dxvk {
// V779: sample-local identities only, never owning references. Masks: terrain=1,
// other direct=2, opaque conversion=4. Separate censuses must not be added together.
struct TextureOwnerCensus {
  struct Image { uint64_t bytes = 0; uint32_t mask = 0; bool recent = false, unknown = false; };
  struct Row { uint64_t bytes = 0, coldBytes = 0, unknownBytes = 0; uint32_t images = 0; };
  std::unordered_map<const void*, Image> images;

  void add(const void* identity, uint64_t bytes, uint32_t mask, bool recent, bool known) {
    if (!identity || !mask || mask >= 8) return;
    auto& image = images[identity];
    if (bytes > image.bytes) image.bytes = bytes;
    image.mask |= mask;
    image.recent |= recent;
    image.unknown |= !known;
  }
  std::array<Row, 8> summarize() const {
    std::array<Row, 8> rows {};
    for (const auto& entry : images) {
      const auto& image = entry.second;
      auto& row = rows[image.mask];
      ++row.images; row.bytes += image.bytes;
      if (!image.recent && !image.unknown) row.coldBytes += image.bytes;
      if (image.unknown) row.unknownBytes += image.bytes;
    }
    return rows;
  }
};
}
