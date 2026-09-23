#include "../util/util_kenshi_telemetry.h"
#include "d3d11_kenshi_ogre_culling.h"
#include "d3d11_kenshi_executable.h"
#include "d3d11_kenshi_culling_policy.h"
#include "d3d11_kenshi_terrain_culling.h"
#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"
#include "../util/util_kenshi_terrain_profile.h"

#include <atomic>
#include <array>
#include <new>
#include <windows.h>

namespace dxvk {
  namespace {
    constexpr std::uintptr_t kHwCullRva = 0x12fba0;
    constexpr std::uintptr_t kInstanceImplRva = 0x1286a0;
    constexpr std::uintptr_t kNativeCullRva = 0x1d7a90;
    using InstanceImpl = void (__fastcall *)(void*, const void*, const void*, std::uint32_t);
    using NativeCull = void (__fastcall *)(std::size_t, kenshi_culling::ObjectData*,
      const void*, std::uint32_t, void*, const void*);
    InstanceImpl s_instanceImpl = nullptr;
    NativeCull s_nativeCull = nullptr;
    const void* s_hwCullMethod = nullptr;
    const void* s_staticFeatureEntityVtable = nullptr;
    const void* s_rigidFeatureVtable = nullptr;
    const void* const* s_moonControllers = nullptr;
    std::atomic<std::uint32_t> s_installState = 0;

    thread_local bool t_hwInstances = false;
    thread_local bool t_inCull = false;
    thread_local std::vector<kenshi_culling::PackedAabb> t_boxes;
    struct ThreadStats {
      std::uint64_t calls = 0, outer = 0, inner = 0, nearLanes = 0, farLanes = 0, fallback = 0;
    };
    thread_local ThreadStats t_stats;

    // Shipped Kenshi constructs Moon/Moon2 with planet01.mesh, stores their controllers in these two
    // globals, and stores the Entity at +0. Validate the constructor/call sites before reading any
    // game-owned pointer.
    bool supportedMoonLayout(const std::uint8_t* game, std::size_t imageSize) {
      const auto* profile = kenshi_executable::identify(game);
      if (!profile || imageSize < profile->imageSize) return false;
      for (const auto& guard : profile->moonGuards)
        if (!kenshi_executable::matches(game, guard.rva, guard.hex)) return false;
      return true;
    }

    void readMoonEntities(const void** moons) {
      if (!s_moonControllers) return;
      // Resolve afresh for each outer cull. Do not retain an Entity across game
      // scene changes. OGRE's workers run after the game's scene update barrier.
      // An unavailable controller during startup/teardown leaves native culling.
      __try {
        for (unsigned i = 0; i < 2; ++i) {
          const void* controller = s_moonControllers[i];
          moons[i] = controller ? *static_cast<const void* const*>(controller) : nullptr;
        }
      } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
          ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        moons[0] = moons[1] = nullptr;
      }
    }

    struct NativeCullOutput { const void* const* data; std::size_t size, capacity; };
    static_assert(sizeof(NativeCullOutput) == 24);

    struct BoolScope {
      bool& value;
      bool previous;
      explicit BoolScope(bool& flag) : value(flag), previous(flag) { value = true; }
      ~BoolScope() { value = previous; }
    };

