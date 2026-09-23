#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "dx11_common.h"

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT,
  const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE g_d3d11System = nullptr;
static PFN_D3D11CreateDevice g_CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain g_CreateDeviceAndSwapChain = nullptr;

static bool ensureSystemD3D11() {
  if (g_d3d11System) return true;
  g_d3d11System = dx11_bridge_getgoing::loadSystemDll("d3d11.dll");
  if (!g_d3d11System) return false;
  g_CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(g_d3d11System, "D3D11CreateDevice"));
  g_CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(g_d3d11System, "D3D11CreateDeviceAndSwapChain"));
  if (!g_CreateDevice || !g_CreateDeviceAndSwapChain) {
    dx11_bridge_getgoing::logLine("d3d11", "System d3d11.dll is missing required exports.");
    return false;
  }
  dx11_bridge_getgoing::logLine("d3d11", "Loaded system d3d11.dll exports.");
  return true;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    dx11_bridge_getgoing::setModule(hinst);
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_getgoing::logLine("d3d11", "Loaded x86 DX11 bridge bring-up d3d11.dll.");
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDevice intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11()) return E_FAIL;

  HRESULT hr = g_CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels,
    FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);

  char msg[128] = {};
  wsprintfA(msg, "D3D11CreateDevice forwarded hr=0x%08lX", static_cast<unsigned long>(hr));
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
  IDXGIAdapter* pAdapter,
  D3D_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  const D3D_FEATURE_LEVEL* pFeatureLevels,
  UINT FeatureLevels,
  UINT SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
  IDXGISwapChain** ppSwapChain,
  ID3D11Device** ppDevice,
  D3D_FEATURE_LEVEL* pFeatureLevel,
  ID3D11DeviceContext** ppImmediateContext) {

  dx11_bridge_getgoing::logLine("d3d11", "D3D11CreateDeviceAndSwapChain intercepted.");
  dx11_bridge_getgoing::launchBridgeServerOnce();
  if (!ensureSystemD3D11()) return E_FAIL;

  HRESULT hr = g_CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
    pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
    ppDevice, pFeatureLevel, ppImmediateContext);

  char msg[128] = {};
  wsprintfA(msg, "D3D11CreateDeviceAndSwapChain forwarded hr=0x%08lX", static_cast<unsigned long>(hr));
  dx11_bridge_getgoing::logLine("d3d11", msg);
  return hr;
}
