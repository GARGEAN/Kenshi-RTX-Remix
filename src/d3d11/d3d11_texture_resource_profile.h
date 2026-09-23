#pragma once

#include <cctype>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dxvk {
  // Immutable reflection metadata, populated lazily by the draw-thread cache.
  class D3D11TextureResourceProfile {
  public:
    struct TerrainSlots {
      uint32_t diffuse = UINT32_MAX, normal = UINT32_MAX;
      uint32_t overlay = UINT32_MAX, blend = UINT32_MAX;
      bool declaresOverlay = false;
      std::array<uint32_t, 3> setDiffuse { UINT32_MAX, UINT32_MAX, UINT32_MAX };
      std::array<uint32_t, 3> setNormal { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    };
    template<typename Lookup>
    void initialize(Lookup&& lookup) {
      m_names.clear();
      m_waterColourSlot = UINT32_MAX;
      m_terrain = {};
      uint32_t colourSlot = UINT32_MAX;
      bool flow = false, normal = false;
      for (uint32_t slot = 0; slot < 128u; ++slot) {
        std::string name = lookup(slot);
        if (name.empty()) continue;
        m_names.resize(slot + 1);
        auto& entry = m_names[slot];
        entry.original = std::move(name);
        entry.lower = entry.original;
        for (char& c : entry.lower)
          c = char(::tolower(static_cast<unsigned char>(c)));
        if (colourSlot == UINT32_MAX && entry.lower.find("colourmap") != std::string::npos)
          colourSlot = slot;
        flow |= entry.lower.find("flowmap") != std::string::npos;
        normal |= entry.lower.find("normalmap") != std::string::npos;
      }
      if (flow && normal) m_waterColourSlot = colourSlot;
      // V699: preserve the old scans' ordering, else-if rules and t0 validity.
      for (uint32_t slot = 0; slot < slotCount(); ++slot) {
        const auto& name = lowerName(slot);
        if (name.find("diffusemaps") != std::string::npos && m_terrain.diffuse == UINT32_MAX)
          m_terrain.diffuse = slot;
        else if (name.find("normalmaps") != std::string::npos && m_terrain.normal == UINT32_MAX)
          m_terrain.normal = slot;
        else if (name.find("overlaymap") != std::string::npos)
          m_terrain.declaresOverlay = true;
        if (m_terrain.overlay == UINT32_MAX && name.find("overlaymap") != std::string::npos)
          m_terrain.overlay = slot;
        if (name.find("blendmap") != std::string::npos) {
          if (m_terrain.blend == UINT32_MAX) m_terrain.blend = slot;
          continue;
        }
        const size_t diffusePos = name.find("diffusemaps");
        const size_t normalPos = name.find("normalmaps");
        const bool isDiffuse = diffusePos != std::string::npos;
        const size_t stemPos = isDiffuse ? diffusePos : normalPos;
        if (stemPos == std::string::npos) continue;
        const std::string suffix = name.substr(stemPos + (isDiffuse ? 11u : 10u));
        if (suffix.size() != 1u || suffix[0] < '1' || suffix[0] > '3') continue;
        const uint32_t set = uint32_t(suffix[0] - '1');
        if (isDiffuse) m_terrain.setDiffuse[set] = slot;
        else m_terrain.setNormal[set] = slot;
      }
    }
    const std::string& name(uint32_t slot) const {
      return slot < m_names.size() ? m_names[slot].original : emptyName();
    }
    const std::string& lowerName(uint32_t slot) const {
      return slot < m_names.size() ? m_names[slot].lower : emptyName();
    }
    uint32_t slotCount() const { return uint32_t(m_names.size()); }
    uint32_t waterColourSlot() const { return m_waterColourSlot; }
    const TerrainSlots& terrainSlots() const { return m_terrain; }
  private:
    static const std::string& emptyName() { static const std::string empty; return empty; }
    struct Name { std::string original, lower; };
    std::vector<Name> m_names;
    uint32_t m_waterColourSlot = UINT32_MAX;
    TerrainSlots m_terrain;
  };
}
