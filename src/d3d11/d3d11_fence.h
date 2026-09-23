#pragma once

#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

#include "d3d11_device_child.h"

namespace dxvk {

  class D3D11Fence : public D3D11DeviceChild<ID3D11Fence> {
  public:
    D3D11Fence(D3D11Device* device, UINT64 initialValue);

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                  riid,
            void**                  ppvObject) final;

    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
      const SECURITY_ATTRIBUTES*    pAttributes,
            DWORD                   dwAccess,
            LPCWSTR                 lpName,
            HANDLE*                 pHandle) final;

    UINT64 STDMETHODCALLTYPE GetCompletedValue() final;

    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(
            UINT64                  Value,
            HANDLE                  hEvent) final;

    void Signal(UINT64 value);
    void Wait(UINT64 value);

  private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::vector<std::pair<UINT64, HANDLE>> m_events;
    UINT64 m_value;
  };

}
