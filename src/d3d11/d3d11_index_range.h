#pragma once

#include <emmintrin.h>
#include <algorithm>
#include <cstdint>

namespace dxvk::index_range {
  struct Result {
    bool valid;
    uint32_t count;
  };

#if defined(_MSC_VER)
#define DXVK_INDEX_INLINE __forceinline
#else
#define DXVK_INDEX_INLINE inline __attribute__((always_inline))
#endif

  static DXVK_INDEX_INLINE __m128i unsignedMax(__m128i a, __m128i b) {
    const __m128i sign = _mm_set1_epi32(INT32_MIN);
    const __m128i greater = _mm_cmpgt_epi32(_mm_xor_si128(a, sign), _mm_xor_si128(b, sign));
    return _mm_or_si128(_mm_and_si128(greater, a), _mm_andnot_si128(greater, b));
  }

  template<bool Restart>
  static DXVK_INDEX_INLINE void accumulate(__m128i values, __m128i sentinel, __m128i& maximum) {
    if (Restart)
      values = _mm_andnot_si128(_mm_cmpeq_epi32(values, sentinel), values);
    maximum = unsignedMax(maximum, values);
  }

  // V686: exact maximum and range validation using SSE2, with no persistent cache.
  // Caller proves that all count elements are readable before entering this scan.
  template<class T, bool Restart>
  inline Result scan(const T* indices, uint32_t count, uint32_t vertexLimit) {
    static_assert(sizeof(T) == 2 || sizeof(T) == 4);
    if (!count)
      return { false, 0 };

    __m128i maximum = _mm_setzero_si128();
    uint32_t i = 0;
    constexpr uint32_t width = 16 / sizeof(T);
    if constexpr (sizeof(T) == 2) {
      const __m128i sign = _mm_set1_epi16(-32768);
      __m128i packedMax = sign;
      for (; count - i >= width; i += width) {
        __m128i values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i));
        if (Restart)
          values = _mm_andnot_si128(_mm_cmpeq_epi16(values, _mm_set1_epi16(-1)), values);
        packedMax = _mm_max_epi16(packedMax, _mm_xor_si128(values, sign));
      }
      packedMax = _mm_xor_si128(packedMax, sign);
      maximum = unsignedMax(_mm_unpacklo_epi16(packedMax, _mm_setzero_si128()),
                            _mm_unpackhi_epi16(packedMax, _mm_setzero_si128()));
    } else {
      const __m128i sentinel = _mm_set1_epi32(-1);
      __m128i m1 = maximum, m2 = maximum, m3 = maximum;
      // Independent accumulators avoid serializing the whole scan on one maximum.
      for (; count - i >= 16; i += 16) {
        accumulate<Restart>(_mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i)), sentinel, maximum);
        accumulate<Restart>(_mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i + 4)), sentinel, m1);
        accumulate<Restart>(_mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i + 8)), sentinel, m2);
        accumulate<Restart>(_mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i + 12)), sentinel, m3);
      }
      maximum = unsignedMax(unsignedMax(maximum, m1), unsignedMax(m2, m3));
      for (; count - i >= width; i += width)
        accumulate<Restart>(_mm_loadu_si128(reinterpret_cast<const __m128i*>(indices + i)), sentinel, maximum);
    }

    alignas(16) uint32_t lanes[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(lanes), maximum);
    uint32_t result = std::max(std::max(lanes[0], lanes[1]), std::max(lanes[2], lanes[3]));
    for (; i < count; ++i) {
      const uint32_t value = indices[i];
      if (Restart && value == T(-1))
        continue;
      result = std::max(result, value);
    }

    // Checking the exact maximum also validates every non-restart index.
    // Validate BEFORE adding one, including when a 32-bit index is UINT32_MAX.
    if (result >= vertexLimit)
      return { false, 0 };
    if (Restart && result == 0) {
      // Zero after masking can mean real index zero or only restart markers.
      for (uint32_t j = 0; j < count; ++j)
        if (indices[j] == 0)
          return { true, 1 };
      return { false, 0 };
    }
    return { true, result + 1 };
  }

  template<class T>
  inline Result scan(const T* indices, uint32_t count, uint32_t vertexLimit, bool restart) {
    return restart ? scan<T, true>(indices, count, vertexLimit)
                   : scan<T, false>(indices, count, vertexLimit);
  }

#undef DXVK_INDEX_INLINE
}
