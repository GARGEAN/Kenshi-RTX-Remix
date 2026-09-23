/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

#include "../../../src/lssusd/usd_mesh_importer.h"
#include "../../../src/util/log/log.h"
#include "../../../src/util/util_error.h"

#include "../../../src/lssusd/usd_include_begin.h"
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include "../../../src/lssusd/usd_include_end.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace dxvk {
Logger Logger::s_instance("test_usd_mesh_importer.log");
}

namespace {

void expect(bool value, const char* message) {
  if (!value) {
    throw dxvk::DxvkError(message);
  }
}

void expectNear(float actual, float expected, const char* message) {
  if (std::abs(actual - expected) > 1e-6f) {
    throw dxvk::DxvkError(std::string(message) + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
  }
}

uint32_t readU32(const std::vector<float>& data, size_t floatOffset) {
  uint32_t result = 0;
  std::memcpy(&result, &data[floatOffset], sizeof(result));
  return result;
}

const lss::UsdMeshImporter::VertexDeclaration* findDecl(
    const lss::UsdMeshImporter& importer,
    lss::UsdMeshImporter::Attributes attribute) {
  for (const auto& decl : importer.GetVertexDecl()) {
    if (decl.attribute == attribute) {
      return &decl;
    }
  }

  return nullptr;
}

pxr::VtVec3fArray makeVec3Array(std::initializer_list<pxr::GfVec3f> values) {
  pxr::VtVec3fArray result(values.size());
  size_t index = 0;
  for (const auto& value : values) {
    result[index++] = value;
  }
  return result;
}

pxr::VtVec2fArray makeVec2Array(std::initializer_list<pxr::GfVec2f> values) {
  pxr::VtVec2fArray result(values.size());
  size_t index = 0;
  for (const auto& value : values) {
    result[index++] = value;
  }
  return result;
}

pxr::VtVec4fArray makeVec4Array(std::initializer_list<pxr::GfVec4f> values) {
  pxr::VtVec4fArray result(values.size());
  size_t index = 0;
  for (const auto& value : values) {
    result[index++] = value;
  }
  return result;
}

void testDefaultOrientationAndTypedPrimvars() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Quad"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 4 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2, 3 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 1.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  // This intentionally conflicts with primvars:normals below. The importer
  // should prefer the primvar so normal maps do not get inverted by fallback data.
  mesh.CreateNormalsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, -1.0f),
    GfVec3f(0.0f, 0.0f, -1.0f),
    GfVec3f(0.0f, 0.0f, -1.0f),
    GfVec3f(0.0f, 0.0f, -1.0f),
  }));

  UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
  UsdGeomPrimvar normals = primvars.CreatePrimvar(
    TfToken("normals"),
    SdfValueTypeNames->Normal3fArray,
    UsdGeomTokens->vertex);
  normals.Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 1.0f),
    GfVec3f(0.0f, 0.0f, 1.0f),
    GfVec3f(0.0f, 0.0f, 1.0f),
    GfVec3f(0.0f, 0.0f, 1.0f),
  }));

  UsdGeomPrimvar st = primvars.CreatePrimvar(
    TfToken("st"),
    SdfValueTypeNames->TexCoord2fArray,
    UsdGeomTokens->vertex);
  st.Set(makeVec2Array({
    GfVec2f(0.0f, 0.0f),
    GfVec2f(1.0f, 0.0f),
    GfVec2f(1.0f, 1.0f),
    GfVec2f(0.0f, 1.0f),
  }));

  UsdGeomPrimvar displayColor = mesh.CreateDisplayColorPrimvar(UsdGeomTokens->vertex);
  displayColor.Set(makeVec3Array({
    GfVec3f(1.0f, 1.0f, 1.0f),
    GfVec3f(2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()),
    GfVec3f(0.0f, 1.0f, 0.0f),
    GfVec3f(0.0f, 0.0f, 1.0f),
  }));

  UsdGeomPrimvar displayOpacity = mesh.CreateDisplayOpacityPrimvar(UsdGeomTokens->vertex);
  pxr::VtFloatArray opacities(4);
  opacities[0] = 1.0f;
  opacities[1] = std::numeric_limits<float>::quiet_NaN();
  opacities[2] = 1.0f;
  opacities[3] = 1.0f;
  displayOpacity.Set(opacities);

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  expect(importer.IsRightHanded(), "USD meshes with no authored orientation must default to right-handed");
  expect(importer.GetSubMeshes().size() == 1, "Expected one submesh");

  const auto& indices = importer.GetSubMeshes()[0].indexBuffer;
  static constexpr uint32_t expectedIndices[] = { 0, 1, 2, 0, 3, 1 };
  static constexpr size_t expectedIndexCount = sizeof(expectedIndices) / sizeof(expectedIndices[0]);
  expect(indices.size() == expectedIndexCount, "Unexpected index count");
  for (size_t i = 0; i < expectedIndexCount; i++) {
    expect(indices[i] == expectedIndices[i], "Default USD orientation produced flipped triangle indices");
  }

  const auto* normalDecl = findDecl(importer, lss::UsdMeshImporter::Normals);
  const auto* texcoordDecl = findDecl(importer, lss::UsdMeshImporter::Texcoords);
  const auto* colorDecl = findDecl(importer, lss::UsdMeshImporter::Colors);
  expect(normalDecl != nullptr, "Expected imported normal declaration");
  expect(texcoordDecl != nullptr, "Expected imported texcoord declaration");
  expect(colorDecl != nullptr, "Expected imported color declaration");

  const auto& data = importer.GetVertexData();
  const size_t stride = importer.GetVertexStride() / sizeof(float);

  expect(readU32(data, normalDecl->offset / sizeof(float)) == 0x80008000u,
    "primvars:normals was not imported as the active +Z normal");

  expectNear(data[stride + texcoordDecl->offset / sizeof(float) + 0], 1.0f, "Vertex 1 texcoord.x");
  expectNear(data[stride + texcoordDecl->offset / sizeof(float) + 1], 0.0f, "Vertex 1 texcoord.y");
  expectNear(data[(stride * 2) + texcoordDecl->offset / sizeof(float) + 0], 1.0f, "Vertex 2 texcoord.x");
  expectNear(data[(stride * 2) + texcoordDecl->offset / sizeof(float) + 1], 1.0f, "Vertex 2 texcoord.y");

  expect(readU32(data, (stride * 2) + colorDecl->offset / sizeof(float)) == 0xffff00ffu,
    "Color and opacity should be clamped and sanitized before hashing/import");
}

