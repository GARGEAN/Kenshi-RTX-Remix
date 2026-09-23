#pragma once

#include "d3d11_include.h"

#include "../util/sync/sync_spinlock.h"
#include "../util/sync/sync_recursive.h"

namespace dxvk {
  
  /**
   * \brief Device lock
   * 
   * Lightweight RAII wrapper that implements
   * a subset of the functionality provided by
   * \c std::unique_lock, with the goal of being
   * cheaper to construct and destroy.
   */  
  class D3D11DeviceLock {

  public:

    D3D11DeviceLock()
    : m_mutex(nullptr) { }

    D3D11DeviceLock(sync::RecursiveSpinlock& mutex)
    : m_mutex(&mutex) {
      mutex.lock();
    }

    D3D11DeviceLock(D3D11DeviceLock&& other)
    : m_mutex(other.m_mutex) {
      other.m_mutex = nullptr;
    }

    D3D11DeviceLock& operator = (D3D11DeviceLock&& other) {
      if (m_mutex)
        m_mutex->unlock();
      
      m_mutex = other.m_mutex;
      other.m_mutex = nullptr;
      return *this;
    }

    ~D3D11DeviceLock() {
      if (unlikely(m_mutex != nullptr))
        m_mutex->unlock();
    }

  private:

    sync::RecursiveSpinlock* m_mutex;
    
  };

  
  /**
   * \brief D3D11 context lock
   * 
   * Can be queried from any D3D11 context in order
   * to make individual calls thread-safe. Provides
   * methods to lock the context explicitly.
   */
  class D3D11Multithread : public ID3D10Multithread {

  public:

    D3D11Multithread(
            IUnknown*             pParent,
            BOOL                  Protected);
    
    ~D3D11Multithread();

    ULONG STDMETHODCALLTYPE AddRef() final;
    
    ULONG STDMETHODCALLTYPE Release() final;
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                riid,
            void**                ppvObject) final;
    
    void STDMETHODCALLTYPE Enter() final;

    void STDMETHODCALLTYPE Leave() final;

    BOOL STDMETHODCALLTYPE SetMultithreadProtected(
            BOOL                  bMTProtect) final;

    BOOL STDMETHODCALLTYPE GetMultithreadProtected() final;

    D3D11DeviceLock AcquireLock() {
      return unlikely(m_protected)
        ? D3D11DeviceLock(m_mutex)
        : D3D11DeviceLock();
    }
    
  private:

    IUnknown* m_parent;
    BOOL      m_protected;

    sync::RecursiveSpinlock m_mutex;

  };

}
