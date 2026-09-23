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

#include "../../../src/dxvk/rtx_render/rtx_lights_data.h"
#include "../../../src/util/log/log.h"
#include "../../../src/util/util_error.h"

#include "../../../src/lssusd/usd_include_begin.h"
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include "../../../src/lssusd/usd_include_end.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace dxvk {
Logger Logger::s_instance("test_usd_lux_import.log");
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

void testLegacyDoubleUsdLuxValuesImport() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_lux_import.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdLuxSphereLight light = UsdLuxSphereLight::Define(stage, SdfPath("/World/Light"));
  expect(static_cast<bool>(light), "Failed to define USD sphere light");

  UsdPrim prim = light.GetPrim();
  prim.CreateAttribute(TfToken("radius"), SdfValueTypeNames->Double).Set(2.0);
  prim.CreateAttribute(TfToken("intensity"), SdfValueTypeNames->Double).Set(4.0);
  prim.CreateAttribute(TfToken("exposure"), SdfValueTypeNames->Double).Set(1.0);
  prim.CreateAttribute(TfToken("color"), SdfValueTypeNames->Color3d).Set(GfVec3d(0.25, 0.5, 1.0));

  GfMatrix4f transform(1.0f);
  auto data = dxvk::LightData::tryCreate(prim, &transform, false, true);
  expect(data.has_value(), "Expected USD Lux sphere light to import");

  dxvk::RtLight rtLight = data->toRtLight();
  expect(rtLight.getType() == dxvk::RtLightType::Sphere, "Expected imported light to be a sphere light");
  expectNear(rtLight.getSphereLight().getRadius(), 2.0f, "Legacy double radius was not imported");

  const dxvk::Vector3 radiance = rtLight.getRadiance();
  expectNear(radiance.x, 2.0f, "Legacy Color3d radiance.x was not imported");
  expectNear(radiance.y, 4.0f, "Legacy Color3d radiance.y was not imported");
  expectNear(radiance.z, 8.0f, "Legacy Color3d radiance.z was not imported");
}

void testUsdLuxNaNValuesAreSanitized() {
  using namespace pxr;

  auto stage = UsdStage::CreateInMemory("test_usd_lux_nan_import.usda");
  expect(static_cast<bool>(stage), "Failed to create in-memory USD stage");

  UsdLuxSphereLight light = UsdLuxSphereLight::Define(stage, SdfPath("/World/Light"));
  expect(static_cast<bool>(light), "Failed to define USD sphere light");

  UsdPrim prim = light.GetPrim();
  prim.CreateAttribute(TfToken("inputs:radius"), SdfValueTypeNames->Float).Set(std::numeric_limits<float>::quiet_NaN());
  prim.CreateAttribute(TfToken("inputs:intensity"), SdfValueTypeNames->Float).Set(std::numeric_limits<float>::quiet_NaN());
  prim.CreateAttribute(TfToken("inputs:color"), SdfValueTypeNames->Color3f).Set(GfVec3f(
    std::numeric_limits<float>::quiet_NaN(),
    0.5f,
    std::numeric_limits<float>::quiet_NaN()));

  GfMatrix4f transform(1.0f);
  auto data = dxvk::LightData::tryCreate(prim, &transform, false, true);
  expect(data.has_value(), "Expected USD Lux sphere light with malformed values to import with defaults/sanitized values");

  dxvk::RtLight rtLight = data->toRtLight();
  expect(rtLight.getType() == dxvk::RtLightType::Sphere, "Expected imported light to be a sphere light");
  expectNear(rtLight.getSphereLight().getRadius(), 0.0f, "NaN radius should fall back to the sanitized default");

  const dxvk::Vector3 radiance = rtLight.getRadiance();
  expect(std::isfinite(radiance.x) && std::isfinite(radiance.y) && std::isfinite(radiance.z),
    "NaN USD Lux values must not survive into light radiance");
}

} // anonymous namespace

int main() {
  try {
    std::cout << "Begin USD Lux import tests" << std::endl;
    testLegacyDoubleUsdLuxValuesImport();
    testUsdLuxNaNValuesAreSanitized();
    std::cout << "All USD Lux import tests passed" << std::endl;
  } catch (const dxvk::DxvkError& e) {
    std::cerr << e.message() << std::endl;
    return 1;
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  return 0;
}
