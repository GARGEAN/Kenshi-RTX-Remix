#include <utility>

#include "d3d11_device_lock.h"

namespace dxvk {

  D3D11Multithread::D3D11Multithread(
          IUnknown*             pParent,
          BOOL                  Protected)
  : m_parent    (pParent),
    m_protected (Protected) {
    
  }


  D3D11Multithread::~D3D11Multithread() {

  }


  ULONG STDMETHODCALLTYPE D3D11Multithread::AddRef() {
    return m_parent->AddRef();
  }

  
  ULONG STDMETHODCALLTYPE D3D11Multithread::Release() {
    return m_parent->Release();
  }

  
  HRESULT STDMETHODCALLTYPE D3D11Multithread::QueryInterface(
          REFIID                riid,
          void**                ppvObject) {
    return m_parent->QueryInterface(riid, ppvObject);
  }

  
  void STDMETHODCALLTYPE D3D11Multithread::Enter() {
    if (m_protected)
      m_mutex.lock();
  }


  void STDMETHODCALLTYPE D3D11Multithread::Leave() {
    if (m_protected)
      m_mutex.unlock();
  }


  BOOL STDMETHODCALLTYPE D3D11Multithread::SetMultithreadProtected(
          BOOL                  bMTProtect) {
    return std::exchange(m_protected, bMTProtect);
  }


  BOOL STDMETHODCALLTYPE D3D11Multithread::GetMultithreadProtected() {
    return m_protected;
  }

}