    void callBounded(std::size_t count, kenshi_culling::ObjectData* data,
        const void* frustum, std::uint32_t flags, void* output, const void* camera,
        bool inner) {
      if (count == 0 || !data || !camera || !frustum || t_inCull) {
        s_nativeCull(count, data, frustum, flags, output, camera);
        return;
      }
      const float radius = kenshi_culling::runtimeRadius.load(std::memory_order_relaxed);
      const float featureRadius = kenshi_culling::runtimeFeatureRadius.load(std::memory_order_relaxed);
      const void* moons[2] = {};
      if (!inner) readMoonEntities(moons);
      if (radius <= 0.0f && (inner || featureRadius <= 0.0f) && !moons[0] && !moons[1]) {
        s_nativeCull(count, data, frustum, flags, output, camera);
        return;
      }
      BoolScope active(t_inCull);
      alignas(16) kenshi_culling::ObjectData copy;
      kenshi_culling::Counts counts;
      bool changed = false;
      try {
        changed = kenshi_culling::prepare(count, *data,
          reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(camera) + 0x564),
          inner, s_hwCullMethod, copy, t_boxes, counts, radius, s_staticFeatureEntityVtable,
          featureRadius, s_rigidFeatureVtable, moons[0], moons[1]);
      } catch (const std::bad_alloc&) {
        ++t_stats.fallback;
      }
      if (count > kenshi_culling::kMaxSlots)
        ++t_stats.fallback;
      // Original pass planes and all native distance/visibility/caster checks.
      // Nearby selected lanes and the two moons have infinite private half-extents.
      // Sample only moon-containing calls: bounded native acceptance evidence,
      // not a full-state oracle or a per-frame scene scan.
      static thread_local std::uint64_t moonCalls = 0;
      const bool sampleMoons = kenshi_telemetry::enabled() && changed && counts.moonMask && output && (++moonCalls % 600 == 1);
      const std::size_t outputStart = sampleMoons ? static_cast<NativeCullOutput*>(output)->size : 0;
      s_nativeCull(count, changed ? &copy : data, frustum, flags, output, camera);
      if (sampleMoons) {
        const auto& result = *static_cast<const NativeCullOutput*>(output);
        const bool valid = (!result.size || result.data) && result.size >= outputStart && result.size <= result.capacity
          && result.size - outputStart <= count;
        unsigned accepted = 0;
        if (valid) for (std::size_t i = outputStart; i < result.size; ++i) {
          if (result.data[i] == moons[0]) accepted |= 1;
          if (result.data[i] == moons[1]) accepted |= 2;
        }
        KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiMoons V784] thread=", GetCurrentThreadId(),
          " moonCullCalls=", moonCalls, " relaxedMask=", counts.moonMask,
          " acceptedMask=", accepted, " outputValid=", valid, " passFlags=", flags,
          " (bits 1=Moon 2=Moon2; native visibility/distance/caster checks retained)"));
      }
      if (!terrain_profile::diagnosticsEnabled()) return;
      t_stats.nearLanes += counts.nearLanes;
      t_stats.farLanes += counts.farLanes;
      if (inner) ++t_stats.inner; else ++t_stats.outer;
      if ((++t_stats.calls & 0x7fffull) == 0) {
        KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiCulling V681] thread=", GetCurrentThreadId(),
          " outerCalls=", t_stats.outer, " innerCalls=", t_stats.inner,
          " nearLanes=", t_stats.nearLanes, " farNativeLanes=", t_stats.farLanes,
          " fallback=", t_stats.fallback));
      }
    }

    void __fastcall cullScene(std::size_t count, kenshi_culling::ObjectData* data,
        const void* frustum, std::uint32_t flags, void* output, const void* camera) {
      callBounded(count, data, frustum, flags, output, camera, false);
    }
    void __fastcall cullThreadedInstances(std::size_t count, kenshi_culling::ObjectData* data,
        const void* frustum, std::uint32_t flags, void* output, const void* camera) {
      if (t_hwInstances)
        callBounded(count, data, frustum, flags, output, camera, true);
      else
        s_nativeCull(count, data, frustum, flags, output, camera);
    }
    void __fastcall cullSingleThreadInstances(std::size_t count, kenshi_culling::ObjectData* data,
        const void* frustum, std::uint32_t flags, void* output, const void* camera) {
      // This call site belongs exclusively to InstanceBatchHW::updateVertexBuffer.
      callBounded(count, data, frustum, flags, output, camera, true);
    }
    void __fastcall cullHwBatch(void* movable, const void* frustum,
        const void* camera, std::uint32_t flags) {
      BoolScope scope(t_hwInstances);
      // Keep the actual pass frustum. The native-culler wrapper relaxes only
      // nearby instance lanes, rather than neutralizing all planes for the batch.
      s_instanceImpl(static_cast<std::uint8_t*>(movable) - 0x58, frustum, camera, flags);
    }

    void* allocateRelayNear(void* target) {
      SYSTEM_INFO info = {};
      GetSystemInfo(&info);
      const std::uintptr_t granularity = info.dwAllocationGranularity;
      const auto address = reinterpret_cast<std::uintptr_t>(target);
      const auto aligned = address & ~(granularity - 1u);
      for (std::uintptr_t offset = granularity; offset < 0x40000000u; offset += granularity) {
        for (unsigned direction = 0; direction < 2; ++direction) {
          if (direction && aligned < offset) continue;
          const auto candidate = direction ? aligned - offset : aligned + offset;
          if (void* relay = VirtualAlloc(reinterpret_cast<void*>(candidate), 64,
              MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
            return relay;
        }
      }
      return nullptr;
    }
  }

  bool RemixInstallKenshiInstanceBatchCullingPatch() {
    std::uint32_t expected = 0;
    if (!s_installState.compare_exchange_strong(expected, 1))
      return expected == 2;
    if (env::getEnvVar("DXVK_KENSHI_INSTANCE_ANTICULL") == "0") {
      KENSHI_DIAGNOSTIC_INFO("[KenshiCulling V681] disabled; native culling and Remix retention retained.");
      s_installState.store(3);
      return false;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"OgreMain_x64.dll"));
    if (!base) {
      Logger::warn("[KenshiCulling V681] OGRE unavailable; no culling changes.");
      s_installState.store(3);
      return false;
    }

    struct Patch {
      std::uintptr_t rva;
      const void* handler;
      std::uint8_t opcode;
      std::array<std::uint8_t, 9> expected;
      std::size_t expectedSize;
      DWORD protection = 0;
      std::array<std::uint8_t, 5> replacement = {};
    };
    std::array<Patch, 4> patches = {{
      { kHwCullRva, reinterpret_cast<const void*>(&cullHwBatch), 0xe9,
        {0x48,0x83,0xc1,0xa8,0xe9,0xf7,0x8a,0xff,0xff}, 9 },
      { 0x2c19b3, reinterpret_cast<const void*>(&cullScene), 0xe8,
        {0xe8,0xd8,0x60,0xf1,0xff}, 5 },
      { 0x128771, reinterpret_cast<const void*>(&cullThreadedInstances), 0xe8,
        {0xe8,0x1a,0xf3,0x0a,0x00}, 5 },
      { 0x130587, reinterpret_cast<const void*>(&cullSingleThreadInstances), 0xe8,
        {0xe8,0x04,0x75,0x0a,0x00}, 5 }
    }};
    // Validate every site before writing any of them. Unknown binaries fail closed.
    for (const auto& patch : patches) {
      if (std::memcmp(base + patch.rva, patch.expected.data(), patch.expectedSize) != 0) {
        Logger::warn("[KenshiCulling V681] unsupported OgreMain bytes; no culling changes.");
        s_installState.store(3);
        return false;
      }
    }
    auto* relay = static_cast<std::uint8_t*>(allocateRelayNear(base + kHwCullRva));
    if (!relay) {
      Logger::warn("[KenshiCulling V681] relay allocation failed; no culling changes.");
      s_installState.store(3);
      return false;
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
      auto& patch = patches[i];
      const auto displacement = reinterpret_cast<std::intptr_t>(relay + i * 16)
        - reinterpret_cast<std::intptr_t>(base + patch.rva + 5);
      if (displacement < std::numeric_limits<std::int32_t>::min()
       || displacement > std::numeric_limits<std::int32_t>::max()) {
        VirtualFree(relay, 0, MEM_RELEASE);
        s_installState.store(3);
        Logger::warn("[KenshiCulling V681] relay out of range; no culling changes.");
        return false;
      }
      std::uint8_t code[14] = {0xff,0x25,0,0,0,0};
      std::memcpy(code + 6, &patch.handler, sizeof(patch.handler));
      std::memcpy(relay + i * 16, code, sizeof(code));
      patch.replacement[0] = patch.opcode;
      const auto relative = static_cast<std::int32_t>(displacement);
      std::memcpy(patch.replacement.data() + 1, &relative, sizeof(relative));
    }
    // Acquire all write permissions before committing the four-site patch.
    for (std::size_t i = 0; i < patches.size(); ++i) {
      auto& patch = patches[i];
      if (!VirtualProtect(base + patch.rva, 5, PAGE_EXECUTE_READWRITE, &patch.protection)) {
        for (std::size_t j = i; j > 0; --j) {
          auto& prior = patches[j - 1];
          DWORD ignored;
          VirtualProtect(base + prior.rva, 5, prior.protection, &ignored);
        }
        VirtualFree(relay, 0, MEM_RELEASE);
        s_installState.store(3);
        Logger::warn("[KenshiCulling V681] patch protection failed; no culling changes.");
        return false;
      }
    }
    s_instanceImpl = reinterpret_cast<InstanceImpl>(base + kInstanceImplRva);
    s_nativeCull = reinterpret_cast<NativeCull>(base + kNativeCullRva);
    s_hwCullMethod = base + kHwCullRva;
    // Additional eligibility only after validating the accessors establishing
    // Entity's queue, ObjectData flags and static memory-manager layout.
    constexpr std::uint8_t staticGetter[] = {
      0x48,0x8b,0x91,0x28,0x01,0x00,0x00,0x83,0xba,0xa0,0x00,0x00,0x00,0x01,0x0f,0x94,0xc0,0xc3 };
    constexpr std::uint8_t queueGetter[] = {0x0f,0xb6,0x41,0x30,0xc3};
    constexpr std::uint8_t flagsGetter[] = {
      0x0f,0xb6,0x51,0x38,0x48,0x8b,0x41,0x78,0x8b,0x04,0x90,0x25,0xff,0xff,0xff,0x1f,0xc3 };
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (headers->OptionalHeader.SizeOfImage >= 0x67be58
        && std::memcmp(base + 0x1d7390, staticGetter, sizeof(staticGetter)) == 0
        && std::memcmp(base + 0x1d7580, queueGetter, sizeof(queueGetter)) == 0
        && std::memcmp(base + 0x1dbd50, flagsGetter, sizeof(flagsGetter)) == 0) {
      const auto* entityVtable = reinterpret_cast<const void* const*>(base + 0x67be08);
      if (entityVtable[8] == base + 0xc1080 && entityVtable[9] == base + 0x274c0)
        s_staticFeatureEntityVtable = entityVtable;
    }
    // Forests lives in the executable. Its exact rigid vtable and RTTI distinguish
    // it from WindBatchedGeometry, which shares the same cull virtual method.
    auto* game = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (s_staticFeatureEntityVtable && game) {
      const auto* gameHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
      const auto* gameProfile = kenshi_executable::identify(game);
      if (supportedMoonLayout(game, gameHeaders->OptionalHeader.SizeOfImage))
        s_moonControllers = reinterpret_cast<const void* const*>(game + gameProfile->moons);
      constexpr char rigidName[] = ".?AVBatchedGeometry@Forests@@";
      const std::uintptr_t rigidTypeRva = gameProfile ? gameProfile->rigidType : 0;
      if (gameProfile && gameHeaders->OptionalHeader.SizeOfImage >= rigidTypeRva + 16 + sizeof(rigidName)) {
        const auto* rigid = reinterpret_cast<const void* const*>(game + gameProfile->rigidVtable);
        const auto* col = reinterpret_cast<const std::uint32_t*>(game + gameProfile->rigidCol);
        if (rigid[-1] == col && col[0] == 1 && col[1] == 0 && col[2] == 0
            && col[3] == rigidTypeRva && col[5] == gameProfile->rigidCol
            && rigid[2] == game + gameProfile->rigidMethod2 && rigid[8] == game + gameProfile->rigidMethod8
            && rigid[9] == game + gameProfile->rigidMethod9
            && std::memcmp(game + rigidTypeRva + 16, rigidName, sizeof(rigidName)) == 0)
          s_rigidFeatureVtable = rigid;
      }
    }
    KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiCulling V746] static terrain feature Entities=",
      s_staticFeatureEntityVtable ? "enabled" : "unsupported; native culling retained",
      "; categories=0x80,0x20; rigid Forests=", s_rigidFeatureVtable ? "enabled" : "unsupported; native culling retained",
      "; static/queue24; independent Terrain Features toggle/radius; native distance/visibility retained."));
    KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiMoons V784] native moon exception=",
      s_moonControllers ? "enabled; exact two Entity pointers, queue6, no offscreen radius"
                        : "unsupported; native culling retained"));
    DWORD ignored;
    VirtualProtect(relay, 64, PAGE_EXECUTE_READ, &ignored);
    FlushInstructionCache(GetCurrentProcess(), relay, 64);
    for (const auto& patch : patches) {
      std::memcpy(base + patch.rva, patch.replacement.data(), 5);
      FlushInstructionCache(GetCurrentProcess(), base + patch.rva, 5);
    }
    // Reverse order also handles the case of sites sharing a protection page.
    for (auto it = patches.rbegin(); it != patches.rend(); ++it)
      VirtualProtect(base + it->rva, 5, it->protection, &ignored);
    s_installState.store(2);
    KENSHI_DIAGNOSTIC_INFO(str::format("[KenshiCulling V681] bounded HW batches/instances installed; radius=",
      kenshi_culling::kOffscreenRadius,
      " native distance/visibility/caster checks preserved; threaded and single-thread routes covered."));
    return true;
  }
}

#include "d3d11_kenshi_terrain_culling.inl"
