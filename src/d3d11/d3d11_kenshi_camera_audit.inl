// Included inside namespace dxvk. Detects camera cuts from applied native operations.
namespace native_camera_audit {
using namespace kenshi_camera_audit;
using Binary = void (__fastcall*)(void*, const void*);
using Unary = void (__fastcall*)(void*);
using Update = void (__fastcall*)(void*, bool);
using NodePosition = float* (__fastcall*)(const void*, float*);
using CameraView = const float* (__fastcall*)(const void*, bool);
static uint8_t* executable = nullptr;
static const kenshi_executable::Profile* executableProfile = nullptr;
static Binary originalTeleport, originalFollow, originalLoad, originalSetup, originalSetPosition;
static Unary originalReset, originalStop;
static Update originalUpdate;
static NodePosition nodePosition;
static CameraView cameraView;
static std::mutex mutex;
static Snapshot snapshot;
static std::atomic<uint64_t> revision{0};
static std::atomic<unsigned> installState{0};
static uint64_t reportedRequest = 0;
static Handle resolvedTarget{};
static bool haveResolvedTarget = false;
static thread_local void* updatingCamera = nullptr;

// Guard only our probes. Exceptions from the original native operations retain
// their original behavior and are never swallowed by this observer.
static void* mainCamera() {
  __try {
    const auto* player = *reinterpret_cast<uint8_t**>(executable + executableProfile->player);
    return player ? *reinterpret_cast<void* const*>(player + 0x30) : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static Pose readPose(void* camera, bool includeView = true) {
  Pose out;
  __try {
    if (!camera) return out;
    auto* p = static_cast<const uint8_t*>(camera);
    out.camera = reinterpret_cast<uintptr_t>(camera);
    out.main = camera == mainCamera();
    out.ogreCamera = *reinterpret_cast<const uintptr_t*>(p + 0x68);
    auto* center = *reinterpret_cast<void* const*>(p + 0x58);
    if (!center || !out.ogreCamera) return out;
    std::memcpy(out.target.data(), p + 0x30, sizeof(out.target));
    std::memcpy(out.offset.data(), p + 0x48, sizeof(out.offset));
    nodePosition(center, out.center.data());
    const auto* ogre = reinterpret_cast<const void*>(out.ogreCamera);
    // The follow setter probe runs before the game's transform-finalization
    // call. Read only its center/handle there; don't force lazy camera updates.
    if (includeView) {
      std::memcpy(out.view.data(), cameraView(ogre, true), sizeof(out.view));
      out.hasView = true;
    }
    out.valid = true;
    for (float v : out.view) out.valid &= std::isfinite(v);
    for (unsigned i = 0; i < 3; ++i)
      out.valid &= std::isfinite(out.center[i]);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out.valid = false; }
  return out;
}
static bool readTarget(const void* object, Handle& out) {
  __try {
    if (!object) return false;
    std::memcpy(out.data(), static_cast<const uint8_t*>(object) + 0x60, sizeof(out));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void changed() {
  ++snapshot.version;
  revision.store(snapshot.version, std::memory_order_release);
}
static void begin() {
  std::lock_guard<std::mutex> lock(mutex);
  ++snapshot.busy;
  changed();
}
static void publish(const Pose& pose, bool newGeneration = false) {
  if (!pose.main) return;
  if (newGeneration || snapshot.pose.camera != pose.camera) {
    ++snapshot.generation;
    snapshot.pendingRequest = 0;
    snapshot.pendingFollowCut = false;
    reportedRequest = 0;
    haveResolvedTarget = false;
  }
  snapshot.pose = pose;
  snapshot.nativeThread = GetCurrentThreadId();
}
static void cut(CutReason reason) {
  ++snapshot.cuts;
  snapshot.reason = reason;
}
static void finish(const char* kind, const Pose& before, const Pose& after) {
  std::lock_guard<std::mutex> lock(mutex);
  publish(after, std::strcmp(kind, "setup") == 0);
  if (std::strcmp(kind, "follow-request") == 0 && after.main) {
    snapshot.pendingRequest = ++snapshot.requests;
    // followObject only copies the handle and zeros offset. Identical writes
    // cannot cause a jump, even if the character moves before the next update.
    // Preserve an earlier effective request if an identical request repeats.
    snapshot.pendingFollowCut |= before.valid && after.valid
      && (before.target != after.target || before.offset != after.offset);
  }
  const bool stop = std::strcmp(kind, "stop-follow") == 0;
  if (after.main && (stop || std::strcmp(kind, "load") == 0 || std::strcmp(kind, "reset") == 0)) {
    snapshot.pendingRequest = 0;
    snapshot.pendingFollowCut = false;
  }
  if (after.main && after.valid) {
    // Loading replaces world/camera state even when the saved pose is identical.
    if (std::strcmp(kind, "load") == 0) cut(CutReason::Load);
    else if (before.valid && (before.center != after.center || before.view != after.view)) {
      if (std::strcmp(kind, "teleport") == 0) cut(CutReason::Teleport);
      if (std::strcmp(kind, "reset") == 0) cut(CutReason::Reset);
    }
  }
  --snapshot.busy;
  changed();
}
static void binary(const char* kind, Binary original, void* camera, const void* arg) {
  begin();
  const auto before = readPose(camera);
  original(camera, arg);
  const auto after = readPose(camera);
  finish(kind, before, after);
}
static void unary(const char* kind, Unary original, void* camera) {
  begin();
  const auto before = readPose(camera);
  original(camera);
  const auto after = readPose(camera);
  finish(kind, before, after);
}
static __declspec(noinline) void __fastcall teleport(void* c, const void* p) {
  binary("teleport", originalTeleport, c, p);
}
static __declspec(noinline) void __fastcall follow(void* c, const void* p) {
  binary("follow-request", originalFollow, c, p);
}
static __declspec(noinline) void __fastcall load(void* c, const void* p) {
  binary("load", originalLoad, c, p);
}
static __declspec(noinline) void __fastcall reset(void* c) {
  unary("reset", originalReset, c);
}
static __declspec(noinline) void __fastcall stop(void* c) {
  unary("stop-follow", originalStop, c);
}
static __declspec(noinline) void __fastcall setup(void* player, const void* camera) {
  begin();
  const auto before = readPose(mainCamera());
  originalSetup(player, camera);
  finish("setup", before, readPose(mainCamera()));
}
static void __fastcall update(void* camera, bool controls) {
  begin();
  void* previous = updatingCamera;
  updatingCamera = camera;
  originalUpdate(camera, controls);
  updatingCamera = previous;
  const auto after = readPose(camera);
  std::lock_guard<std::mutex> lock(mutex);
  publish(after);
  --snapshot.busy;
  changed();
}
// The guarded call at 0x6b1361 has RCX=center, RDX=destination and RDI=resolved
// RootObject. Its relay copies RDI to R8 without changing the original arguments.
static __declspec(noinline) void __fastcall applyFollow(void* node, const void* value, const void* object) {
  Handle resolved{};
  const bool main = updatingCamera && updatingCamera == mainCamera();
  const bool resolvedValid = main && readTarget(object, resolved);
  bool report = false;
  if (main) {
    std::lock_guard<std::mutex> lock(mutex);
    report = (snapshot.pendingRequest && snapshot.pendingRequest != reportedRequest)
      || (snapshot.pendingRequest && resolvedValid && resolved == snapshot.pose.target)
      || (resolvedValid && (!haveResolvedTarget || resolved != resolvedTarget));
  }
  const auto before = report ? readPose(updatingCamera, false) : Pose{};
  originalSetPosition(node, value);
  if (!main) return;
  const auto applied = report ? readPose(updatingCamera, false) : Pose{};
  std::lock_guard<std::mutex> lock(mutex);
  const auto request = snapshot.pendingRequest;
  if (report) {
    // The game replaces the center here; continuous following is not an event.
    // Also cover a native fallback switching to a different resolved character.
    const bool targetSwitch = resolvedValid && haveResolvedTarget && resolved != resolvedTarget;
    if (before.valid && applied.valid && before.center != applied.center
        && (snapshot.pendingFollowCut || targetSwitch)) cut(CutReason::Follow);
    snapshot.pendingFollowCut = false;
    reportedRequest = request;
    if (request && resolvedValid && resolved == applied.target) {
      snapshot.pendingRequest = 0;
    }
  }
  if (resolvedValid) { resolvedTarget = resolved; haveResolvedTarget = true; }
  changed();
}

static bool bytes(const uint8_t* base, size_t rva, const char* hex) {
  for (size_t i = 0; hex[i]; i += 2) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    if (base[rva + i / 2] != uint8_t(digit(hex[i]) * 16 + digit(hex[i + 1]))) return false;
  }
  return true;
}
static bool identity(const uint8_t* base, uint32_t timestamp, uint32_t size) {
  if (!base) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
  const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return pe->Signature == IMAGE_NT_SIGNATURE && pe->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
    && pe->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
    && pe->FileHeader.TimeDateStamp == timestamp && pe->OptionalHeader.SizeOfImage == size;
}
static void writeRelay(uint8_t* entry, const void* function, bool target) {
  if (target) { entry[0] = 0x4c; entry[1] = 0x8b; entry[2] = 0xc7; entry += 3; }
  const uint8_t opcode[] = {0xff,0x25,0,0,0,0};
  std::memcpy(entry, opcode, 6);
  std::memcpy(entry + 6, &function, 8);
}
static void install() {
  unsigned expected = 0;
  if (!installState.compare_exchange_strong(expected, 1)) return;
  auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* ogre = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"OgreMain_x64.dll"));
  struct Guard { uint32_t rva; const char* hex; };
  const auto* gameProfile = kenshi_executable::identify(exe);
  if (!gameProfile) {
    installState.store(3);
    Logger::warn("[CameraNative V794] unrecognized executable; native hooks left untouched");
    return;
  }
  const auto& gameGuards = gameProfile->cameraGuards;
  constexpr Guard ogreGuards[] = {
    {0x1de1c0,"440fb649384c8b4150438b04888902438b44881089420443"},
    {0x82c40,"40534883ec20488b01488bd9ff9088000000488d83640500"},
    {0x85450,"40534883ec20488b01488bd984d27413ff9088000000488d"}
  };
  struct Patch { uint32_t rva; const void* function; const char* hex; bool call; bool target; DWORD protection = 0; };
  Patch patches[] = {
    {0x25c2f, reinterpret_cast<const void*>(&teleport), "e9dc9c6800", false, false},
    {0x516a4, reinterpret_cast<const void*>(&follow), "e977ce6500", false, false},
    {0x21571, reinterpret_cast<const void*>(&load), "e9eac87800", false, false},
    {0x50619, reinterpret_cast<const void*>(&reset), "e9620e6600", false, false},
    {0x38fe6, reinterpret_cast<const void*>(&update), "e9a57f6700", false, false},
    {0x71c6, reinterpret_cast<const void*>(&setup), "e9f5b47e00", false, false},
    {0x4272b, reinterpret_cast<const void*>(&stop), "e930be6600", false, false},
    {0x6b1361, reinterpret_cast<const void*>(&applyFollow), "ff15816bb901", true, true}
  };
  for (size_t i = 0; i < std::size(patches); ++i) {
    patches[i].rva = gameProfile->cameraPatches[i].rva;
    patches[i].hex = gameProfile->cameraPatches[i].hex;
  }
  bool supported = identity(ogre, 0x5ca5f929, 0x9c9000);
  if (supported) {
    for (const auto& g : gameGuards) supported &= bytes(exe, g.rva, g.hex);
    for (const auto& g : ogreGuards) supported &= bytes(ogre, g.rva, g.hex);
    for (const auto& p : patches) supported &= bytes(exe, p.rva, p.hex);
  }
  if (!supported) {
    installState.store(3);
    Logger::warn("[CameraNative V786] unsupported signatures; native cuts unavailable; movement cuts disabled");
    return;
  }
  // D3D11 context construction precedes gameplay camera registration. Refuse a
  // late install rather than write executable instructions on an active camera.
  executable = exe;
  executableProfile = gameProfile;
  if (mainCamera()) {
    installState.store(3);
    Logger::warn("[CameraNative V786] late install refused; native cuts unavailable; movement cuts disabled");
    return;
  }
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const uintptr_t granularity = info.dwAllocationGranularity;
  const auto aligned = reinterpret_cast<uintptr_t>(exe) & ~(granularity - 1);
  uint8_t* relay = nullptr;
  for (uintptr_t off = granularity; off < 0x40000000 && !relay; off += granularity) {
    for (unsigned direction = 0; direction < 2 && !relay; ++direction) {
      if (direction && aligned < off) continue;
      relay = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(direction ? aligned - off : aligned + off),
        4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
  }
  if (!relay) {
    installState.store(3);
    Logger::warn("[CameraNative V785] relay allocation failed; observer unavailable");
    return;
  }
  std::array<std::array<uint8_t, 6>, 8> replacements{};
  for (size_t i = 0; i < std::size(patches); ++i) {
    const auto& p = patches[i];
    uint8_t* entry = relay + i * 32;
    writeRelay(entry, p.function, p.target);
    const auto delta = reinterpret_cast<intptr_t>(entry) - reinterpret_cast<intptr_t>(exe + p.rva + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) {
      VirtualFree(relay, 0, MEM_RELEASE); installState.store(3); return;
    }
    replacements[i][0] = p.call ? 0xe8 : 0xe9;
    const int32_t relative = static_cast<int32_t>(delta);
    std::memcpy(replacements[i].data() + 1, &relative, 4);
    replacements[i][5] = 0x90;
  }
  DWORD ignored;
  if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &ignored)) {
    VirtualFree(relay, 0, MEM_RELEASE); installState.store(3); return;
  }
  FlushInstructionCache(GetCurrentProcess(), relay, 4096);
  size_t writable = 0;
  for (; writable < std::size(patches); ++writable) {
    auto& p = patches[writable];
    if (!VirtualProtect(exe + p.rva, p.call ? 6 : 5, PAGE_EXECUTE_READWRITE, &p.protection)) break;
  }
  if (writable != std::size(patches)) {
    while (writable) { auto& p = patches[--writable]; VirtualProtect(exe + p.rva, p.call ? 6 : 5, p.protection, &ignored); }
    VirtualFree(relay, 0, MEM_RELEASE); installState.store(3);
    Logger::warn("[CameraNative V785] patch protection failed; observer unavailable");
    return;
  }
  originalTeleport = reinterpret_cast<Binary>(exe + gameProfile->cameraGuards[0].rva);
  originalFollow = reinterpret_cast<Binary>(exe + gameProfile->cameraGuards[1].rva);
  originalLoad = reinterpret_cast<Binary>(exe + gameProfile->cameraGuards[2].rva);
  originalSetup = reinterpret_cast<Binary>(exe + gameProfile->cameraGuards[5].rva);
  originalReset = reinterpret_cast<Unary>(exe + gameProfile->cameraGuards[3].rva);
  originalStop = reinterpret_cast<Unary>(exe + gameProfile->cameraGuards[6].rva);
  originalUpdate = reinterpret_cast<Update>(exe + gameProfile->cameraGuards[4].rva);
  originalSetPosition = *reinterpret_cast<Binary*>(exe + gameProfile->setPositionIat);
  nodePosition = reinterpret_cast<NodePosition>(ogre + 0x1de1c0);
  cameraView = reinterpret_cast<CameraView>(ogre + 0x85450);
  for (size_t i = 0; i < std::size(patches); ++i) {
    auto& p = patches[i];
    std::memcpy(exe + p.rva, replacements[i].data(), p.call ? 6 : 5);
    FlushInstructionCache(GetCurrentProcess(), exe + p.rva, p.call ? 6 : 5);
  }
  // Reverse order matters when multiple patch sites occupy one memory page.
  for (size_t i = std::size(patches); i > 0; --i) {
    auto& p = patches[i - 1]; VirtualProtect(exe + p.rva, p.call ? 6 : 5, p.protection, &ignored);
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    snapshot.installed = true;
    changed();
  }
  installState.store(2);
  KENSHI_DIAGNOSTIC_INFO("[KenshiCamera V787] native applied cuts enabled; temporary telemetry removed");
}
static uint64_t currentVersion() { return revision.load(std::memory_order_acquire); }
static Submission capture(uint64_t before) {
  static thread_local Snapshot cached;
  if (cached.version != currentVersion()) {
    std::lock_guard<std::mutex> lock(mutex);
    cached = snapshot;
  }
  return {cached, GetCurrentThreadId(), coherent(before, cached)};
}
}
