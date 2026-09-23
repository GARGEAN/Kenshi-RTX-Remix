#include "dxgi_factory.h"
#include "../util/util_kenshi_telemetry.h"
#include "dxgi_include.h"

#include <delayimp.h>

#include "../util/util_env.h"

// DX11_V282_SATELLITE_DELAYLOAD: same policy as d3d11_main.cpp - a missing
// delay-loaded satellite is logged at first use instead of killing the
// process at load time with no logs.
static FARPROC WINAPI remixDxgiDelayLoadFailureHook(unsigned dliNotify, PDelayLoadInfo pdli) {
  if ((dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc)
   && pdli != nullptr && pdli->szDll != nullptr) {
    char msg[320];
    size_t pos = 0;
    const char* head = "MISSING SATELLITE DLL (copy the FULL x64 payload next to the game exe): ";
    for (const char* s = head; *s != '\0' && pos < sizeof(msg) - 1; ++s)
      msg[pos++] = *s;
    for (const char* s = pdli->szDll; *s != '\0' && pos < sizeof(msg) - 1; ++s)
      msg[pos++] = *s;
    msg[pos] = '\0';
    dxvk::env::remixAppendBootLine("dxgi.dll", msg);
    dxvk::Logger::err(std::string("[Remix-DX11] ") + msg);
  }
  return nullptr;
}
extern "C" const PfnDliHook __pfnDliFailureHook2 = remixDxgiDelayLoadFailureHook;

// DX11_V290_RUNTIME_DIR: same delay-load redirect as d3d11_main.cpp - prefer
// the dedicated Remix runtime directory when it exists.
static FARPROC WINAPI remixDxgiDelayLoadNotifyHook(unsigned dliNotify, PDelayLoadInfo pdli) {
  if (dliNotify == dliNotePreLoadLibrary
   && pdli != nullptr && pdli->szDll != nullptr) {
    wchar_t fullPath[MAX_PATH];
    const DWORD dirLen = dxvk::env::remixResolveRuntimeDirectoryW(fullPath, MAX_PATH);
    if (dirLen != 0) {
      size_t pos = dirLen;
      if (pos < MAX_PATH - 1)
        fullPath[pos++] = L'\\';
      const char* name = pdli->szDll;
      for (; *name != '\0' && pos < MAX_PATH - 1; ++name)
        fullPath[pos++] = wchar_t(static_cast<unsigned char>(*name));
      fullPath[pos] = L'\0';
      if (*name == '\0'
       && ::GetFileAttributesW(fullPath) != INVALID_FILE_ATTRIBUTES) {
        const HMODULE loaded =
          ::LoadLibraryExW(fullPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (loaded != nullptr)
          return reinterpret_cast<FARPROC>(loaded);
      }
    }
  }
  return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = remixDxgiDelayLoadNotifyHook;

// Same DLL search path fix as d3d11_main.cpp — ensures Remix runtime DLLs
// are found in the game directory when loaded through launchers with
// restricted search paths.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
    // DX11_V282_BOOT_BREADCRUMB (kernel32-only, loader-lock safe; see
    // util_env.h - proves the DLL attached even when no other log exists).
    dxvk::env::remixAppendBootLine("dxgi.dll",
      dxvk::env::shouldBypassRemixForCurrentProcess() ? "attached (bypass)" : "attached");
    if (!dxvk::env::shouldBypassRemixForCurrentProcess()) {
      wchar_t path[MAX_PATH];
      if (GetModuleFileNameW(hModule, path, MAX_PATH)) {
        wchar_t* sep = wcsrchr(path, L'\\');
        if (sep) {
          *sep = L'\0';
          SetDllDirectoryW(path);
          AddDllDirectory(path);
        }
      }
      // DX11_V290_RUNTIME_DIR: dxgi.dll is loaded BEFORE d3d11.dll in the
      // standard adapter-enumeration flow, so registering the runtime
      // directory here is what allows d3d11.dll's hard USD/python/boost
      // load-time imports to resolve from rtx-remix\runtime (or
      // DXVK_REMIX_RUNTIME_DIR) instead of the game folder.
      wchar_t runtimeDir[MAX_PATH];
      if (dxvk::env::remixResolveRuntimeDirectoryW(runtimeDir, MAX_PATH) != 0) {
        SetDllDirectoryW(runtimeDir);
        AddDllDirectory(runtimeDir);
        dxvk::env::remixAppendBootLine("dxgi.dll", "runtime directory registered (rtx-remix\\runtime or DXVK_REMIX_RUNTIME_DIR)");
      }
    }
    }
    return TRUE;
}

namespace dxvk {
  
  Logger Logger::s_instance("dxgi.log");

  // Intel Vulkan ICD (igvk64.dll/ControlLib.dll) calls CreateDXGIFactory1 from
  // inside vkCreateInstance to enumerate adapters. That re-enters our dxgi.dll,
  // creates another DxvkInstance, calls vkCreateInstance again → stack overflow.
  // Guard: on reentrant calls forward to the real system dxgi.dll instead.
  static HRESULT forwardToSystemDxgi(UINT Flags, const char* exportName, REFIID riid, void** ppFactory) {
    wchar_t sysPath[MAX_PATH];
    GetSystemDirectoryW(sysPath, MAX_PATH);
    wcscat_s(sysPath, L"\\dxgi.dll");

    HMODULE hSys = LoadLibraryExW(sysPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hSys) return E_FAIL;

    if (!std::strcmp(exportName, "CreateDXGIFactory2")) {
      using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
      auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(hSys, exportName));

      if (!fn) {
        FreeLibrary(hSys);
        return E_FAIL;
      }

      return fn(Flags, riid, ppFactory);
    }

