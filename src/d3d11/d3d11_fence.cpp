#include "d3d11_fence.h"

#include "d3d11_device.h"

#include <algorithm>

namespace dxvk {

  D3D11Fence::D3D11Fence(D3D11Device* device, UINT64 initialValue)
  : D3D11DeviceChild<ID3D11Fence>(device)
  , m_value(initialValue) {

  }


  HRESULT STDMETHODCALLTYPE D3D11Fence::QueryInterface(REFIID riid, void** ppvObject) {
    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11Fence)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11Fence::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11Fence::CreateSharedHandle(
    const SECURITY_ATTRIBUTES* pAttributes,
          DWORD                dwAccess,
          LPCWSTR              lpName,
          HANDLE*              pHandle) {
    InitReturnPtr(pHandle);

    static bool s_errorShown = false;
    if (!s_errorShown) {
      s_errorShown = true;
      Logger::warn("D3D11Fence::CreateSharedHandle: Shared fence handles are not implemented");
    }

    return E_NOTIMPL;
  }


  UINT64 STDMETHODCALLTYPE D3D11Fence::GetCompletedValue() {
    std::lock_guard lock(m_mutex);
    return m_value;
  }


  HRESULT STDMETHODCALLTYPE D3D11Fence::SetEventOnCompletion(UINT64 Value, HANDLE hEvent) {
    if (hEvent == nullptr)
      return E_INVALIDARG;

    bool completed = false;
    {
      std::lock_guard lock(m_mutex);
      completed = m_value >= Value;

      if (!completed)
        m_events.push_back({ Value, hEvent });
    }

    if (completed)
      SetEvent(hEvent);

    return S_OK;
  }


  void D3D11Fence::Signal(UINT64 value) {
    std::vector<HANDLE> completedEvents;

    {
      std::lock_guard lock(m_mutex);
      if (value <= m_value)
        return;

      m_value = value;

      auto nextEvent = std::remove_if(m_events.begin(), m_events.end(),
        [&] (const auto& event) {
          if (event.first > m_value)
            return false;

          completedEvents.push_back(event.second);
          return true;
        });

      m_events.erase(nextEvent, m_events.end());
    }

    for (HANDLE event : completedEvents)
      SetEvent(event);

    m_cond.notify_all();
  }


  void D3D11Fence::Wait(UINT64 value) {
    std::unique_lock lock(m_mutex);
    m_cond.wait(lock, [&] { return m_value >= value; });
  }

}