void testUnexpectedTexcoordSizeIsSkipped() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_bad_texcoord.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Triangle"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 3 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
  UsdGeomPrimvar badTexcoord = primvars.CreatePrimvar(
    TfToken("st"),
    SdfValueTypeNames->Float3Array,
    UsdGeomTokens->vertex);
  badTexcoord.Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  expect(findDecl(importer, lss::UsdMeshImporter::Texcoords) == nullptr,
    "Unexpected texcoord element sizes must be skipped instead of sampled into TexCoord2f storage");
}

void testIncompleteSkeletonImportsAsStaticGeometry() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_incomplete_skel.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Triangle"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 3 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  UsdSkelBindingAPI skelBinding = UsdSkelBindingAPI::Apply(mesh.GetPrim());
  UsdGeomPrimvar jointWeights = skelBinding.CreateJointWeightsPrimvar(false, 4);
  pxr::VtFloatArray weights(12);
  for (size_t i = 0; i < weights.size(); i++) {
    weights[i] = (i % 4) == 0 ? 1.0f : 0.0f;
  }
  jointWeights.Set(weights);

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  expect(findDecl(importer, lss::UsdMeshImporter::BlendIndices) == nullptr,
    "Incomplete skeleton indices must not create blend index vertex data");
  expect(findDecl(importer, lss::UsdMeshImporter::BlendWeights) == nullptr,
    "Incomplete skeleton weights must not create blend weight vertex data");
}

void testSkeletonJointDataIsSanitized() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_skel_sanitize.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Triangle"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 3 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  UsdSkelBindingAPI skelBinding = UsdSkelBindingAPI::Apply(mesh.GetPrim());
  UsdGeomPrimvar jointIndices = skelBinding.CreateJointIndicesPrimvar(false, 4);
  UsdGeomPrimvar jointWeights = skelBinding.CreateJointWeightsPrimvar(false, 4);

  jointIndices.Set(VtIntArray({
    300, 1, 2, 3,
    4, 5, 6, 7,
    8, 9, 10, 11,
  }));

  VtFloatArray weights(12);
  for (size_t i = 0; i < weights.size(); i++) {
    weights[i] = (i % 4) == 0 ? 1.0f : 0.0f;
  }
  weights[4] = std::numeric_limits<float>::quiet_NaN();
  jointWeights.Set(weights);

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);

  const auto* blendIndicesDecl = findDecl(importer, lss::UsdMeshImporter::BlendIndices);
  const auto* blendWeightsDecl = findDecl(importer, lss::UsdMeshImporter::BlendWeights);
  expect(blendIndicesDecl != nullptr, "Expected imported blend index declaration");
  expect(blendWeightsDecl != nullptr, "Expected imported blend weight declaration");

  const auto& data = importer.GetVertexData();
  const size_t stride = importer.GetVertexStride() / sizeof(float);
  size_t vertexWithNanWeight = data.size();
  for (size_t i = 0; i < importer.GetNumVertices(); i++) {
    const size_t vertexOffset = i * stride;
    if (std::abs(data[vertexOffset + 0] - 1.0f) <= 1e-6f &&
        std::abs(data[vertexOffset + 1] - 0.0f) <= 1e-6f &&
        std::abs(data[vertexOffset + 2] - 0.0f) <= 1e-6f) {
      vertexWithNanWeight = vertexOffset;
      break;
    }
  }

  expect(readU32(data, blendIndicesDecl->offset / sizeof(float)) == 0x030201ffu,
    "Joint indices must be clamped before byte packing instead of wrapping");
  expect(vertexWithNanWeight != data.size(), "Could not find vertex carrying the non-finite joint weight");
  expectNear(data[vertexWithNanWeight + blendWeightsDecl->offset / sizeof(float)], 0.0f,
    "Non-finite joint weights must be sanitized before vertex packing");
}

