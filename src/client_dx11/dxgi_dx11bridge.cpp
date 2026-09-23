#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include "dx11_common.h"

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);

static HMODULE g_dxgiSystem = nullptr;
static FARPROC getDxgiProc(const char* name) {
  if (!g_dxgiSystem) {
    g_dxgiSystem = dx11_bridge_getgoing::loadSystemDll("dxgi.dll");
    if (g_dxgiSystem) dx11_bridge_getgoing::logLine("dxgi", "Loaded system dxgi.dll.");
  }
  return g_dxgiSystem ? GetProcAddress(g_dxgiSystem, name) : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("dxgi", "Loaded x86 DX11 bridge bring-up dxgi.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory>(getDxgiProc("CreateDXGIFactory"));
  return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory1 intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory1>(getDxgiProc("CreateDXGIFactory1"));
  return fn ? fn(riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
  dx11_bridge_getgoing::logLine("dxgi", "CreateDXGIFactory2 intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  auto fn = reinterpret_cast<PFN_CreateDXGIFactory2>(getDxgiProc("CreateDXGIFactory2"));
  return fn ? fn(Flags, riid, ppFactory) : E_FAIL;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
  auto fn = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(getDxgiProc("DXGIDeclareAdapterRemovalSupport"));
  return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** ppDebug) {
  auto fn = reinterpret_cast<PFN_DXGIGetDebugInterface1>(getDxgiProc("DXGIGetDebugInterface1"));
  return fn ? fn(Flags, riid, ppDebug) : E_NOTIMPL;
}
