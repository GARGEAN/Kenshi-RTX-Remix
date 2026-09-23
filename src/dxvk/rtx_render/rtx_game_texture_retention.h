#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace dxvk {

// V776: metadata only. No image/view references and no GPU record changes.
class GameTextureRetention {
public:
  static constexpr uint32_t unusedFrames = 600;
  struct Slot {
    uint64_t key = 0;
    uint32_t lastUsed = 0;
    bool terrain = false;
  };

  void touch(uint32_t index, uint64_t key, uint32_t frame, bool terrain = false) {
    if (index >= slots.size()) slots.resize(size_t(index) + 1);
    auto& slot = slots[index];
    if (slot.key != key) slot = {};
    slot.key = key;
    slot.lastUsed = frame;
    slot.terrain |= terrain;
  }

  void keepAlive(uint32_t index, uint32_t frame) {
    if (index < slots.size()) slots[index].lastUsed = frame;
  }

  bool eligible(uint32_t index, uint64_t key, uint32_t frame) const {
    if (index >= slots.size()) return false;
    const auto& slot = slots[index];
    // V780: the shared protection pass covers every material family, not only terrain.
    return slot.key != 0 && slot.key == key && frame >= slot.lastUsed
      && frame - slot.lastUsed > unusedFrames;
  }

  void forget(uint32_t index) { if (index < slots.size()) slots[index] = {}; }
  void clear() { slots.clear(); cursor = 0; released = 0; }

  std::vector<Slot> slots;
  uint32_t cursor = 0;
  uint64_t released = 0;
};

// Normal indices occupy float payload bits; do not convert them numerically.
template<typename Args, typename Fn>
void forEachKenshiTerrainTexture(const Args& args, Fn&& fn) {
  if (!args.set) return;
  fn(args.layerTexture0); fn(args.layerTexture1); fn(args.layerTexture2);
  fn(args.layerTexture3); fn(args.layerTexture4); fn(args.layerTexture5);
  fn(args.overlayTexture);
  const float packed[] = {args.slopeMin.z, args.slopeMin.w, args.slopeMax.z};
  for (const float& value : packed) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    fn(bits & 0xffffu); fn(bits >> 16u);
  }
  if (args.blendMask) fn(args.blendTexture);
}

} // namespace dxvk