    using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(hSys, exportName));

    if (!fn) {
      FreeLibrary(hSys);
      return E_FAIL;
    }

    return fn(riid, ppFactory);
  }

  HRESULT createDxgiFactory(UINT Flags, const char* exportName, REFIID riid, void **ppFactory) {
    if (env::shouldBypassRemixForCurrentProcess()) {
      Logger::info(str::format("DXGI bypass for helper process: ", env::getExeName()));
      return forwardToSystemDxgi(Flags, exportName, riid, ppFactory);
    }

    static thread_local bool s_creating = false;
    if (s_creating)
      return forwardToSystemDxgi(Flags, exportName, riid, ppFactory);

    // DX11_V238_INTEROP_DXGI_PASSTHROUGH: the Intel D3D11/D3D12-interop present path needs a REAL DXGI
    // factory (the present runs on a real D3D12 device that bypasses the broken Intel Vulkan WSI).
    // D3D12CreateDevice internally calls CreateDXGIFactory2 by name, which resolves to THIS (our Remix)
    // dxgi.dll and would loop back into DXVK/Vulkan. While the interop sets this env flag (only around
    // its own D3D12 device/swapchain creation), forward to the real system dxgi so the present device
    // gets real DXGI. The game's own DXGI calls (flag clear) still go through DXVK/Remix as normal.
    if (env::getEnvVar("DXVK_REMIX_DXGI_PASSTHROUGH") == "1")
      return forwardToSystemDxgi(Flags, exportName, riid, ppFactory);

    s_creating = true;
    HRESULT hr;
    try {
      Com<DxgiFactory> factory = new DxgiFactory(Flags);
      hr = factory->QueryInterface(riid, ppFactory);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }
    s_creating = false;
    return hr;
  }
}

extern "C" {
  __declspec(dllexport) void WINAPI RemixSetKenshiTelemetry(BOOL enabled) {
    dxvk::kenshi_telemetry::setEnabled(enabled != FALSE);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    dxvk::Logger::info("CreateDXGIFactory2: Ignoring flags");
    return dxvk::createDxgiFactory(Flags, "CreateDXGIFactory2", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory1", riid, ppFactory);
  }
  
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **ppFactory) {
    return dxvk::createDxgiFactory(0, "CreateDXGIFactory", riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    static bool enabled = false;

    if (std::exchange(enabled, true))
      return 0x887a0036; // DXGI_ERROR_ALREADY_EXISTS;

    dxvk::Logger::warn("DXGIDeclareAdapterRemovalSupport: Stub");
    return S_OK;
  }

  DLLEXPORT HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug) {
    static bool errorShown = false;

    if (!std::exchange(errorShown, true))
      dxvk::Logger::warn("DXGIGetDebugInterface1: Stub");

    return E_NOINTERFACE;
  }

  // DX11_V282_SYS_DLL_EXPORTS: the SYSTEM d3d11.dll and d3d10/d3d10_1.dll
  // import these from "dxgi.dll" BY NAME. When the launcher bypass (V279)
  // loads the system d3d11.dll into a process where OUR dxgi.dll is already
  // resident, or any module loads system D3D10, the loader resolves those
  // imports against us; missing names failed the load with
  // STATUS_ENTRYPOINT_NOT_FOUND - games/launchers dying with no logs. Stubs
  // satisfy the loader; D3D10 device creation through Remix's DXGI stays
  // unsupported (E_NOTIMPL). x64 calling convention makes the exact
  // signatures loader-irrelevant; these match the documented DDK shapes.
  DLLEXPORT HRESULT __stdcall DXGID3D10CreateDevice(
    HMODULE hModule, void* pFactory, void* pAdapter, UINT Flags, void* unknown, void** ppDevice) {
    static bool warned = false;
    if (!std::exchange(warned, true))
      dxvk::Logger::warn("DXGID3D10CreateDevice: Stub (D3D10 devices unsupported)");
    return E_NOTIMPL;
  }

  DLLEXPORT HRESULT __stdcall DXGID3D10CreateLayeredDevice(
    void* unknown0, void* unknown1, void* unknown2, void* unknown3, void* unknown4) {
    static bool warned = false;
    if (!std::exchange(warned, true))
      dxvk::Logger::warn("DXGID3D10CreateLayeredDevice: Stub (D3D10 devices unsupported)");
    return E_NOTIMPL;
  }

  DLLEXPORT SIZE_T __stdcall DXGID3D10GetLayeredDeviceSize(
    const void* pLayers, UINT NumLayers) {
    static bool warned = false;
    if (!std::exchange(warned, true))
      dxvk::Logger::warn("DXGID3D10GetLayeredDeviceSize: Stub (D3D10 devices unsupported)");
    return 0;
  }

  DLLEXPORT HRESULT __stdcall DXGID3D10RegisterLayers(
    const void* pLayers, UINT NumLayers) {
    static bool warned = false;
    if (!std::exchange(warned, true))
      dxvk::Logger::warn("DXGID3D10RegisterLayers: Stub (D3D10 devices unsupported)");
    return E_NOTIMPL;
  }

}
