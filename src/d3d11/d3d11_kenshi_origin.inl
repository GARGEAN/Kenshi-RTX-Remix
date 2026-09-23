// Included in d3d11_rtx.cpp, inside namespace dxvk. Read-only native integration.
namespace native_origin {
using Getter = void (__fastcall *)(const void*, float*);
static Getter getter = nullptr;
static uint8_t* executable = nullptr;
static const kenshi_executable::Profile* executableProfile = nullptr;
static bool unsupported = false;
static const void* publisher = nullptr;
static bool published = false;
static kenshi_origin::Snapshot lastPublished;

static bool initialize() {
  if (getter) return true;
  if (unsupported) return false;
  auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto* ogre = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"OgreMain_x64.dll"));
  if (!ogre) return false;
  const auto* gameProfile = kenshi_executable::identify(exe);
  const auto* ogrePe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(ogre + reinterpret_cast<const IMAGE_DOS_HEADER*>(ogre)->e_lfanew);
  const uint8_t getterBytes[] = {0x48,0x8b,0x81,0xf0,0x05,0x00,0x00,0xf3,0x0f,0x10,0x0d,0x71,0x1f,0x3e,0x00,0x44,0x0f,0xb6,0x40,0x38,0x48,0x8b,0x48,0x50};
  const uint8_t setterCall[] = {0xff,0x93,0x90,0x06,0x00,0x00};
  auto* entry = GetProcAddress(reinterpret_cast<HMODULE>(ogre), "?getRelativeOrigin@SceneManager@Ogre@@QEBA?AVVector3@2@XZ");
  if (!gameProfile
      || ogrePe->Signature != IMAGE_NT_SIGNATURE || ogrePe->FileHeader.TimeDateStamp != 0x5ca5f929
      || ogrePe->OptionalHeader.SizeOfImage != 0x9c9000
      || reinterpret_cast<uint8_t*>(entry) != ogre + 0x2c7af0
      || std::memcmp(ogre + 0x2c7af0, getterBytes, sizeof(getterBytes))
      || !kenshi_executable::matches(exe, gameProfile->originUpdate, gameProfile->originPrefix)
      || std::memcmp(exe + gameProfile->originSetter, setterCall, sizeof(setterCall))) {
    unsupported = true;
    Logger::warn("[KenshiOrigin] native signature mismatch; original cut policy retained");
    return false;
  }
  executable = exe;
  executableProfile = gameProfile;
  getter = reinterpret_cast<Getter>(entry);
  return true;
}

// No C++ unwinding objects in this SEH boundary. The observed x64 ABI is this
// in RCX and the 12-byte Vector3 return destination in RDX.
static bool read(kenshi_origin::Snapshot& out) {
  __try {
    const auto* game = *reinterpret_cast<const uint8_t* const*>(executable + executableProfile->game);
    if (!game) return false;
    const auto* manager = *reinterpret_cast<const void* const*>(game + 0x60);
    if (!manager) return false;
    getter(manager, out.origin.data());
    out.manager = reinterpret_cast<uintptr_t>(manager);
    out.valid = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = {};
    return false;
  }
}
}