void testMissingNormalsAreGenerated() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_generated_normals.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Triangle"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 3 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  const auto* normalDecl = findDecl(importer, lss::UsdMeshImporter::Normals);
  expect(normalDecl != nullptr, "Meshes without authored normals should get generated normals");

  const auto& data = importer.GetVertexData();
  const size_t stride = importer.GetVertexStride() / sizeof(float);
  const uint32_t expectedPackedNormal = 0xffffffffu;
  for (size_t i = 0; i < importer.GetNumVertices(); i++) {
    expect(readU32(data, i * stride + normalDecl->offset / sizeof(float)) == expectedPackedNormal,
      "Generated winding-derived normal was not packed correctly");
  }
}

void testCorruptNormalsFallBackToGeneratedNormals() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_corrupt_normals.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Triangle"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 3 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
  UsdGeomPrimvar normals = primvars.CreatePrimvar(
    TfToken("normals"),
    SdfValueTypeNames->Normal3fArray,
    UsdGeomTokens->vertex);
  normals.Set(makeVec3Array({
    GfVec3f(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f),
    GfVec3f(0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f),
    GfVec3f(0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()),
  }));

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  const auto* normalDecl = findDecl(importer, lss::UsdMeshImporter::Normals);
  expect(normalDecl != nullptr, "Expected normal declaration");

  const auto& data = importer.GetVertexData();
  const size_t stride = importer.GetVertexStride() / sizeof(float);
  const uint32_t expectedPackedNormal = 0xffffffffu;
  for (size_t i = 0; i < importer.GetNumVertices(); i++) {
    expect(readU32(data, i * stride + normalDecl->offset / sizeof(float)) == expectedPackedNormal,
      "Corrupt authored normals should fall back to generated normals");
  }
}

void testInvalidGeomSubsetIndicesAreSkipped() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_bad_subset.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Quad"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 4 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2, 3 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 1.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  UsdGeomSubset subset = UsdGeomSubset::Define(stage, SdfPath("/World/Quad/Subset"));
  subset.CreateIndicesAttr().Set(VtIntArray({ 0, 99, -1 }));

  lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
  expect(importer.GetSubMeshes().size() == 1, "Expected one subset mesh");
  expect(importer.GetSubMeshes()[0].indexBuffer.size() == 6,
    "Invalid subset face indices should be skipped while valid face triangles remain");
}

void testInvalidTopologyDoesNotGenerateZeroTriangles() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_mesh_importer_invalid_topology.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/Broken"));
  expect(static_cast<bool>(mesh), "Failed to define USD mesh");

  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray({ 4 }));
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray({ 0, 1, 2 }));
  mesh.CreatePointsAttr().Set(makeVec3Array({
    GfVec3f(0.0f, 0.0f, 0.0f),
    GfVec3f(1.0f, 0.0f, 0.0f),
    GfVec3f(0.0f, 1.0f, 0.0f),
  }));

  bool threw = false;
  try {
    lss::UsdMeshImporter importer(mesh.GetPrim(), 4);
    (void) importer;
  } catch (const dxvk::DxvkError&) {
    threw = true;
  }

  expect(threw, "Invalid USD topology should not import as zero-filled triangles");
}

} // anonymous namespace

int main() {
  try {
    std::cout << "Begin USD mesh importer tests" << std::endl;
    testDefaultOrientationAndTypedPrimvars();
    testUnexpectedTexcoordSizeIsSkipped();
    testIncompleteSkeletonImportsAsStaticGeometry();
    testSkeletonJointDataIsSanitized();
    testMissingNormalsAreGenerated();
    testCorruptNormalsFallBackToGeneratedNormals();
    testInvalidGeomSubsetIndicesAreSkipped();
    testInvalidTopologyDoesNotGenerateZeroTriangles();
    std::cout << "All USD mesh importer tests passed" << std::endl;
  } catch (const dxvk::DxvkError& e) {
    std::cerr << e.message() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}
