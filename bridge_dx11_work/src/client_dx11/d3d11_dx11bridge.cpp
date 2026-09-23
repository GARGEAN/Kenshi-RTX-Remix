#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include "dx11_bridge_client.h"
#include "dx11_capture_remix.h"

#define DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER 1

using PFN_D3D11CreateDevice = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (WINAPI *)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static HMODULE gSystemD3D11 = nullptr;
static PFN_D3D11CreateDevice pD3D11CreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain pD3D11CreateDeviceAndSwapChain = nullptr;

static volatile LONG gV219HooksInstalled = 0;
static volatile LONG gV219PresentCount = 0;
static volatile LONG gV219DrawCount = 0;
static volatile LONG gV219ResourceCount = 0;
static thread_local bool gV219InsideHook = false;

static void V219Log(const char* tag, const char* text) {
  dx11_bridge_client::LogLine(tag, text);
}

static HMODULE LoadSystemD3D11V219() {
  if (gSystemD3D11) return gSystemD3D11;
  gSystemD3D11 = dx11_bridge_client::LoadSystemDll("d3d11.dll");
  if (!gSystemD3D11) return nullptr;
  pD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(gSystemD3D11, "D3D11CreateDevice"));
  pD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(GetProcAddress(gSystemD3D11, "D3D11CreateDeviceAndSwapChain"));
  if (!pD3D11CreateDevice || !pD3D11CreateDeviceAndSwapChain) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: system d3d11.dll missing required exports.");
    return nullptr;
  }
  return gSystemD3D11;
}

template <typename T>
static bool V219HookVTable(void* object, size_t slot, void* detour, T* original, const char* name) {
  if (!object || !detour || !original) return false;
  void*** obj = reinterpret_cast<void***>(object);
  void** vt = *obj;
  if (!vt) return false;

  if (vt[slot] == detour) {
    return true;
  }

  DWORD oldProtect = 0;
  if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: VirtualProtect failed for %s slot=%u err=%lu.", name, (unsigned) slot, GetLastError());
    V219Log("hook", msg);
    return false;
  }

  if (*original == nullptr) {
    *original = reinterpret_cast<T>(vt[slot]);
  }

  vt[slot] = detour;

  DWORD ignored = 0;
  VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), &vt[slot], sizeof(void*));

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: hooked %s vtable slot %u.", name, (unsigned) slot);
  V219Log("hook", msg);
  return true;
}

// IDXGISwapChain slots.
// DX11_V229_PRESENT1_AND_DEFERRED_SWAPCHAINS
using PFN_SwapPresent = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using PFN_SwapPresent1 = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_SwapResizeBuffers = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_SwapPresent oSwapPresent = nullptr;
static PFN_SwapPresent1 oSwapPresent1 = nullptr;
static PFN_SwapResizeBuffers oSwapResizeBuffers = nullptr;

// ID3D11DeviceContext slots.
using PFN_DrawIndexed = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
static PFN_DrawIndexed oDrawIndexed = nullptr;
static PFN_Draw oDraw = nullptr;
static PFN_DrawIndexedInstanced oDrawIndexedInstanced = nullptr;
static PFN_DrawInstanced oDrawInstanced = nullptr;

// ID3D11Device slots.
using PFN_CreateBuffer = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using PFN_CreateTexture2D = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateSRV = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PFN_CreateVertexShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using PFN_CreatePixelShader = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CreateBuffer oCreateBuffer = nullptr;
static PFN_CreateTexture2D oCreateTexture2D = nullptr;
static PFN_CreateSRV oCreateSRV = nullptr;
static PFN_CreateVertexShader oCreateVertexShader = nullptr;
static PFN_CreatePixelShader oCreatePixelShader = nullptr;

