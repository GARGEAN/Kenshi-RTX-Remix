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

#include "../util/util_math.h"
#include "../util/util_vector.h"
#include "../util/util_error.h"
#include "../util/util_string.h"
#include "../util/util_fast_cache.h"
#include "../util/log/log.h"
#include "../tracy/Tracy.hpp"
#include "hd/usd_mesh_util.h"
#include "usd_mesh_samplers.h"
#include "usd_mesh_importer.h"
#include "game_exporter_common.h"

#include "usd_include_begin.h"
#include <pxr/pxr.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h> 
#include <pxr/usd/usdSkel/bindingAPI.h>
#include "usd_include_end.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ARGB color packing helper.
#ifndef DXVK_COLOR_ARGB
#define DXVK_COLOR_ARGB(a,r,g,b) \
  ((uint32_t)((((a)&0xffu)<<24)|(((r)&0xffu)<<16)|(((g)&0xffu)<<8)|((b)&0xffu)))
#endif

using namespace pxr;
using namespace dxvk;

namespace lss {
  inline static const TfToken kFaceVertexCounts("faceVertexCounts");
  inline static const TfToken kFaceVertexIndices("faceVertexIndices");
  inline static const TfToken kHoleIndices("holeIndices");
  inline static const TfToken kNormalsPrimvar("primvars:normals");
  inline static const TfToken kNormalsAttribute("normals");
  inline static const TfToken kColorAttribute("displayColor");
  inline static const TfToken kOpacityAttribute("displayOpacity");
  inline static const TfToken kPoints("points");
  inline static const TfToken kUvs[] = {
    TfToken("primvars:st"),
    TfToken("primvars:uv"),
    TfToken("primvars:st0"),
    TfToken("primvars:st1"),
    TfToken("primvars:st2"),
    TfToken("primvars:UVMap"),
  };
  inline static const TfToken kDoubleSided("doubleSided");
  inline static const TfToken kOrientation("orientation");
  inline static const TfToken kRightHanded("rightHanded");


  size_t sizeOfUsdType(const std::type_info& type) {
    if (type == typeid(GfVec4f))
      return sizeof(GfVec4f);
    if (type == typeid(GfVec3f))
      return sizeof(GfVec3f);
    if (type == typeid(GfVec2f))
      return sizeof(GfVec2f);
    if (type == typeid(int))
      return sizeof(int);
    if (type == typeid(float))
      return sizeof(float);
    return 0;
  }


  const std::vector<uint32_t> UsdMeshImporter::generateSubsetIndices(const UsdGeomSubset& subset, const std::vector<uint32_t>& indices, const UsdMeshImporter::FaceToTriangleMap& triangleMap) {
    ZoneScoped;
    std::vector<uint32_t> subsetIndices;

    VtIntArray faceIndices;
    subset.GetIndicesAttr().Get(&faceIndices);
    for (const int& faceIdx : faceIndices) {
      if (faceIdx < 0 || static_cast<size_t>(faceIdx) >= triangleMap.size()) {
        Logger::warn(str::format("Skipping USD geom subset face index outside mesh topology: subset=",
          subset.GetPath().GetString(),
          " face=",
          faceIdx));
        continue;
      }

      const IndexRange& range = triangleMap[static_cast<size_t>(faceIdx)];
      if (range.start >= range.end || range.end > indices.size()) {
        Logger::warn(str::format("Skipping USD geom subset face with no valid triangulated range: subset=",
          subset.GetPath().GetString(),
          " face=",
          faceIdx));
        continue;
      }

      for (uint32_t i = range.start; i < range.end; i++) {
        subsetIndices.emplace_back(indices[i]);
      }
    }

    return subsetIndices;
  }


