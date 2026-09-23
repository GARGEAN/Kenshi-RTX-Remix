#pragma once

#include <windows.h>

#include "module_processing.h"

#include "remix_api.h"

#include "util_bridge_assert.h"
#include "util_modulecommand.h"

#include "log/log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <vector>

using namespace Commands;

// DX11_V225: the module/"factory" level commands are served from a DXGI factory.
extern IDXGIFactory1* gpD3D;

#define PULL(type, name) const auto& name = (type)ModuleBridge::get_data()
#define PULL_I(name) PULL(INT, name)
#define PULL_U(name) PULL(UINT, name)
#define PULL_D(name) PULL(DWORD, name)
#define PULL_H(name) PULL(HRESULT, name)
#define PULL_HND(name) \
            PULL_U(name); \
            assert(name != NULL)
#define PULL_DATA(size, name) \
            uint32_t name##_len = ModuleBridge::get_data((void**)&name); \
            assert(name##_len == 0 || size == name##_len)
#define PULL_OBJ(type, name) \
            type* name = nullptr; \
            PULL_DATA(sizeof(type), name)
#define CHECK_DATA_OFFSET (ModuleBridge::get_data_pos() == rpcHeader.dataOffset)
#define GET_HND(name) \
            const auto& name = rpcHeader.pHandle; \
            assert(name != NULL)
#define GET_HDR_VAL(name) \
            const DWORD& name = rpcHeader.pHandle;
#define GET_RES(name, map) \
            GET_HND(name##Handle); \
            const auto& name = map[name##Handle]; \
            assert(name != NULL)

// NOTE: MSDN states HWNDs are safe to cross x86-->x64 boundary, and that a truncating cast should be used: https://docs.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication?redirectedfrom=MSDN
#define TRUNCATE_HANDLE(type, input) (type)(size_t)(input)

namespace {
  // DX11_V225: enumerate a DXGI adapter by index (caller releases). Returns null
  // if the index is out of range or the factory is unavailable.
  IDXGIAdapter1* getDxgiAdapter(UINT index) {
    if (!gpD3D) {
      return nullptr;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (gpD3D->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      return nullptr;
    }
    return adapter;
  }

  // DX11_V225: first output (monitor) of the given adapter (caller releases).
  IDXGIOutput* getDxgiOutput(UINT adapterIndex, UINT outputIndex = 0) {
    IDXGIAdapter1* adapter = getDxgiAdapter(adapterIndex);
    if (!adapter) {
      return nullptr;
    }
    IDXGIOutput* output = nullptr;
    HRESULT hr = adapter->EnumOutputs(outputIndex, &output);
    adapter->Release();
    return SUCCEEDED(hr) ? output : nullptr;
  }
}

void processModuleCommandQueue(std::atomic<bool>* const pbSignalEnd) {
  bool destroyReceived = false;
  while (RESULT_SUCCESS(ModuleBridge::waitForCommand(
    Commands::Bridge_Any, 0, pbSignalEnd))) {
    const Header rpcHeader = ModuleBridge::pop_front();
    PULL_U(currentUID);
#if defined(_DEBUG) || defined(DEBUGOPT)
    if (GlobalOptions::getLogServerCommands()) {
      Logger::info("Module Processing: " + toString(rpcHeader.command) + " UID: " + std::to_string(currentUID));
    }
#endif
    // The mother of all switch statements - every call in the DX11 module/factory
    // interface is mapped here, served from a DXGI factory (gpD3D).
    switch (rpcHeader.command) {
      /*
       * Module / factory interface
       */
      case IDirect3D11Ex_QueryInterface:
        break;
      case IDirect3D11Ex_AddRef:
      {
        // The server controls its own factory lifetime completely - no op
        break;
      }
      case IDirect3D11Ex_Destroy:
      {
        bridge_util::Logger::info("D3D11 Module destroyed.");
        destroyReceived = true;
        break;
      }
      case IDirect3D11Ex_RegisterSoftwareDevice:
        break;
      case IDirect3D11Ex_GetAdapterCount:
      {
        UINT cnt = 0;
        for (;;) {
          IDXGIAdapter1* adapter = getDxgiAdapter(cnt);
          if (!adapter) {
            break;
          }
          adapter->Release();
          ++cnt;
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterIdentifier:
      {
        PULL_U(Adapter);
        PULL_D(Flags);
        DXGI_ADAPTER_DESC1 desc = {};
        IDXGIAdapter1* adapter = getDxgiAdapter(Adapter);
        HRESULT hresult = E_FAIL;
        if (adapter) {
          hresult = adapter->GetDesc1(&desc);
          adapter->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(DXGI_ADAPTER_DESC1), &desc);
          }
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterModeCount:
      {
        PULL_U(Adapter);
        PULL(DXGI_FORMAT, Format);
        UINT cnt = 0;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          output->GetDisplayModeList(Format, 0, &cnt, nullptr);
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D11Ex_EnumAdapterModes:
      {
        PULL_U(Adapter);
        PULL(DXGI_FORMAT, Format);
        PULL_U(Mode);
        DXGI_MODE_DESC pMode = {};
        HRESULT hresult = E_FAIL;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          UINT cnt = 0;
          if (SUCCEEDED(output->GetDisplayModeList(Format, 0, &cnt, nullptr)) && Mode < cnt) {
            std::vector<DXGI_MODE_DESC> modes(cnt);
            if (SUCCEEDED(output->GetDisplayModeList(Format, 0, &cnt, modes.data()))) {
              pMode = modes[Mode];
              hresult = S_OK;
            }
          }
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(DXGI_MODE_DESC), &pMode);
          }
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterDisplayMode:
      {
        PULL_U(Adapter);
        DXGI_MODE_DESC pMode = {};
        HRESULT hresult = E_FAIL;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          DXGI_OUTPUT_DESC outDesc = {};
          if (SUCCEEDED(output->GetDesc(&outDesc))) {
            pMode.Width = outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left;
            pMode.Height = outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top;
            pMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            pMode.RefreshRate.Numerator = 60;
            pMode.RefreshRate.Denominator = 1;
            hresult = S_OK;
          }
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(DXGI_MODE_DESC), &pMode);
          }
        }
        break;
      }
      case IDirect3D11Ex_CheckDeviceType:
      {
        PULL_U(Adapter);
        PULL_U(DevType);
        PULL(DXGI_FORMAT, AdapterFormat);
        PULL(DXGI_FORMAT, BackBufferFormat);
        PULL(BOOL, bWindowed);
        // DX11 does not gate device types the way D3D11 did; accept.
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
        }
        break;
      }
      case IDirect3D11Ex_CheckDeviceFormat:
      {
        PULL_U(Adapter);
        PULL_U(DeviceType);
        PULL(DXGI_FORMAT, AdapterFormat);
        PULL_D(Usage);
        PULL_U(RType);
        PULL(DXGI_FORMAT, CheckFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
        }
        break;
      }
      case IDirect3D11Ex_CheckDeviceMultiSampleType:
      {
        PULL_U(Adapter);
        PULL_U(DeviceType);
        PULL(DXGI_FORMAT, SurfaceFormat);
        PULL(BOOL, Windowed);
        PULL_U(MultiSampleType);

        // Without a device we cannot query exact quality levels; report a single level.
        DWORD QualityLevels = 1;
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
          c.send_data(QualityLevels);
        }
        break;
      }
      case IDirect3D11Ex_CheckDepthStencilMatch:
      {
        PULL_U(Adapter);
        PULL_U(DeviceType);
        PULL(DXGI_FORMAT, AdapterFormat);
        PULL(DXGI_FORMAT, RenderTargetFormat);
        PULL(DXGI_FORMAT, DepthStencilFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
        }
        break;
      }
      case IDirect3D11Ex_CheckDeviceFormatConversion:
      {
        PULL_U(Adapter);
        PULL_U(DeviceType);
        PULL(DXGI_FORMAT, SourceFormat);
        PULL(DXGI_FORMAT, TargetFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
        }
        break;
      }
      case IDirect3D11Ex_GetDeviceCaps:
      {
        PULL_U(Adapter);
        PULL_U(DeviceType);
        // DX11 has no D3DCAPS9 equivalent; the client only checks the HRESULT.
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data((HRESULT) S_OK);
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterMonitor:
      {
        PULL_U(Adapter);
        HMONITOR hmonitor = nullptr;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          DXGI_OUTPUT_DESC outDesc = {};
          if (SUCCEEDED(output->GetDesc(&outDesc))) {
            hmonitor = outDesc.Monitor;
          }
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          // Truncate handle before sending back to client because it expects a 32-bit size handle
          c.send_data(TRUNCATE_HANDLE(uint32_t, hmonitor));
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterModeCountEx:
      {
        PULL_U(Adapter);
        // Consume the (legacy) mode filter blob the client sends.
        void* modeFilter = nullptr;
        uint32_t modeFilter_len = ModuleBridge::get_data((void**)&modeFilter);
        (void) modeFilter_len;
        UINT cnt = 0;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &cnt, nullptr);
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterLUID:
      {
        PULL_U(Adapter);
        LUID pLUID = {};
        HRESULT hresult = E_FAIL;
        IDXGIAdapter1* adapter = getDxgiAdapter(Adapter);
        if (adapter) {
          DXGI_ADAPTER_DESC1 desc = {};
          if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            pLUID = desc.AdapterLuid;
            hresult = S_OK;
          }
          adapter->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(LUID), &pLUID);
          }
        }
        break;
      }
      case IDirect3D11Ex_EnumAdapterModesEx:
      {
        PULL_U(Adapter);
        PULL_U(Mode);
        void* pFilter = nullptr;
        uint32_t pFilter_len = ModuleBridge::get_data((void**)&pFilter);
        (void) pFilter_len;
        DXGI_MODE_DESC pMode = {};
        HRESULT hresult = E_FAIL;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          UINT cnt = 0;
          if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &cnt, nullptr)) && Mode < cnt) {
            std::vector<DXGI_MODE_DESC> modes(cnt);
            if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &cnt, modes.data()))) {
              pMode = modes[Mode];
              hresult = S_OK;
            }
          }
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(DXGI_MODE_DESC), &pMode);
          }
        }
        break;
      }
      case IDirect3D11Ex_GetAdapterDisplayModeEx:
      {
        PULL_U(Adapter);
        void* pModeIn = nullptr;
        uint32_t pModeIn_len = ModuleBridge::get_data((void**)&pModeIn);
        (void) pModeIn_len;
        void* pRotationIn = nullptr;
        uint32_t pRotationIn_len = ModuleBridge::get_data((void**)&pRotationIn);
        (void) pRotationIn_len;
        DXGI_MODE_DESC pMode = {};
        DXGI_MODE_ROTATION pRotation = DXGI_MODE_ROTATION_IDENTITY;
        HRESULT hresult = E_FAIL;
        IDXGIOutput* output = getDxgiOutput(Adapter);
        if (output) {
          DXGI_OUTPUT_DESC outDesc = {};
          if (SUCCEEDED(output->GetDesc(&outDesc))) {
            pMode.Width = outDesc.DesktopCoordinates.right - outDesc.DesktopCoordinates.left;
            pMode.Height = outDesc.DesktopCoordinates.bottom - outDesc.DesktopCoordinates.top;
            pMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            pMode.RefreshRate.Numerator = 60;
            pMode.RefreshRate.Denominator = 1;
            pRotation = outDesc.Rotation;
            hresult = S_OK;
          }
          output->Release();
        }
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(DXGI_MODE_DESC), &pMode);
            c.send_data(sizeof(DXGI_MODE_ROTATION), &pRotation);
          }
        }
        break;
      }
    }
  }
  // Check if we exited the command processing loop unexpectedly while the bridge is still enabled
  if (!destroyReceived && gbBridgeRunning) {
    Logger::info("The module command processing loop was exited unexpectedly, either due to timing out or some other command queue issue.");
  }
}
