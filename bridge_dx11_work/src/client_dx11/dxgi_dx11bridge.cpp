#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"

#define DX11_V229_COMPLETE_SWAPCHAIN_COVERAGE 1

using PFN_CreateDXGIFactory = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI *)(UINT, REFIID, void**);

static HMODULE gSystemDxgi = nullptr;
static PFN_CreateDXGIFactory pCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 pCreateDXGIFactory2 = nullptr;

using PFN_FactoryCreateSwapChain = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_Factory2CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Factory2CreateSwapChainForCoreWindow = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Factory2CreateSwapChainForComposition = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_FactoryMediaCreateSwapChainForCompositionSurfaceHandle = HRESULT (STDMETHODCALLTYPE *)(IDXGIFactoryMedia*, IUnknown*, HANDLE, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

static PFN_FactoryCreateSwapChain oFactoryCreateSwapChain = nullptr;
static PFN_Factory2CreateSwapChainForHwnd oFactory2CreateSwapChainForHwnd = nullptr;
static PFN_Factory2CreateSwapChainForCoreWindow oFactory2CreateSwapChainForCoreWindow = nullptr;
static PFN_Factory2CreateSwapChainForComposition oFactory2CreateSwapChainForComposition = nullptr;
static PFN_FactoryMediaCreateSwapChainForCompositionSurfaceHandle oFactoryMediaCreateSwapChainForCompositionSurfaceHandle = nullptr;

static SRWLOCK gPendingSwapChainLockV229 = SRWLOCK_INIT;
static IDXGISwapChain* gPendingSwapChainsV229[16] = {};
static UINT gPendingSwapChainCountV229 = 0;

static void DLog(const char* text) {
  dx11_bridge_client::LogLine("dxgi", text);
}

static HMODULE LoadSystemDxgiV229() {
  if (gSystemDxgi) return gSystemDxgi;
  gSystemDxgi = dx11_bridge_client::LoadSystemDll("dxgi.dll");
  if (!gSystemDxgi) return nullptr;
  pCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory"));
  pCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory1"));
  pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(gSystemDxgi, "CreateDXGIFactory2"));
  return gSystemDxgi;
}

template <typename T>
static bool HookVTableV229(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;
  if (vt[slot] == detour) return true;

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    char msg[320] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V229: VirtualProtect failed for %s slot=%u err=%lu.", name, (unsigned) slot, GetLastError());
    DLog(msg);
    return false;
  }

  if (*original == nullptr) *original = reinterpret_cast<T>(vt[slot]);
  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[320] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V229: hooked %s slot=%u on its validated interface.", name, (unsigned) slot);
  DLog(msg);
  return true;
}

static HMODULE FindLocalD3D11ProxyV229() {
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&FindLocalD3D11ProxyV229), &self);
  if (self) {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(self, path, MAX_PATH)) {
      char* slash = strrchr(path, '\\');
      if (slash) {
        strcpy_s(slash + 1, MAX_PATH - static_cast<size_t>((slash + 1) - path), "d3d11.dll");
        HMODULE local = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path, &local)) return local;
      }
    }
  }
  return GetModuleHandleA("d3d11.dll");
}

static void QueuePendingSwapChainV229(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  AcquireSRWLockExclusive(&gPendingSwapChainLockV229);
  for (UINT i = 0; i < gPendingSwapChainCountV229; ++i) {
    if (gPendingSwapChainsV229[i] == swapChain) {
      ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
      return;
    }
  }
  if (gPendingSwapChainCountV229 < _countof(gPendingSwapChainsV229)) {
    swapChain->AddRef();
    gPendingSwapChainsV229[gPendingSwapChainCountV229++] = swapChain;
    ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
    DLog("DX11_V229: queued swapchain created before the local d3d11 Present hook was available.");
    return;
  }
  ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);
  DLog("DX11_V229: pending swapchain queue is full; newest swapchain could not be retained.");
}

using PFN_D3D11SwapChainHook = void (WINAPI *)(IDXGISwapChain*);
static bool TryInstallD3D11SwapChainHookV229(IDXGISwapChain* swapChain) {
  if (!swapChain) return false;
  HMODULE d3d11 = FindLocalD3D11ProxyV229();
  auto hook = d3d11
    ? reinterpret_cast<PFN_D3D11SwapChainHook>(GetProcAddress(d3d11, "DX11BridgeInstallSwapChainCapture"))
    : nullptr;
  if (!hook) {
    QueuePendingSwapChainV229(swapChain);
    return false;
  }
  hook(swapChain);
  DLog("DX11_V229: installed d3d11 Present/Present1 capture on a discovered swapchain.");
  return true;
}

extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallPendingSwapChains() {
  IDXGISwapChain* pending[_countof(gPendingSwapChainsV229)] = {};
  UINT count = 0;
  AcquireSRWLockExclusive(&gPendingSwapChainLockV229);
  count = gPendingSwapChainCountV229;
  for (UINT i = 0; i < count; ++i) {
    pending[i] = gPendingSwapChainsV229[i];
    gPendingSwapChainsV229[i] = nullptr;
  }
  gPendingSwapChainCountV229 = 0;
  ReleaseSRWLockExclusive(&gPendingSwapChainLockV229);

  for (UINT i = 0; i < count; ++i) {
    TryInstallD3D11SwapChainHookV229(pending[i]);
    pending[i]->Release();
  }
  if (count) DLog("DX11_V229: drained deferred swapchains after d3d11 proxy initialization.");
}

static void OnSwapChainCreatedV229(const char* path, IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  char msg[320] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V229: %s returned a swapchain; starting bridge and installing presentation capture.", path);
  DLog(msg);
  dx11_bridge_client::EnsureServer();
  TryInstallD3D11SwapChainHookV229(swapChain);
}

static HRESULT STDMETHODCALLTYPE HFactoryCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  HRESULT hr = oFactoryCreateSwapChain ? oFactoryCreateSwapChain(self, device, desc, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory::CreateSwapChain", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForHwnd ? oFactory2CreateSwapChainForHwnd(self, device, hwnd, desc, fsDesc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForHwnd", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForCoreWindow ? oFactory2CreateSwapChainForCoreWindow(self, device, window, desc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForCoreWindow", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactory2CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactory2CreateSwapChainForComposition ? oFactory2CreateSwapChainForComposition(self, device, desc, restrictToOutput, swapChain) : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactory2::CreateSwapChainForComposition", *swapChain);
  return hr;
}

static HRESULT STDMETHODCALLTYPE HFactoryMediaCreateSwapChainForCompositionSurfaceHandle(IDXGIFactoryMedia* self, IUnknown* device, HANDLE surface, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
  HRESULT hr = oFactoryMediaCreateSwapChainForCompositionSurfaceHandle
    ? oFactoryMediaCreateSwapChainForCompositionSurfaceHandle(self, device, surface, desc, restrictToOutput, swapChain)
    : DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr) && swapChain && *swapChain) OnSwapChainCreatedV229("IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle", *swapChain);
  return hr;
}

static void InstallFactoryHooksV229(void* factory) {
  if (!factory) return;
  IUnknown* unknown = reinterpret_cast<IUnknown*>(factory);

  IDXGIFactory* factory0 = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory0))) && factory0) {
    HookVTableV229(factory0, 10, reinterpret_cast<void*>(&HFactoryCreateSwapChain), &oFactoryCreateSwapChain, "IDXGIFactory::CreateSwapChain");
    factory0->Release();
  }

  IDXGIFactory2* factory2 = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))) && factory2) {
    HookVTableV229(factory2, 15, reinterpret_cast<void*>(&HFactory2CreateSwapChainForHwnd), &oFactory2CreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
    HookVTableV229(factory2, 16, reinterpret_cast<void*>(&HFactory2CreateSwapChainForCoreWindow), &oFactory2CreateSwapChainForCoreWindow, "IDXGIFactory2::CreateSwapChainForCoreWindow");
    HookVTableV229(factory2, 24, reinterpret_cast<void*>(&HFactory2CreateSwapChainForComposition), &oFactory2CreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
    factory2->Release();
  }

  IDXGIFactoryMedia* factoryMedia = nullptr;
  if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGIFactoryMedia), reinterpret_cast<void**>(&factoryMedia))) && factoryMedia) {
    HookVTableV229(factoryMedia, 3, reinterpret_cast<void*>(&HFactoryMediaCreateSwapChainForCompositionSurfaceHandle), &oFactoryMediaCreateSwapChainForCompositionSurfaceHandle, "IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle");
    factoryMedia->Release();
  }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    DLog("DX11_V229: game loaded root dxgi.dll with complete, interface-safe swapchain capture.");
  }
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) dx11_bridge_client::Detach();
  return TRUE;
}

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory1 intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory1) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory1(riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
  DLog("DX11_V229: CreateDXGIFactory2 intercepted.");
  if (!LoadSystemDxgiV229() || !pCreateDXGIFactory2) return DXGI_ERROR_UNSUPPORTED;
  HRESULT hr = pCreateDXGIFactory2(flags, riid, ppFactory);
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) InstallFactoryHooksV229(*ppFactory);
  return hr;
}