  UsdMeshImporter::UsdMeshImporter(const UsdPrim& meshPrim, const uint32_t limitedNumBonesPerVertex)
    : m_meshPrim(UsdGeomMesh(meshPrim))
    , m_limitedNumBonesPerVertex(limitedNumBonesPerVertex) {
    ZoneScoped;
    if (!meshPrim.IsA<UsdGeomMesh>()) {
      throw DxvkError(str::format("Tried to process mesh, but it doesnt appear to be a valid USD mesh, id=.", m_meshPrim.GetPath().GetString()));
    }

    if (!m_meshPrim.GetPointsAttr().HasValue()) {
      throw DxvkError(str::format("Tried to process mesh with no vertex positions, id=.", m_meshPrim.GetPath().GetString()));
    }

    TfToken orientation;
    VtIntArray faceIndices, faceCounts, holeIndices;
    const bool orientationAuthored = m_meshPrim.GetOrientationAttr().Get(&orientation);
    if (!orientationAuthored) {
      orientation = kRightHanded;
    }
    m_meshPrim.GetFaceVertexIndicesAttr().Get(&faceIndices);
    m_meshPrim.GetFaceVertexCountsAttr().Get(&faceCounts);
    m_meshPrim.GetHoleIndicesAttr().Get(&holeIndices);

    UsdMeshUtil meshUtil(orientation, faceCounts, faceIndices, holeIndices);
    VtVec3iArray triangleIndices;
    VtIntArray trianglePrimitiveParams;
    meshUtil.ComputeTriangleIndices(&triangleIndices, &trianglePrimitiveParams);

    const uint32_t numTriangles = triangleIndices.size();

    if (numTriangles == 0) {
      throw DxvkError(str::format("Tried to process mesh with no triangles, id=.", m_meshPrim.GetPath().GetString()));
    }

    std::unique_ptr<GeomPrimvarSampler> pMeshSamplers[Attributes::Count] = { 0 };
    generateTriangleSamplers(meshUtil, triangleIndices, trianglePrimitiveParams, pMeshSamplers);

    if (pMeshSamplers[Attributes::VertexPositions] == nullptr) {
      throw DxvkError(str::format("Tried to process mesh with no vertex positions, id=.", m_meshPrim.GetPath().GetString()));
    }

    std::vector<UsdGeomSubset> geomSubsets;
    auto children = meshPrim.GetFilteredChildren(UsdPrimIsActive);
    for (auto child : children) {
      if (child.IsA<UsdGeomSubset>()) {
        geomSubsets.push_back(UsdGeomSubset(child));
      }
    }

    m_vertexStride = generateVertexDeclaration(pMeshSamplers);

    std::vector<uint32_t> indices;
    FaceToTriangleMap faceToTriangles(geomSubsets.size() > 0 ? faceCounts.size() : 0);
    triangulate(numTriangles, m_vertexStride / sizeof(float), pMeshSamplers, trianglePrimitiveParams, indices, faceToTriangles);

    m_numVertices = m_vertexData.size() * sizeof(float) / m_vertexStride;

    if (geomSubsets.empty()) {
      m_meshes.emplace_back(std::move(indices), meshPrim);
    } else {
      for (const UsdGeomSubset& subset : geomSubsets) {
        m_meshes.emplace_back(std::move(generateSubsetIndices(subset, indices, faceToTriangles)), subset.GetPrim());
      }
    }

    UsdAttribute doubleSidedAttribute;
    if ((doubleSidedAttribute = meshPrim.GetAttribute(kDoubleSided)).HasAuthoredValue()) {
      bool doubleSided = true;
      doubleSidedAttribute.Get(&doubleSided);
      m_doubleSided = doubleSided ? IsDoubleSided : IsSingleSided;
    }

    m_isRightHanded = orientation == kRightHanded;

    // Calculate bounding box using USD's built-in ComputeExtent function
    pxr::VtVec3fArray points;
    pxr::VtVec3fArray extent;
    
    if (m_meshPrim.GetPointsAttr().Get(&points)) {
      if (UsdGeomMesh::ComputeExtent(points, &extent) && extent.size() == 2) {
        // USD ComputeExtent returns [min, max] as VtVec3fArray
        const pxr::GfVec3f& minExtent = extent[0];
        const pxr::GfVec3f& maxExtent = extent[1];
        
        m_boundingBox = dxvk::AxisAlignedBoundingBox{
            dxvk::Vector3(minExtent[0], minExtent[1], minExtent[2]),
            dxvk::Vector3(maxExtent[0], maxExtent[1], maxExtent[2])};
      } else {
        Logger::warn(str::format("Could not compute bounding box for mesh: ", m_meshPrim.GetPath().GetString()));
      }
    }
  }


