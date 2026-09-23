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

#include "../../../src/dxvk/rtx_render/rtx_materials.h"
#include "../../../src/util/log/log.h"
#include "../../../src/util/util_error.h"

#include "../../../src/lssusd/usd_include_begin.h"
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/shader.h>
#include "../../../src/lssusd/usd_include_end.h"

#include <cmath>
#include <iostream>
#include <string>

namespace dxvk {
Logger Logger::s_instance("test_usd_shade_import.log");
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

void testUsdShadeTypedConstantsImport() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_shade_import.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdShadeShader shader = UsdShadeShader::Define(stage, SdfPath("/World/Looks/TestMaterial/Shader"));
  expect(static_cast<bool>(shader), "Failed to define USD shader");

  UsdPrim prim = shader.GetPrim();
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getAlbedoConstantToken(), SdfValueTypeNames->Color3d).Set(GfVec3d(0.25, 0.5, 0.75));
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getOpacityConstantToken(), SdfValueTypeNames->Double).Set(0.5);
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getEnableEmissionToken(), SdfValueTypeNames->Int).Set(1);
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getBlendTypeToken(), SdfValueTypeNames->Int).Set(static_cast<int>(BlendType::kReverseColor));
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getSpriteSheetRowsToken(), SdfValueTypeNames->Int).Set(300);
  prim.CreateAttribute(dxvk::OpaqueMaterialData::getAlphaTestReferenceValueToken(), SdfValueTypeNames->Int).Set(-5);

  auto material = dxvk::OpaqueMaterialData::deserialize(
    [](const pxr::UsdPrim&, const pxr::TfToken&) { return dxvk::TextureRef {}; },
    prim);

  const dxvk::Vector3 albedo = material.getAlbedoConstant();
  expectNear(albedo.x, 0.25f, "Color3d albedo.x was not imported");
  expectNear(albedo.y, 0.5f, "Color3d albedo.y was not imported");
  expectNear(albedo.z, 0.75f, "Color3d albedo.z was not imported");
  expectNear(material.getOpacityConstant(), 0.5f, "Double opacity was not imported");
  expect(material.getEnableEmission(), "Numeric USD bool was not imported");
  expect(material.getBlendType() == BlendType::kReverseColor, "Numeric USD enum was not imported");
  expect(material.getSpriteSheetRows() == 255, "USD byte material value was not clamped high");
  expect(material.getAlphaTestReferenceValue() == 0, "USD byte material value was not clamped low");
}

} // anonymous namespace

int main() {
  try {
    std::cout << "Begin USD Shade import tests" << std::endl;
    testUsdShadeTypedConstantsImport();
    std::cout << "All USD Shade import tests passed" << std::endl;
  } catch (const dxvk::DxvkError& e) {
    std::cerr << e.message() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}
