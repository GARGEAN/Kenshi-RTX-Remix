// Included only by d3d11_kenshi_ogre_culling.cpp; shares its relay allocator.
namespace dxvk {
  namespace {
    using TerrainSelect = void (__fastcall *)(void*, const void*);
    using TerrainQuery = std::uint32_t (__fastcall *)(const void*, const float*, std::uint32_t);
    TerrainSelect s_terrainSelect = nullptr;
    TerrainQuery s_terrainQuery = nullptr;
    std::atomic<unsigned> s_terrainInstallState {0};
    std::atomic<float> s_terrainRadius {0};
    DWORD s_terrainThread = 0;
    struct TerrainScope {
      const void* wrapper = nullptr;
      float position[3] = {};
      double radiusSquared = 0;
    };
    thread_local TerrainScope t_terrainScope;

    std::uint32_t __fastcall queryTerrain(const void* wrapper, const float* box, std::uint32_t mask) {
      const auto result = s_terrainQuery(wrapper, box, mask);
      auto& scope = t_terrainScope;
      if (scope.wrapper != wrapper || !scope.radiusSquared) return result;
      if (!kenshi_terrain_culling::rescue(box, scope.position, scope.radiusSquared, mask, result)) return result;
      // Coverage is bounded by radius and the native loaded/LOD-selected tree,
      // not visit order or a rescue count that can truncate larger radii.
      // Never return 1: partially rescued parents must still distance-test children.
      return mask;
    }

    void __fastcall selectTerrain(void* tree, const void* wrapper) {
      const float radius = s_terrainRadius.load(std::memory_order_relaxed);
      if (radius <= 0 || GetCurrentThreadId() != s_terrainThread) {
        s_terrainSelect(tree, wrapper);
        return;
      }
      const TerrainScope previous = t_terrainScope;
      t_terrainScope = {};
      // _notifyCurrentCamera itself distinguishes its ordinary camera from the
      // reflected pass using positive world Y. Read the adjusted wrapper space.
      if (!previous.wrapper && kenshi_terrain_culling::cameraPosition(wrapper, t_terrainScope.position)
          && t_terrainScope.position[1] > 0) {
        t_terrainScope.wrapper = wrapper;
        t_terrainScope.radiusSquared = double(radius) * radius;
      }
      struct Restore { TerrainScope previous; ~Restore() { t_terrainScope = previous; } } restore {previous};
      s_terrainSelect(tree, wrapper);
    }

    bool installTerrainCulling() {
      unsigned expected = 0;
      if (!s_terrainInstallState.compare_exchange_strong(expected, 1)) return expected == 2;
      auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"Plugin_Terrain_x64.dll"));
      if (!base) { s_terrainInstallState.store(0); return false; } // Retry after plugin load.
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
      const std::uint8_t selectBytes[16] = {0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x48,0x83,0xec,0x28,0x48,0x8b};
      const std::uint8_t queryBytes[16] = {0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x48};
      struct Patch { unsigned rva; const void* handler; std::uint8_t original[5]; DWORD protection; std::uint8_t replacement[5]; };
      Patch patches[] = {
        {0x12853, reinterpret_cast<const void*>(&selectTerrain), {0xe8,0xd8,0xf5,0xfe,0xff}, 0, {}},
        {0x398e, reinterpret_cast<const void*>(&queryTerrain), {0xe8,0xbd,0xe2,0x00,0x00}, 0, {}}
      };
      if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp != 0x65fb1113
          || pe->OptionalHeader.SizeOfImage != 0x56000
          || std::memcmp(base + 0x1e30, selectBytes, sizeof(selectBytes))
          || std::memcmp(base + 0x11c50, queryBytes, sizeof(queryBytes))
          || std::memcmp(base + patches[0].rva, patches[0].original, 5)
          || std::memcmp(base + patches[1].rva, patches[1].original, 5)) {
        s_terrainInstallState.store(3);
        Logger::warn("[KenshiTerrainCulling V687] unsupported/modified terrain plugin; no patches installed.");
        return false;
      }
      auto* relay = static_cast<std::uint8_t*>(allocateRelayNear(base + patches[0].rva));
      if (!relay) {
        s_terrainInstallState.store(3);
        Logger::warn("[KenshiTerrainCulling V687] relay allocation failed; native behavior retained.");
        return false;
      }
      for (unsigned i = 0; i < 2; ++i) {
        auto& p = patches[i];
        const auto displacement = reinterpret_cast<std::intptr_t>(relay + i * 16)
          - reinterpret_cast<std::intptr_t>(base + p.rva + 5);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
          VirtualFree(relay, 0, MEM_RELEASE); s_terrainInstallState.store(3); return false;
        }
        std::uint8_t code[14] = {0xff,0x25,0,0,0,0};
        std::memcpy(code + 6, &p.handler, sizeof(p.handler));
        std::memcpy(relay + i * 16, code, sizeof(code));
        p.replacement[0] = 0xe8;
        const auto relative = static_cast<std::int32_t>(displacement);
        std::memcpy(p.replacement + 1, &relative, sizeof(relative));
      }
      DWORD ignored;
      if (!VirtualProtect(relay, 64, PAGE_EXECUTE_READ, &ignored)) {
        VirtualFree(relay, 0, MEM_RELEASE); s_terrainInstallState.store(3); return false;
      }
      FlushInstructionCache(GetCurrentProcess(), relay, 64);
      for (unsigned i = 0; i < 2; ++i) {
        if (!VirtualProtect(base + patches[i].rva, 5, PAGE_EXECUTE_READWRITE, &patches[i].protection)) {
          for (unsigned j = i; j > 0; --j) VirtualProtect(base + patches[j-1].rva, 5, patches[j-1].protection, &ignored);
          VirtualFree(relay, 0, MEM_RELEASE); s_terrainInstallState.store(3);
          Logger::warn("[KenshiTerrainCulling V687] patch permissions failed; native behavior retained.");
          return false;
        }
      }
      s_terrainSelect = reinterpret_cast<TerrainSelect>(base + 0x1e30);
      s_terrainQuery = reinterpret_cast<TerrainQuery>(base + 0x11c50);
      s_terrainThread = GetCurrentThreadId();
      for (auto& p : patches) {
        std::memcpy(base + p.rva, p.replacement, 5);
        FlushInstructionCache(GetCurrentProcess(), base + p.rva, 5);
      }
      for (unsigned i = 2; i > 0; --i) VirtualProtect(base + patches[i-1].rva, 5, patches[i-1].protection, &ignored);
      s_terrainInstallState.store(2);
      KENSHI_DIAGNOSTIC_INFO("[KenshiTerrainCulling V687] bounded terrain installed; camera-wrapper world space; inherited plane masks; native LOD/loading retained.");
      return true;
    }
  }

  void RemixUpdateKenshiTerrainCulling(bool enabled, float radius) {
    installTerrainCulling();
    // All finite float radii square safely in the double-precision scope.
    const float value = enabled && std::isfinite(radius)
      ? std::max(radius, 100.0f) : 0.0f;
    if (s_terrainRadius.exchange(value, std::memory_order_relaxed) != value)
      KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiTerrainCulling V687] live settings: enabled=", value > 0,
        " radius=", value, " installed=", s_terrainInstallState.load() == 2,
        " radiusCeiling=none nodeCountCutoff=none"));
  }
}