  uint32_t UsdMeshImporter::generateVertexDeclaration(std::unique_ptr<GeomPrimvarSampler>* ppMeshSamplers) {
    size_t offset = 0;
    const size_t size = sizeof(float) * 3;
    m_vertexDecl.emplace_back(VertexDeclaration { Attributes::VertexPositions, offset, size });
    offset += size;
    if (ppMeshSamplers[Attributes::Normals]) {
      const size_t size = sizeof(std::uint32_t);
      m_vertexDecl.emplace_back(VertexDeclaration { Attributes::Normals, offset, size });
      offset += size;
    }
    if (ppMeshSamplers[Attributes::Texcoords]) {
      const size_t size = sizeof(float) * 2;
      m_vertexDecl.emplace_back(VertexDeclaration { Attributes::Texcoords, offset, size });
      offset += size;
    }
    if (ppMeshSamplers[Attributes::Colors] || ppMeshSamplers[Attributes::Opacity]) {
      const size_t size = sizeof(uint32_t);
      m_vertexDecl.emplace_back(VertexDeclaration { Attributes::Colors, offset, size });
      offset += size;
    }
    if (ppMeshSamplers[Attributes::BlendWeights] && m_limitedNumBonesPerVertex > 0) {
      const size_t size = sizeof(float) * (m_limitedNumBonesPerVertex - 1);
      m_vertexDecl.emplace_back(VertexDeclaration { Attributes::BlendWeights, offset, size });
      offset += size;
    }
    if (ppMeshSamplers[Attributes::BlendIndices] && m_limitedNumBonesPerVertex > 0) {
      const size_t size = dxvk::align(m_limitedNumBonesPerVertex, 4);
      m_vertexDecl.emplace_back(VertexDeclaration { Attributes::BlendIndices, offset, size });
      offset += size;
    }

    return offset;
  }


  void UsdMeshImporter::generateTriangleSamplers(UsdMeshUtil& meshUtil, const VtVec3iArray& usdIndices, const VtIntArray& trianglePrimitiveParams, std::unique_ptr<GeomPrimvarSampler>* ppMeshSamplers) {
    ZoneScoped;
    struct PrimvarDescriptor {
      UsdGeomPrimvar primvar;
      Attributes vertexAttribute;
      size_t expectedSize = 0;
    };
    std::vector<PrimvarDescriptor> primvars;

    auto addPrimvarFromAttribute = [&](const UsdAttribute& attribute, Attributes vertexAttribute, size_t expectedSize) -> bool {
      if (!attribute.HasValue())
        return false;
      const UsdGeomPrimvar primvar(attribute);
      primvars.emplace_back(PrimvarDescriptor { primvar, vertexAttribute, expectedSize });
      return true;
    };

    auto addPrimvarFromAttributeName = [&](const TfToken& name, Attributes vertexAttribute, size_t expectedSize) -> bool {
      if (!m_meshPrim.GetPrim().HasAttribute(name))
        return false;
      const UsdAttribute& attribute = m_meshPrim.GetPrim().GetAttribute(name);
      return addPrimvarFromAttribute(attribute, vertexAttribute, expectedSize);
    };

    if (!addPrimvarFromAttribute(m_meshPrim.GetPointsAttr(), Attributes::VertexPositions, sizeof(float) * 3))
      if (!addPrimvarFromAttributeName(kPoints, Attributes::VertexPositions, sizeof(float) * 3))
        throw DxvkError(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", has no points attribute."));

    // Note: Currently normals are still encoded in the USD as 3xfloat32, unlike the 32 bit octahedral encoding used after processing.
    // USD gives primvars:normals precedence over the plain normals attribute when both are authored.
    if (!addPrimvarFromAttributeName(kNormalsPrimvar, Attributes::Normals, sizeof(float) * 3))
      if (!addPrimvarFromAttribute(m_meshPrim.GetNormalsAttr(), Attributes::Normals, sizeof(float) * 3))
        addPrimvarFromAttributeName(kNormalsAttribute, Attributes::Normals, sizeof(float) * 3);

    if (!addPrimvarFromAttribute(m_meshPrim.GetDisplayColorAttr(), Attributes::Colors, sizeof(float) * 3))
      addPrimvarFromAttributeName(kColorAttribute, Attributes::Colors, sizeof(float) * 3);

