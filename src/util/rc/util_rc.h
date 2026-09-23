#pragma once

#include <atomic>

namespace dxvk {
  
  /**
   * \brief Reference-counted object
   */
  class RcObject {
    
  public:
    
    /**
     * \brief Increments reference count
     * \returns New reference count
     */
    uint32_t incRef() {
      return ++m_refCount;
    }
    
    /**
     * \brief Decrements reference count
     * \returns New reference count
     */
    uint32_t decRef() {
      return --m_refCount;
    }

    /**
     * \brief Current reference count
     *
     * Snapshot only. Reliable for "am I the sole owner" checks made by a
     * thread that itself holds one of the counted references (the count can
     * only be raised again through that same reference).
     * \returns Current reference count
     */
    uint32_t refCount() const {
      return m_refCount.load();
    }

  private:
    
    std::atomic<uint32_t> m_refCount = { 0u };
    
  };
  
}