// ID3D11Device::CreateInputLayout + ID3D11DeviceContext upload paths (DX11_V226_CAPTURE_TO_REMIX).
using PFN_CreateInputLayout = HRESULT (STDMETHODCALLTYPE *)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
using PFN_Map = HRESULT (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void (STDMETHODCALLTYPE *)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static PFN_CreateInputLayout oCreateInputLayout = nullptr;
static PFN_Map oMap = nullptr;
static PFN_Unmap oUnmap = nullptr;
static PFN_UpdateSubresource oUpdateSubresource = nullptr;

static void V219NotifyDraw(const char* what) {
  LONG n = InterlockedIncrement(&gV219DrawCount);
  if (n <= 16 || (n % 500) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static void V219NotifyResource(const char* what) {
  LONG n = InterlockedIncrement(&gV219ResourceCount);
  if (n <= 16 || (n % 250) == 0) {
    char msg[256] = {};
    sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured %s count=%ld in game process.", what, n);
    V219Log("capture", msg);
  }
}

static HRESULT STDMETHODCALLTYPE HSwapPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
  if (!gV219InsideHook) {
    gV219InsideHook = true;
    LONG n = InterlockedIncrement(&gV219PresentCount);
    if (n <= 8 || (n % 60) == 0) {
      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::Present count=%ld in game process.", n);
      V219Log("capture", msg);
    }
    dx11_bridge_client::EnsureServer();
    // DX11_V265_BRIDGE_PRESENT_CAMERA: frame boundary - send Startup (once,
    // game HWND), SetupCamera and Present to the server so the Remix runtime
    // attaches to and presents INTO the game window every frame.
    dx11_capture::OnPresent(self);
    gV219InsideHook = false;
  }
  return oSwapPresent ? oSwapPresent(self, syncInterval, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static HRESULT STDMETHODCALLTYPE HSwapPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
  if (!gV219InsideHook) {
    gV219InsideHook = true;
    LONG n = InterlockedIncrement(&gV219PresentCount);
    if (n <= 8 || (n % 60) == 0) {
      char msg[256] = {};
      sprintf_s(msg, sizeof(msg), "DX11_V229: captured IDXGISwapChain1::Present1 count=%ld in game process.", n);
      V219Log("capture", msg);
    }
    dx11_bridge_client::EnsureServer();
    dx11_capture::OnPresent(self);
    gV219InsideHook = false;
  }
  return oSwapPresent1 ? oSwapPresent1(self, syncInterval, flags, parameters) : DXGI_ERROR_DEVICE_REMOVED;
}
static HRESULT STDMETHODCALLTYPE HSwapResizeBuffers(IDXGISwapChain* self, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT flags) {
  V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: captured IDXGISwapChain::ResizeBuffers in game process.");
  return oSwapResizeBuffers ? oSwapResizeBuffers(self, bufferCount, width, height, newFormat, flags) : DXGI_ERROR_DEVICE_REMOVED;
}

static void STDMETHODCALLTYPE HDrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexed");
  if (oDrawIndexed) oDrawIndexed(self, indexCount, startIndexLocation, baseVertexLocation);
  dx11_capture::CaptureDrawIndexed(self, indexCount, startIndexLocation, baseVertexLocation);
}

static void STDMETHODCALLTYPE HDraw(ID3D11DeviceContext* self, UINT vertexCount, UINT startVertexLocation) {
  V219NotifyDraw("ID3D11DeviceContext::Draw");
  if (oDraw) oDraw(self, vertexCount, startVertexLocation);
  dx11_capture::CaptureDraw(self, vertexCount, startVertexLocation);
}

static void STDMETHODCALLTYPE HDrawIndexedInstanced(ID3D11DeviceContext* self, UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawIndexedInstanced");
  if (oDrawIndexedInstanced) oDrawIndexedInstanced(self, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HDrawInstanced(ID3D11DeviceContext* self, UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation) {
  V219NotifyDraw("ID3D11DeviceContext::DrawInstanced");
  if (oDrawInstanced) oDrawInstanced(self, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

static HRESULT STDMETHODCALLTYPE HCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
  V219NotifyResource("ID3D11Device::CreateBuffer");
  HRESULT hr_cap = oCreateBuffer ? oCreateBuffer(self, desc, data, out) : E_FAIL;
  if (SUCCEEDED(hr_cap) && desc && out && *out) { dx11_capture::RecordBufferCreate(*out, data ? data->pSysMem : nullptr, desc->ByteWidth, desc->BindFlags); }
  return hr_cap;
}

static HRESULT STDMETHODCALLTYPE HCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
  V219NotifyResource("ID3D11Device::CreateTexture2D");
  HRESULT hr_cap = oCreateTexture2D ? oCreateTexture2D(self, desc, data, out) : E_FAIL;
  // DX11_V258_BRIDGE_TEXTURE_CONTENT_HASH: record a content-derived identity so
  // material hashes are identical across runs (pointer hashes are ASLR-random).
  if (SUCCEEDED(hr_cap) && desc && out && *out) {
    dx11_capture::RecordTexture2DCreate(*out, data ? data->pSysMem : nullptr,
      data ? data->SysMemPitch : 0u, desc->Width, desc->Height,
      (uint32_t) desc->Format, desc->MipLevels, desc->ArraySize, desc->BindFlags);
  }
  return hr_cap;
}

static HRESULT STDMETHODCALLTYPE HCreateSRV(ID3D11Device* self, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
  V219NotifyResource("ID3D11Device::CreateShaderResourceView");
  return oCreateSRV ? oCreateSRV(self, resource, desc, out) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HCreateVertexShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
  V219NotifyResource("ID3D11Device::CreateVertexShader");
  HRESULT hr = oCreateVertexShader ? oCreateVertexShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: reflect the shader to locate its world transform.
  if (SUCCEEDED(hr) && out && *out && bytecode) {
    dx11_capture::RecordVertexShaderCreate(*out, bytecode, (size_t) bytecodeLength);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HCreateInputLayout(ID3D11Device* self, const D3D11_INPUT_ELEMENT_DESC* descs, UINT num, const void* sig, SIZE_T sigLen, ID3D11InputLayout** out) {
  HRESULT hr = oCreateInputLayout ? oCreateInputLayout(self, descs, num, sig, sigLen, out) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: cache the element layout for vertex decoding.
  if (SUCCEEDED(hr) && out && *out && descs) {
    dx11_capture::RecordInputLayoutCreate(*out, descs, num);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE HMap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) {
  HRESULT hr = oMap ? oMap(self, res, sub, mapType, mapFlags, mapped) : E_FAIL;
  // DX11_V226_CAPTURE_TO_REMIX: remember the mapped pointer; snapshot it on Unmap.
  if (SUCCEEDED(hr) && mapped && mapped->pData) {
    dx11_capture::RecordMap(res, sub, mapped->pData);
  }
  return hr;
}

static void STDMETHODCALLTYPE HUnmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub) {
  // DX11_V226_CAPTURE_TO_REMIX: copy the written bytes BEFORE the driver unmaps.
  dx11_capture::RecordUnmap(res, sub);
  if (oUnmap) oUnmap(self, res, sub);
}

static void STDMETHODCALLTYPE HUpdateSubresource(ID3D11DeviceContext* self, ID3D11Resource* res, UINT dstSub, const D3D11_BOX* box, const void* src, UINT rowPitch, UINT depthPitch) {
  if (oUpdateSubresource) oUpdateSubresource(self, res, dstSub, box, src, rowPitch, depthPitch);
  // DX11_V226_CAPTURE_TO_REMIX: capture full-resource buffer uploads.
  if (!box) {
    dx11_capture::RecordUpdateSubresource(res, dstSub, src, rowPitch);
  }
}

static HRESULT STDMETHODCALLTYPE HCreatePixelShader(ID3D11Device* self, const void* bytecode, SIZE_T bytecodeLength, ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
  V219NotifyResource("ID3D11Device::CreatePixelShader");
  return oCreatePixelShader ? oCreatePixelShader(self, bytecode, bytecodeLength, linkage, out) : E_FAIL;
}

extern "C" __declspec(dllexport) void WINAPI DX11BridgeInstallSwapChainCapture(IDXGISwapChain* swapChain) {
  if (!swapChain) return;
  V219HookVTable(swapChain, 8, reinterpret_cast<void*>(&HSwapPresent), &oSwapPresent, "IDXGISwapChain::Present");
  V219HookVTable(swapChain, 13, reinterpret_cast<void*>(&HSwapResizeBuffers), &oSwapResizeBuffers, "IDXGISwapChain::ResizeBuffers");

  IDXGISwapChain1* swapChain1 = nullptr;
  if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1) {
    V219HookVTable(swapChain1, 22, reinterpret_cast<void*>(&HSwapPresent1), &oSwapPresent1, "IDXGISwapChain1::Present1");
    swapChain1->Release();
  }
}

static void V219InstallCapture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
  if (InterlockedCompareExchange(&gV219HooksInstalled, 1, 0) == 0) {
    V219Log("capture", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: installing real in-game DX11 capture hooks.");
  }

  if (device) {
    // ID3D11Device vtable slots.
    V219HookVTable(device, 3, reinterpret_cast<void*>(&HCreateBuffer), &oCreateBuffer, "ID3D11Device::CreateBuffer");
    V219HookVTable(device, 5, reinterpret_cast<void*>(&HCreateTexture2D), &oCreateTexture2D, "ID3D11Device::CreateTexture2D");
    V219HookVTable(device, 7, reinterpret_cast<void*>(&HCreateSRV), &oCreateSRV, "ID3D11Device::CreateShaderResourceView");
    V219HookVTable(device, 11, reinterpret_cast<void*>(&HCreateInputLayout), &oCreateInputLayout, "ID3D11Device::CreateInputLayout");
    V219HookVTable(device, 12, reinterpret_cast<void*>(&HCreateVertexShader), &oCreateVertexShader, "ID3D11Device::CreateVertexShader");
    V219HookVTable(device, 15, reinterpret_cast<void*>(&HCreatePixelShader), &oCreatePixelShader, "ID3D11Device::CreatePixelShader");
  }

  ID3D11DeviceContext* localContext = nullptr;
  if (!context && device) {
    device->GetImmediateContext(&localContext);
    context = localContext;
  }

  if (context) {
    // ID3D11DeviceContext vtable slots.
    V219HookVTable(context, 12, reinterpret_cast<void*>(&HDrawIndexed), &oDrawIndexed, "ID3D11DeviceContext::DrawIndexed");
    V219HookVTable(context, 13, reinterpret_cast<void*>(&HDraw), &oDraw, "ID3D11DeviceContext::Draw");
    V219HookVTable(context, 14, reinterpret_cast<void*>(&HMap), &oMap, "ID3D11DeviceContext::Map");
    V219HookVTable(context, 15, reinterpret_cast<void*>(&HUnmap), &oUnmap, "ID3D11DeviceContext::Unmap");
    V219HookVTable(context, 20, reinterpret_cast<void*>(&HDrawIndexedInstanced), &oDrawIndexedInstanced, "ID3D11DeviceContext::DrawIndexedInstanced");
    V219HookVTable(context, 21, reinterpret_cast<void*>(&HDrawInstanced), &oDrawInstanced, "ID3D11DeviceContext::DrawInstanced");
    V219HookVTable(context, 48, reinterpret_cast<void*>(&HUpdateSubresource), &oUpdateSubresource, "ID3D11DeviceContext::UpdateSubresource");
  }

  if (localContext) {
    localContext->Release();
  }

  DX11BridgeInstallSwapChainCapture(swapChain);
}

using PFN_DX11BridgeInstallPendingSwapChainsV229 = void (WINAPI *)();
static void TryDrainPendingSwapChainsV229() {
  HMODULE dxgi = GetModuleHandleA("dxgi.dll");
  auto drain = dxgi
    ? reinterpret_cast<PFN_DX11BridgeInstallPendingSwapChainsV229>(GetProcAddress(dxgi, "DX11BridgeInstallPendingSwapChains"))
    : nullptr;
  if (drain) drain();
}
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinst);
    dx11_bridge_client::SetModule(hinst);
    dx11_bridge_client::Attach();
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: game loaded root d3d11.dll client capture layer.");
  }

  if (reason == DLL_PROCESS_DETACH) {
    if (reserved != nullptr) {
      dx11_bridge_client::Detach();
    } else {
      V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: ignoring runtime detach; bridge remains owned until process exit.");
    }
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDevice.");
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDevice) {
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDevice(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, nullptr);
    TryDrainPendingSwapChainsV229();
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDevice returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain intercepted inside game process.");
  if (!dx11_bridge_client::EnsureServer()) {
    V219Log("d3d11", "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: bridge server startup failed before D3D11CreateDeviceAndSwapChain.");
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!LoadSystemD3D11V219() || !pD3D11CreateDeviceAndSwapChain) {
    if (swapChain) *swapChain = nullptr;
    if (device) *device = nullptr;
    if (immediateContext) *immediateContext = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT hr = pD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel, immediateContext);
  if (SUCCEEDED(hr)) {
    V219InstallCapture(device ? *device : nullptr, immediateContext ? *immediateContext : nullptr, swapChain ? *swapChain : nullptr);
    TryDrainPendingSwapChainsV229();
  }

  char msg[256] = {};
  sprintf_s(msg, sizeof(msg), "DX11_V219_REAL_D3D11_CLIENT_CAPTURE_LAYER: D3D11CreateDeviceAndSwapChain returned 0x%08X.", (unsigned) hr);
  V219Log("d3d11", msg);
  return hr;
}