    if (!addPrimvarFromAttribute(m_meshPrim.GetDisplayOpacityAttr(), Attributes::Opacity, sizeof(float)))
      addPrimvarFromAttributeName(kOpacityAttribute, Attributes::Opacity, sizeof(float));

    for (const TfToken& uvName : kUvs) {
      if (addPrimvarFromAttributeName(uvName, Attributes::Texcoords, sizeof(float) * 2)) {
        break;
      }
    }

    if (m_meshPrim.GetPrim().HasAPI<UsdSkelBindingAPI>()) {
      UsdSkelBindingAPI skelBinding(m_meshPrim.GetPrim());
      UsdGeomPrimvar jointIndicesPV = skelBinding.GetJointIndicesPrimvar();
      UsdGeomPrimvar jointWeightsPV = skelBinding.GetJointWeightsPrimvar();

      const bool hasJointIndices = jointIndicesPV.HasValue();
      const bool hasJointWeights = jointWeightsPV.HasValue();
      if (!hasJointIndices || !hasJointWeights) {
        Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", has Skeleton API but incomplete joint primvars. Importing as static geometry."));
      } else {
        const int jointIndexElementSize = jointIndicesPV.GetElementSize();
        const int jointWeightElementSize = jointWeightsPV.GetElementSize();

        if (jointIndexElementSize <= 0 || jointWeightElementSize <= 0) {
          Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", has Skeleton API with zero-sized joint primvars. Importing as static geometry."));
        } else if (jointIndexElementSize != jointWeightElementSize) {
          Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", joint indices and joint weights have mismatched element sizes. Importing as static geometry."));
        } else if (static_cast<uint32_t>(jointIndexElementSize) > MaxSupportedNumBones) {
          Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", uses more bone influences per vertex (", jointIndexElementSize, ") than is currently supported. Importing as static geometry."));
        } else if (m_limitedNumBonesPerVertex == 0) {
          Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", has Skeleton API but rtx.limitedBonesPerVertex is 0. Importing as static geometry."));
        } else {
          m_actualNumBonesPerVertex = static_cast<uint32_t>(jointIndexElementSize);

          if (m_actualNumBonesPerVertex > m_limitedNumBonesPerVertex) {
            Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", uses more bone influences per vertex (", m_actualNumBonesPerVertex, ") than the config defined limit (rtx.limitedBonesPerVertex = ", m_limitedNumBonesPerVertex, "). Reducing the number of bone influences automatically. This may result in some skinned meshes not animating correctly. We suggest optimizing this mesh to only use the minimum number of bone influences."));
          }

          m_limitedNumBonesPerVertex = std::min(m_limitedNumBonesPerVertex, m_actualNumBonesPerVertex);

          primvars.emplace_back(PrimvarDescriptor { jointIndicesPV, Attributes::BlendIndices, sizeof(int) * m_actualNumBonesPerVertex });
          primvars.emplace_back(PrimvarDescriptor { jointWeightsPV, Attributes::BlendWeights, sizeof(float) * m_actualNumBonesPerVertex });
        }
      }
    }

    uint32_t numPoints = 0;
    for (PrimvarDescriptor const& desc : primvars) {
      const UsdGeomPrimvar& pv = desc.primvar;

      VtValue data;
      pv.ComputeFlattened(&data);

      const size_t elementSize = sizeOfUsdType(data.GetElementTypeid()) * pv.GetElementSize();
      if (elementSize == 0) {
        Logger::warn(str::format("Skipping unknown USD type, ", desc.vertexAttribute, ", for primvar, id=", pv.GetName()));
        continue;
      }

      if (desc.expectedSize != 0 && elementSize != desc.expectedSize) {
        Logger::warn(str::format("Skipping unexpected USD type for attribute, ", desc.vertexAttribute, ", primvar, id=", pv.GetName()));
        continue;
      }

      GeomPrimvarSampler* sampler = nullptr;
      if (desc.vertexAttribute == VertexPositions) {
        numPoints = data.GetArraySize();
        sampler = new TriangleVertexSampler(data, usdIndices, elementSize);
      } else {
        TfToken interpolation = pv.GetInterpolation();
        if (interpolation == pxr::TfToken("constant")) {
          sampler = new ConstantSampler(data, elementSize);
        } else if (interpolation == pxr::TfToken("uniform")) {
          sampler = new UniformSampler(data, trianglePrimitiveParams, elementSize);
        } else if (interpolation == pxr::TfToken("vertex") || interpolation == pxr::TfToken("varying")) {
          const uint32_t expectedArraySize = numPoints * pv.GetElementSize(); 
          if (data.GetArraySize() == expectedArraySize) {
            sampler = new TriangleVertexSampler(data, usdIndices, elementSize);
          } else {
            Logger::warn(str::format("Unexpected number of elements found for vertex attribute, ", desc.vertexAttribute, ", for primvar, id=", pv.GetName()));
          }
        } else if (interpolation == pxr::TfToken("faceVarying")) {
          sampler = new TriangleFaceVaryingSampler(data, meshUtil, elementSize);
        } else {
          throw DxvkError(str::format("Unexpected interpolation mode for primvar, id=", pv.GetName()));
        }
      }

      ppMeshSamplers[desc.vertexAttribute].reset(sampler);
    }

    if (ppMeshSamplers[Attributes::Normals] == nullptr && ppMeshSamplers[Attributes::VertexPositions] != nullptr) {
      ppMeshSamplers[Attributes::Normals] = std::make_unique<GeneratedTriangleNormalSampler>(
        *ppMeshSamplers[Attributes::VertexPositions],
        static_cast<uint32_t>(usdIndices.size()));
    }

    if (ppMeshSamplers[Attributes::BlendIndices] != nullptr || ppMeshSamplers[Attributes::BlendWeights] != nullptr) {
      if (ppMeshSamplers[Attributes::BlendIndices] == nullptr || ppMeshSamplers[Attributes::BlendWeights] == nullptr) {
        Logger::warn(str::format("Prim: ", m_meshPrim.GetPath().GetString(), ", has incomplete sampled skeleton data. Importing as static geometry."));
        ppMeshSamplers[Attributes::BlendIndices].reset();
        ppMeshSamplers[Attributes::BlendWeights].reset();
        m_actualNumBonesPerVertex = 0;
        m_limitedNumBonesPerVertex = 0;
      }
    }
  }

  float signNotZero(float x) {
    return x < 0.0f ? -1.0f : 1.0f;
  }

  std::uint32_t f32ToUnorm16(float x) {
    if (!std::isfinite(x)) {
      x = 0.0f;
    }
    x = std::clamp(x, 0.0f, 1.0f);
    const float scalar = (1 << 16) - 1;
    const float conv = x * scalar + 0.5f;

    return static_cast<std::uint32_t>(conv);
  }

  float sanitizeFinite(float value, float fallback = 0.0f) {
    return std::isfinite(value) ? value : fallback;
  }

  float saturateFinite(float value, float fallback = 0.0f) {
    return std::clamp(sanitizeFinite(value, fallback), 0.0f, 1.0f);
  }

  DWORD toUnorm8(float value, float fallback = 0.0f) {
    return static_cast<DWORD>(saturateFinite(value, fallback) * 255.0f + 0.5f);
  }
  
  // Helper function to limit bone influences with a variable number of influences per vertex
  template<uint32_t MaxBones>
  void limitBoneInfluences(const uint32_t* fullIndices,
                           const float* fullWeights,
                           uint32_t numInfluences,
                           uint32_t desiredBoneCount,
                           uint32_t* limitedIndices,
                           float* limitedWeights) {
    // Use a fixed-size array to store valid influences.
    std::pair<uint32_t, float> influences[MaxBones];
    uint32_t validCount = 0;

    // Collect valid influences from the vertex
    for (uint32_t i = 0; i < numInfluences; ++i) {
      if (std::isfinite(fullWeights[i]) && fullWeights[i] > 1e-4f) {  // Filter out negligible weights
        influences[validCount++] = { fullIndices[i], fullWeights[i] };
      }
    }

    // Sort the influences in descending order by weight
    std::sort(influences, influences + validCount,
              [](const std::pair<uint32_t, float>& a, const std::pair<uint32_t, float>& b) {
                return a.second > b.second;
              });

    // Limit the number of influences to the desired maximum.
    uint32_t countToKeep = validCount < desiredBoneCount ? validCount : desiredBoneCount;

    // Renormalize the weights so they sum to 1
    float totalWeight = 0.0f;
    for (uint32_t i = 0; i < countToKeep; ++i) {
      totalWeight += influences[i].second;
    }
    if (totalWeight > 0.0f) {
      for (uint32_t i = 0; i < countToKeep; ++i) {
        influences[i].second /= totalWeight;
      }
    }

    // Write the limited influences into the output arrays
    uint32_t i = 0;
    for (; i < countToKeep; ++i) {
      limitedIndices[i] = influences[i].first;
      limitedWeights[i] = influences[i].second;
    }

    // Zero out the remaining entries, output array must remain at a fixed size
    for (; i < numInfluences; ++i) {
      limitedIndices[i] = 0;
      limitedWeights[i] = 0.0f;
    }
  }

  void UsdMeshImporter::triangulate(const uint32_t numTriangles, 
                                    const uint32_t elementStride,
                                    const std::unique_ptr<GeomPrimvarSampler>* ppMeshSamplers,
                                    const VtIntArray& trianglePrimitiveParams,
                                    std::vector<uint32_t>& indicesOut,
                                    FaceToTriangleMap& triangleMapOut) {
    ZoneScoped;
    const uint32_t numIndices = numTriangles * 3;
    const uint32_t numVertexElements = numIndices * elementStride;
    indicesOut.resize(numIndices);
    m_vertexData.resize(numVertexElements);

    fast_unordered_cache<uint32_t> uniqueVertexToIndex;

    auto sampleGeneratedNormal = [&](uint32_t idx) {
      GfVec3f normal(0.0f, 0.0f, 1.0f);
      if (ppMeshSamplers[Attributes::VertexPositions] == nullptr)
        return normal;

      const uint32_t triBase = (idx / 3u) * 3u;
      GfVec3f p0(0.0f), p1(0.0f), p2(0.0f);
      if (!ppMeshSamplers[Attributes::VertexPositions]->SampleBuffer(static_cast<int>(triBase + 0u), &p0) ||
          !ppMeshSamplers[Attributes::VertexPositions]->SampleBuffer(static_cast<int>(triBase + 1u), &p1) ||
          !ppMeshSamplers[Attributes::VertexPositions]->SampleBuffer(static_cast<int>(triBase + 2u), &p2)) {
        return normal;
      }

      GfVec3f n = GfCross(p1 - p0, p2 - p0);
      const float lenSq = n.GetLengthSq();
      if (!std::isfinite(lenSq) || lenSq <= 1.0e-20f)
        return normal;

      n /= std::sqrt(lenSq);
      if (!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2]))
        return normal;

      return n;
    };

    IndexRange currentFaceMapRange;
    uint32_t uniqueVertexIndex = 0;
    uint32_t prevFaceIdx = 0;
    uint32_t totalOffset = 0;
    auto storeFaceRange = [&](uint32_t faceIdx, const IndexRange& range) {
      if (faceIdx >= triangleMapOut.size()) {
        Logger::warn(str::format("Skipping USD face-to-subset map entry outside topology: mesh=",
          m_meshPrim.GetPath().GetString(),
          " face=",
          faceIdx));
        return;
      }
      triangleMapOut[faceIdx] = range;
    };

    for (uint32_t triIdx = 0; triIdx < numTriangles; triIdx++) {
      for (uint32_t vertIdx = 0; vertIdx < 3; vertIdx++) {
        const uint32_t idx = triIdx * 3 + vertIdx;

        const uint32_t vertexOffset = totalOffset;
        std::memset(&m_vertexData[vertexOffset], 0, m_vertexStride);

        // Sample the vertex attributes to get all the data for this vertex.
        for (const VertexDeclaration& decl : m_vertexDecl) {
          switch (decl.attribute) {
          case Attributes::VertexPositions:
          {
            GfVec3f position(0.0f);
            ppMeshSamplers[decl.attribute]->SampleBuffer(idx, &position);
            m_vertexData[vertexOffset + decl.offset / 4 + 0] = sanitizeFinite(position[0]);
            m_vertexData[vertexOffset + decl.offset / 4 + 1] = sanitizeFinite(position[1]);
            m_vertexData[vertexOffset + decl.offset / 4 + 2] = sanitizeFinite(position[2]);
            break;
          }
          case Attributes::BlendWeights:
            // Do nothing, we decode the blend weights and indices together below
            break;
          case Attributes::BlendIndices:
          {
            if (ppMeshSamplers[Attributes::BlendWeights] == nullptr) {
              assert(0);
            }
            // Temporary storage for the full data.
            uint32_t blendIndicesStorage[MaxSupportedNumBones] = {};
            float blendWeightsStorage[MaxSupportedNumBones] = {};

            // Sample full bone indices...
            ppMeshSamplers[Attributes::BlendIndices]->SampleBuffer(idx, &blendIndicesStorage[0]);
            // ... and the corresponding blend weights
            ppMeshSamplers[Attributes::BlendWeights]->SampleBuffer(idx, &blendWeightsStorage[0]);

            for (uint32_t i = 0; i < m_actualNumBonesPerVertex; i++) {
              blendIndicesStorage[i] = std::min(blendIndicesStorage[i], 255u);
              if (!std::isfinite(blendWeightsStorage[i]) || blendWeightsStorage[i] < 0.0f) {
                blendWeightsStorage[i] = 0.0f;
              }
            }

            // Helper to write the blend data to our vertex buffer
            const auto& writeBlendData = [&](uint32_t* blendIndices, float* blendWeights) {
              // Encode the limited bone indices into compressed byte form
              for (uint32_t j = 0; j < m_limitedNumBonesPerVertex; j += 4) {
                uint32_t vertIndices = 0;
                for (uint32_t k = 0; k < 4 && (j + k) < m_limitedNumBonesPerVertex; ++k) {
                  const uint32_t packedIndex = std::min(blendIndices[j + k], 255u);
                  vertIndices |= packedIndex << (8 * k);
                }
                *(uint32_t*) (&m_vertexData[vertexOffset + decl.offset / 4 + j / 4]) = vertIndices;
              }

              // Write the weights
              const VertexDeclaration* blendWeightsDecl = nullptr;
              for (const VertexDeclaration& decl : m_vertexDecl) {
                if (decl.attribute == Attributes::BlendWeights) {
                  blendWeightsDecl = &decl;
                  break;
                }
              }
              assert(blendWeightsDecl != nullptr);
              memcpy(&m_vertexData[vertexOffset + blendWeightsDecl->offset / 4], &blendWeights[0], blendWeightsDecl->size);
            };

            // Limit the influences
            if (m_actualNumBonesPerVertex != m_limitedNumBonesPerVertex) {
              uint32_t limitedIndices[MaxSupportedNumBones] = {};
              float limitedWeights[MaxSupportedNumBones] = {};
              limitBoneInfluences<MaxSupportedNumBones>(blendIndicesStorage, blendWeightsStorage, m_actualNumBonesPerVertex, m_limitedNumBonesPerVertex, limitedIndices, limitedWeights);
              writeBlendData(&limitedIndices[0], &limitedWeights[0]);
            } else {
              writeBlendData(&blendIndicesStorage[0], &blendWeightsStorage[0]);
            }
            break;
          }
          case Attributes::Colors:
          {
            uint32_t& vertexColor = *(uint32_t*) &m_vertexData[vertexOffset + decl.offset / 4];
            float opacity = 1.0f;  // default to opaque
            if (ppMeshSamplers[Attributes::Opacity]) {
              ppMeshSamplers[Attributes::Opacity]->SampleBuffer(idx, &opacity);
            }
            GfVec3f color(1.0f); // default to white
            if (ppMeshSamplers[Attributes::Colors]) {
              ppMeshSamplers[Attributes::Colors]->SampleBuffer(idx, &color);
            }
            vertexColor = DXVK_COLOR_ARGB(toUnorm8(opacity, 1.0f), toUnorm8(color[0], 1.0f), toUnorm8(color[1], 1.0f), toUnorm8(color[2], 1.0f));
            break;
          }
          case Attributes::Opacity:
          {
            assert(false); // This attribute should never be in the VertexDeclaration.  Presence in the USD leads to Attributes::Colors existing.
            break;
          }
          case Attributes::Texcoords: {
            GfVec2f texcoord(0.0f);
            ppMeshSamplers[decl.attribute]->SampleBuffer(idx, &texcoord);
            m_vertexData[vertexOffset + decl.offset / 4 + 0] = sanitizeFinite(texcoord[0]);
            // Invert texcoord.y for Remix
            m_vertexData[vertexOffset + decl.offset / 4 + 1] = sanitizeFinite(1.f - texcoord[1]);
            break;
          }
          case Attributes::Normals: {
            GfVec3f normal(0.0f, 0.0f, 1.0f);
            if (!ppMeshSamplers[decl.attribute]->SampleBuffer(idx, &normal) ||
                !std::isfinite(normal[0]) ||
                !std::isfinite(normal[1]) ||
                !std::isfinite(normal[2])) {
              normal = sampleGeneratedNormal(idx);
            }
            uint32_t& normalStorage = *reinterpret_cast<uint32_t*>(&m_vertexData[vertexOffset + decl.offset / 4]);

            const float maxMag = std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]);
            if (!std::isfinite(maxMag) || maxMag <= 1e-20f) {
              normal = sampleGeneratedNormal(idx);
            }

            const float safeMaxMag = std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]);
            const float inverseMag = safeMaxMag <= 1e-20f ? 0.0f : (1.0f / safeMaxMag);
            float x = normal[0] * inverseMag;
            float y = normal[1] * inverseMag;

            if (normal[2] < 0.0f) {
              const auto originalXSign = signNotZero(x);
              const auto originalYSign = signNotZero(y);
              const auto inverseAbsX = 1.0f - std::abs(x);
              const auto inverseAbsY = 1.0f - std::abs(y);

              x = inverseAbsY * originalXSign;
              y = inverseAbsX * originalYSign;
            }

            // Signed->Unsigned octahedral
            x = x * 0.5f + 0.5f;
            y = y * 0.5f + 0.5f;

            normalStorage = f32ToUnorm16(x) | (f32ToUnorm16(y) << 16);

            break;
          }
          default: {
            ppMeshSamplers[decl.attribute]->SampleBuffer(idx, &m_vertexData[vertexOffset + decl.offset / 4]);
            break;
          }
          }
        }

        // If we've indexed this vertex before, no need to waste memory
        const XXH64_hash_t vHash = XXH3_64bits(&m_vertexData[totalOffset], m_vertexStride);

        const auto& existingVertex = uniqueVertexToIndex.find(vHash);
        if (existingVertex == uniqueVertexToIndex.end()) {
          std::memcpy(&m_vertexData[uniqueVertexIndex * elementStride], &m_vertexData[totalOffset], m_vertexStride);
          uniqueVertexToIndex[vHash] = indicesOut[idx] = uniqueVertexIndex++;
          // if this was unique, then offset the vertex data write pointer, if not, overwrite the last vertex entry
          totalOffset += elementStride;
        } else {
#ifndef NDEBUG
          // Check for hash collisions
          assert(memcmp(&m_vertexData[existingVertex->second * elementStride], &m_vertexData[totalOffset], m_vertexStride) == 0);
#endif
          indicesOut[idx] = existingVertex->second;
        }
      }

      // Build the face to index mapping for geom subsets
      if (triangleMapOut.size() > 0) {
        const uint32_t faceIdx = UsdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(trianglePrimitiveParams[triIdx]);
        if (faceIdx != prevFaceIdx) {
          currentFaceMapRange.end = triIdx * 3;
          storeFaceRange(prevFaceIdx, currentFaceMapRange);
          currentFaceMapRange.start = currentFaceMapRange.end; // restart the count
          prevFaceIdx = faceIdx;
        }
      }
    }

    if (triangleMapOut.size() > 0 && prevFaceIdx != 0xFFFFFFFF) {
      // Add the last face to mapping
      currentFaceMapRange.end = numTriangles * 3;
      storeFaceRange(prevFaceIdx, currentFaceMapRange);
    }

    m_vertexData.resize(uniqueVertexIndex * elementStride);
  }
}
