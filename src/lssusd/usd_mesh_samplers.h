/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include "hd/usd_mesh_util.h"
#include "../util/util_error.h"

#include <bitset>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <type_traits>
#include <vector>

#include "usd_include_begin.h"
#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4i.h>
#include "usd_include_end.h"

namespace lss {
  namespace detail {
    template <typename Fn>
    bool visitUsdArray(pxr::VtValue const& value, Fn&& fn) {
      auto tryVisit = [&](auto* typedTag) -> bool {
        using T = std::remove_pointer_t<decltype(typedTag)>;
        if (!value.IsHolding<pxr::VtArray<T>>()) {
          return false;
        }

        const pxr::VtArray<T>& array = value.UncheckedGet<pxr::VtArray<T>>();
        fn(array.cdata(), array.size(), sizeof(T));
        return true;
      };

      return tryVisit(static_cast<uint8_t*>(nullptr))
          || tryVisit(static_cast<int*>(nullptr))
          || tryVisit(static_cast<float*>(nullptr))
          || tryVisit(static_cast<double*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec2f*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec3f*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec4f*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec2d*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec3d*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec4d*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec2i*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec3i*>(nullptr))
          || tryVisit(static_cast<pxr::GfVec4i*>(nullptr));
    }

    inline std::vector<uint8_t> copyUsdArrayBytes(pxr::VtValue const& value) {
      std::vector<uint8_t> bytes;
      if (!visitUsdArray(value, [&](const void* data, size_t count, size_t elementSize) {
        const size_t byteCount = count * elementSize;
        bytes.resize(byteCount);
        if (byteCount != 0) {
          std::memcpy(bytes.data(), data, byteCount);
        }
      })) {
        throw dxvk::DxvkError("Unsupported USD primvar array type");
      }

      return bytes;
    }
  }

  class BufferSampler {
  public:
    BufferSampler(pxr::VtValue const& buffer, size_t elementSize)
      : m_buffer(detail::copyUsdArrayBytes(buffer))
      , m_numElements(elementSize == 0 ? 0 : m_buffer.size() / elementSize) { }

    bool Sample(int index, void* value, size_t size) const {
      if (index < 0 || size == 0 || m_numElements <= static_cast<size_t>(index)) {
        std::memset(value, 0, size);
        return false;
      }

      const size_t offset = size * static_cast<size_t>(index);
      if (offset > m_buffer.size() || size > m_buffer.size() - offset) {
        std::memset(value, 0, size);
        return false;
      }

      std::memcpy(value, m_buffer.data() + offset, size);

      return true;
    }

  private:
    std::vector<uint8_t> const m_buffer;
    size_t m_numElements;
  };


  class GeomPrimvarSampler {
  public:
    GeomPrimvarSampler() = default;
    virtual ~GeomPrimvarSampler() = default;

    virtual bool SampleBuffer(int index, void* value) const = 0;
  };


  class ConstantSampler : public GeomPrimvarSampler {
  public:
    ConstantSampler(pxr::VtValue const& value, size_t elementSize)
      : m_sampler(value, elementSize)
      , m_elementSize(elementSize) { }

    bool SampleBuffer(int index, void* value) const override {
      return m_sampler.Sample(0, value, m_elementSize);
    }
  private:
    BufferSampler const m_sampler;
    size_t m_elementSize;
  };


  class UniformSampler : public GeomPrimvarSampler {
  public:
    UniformSampler(pxr::VtValue const& value, pxr::VtIntArray const& primitiveParams, size_t elementSize)
      : m_sampler(value, elementSize)
      , m_primitiveParams(primitiveParams)
      , m_elementSize(elementSize) { }

    UniformSampler(pxr::VtValue const& value, size_t elementSize)
      : m_sampler(value, elementSize)
      , m_elementSize(elementSize) { }

    bool SampleBuffer(int index, void* value) const {
      if (m_primitiveParams.empty()) {
        return m_sampler.Sample(index, value, m_elementSize);
      }
      if (index < 0) {
        return false;
      }

      const int triangleIndex = index / 3;
      if (static_cast<size_t>(triangleIndex) >= m_primitiveParams.size()) {
        return false;
      }
      return m_sampler.Sample(UsdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(m_primitiveParams[triangleIndex]), value, m_elementSize);
    }

