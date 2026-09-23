#pragma once

namespace dxvk {
  // A disabled/nested draw must hide its parent's batch. No state survives the
  // original API call, including on an exception or an early return.
  template<class T>
  class ScopedInstanceBatch {
  public:
    ScopedInstanceBatch(T*& slot, T* value) : m_slot(slot), m_previous(slot) { slot = value; }
    ~ScopedInstanceBatch() { m_slot = m_previous; }
    ScopedInstanceBatch(const ScopedInstanceBatch&) = delete;
    ScopedInstanceBatch& operator=(const ScopedInstanceBatch&) = delete;
  private:
    T*& m_slot;
    T* m_previous;
  };

  template<class Matrix>
  bool instanceBatchMirrored(const Matrix& m) {
    const float det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
      - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
      + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return det < 0.0f;
  }

  template<class Transforms, class Matrix>
  void placePreparedInstance(Transforms& transforms, const Matrix& placement) {
    transforms.objectToWorld = placement;
    transforms.objectToView = transforms.worldToView * placement;
  }
}