  private:
    BufferSampler const m_sampler;
    pxr::VtIntArray const m_primitiveParams;
    size_t m_elementSize;
  };


  class TriangleVertexSampler : public GeomPrimvarSampler {
  public:
    TriangleVertexSampler(pxr::VtValue const& value, pxr::VtVec3iArray const& indices, size_t elementSize)
      : m_sampler(value, elementSize)
      , m_indices(indices)
      , m_elementSize(elementSize) { }

    bool SampleBuffer(int index, void* value) const {
      if (index < 0 || static_cast<size_t>(index / 3) >= m_indices.size()) {
        return false;
      }
      return m_sampler.Sample(m_indices[index / 3][index % 3], value, m_elementSize);
    }

  private:
    BufferSampler const m_sampler;
    pxr::VtVec3iArray const m_indices;
    size_t m_elementSize;
  };


  class TriangleFaceVaryingSampler : public GeomPrimvarSampler {
  public:
    TriangleFaceVaryingSampler(pxr::VtValue const& value, UsdMeshUtil& meshUtil, size_t elementSize)
      : m_sampler(triangulate(value, meshUtil, elementSize), elementSize)
      , m_elementSize(elementSize) { }

    bool SampleBuffer(int index, void* value) const {
      return m_sampler.Sample(index, value, m_elementSize);
    }

  private:
    BufferSampler const m_sampler;
    size_t m_elementSize;
    static pxr::VtValue triangulate(pxr::VtValue const& value, UsdMeshUtil& meshUtil, size_t elementSize) {
      if (elementSize == 0) {
        throw dxvk::DxvkError("Could not triangulate zero-sized face-varying data primvar");
      }

      pxr::VtValue triangulated;
      bool ok = false;
      detail::visitUsdArray(value, [&](const void* data, size_t count, size_t sourceElementSize) {
        const size_t byteCount = count * sourceElementSize;
        if (byteCount % elementSize != 0) {
          return;
        }

        ok = meshUtil.ComputeTriangulatedFaceVaryingPrimvar(data, static_cast<int>(byteCount / elementSize), elementSize, &triangulated);
      });

      if (!ok) {
        throw dxvk::DxvkError("Could not triangulate face-varying data primvar");
      }
      return triangulated;
    }

  };

  class GeneratedTriangleNormalSampler : public GeomPrimvarSampler {
  public:
    GeneratedTriangleNormalSampler(const GeomPrimvarSampler& positionSampler, uint32_t numTriangles) {
      m_normals.resize(size_t(numTriangles) * 3u, pxr::GfVec3f(0.0f, 0.0f, 1.0f));

      for (uint32_t triIdx = 0; triIdx < numTriangles; ++triIdx) {
        pxr::GfVec3f p0(0.0f), p1(0.0f), p2(0.0f);
        const int baseIndex = static_cast<int>(triIdx * 3u);
        if (!positionSampler.SampleBuffer(baseIndex + 0, &p0) ||
            !positionSampler.SampleBuffer(baseIndex + 1, &p1) ||
            !positionSampler.SampleBuffer(baseIndex + 2, &p2)) {
          continue;
        }

        const pxr::GfVec3f e1 = p1 - p0;
        const pxr::GfVec3f e2 = p2 - p0;
        pxr::GfVec3f n = pxr::GfCross(e1, e2);
        const float lenSq = n.GetLengthSq();
        if (!std::isfinite(lenSq) || lenSq <= 1.0e-20f) {
          continue;
        }

        n /= std::sqrt(lenSq);
        if (!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2])) {
          continue;
        }

        m_normals[baseIndex + 0] = n;
        m_normals[baseIndex + 1] = n;
        m_normals[baseIndex + 2] = n;
      }
    }

    bool SampleBuffer(int index, void* value) const override {
      if (index < 0 || static_cast<size_t>(index) >= m_normals.size()) {
        std::memset(value, 0, sizeof(pxr::GfVec3f));
        return false;
      }

      std::memcpy(value, &m_normals[static_cast<size_t>(index)], sizeof(pxr::GfVec3f));
      return true;
    }

  private:
    std::vector<pxr::GfVec3f> m_normals;
  };
}
