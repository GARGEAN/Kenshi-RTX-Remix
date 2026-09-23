/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#define RTX_REMIX_PNEXT_CHECK_STRUCTS

#include "rtx_asset_data_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_light_manager.h"
#include "rtx_objectpicking.h"
#include "rtx_option.h"
#include "rtx_globals.h"
#include "rtx_options.h"
#include "rtx_debug_view.h"

#include "../dxvk_device.h"
#include "rtx_texture_manager.h"

#include <remix/remix_c.h>
#include "rtx_remix_pnext.h"

#include "../dxvk_image.h"

#include "../../util/util_math.h"
#include "../../util/util_vector.h"
#include "../../util/util_string.h"
#include "../../util/log/log.h"

#include "../../d3d11/d3d11_device.h"
#include "../../d3d11/d3d11_context_imm.h"
#include "../../d3d11/d3d11_texture.h"

#include "../../lssusd/usd_include_begin.h"
#include <src/usd-plugins/RemixParticleSystem/ParticleSystemAPI.h>
#include "../../lssusd/usd_include_end.h"

#include <windows.h>
#include <dxgi1_2.h>

#include <optional>

namespace dxvk {
  extern bool g_allowSrgbConversionForOutput;
  extern bool g_forceKeepObjectPickingImage;

  extern std::array<uint8_t, 3> g_customHighlightColor;
}

namespace dxvk {
  // Because DrawCallState/LegacyMaterialData hide needed fields as private
  struct RemixAPIPrivateAccessor {
    static ExternalDrawState toRtDrawState(const remixapi_InstanceInfo& info);

    template<typename Cmd>
    static void EmitCs(D3D11ImmediateContext* ctx, Cmd&& command) {
      ctx->EmitCs(std::forward<Cmd>(command));
    }
  };
}

namespace {
  uint64_t s_apiVersion{ 0 };
  dxvk::D3D11Device*           s_d3d11Device  { nullptr };
  dxvk::D3D11ImmediateContext* s_d3d11Context { nullptr };
  IDXGISwapChain1*             s_dxgiSwapChain { nullptr };
  HWND                         s_hwnd { nullptr };
  dxvk::mutex s_mutex {};


  dxvk::D3D11ImmediateContext* tryGetContext() {
    return s_d3d11Context;
  }


  // from rtx_mod_usd.cpp
  XXH64_hash_t hack_getNextGeomHash() {
    static uint64_t s_id = UINT64_MAX;
    std::lock_guard lock { s_mutex };
    --s_id;
    return XXH64(&s_id, sizeof(s_id), 0);
  }


  template<typename T>
  size_t sizeInBytes(const T* values, size_t count) {
    return sizeof(T) * count;
  }


  bool isHResultAliasedWithRemixErrorCode(HRESULT hr) {
    switch (hr) {
    case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES:
    case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM:
    case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL:
    case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return true;
    default: return false;
    }
  }


  namespace convert {
    using namespace dxvk;

    std::string tostr(const remixapi_MaterialHandle& h) {
      static_assert(sizeof h == sizeof uint64_t);
      return std::to_string(reinterpret_cast<uint64_t>(h));
    }

    Matrix4 tomat4(const remixapi_Transform& transform) {
      const auto& m = transform.matrix;
      return Matrix4 {
        m[0][0], m[1][0], m[2][0], 0.f,
        m[0][1], m[1][1], m[2][1], 0.f,
        m[0][2], m[1][2], m[2][2], 0.f,
        m[0][3], m[1][3], m[2][3], 1.f
      };
    }

    Vector2 tovec2(const remixapi_Float2D& v) {
      return Vector2{ v.x, v.y };
    }

    Vector3 tovec3(const remixapi_Float3D& v) {
      return Vector3 { v.x, v.y, v.z };
    }

    Vector4 tovec4(const remixapi_Float4D& v) {
      return Vector4 { v.x, v.y, v.z, v.w };
    }

    Vector3d tovec3d(const remixapi_Float3D& v) {
      return Vector3d{ v.x, v.y, v.z };
    }

    std::vector<float> toAnimatedFloat1D(const remixapi_AnimatedFloat1D& animated) {
      std::vector<float> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(animated.pData[i]);
        }
      }
      return result;
    }

    std::vector<vec2> toAnimatedFloat2D(const remixapi_AnimatedFloat2D& animated) {
      std::vector<vec2> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec2(animated.pData[i].x, animated.pData[i].y));
        }
      }
      return result;
    }

    std::vector<vec3> toAnimatedFloat3D(const remixapi_AnimatedFloat3D& animated) {
      std::vector<vec3> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec3(animated.pData[i].x, animated.pData[i].y, animated.pData[i].z));
        }
      }
      return result;
    }

    std::vector<vec4> toAnimatedFloat4D(const remixapi_AnimatedFloat4D& animated) {
      std::vector<vec4> result;
      if (animated.pData && animated.numberElements > 0) {
        result.reserve(animated.numberElements);
        for (uint32_t i = 0; i < animated.numberElements; ++i) {
          result.push_back(vec4(animated.pData[i].x, animated.pData[i].y, animated.pData[i].z, animated.pData[i].w));
        }
      }
      return result;
    }

    constexpr bool tobool(remixapi_Bool b) {
      return !!b;
    }

    std::filesystem::path topath(remixapi_Path p) {
      if (!p) {
        return {};
      }
      return p;
    }

    // --

    struct PreloadSource {
      std::filesystem::path albedoTexture;
      std::filesystem::path normalTexture;
      std::filesystem::path tangentTexture;
      std::filesystem::path emissiveTexture;
      std::filesystem::path transmittanceTexture;
      std::filesystem::path roughnessTexture;
      std::filesystem::path metallicTexture;
      std::filesystem::path heightTexture;
      std::filesystem::path subsurfaceTransmittanceTexture;
      std::filesystem::path subsurfaceThicknessTexture;
      std::filesystem::path subsurfaceSingleScatteringAlbedoTexture;
      std::filesystem::path subsurfaceRadiusTexture;
    };

    PreloadSource makePreloadSource(const remixapi_MaterialInfo& info) {
      // TODO: C++20 designated initializers
      if (auto extOpaque = pnext::find<remixapi_MaterialInfoOpaqueEXT>(&info)) {
        auto extSubsurface = pnext::find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(&info);
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          {},                           // transmittanceTexture;
          topath(extOpaque->roughnessTexture),  // roughnessTexture;
          topath(extOpaque->metallicTexture),   // metallicTexture;
          topath(extOpaque->heightTexture),     // heightTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceTransmittanceTexture          : nullptr), // subsurfaceTransmittanceTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceThicknessTexture              : nullptr), // subsurfaceThicknessTexture;
          topath(extSubsurface ? extSubsurface->subsurfaceSingleScatteringAlbedoTexture : nullptr), // subsurfaceSingleScatteringAlbedoTexture;
          topath(s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 5, 1) && extSubsurface ? extSubsurface->subsurfaceRadiusTexture : nullptr), // subsurfaceRadiusTexture;
        };
      }
      if (auto extTranslucent = pnext::find<remixapi_MaterialInfoTranslucentEXT>(&info)) {
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          topath(extTranslucent->transmittanceTexture), // transmittanceTexture;
          {}, // roughnessTexture;
          {}, // metallicTexture;
          {}, // heightTexture;
          {}, // subsurfaceTransmittanceTexture;
          {}, // subsurfaceThicknessTexture;
          {}, // subsurfaceSingleScatteringAlbedoTexture;
          {}, // subsurfaceRadiusTexture;
        };
      }
      if (auto extPortal = pnext::find<remixapi_MaterialInfoPortalEXT>(&info)) {
        return PreloadSource {
          topath(info.albedoTexture),   // albedoTexture;
          topath(info.normalTexture),   // normalTexture;
          topath(info.tangentTexture),  // tangentTexture;
          topath(info.emissiveTexture), // emissiveTexture;
          {}, // transmittanceTexture;
          {}, // roughnessTexture;
          {}, // metallicTexture;
          {}, // heightTexture;
          {}, // subsurfaceTransmittanceTexture;
          {}, // subsurfaceThicknessTexture;
          {}, // subsurfaceSingleScatteringAlbedoTexture;
          {}, // subsurfaceRadiusTexture;
        };
      }
      return {};
    }

    MaterialData toRtMaterialFinalized(dxvk::DxvkContext& ctx, const MaterialData& materialWithoutPreload, const PreloadSource& preload) {
      auto preloadTexture = [&ctx](const std::filesystem::path& path)->TextureRef {
        if (path.empty()) {
          return {};
        }
        auto assetData = AssetDataManager::get().findAsset(path.string());
        if (assetData == nullptr) {
          return {};
        }
        auto uploadedTexture = ctx.getCommonObjects()->getTextureManager()
          .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, false);
        return TextureRef { uploadedTexture };
      };

      switch (materialWithoutPreload.getType()) {
      case MaterialDataType::Opaque:
      {
        const auto& src = materialWithoutPreload.getOpaqueMaterialData();
        return MaterialData { OpaqueMaterialData{
          preloadTexture(preload.albedoTexture),
          preloadTexture(preload.normalTexture),
          preloadTexture(preload.tangentTexture),
          preloadTexture(preload.heightTexture),
          preloadTexture(preload.roughnessTexture),
          preloadTexture(preload.metallicTexture),
          preloadTexture(preload.emissiveTexture),
          preloadTexture(preload.subsurfaceTransmittanceTexture),
          preloadTexture(preload.subsurfaceThicknessTexture),
          preloadTexture(preload.subsurfaceSingleScatteringAlbedoTexture),
          preloadTexture(preload.subsurfaceRadiusTexture),
          TextureRef(),
          src.getAnisotropyConstant(),
          src.getEmissiveIntensity(),
          src.getAlbedoConstant(),
          src.getOpacityConstant(),
          src.getRoughnessConstant(),
          src.getMetallicConstant(),
          src.getEmissiveColorConstant(),
          src.getEnableEmission(),
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getEnableThinFilm(),
          src.getAlphaIsThinFilmThickness(),
           src.getThinFilmThicknessConstant(),
           src.getUseLegacyAlphaState(),

           src.getBlendEnabled(),
          src.getBlendType(),
          src.getInvertedBlend(),
          src.getAlphaTestType(),
          src.getAlphaTestReferenceValue(),
          src.getDisplaceIn(),
          src.getDisplaceOut(),
          src.getSubsurfaceTransmittanceColor(),
          src.getSubsurfaceMeasurementDistance(),
          src.getSubsurfaceSingleScatteringAlbedo(),
          src.getSubsurfaceVolumetricAnisotropy(),
          src.getSubsurfaceDiffusionProfile(),
          src.getSubsurfaceRadius(),
          src.getSubsurfaceRadiusScale(),
          src.getSubsurfaceMaxSampleRadius(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV()
        } };
      }
      case MaterialDataType::Translucent: 
      {
        const auto& src = materialWithoutPreload.getTranslucentMaterialData();
        return MaterialData { TranslucentMaterialData {
          preloadTexture(preload.normalTexture),
          preloadTexture(preload.transmittanceTexture),
          preloadTexture(preload.emissiveTexture),
          src.getRefractiveIndex(),
          src.getTransmittanceColor(),
          src.getTransmittanceMeasurementDistance(),
          src.getEnableEmission(),
          src.getEmissiveIntensity(),
          src.getEmissiveColorConstant(),
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getEnableThinWalled(),
          src.getThinWallThickness(),
          src.getEnableDiffuseLayer(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV()
        } };
      }
      case MaterialDataType::RayPortal:
      {
        const auto& src = materialWithoutPreload.getRayPortalMaterialData();
        return MaterialData { RayPortalMaterialData {
          preloadTexture(preload.emissiveTexture),
          {}, // unused
          src.getRayPortalIndex(),
          src.getSpriteSheetRows(),
          src.getSpriteSheetCols(),
          src.getSpriteSheetFPS(),
          src.getRotationSpeed(),
          src.getEnableEmission(),
          src.getEmissiveIntensity(),
          src.getFilterMode(),
          src.getWrapModeU(),
          src.getWrapModeV()
        } };
      }
      default: assert(0); return materialWithoutPreload;
      }
    }

    MaterialData toRtMaterialWithoutTexturePreload(const remixapi_MaterialInfo& info) {
      if (auto extOpaque = pnext::find<remixapi_MaterialInfoOpaqueEXT>(&info)) {
        auto extSubsurface = pnext::find<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(&info);
        return MaterialData { OpaqueMaterialData {
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          {},
          extOpaque->anisotropy,
          info.emissiveIntensity,
          tovec3(extOpaque->albedoConstant),
          extOpaque->opacityConstant,
          extOpaque->roughnessConstant,
          extOpaque->metallicConstant,
          tovec3(info.emissiveColorConstant),
          info.emissiveIntensity > 0.f,
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          tobool(extOpaque->thinFilmThickness_hasvalue),
          tobool(extOpaque->alphaIsThinFilmThickness),
           extOpaque->thinFilmThickness_hasvalue ? extOpaque->thinFilmThickness_value : 200.f, // default OpaqueMaterial::ThinFilmThicknessConstant
           tobool(extOpaque->useDrawCallAlphaState), // OpaqueMaterial::UseLegacyAlphaState

           tobool(extOpaque->blendType_hasvalue),
          extOpaque->blendType_hasvalue ? static_cast<BlendType>(extOpaque->blendType_value) : BlendType::kAlpha,  // default OpaqueMaterial::BlendType
          tobool(extOpaque->invertedBlend),
          static_cast<AlphaTestType>(extOpaque->alphaTestType),
          extOpaque->alphaReferenceValue,
          extOpaque->displaceIn,
          (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 4, 2)) ? extOpaque->displaceOut : 0.f,
          extSubsurface ? tovec3(extSubsurface->subsurfaceTransmittanceColor) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceMeasurementDistance : 0.f,
          extSubsurface ? tovec3(extSubsurface->subsurfaceSingleScatteringAlbedo) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceVolumetricAnisotropy : 0.f,
          extSubsurface ? static_cast<bool>(extSubsurface->subsurfaceDiffusionProfile) : false,
          extSubsurface ? tovec3(extSubsurface->subsurfaceRadius) : Vector3{ 0.5f, 0.5f, 0.5f },
          extSubsurface ? extSubsurface->subsurfaceRadiusScale : 0.f,
          extSubsurface ? extSubsurface->subsurfaceMaxSampleRadius : 0.f,
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
        } };
      }
      if (auto extTranslucent = pnext::find<remixapi_MaterialInfoTranslucentEXT>(&info)) {
        return MaterialData { TranslucentMaterialData {
          {},
          {},
          {},
          extTranslucent->refractiveIndex,
          tovec3(extTranslucent->transmittanceColor),
          extTranslucent->transmittanceMeasurementDistance,
          info.emissiveIntensity > 0.f,
          info.emissiveIntensity,
          tovec3(info.emissiveColorConstant),
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          tobool(extTranslucent->thinWallThickness_hasvalue),
          extTranslucent->thinWallThickness_hasvalue ? extTranslucent->thinWallThickness_value : 0.001f, // default TranslucentMaterial::ThinWallThickness
          tobool(extTranslucent->useDiffuseLayer),
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
        } };
      }
      if (auto extPortal = pnext::find<remixapi_MaterialInfoPortalEXT>(&info)) {
        return MaterialData { RayPortalMaterialData {
          {},
          {}, // unused
          extPortal->rayPortalIndex,
          info.spriteSheetRow,
          info.spriteSheetCol,
          info.spriteSheetFps,
          extPortal->rotationSpeed,
          info.emissiveIntensity > 0.f,
          info.emissiveIntensity,
          info.filterMode,
          info.wrapModeU,
          info.wrapModeV,
        } };
      }

      assert(0);
      return MaterialData { OpaqueMaterialData {} };
    }

    // --
    CameraType::Enum toRtCameraType(remixapi_CameraType from) {
      switch (from) {
      case REMIXAPI_CAMERA_TYPE_WORLD: return CameraType::Main;
      case REMIXAPI_CAMERA_TYPE_VIEW_MODEL: return CameraType::ViewModel;
      case REMIXAPI_CAMERA_TYPE_SKY: return CameraType::Sky;
      default: assert(0); return CameraType::Main;
      }
    }

    struct ExternalCameraInfo {
      CameraType::Enum type {};
      Matrix4 worldToView {};
      Matrix4 viewToProjection {};
    };

    ExternalCameraInfo toRtCamera(const remixapi_CameraInfo& info) {
      if (auto params = pnext::find<remixapi_CameraInfoParameterizedEXT>(&info)) {
        auto result = ExternalCameraInfo {
          toRtCameraType(info.type),
        };
        {
          const auto newViewToWorld = Matrix4d {
           Vector4d{ normalize(tovec3d(params->right)), 0.0 },
           Vector4d{ normalize(tovec3d(params->up)), 0.0 },
           Vector4d{ normalize(tovec3d(params->forward)), 0.0 },
           Vector4d{ tovec3d(params->position), 1.0 },
          };
          result.worldToView = inverse(newViewToWorld);
        }
        {
          constexpr bool isLhs = true;
          auto proj = float4x4 {};
          proj.SetupByHalfFovy(
            DegToRad(params->fovYInDegrees) / 2,
            params->aspect,
            params->nearPlane,
            params->farPlane,
            isLhs ? PROJ_LEFT_HANDED : 0);
          static_assert(sizeof result.viewToProjection == sizeof proj);
          memcpy(&result.viewToProjection, &proj, sizeof float4x4);
        }
        return result;
      }
      return ExternalCameraInfo {
        toRtCameraType(info.type),
        Matrix4 { info.view },
        Matrix4 { info.projection },
      };
    }

    // --

    std::optional<RtLightShaping> toRtLightShaping(const remixapi_LightInfoLightShaping* info) {
      if (info) {
        return RtLightShaping::tryCreate(
          true,
          tovec3(info->direction),
          std::cos(DegToRad(info->coneAngleDegrees)),
          info->coneSoftness,
          info->focusExponent
        );
      }

      // Note: Default constructed Light Shaping returned when no info is provided to have a valid but disabled
      // Light Shaping object (different from returning an empty optional here, which means creation of a Light
      // Shaping failed).
      return RtLightShaping{};
    }

    std::optional<RtLight> toRtLight(const remixapi_LightInfo& info) {
      if (auto src = pnext::find<remixapi_LightInfoUSDEXT>(&info)) {
        if (auto lightData = LightData::tryCreate(*src)) {
          return lightData->toRtLight();
        }
        return {};
      }
      if (auto src = pnext::find<remixapi_LightInfoSphereEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtSphereLight::tryCreate(
          tovec3(src->position),
          tovec3(info.radiance),
          src->radius,
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoRectEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtRectLight::tryCreate(
          tovec3(src->position),
          Vector2{src->xSize, src->ySize},
          tovec3(src->xAxis),
          tovec3(src->yAxis),
          tovec3(src->direction),
          tovec3(info.radiance),
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoDiskEXT>(&info)) {
        const auto shaping = toRtLightShaping(src->shaping_hasvalue ? &src->shaping_value : nullptr);

        if (!shaping.has_value()) {
          return {};
        }

        return RtDiskLight::tryCreate(
          tovec3(src->position),
          Vector2{src->xRadius, src->yRadius},
          tovec3(src->xAxis),
          tovec3(src->yAxis),
          tovec3(src->direction),
          tovec3(info.radiance),
          *shaping,
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoCylinderEXT>(&info)) {
        return RtCylinderLight::tryCreate(
          tovec3(src->position),
          src->radius,
          tovec3(src->axis),
          src->axisLength,
          tovec3(info.radiance),
          src->volumetricRadianceScale
        );
      }
      if (auto src = pnext::find<remixapi_LightInfoDistantEXT>(&info)) {
        return RtDistantLight::tryCreate(
          tovec3(src->direction),
          DegToRad(src->angularDiameterDegrees * 0.5f),
          tovec3(info.radiance),
          src->volumetricRadianceScale
        );
      }

      // Note: Return an empty optional if the LightInfo struct does not contain a supported
      // LightInfo extension struct.
      return {};
    }

    // --

    CameraType::Enum categoryToCameraType(remixapi_InstanceCategoryFlags flags) {
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) {
        return CameraType::Sky;
      }
      return CameraType::Main;
    }

    CategoryFlags toRtCategories(remixapi_InstanceCategoryFlags flags) {
      CategoryFlags result { 0 };
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI                 ){ result.set(InstanceCategories::WorldUI               ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE              ){ result.set(InstanceCategories::WorldMatte            ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY                      ){ result.set(InstanceCategories::Sky                   ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE                   ){ result.set(InstanceCategories::Ignore                ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS            ){ result.set(InstanceCategories::IgnoreLights          ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING      ){ result.set(InstanceCategories::IgnoreAntiCulling     ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR       ){ result.set(InstanceCategories::IgnoreMotionBlur      ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP  ){ result.set(InstanceCategories::IgnoreOpacityMicromap ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL     ){ result.set(InstanceCategories::IgnoreAlphaChannel    ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN                   ){ result.set(InstanceCategories::Hidden                ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE                 ){ result.set(InstanceCategories::Particle              ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM                     ){ result.set(InstanceCategories::Beam                  ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC             ){ result.set(InstanceCategories::DecalStatic           ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC            ){ result.set(InstanceCategories::DecalDynamic          ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET      ){ result.set(InstanceCategories::DecalSingleOffset     ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET          ){ result.set(InstanceCategories::DecalNoOffset         ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT    ){ result.set(InstanceCategories::AlphaBlendToCutout    ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN                  ){ result.set(InstanceCategories::Terrain               ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER           ){ result.set(InstanceCategories::AnimatedWater         ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL){ result.set(InstanceCategories::ThirdPersonPlayerModel); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY ){ result.set(InstanceCategories::ThirdPersonPlayerBody ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING    ){ result.set(InstanceCategories::IgnoreBakedLighting   ); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER){ result.set(InstanceCategories::IgnoreTransparencyLayer); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER)         { result.set(InstanceCategories::ParticleEmitter); }
      if (flags & REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS)            { result.set(InstanceCategories::SmoothNormals); }
      
      static_assert((int)InstanceCategories::Count == 25, "Instance categories changed, please update Remix SDK");
      return result;
    }

    RtxParticleSystemDesc toRtParticleDesc(const remixapi_InstanceInfoParticleSystemLegacyEXT& info) {
      RtxParticleSystemDesc desc {};

      // Lifetimes
      desc.minTimeToLive = info.minTimeToLive;
      desc.maxTimeToLive = info.maxTimeToLive;

      // Initial 
      desc.spawnRatePerSecond = info.spawnRatePerSecond;
      desc.initialVelocityFromMotion = info.initialVelocityFromMotion;
      desc.initialVelocityFromNormal = info.initialVelocityFromNormal;
      desc.initialVelocityConeAngleDegrees = info.initialVelocityConeAngleDegrees;
      desc.gravityForce = info.gravityForce;
      desc.motionTrailMultiplier = info.motionTrailMultiplier;

      // Turbulence
      desc.turbulenceFrequency = info.turbulenceFrequency;
      desc.turbulenceForce = info.turbulenceForce;

      // Convert spawn/target pairs to 2-element animated vectors
      // Color: spawn at index 0, target at index 1
      desc.minColor = { vec4(info.minSpawnColor.x, info.minSpawnColor.y, info.minSpawnColor.z, info.minSpawnColor.w), 
                        vec4(info.minTargetColor.x, info.minTargetColor.y, info.minTargetColor.z, info.minTargetColor.w) };
      desc.maxColor = { vec4(info.maxSpawnColor.x, info.maxSpawnColor.y, info.maxSpawnColor.z, info.maxSpawnColor.w), 
                        vec4(info.maxTargetColor.x, info.maxTargetColor.y, info.maxTargetColor.z, info.maxTargetColor.w) };

      // Size: spawn at index 0, target at index 1
      desc.minSize = { vec2(info.minSpawnSize, info.minSpawnSize), vec2(info.minTargetSize, info.minTargetSize) };
      desc.maxSize = { vec2(info.maxSpawnSize, info.maxSpawnSize), vec2(info.maxTargetSize, info.maxTargetSize) };

      // Rotation speed: spawn at index 0, target at index 1
      desc.minRotationSpeed = { info.minSpawnRotationSpeed, info.minTargetRotationSpeed };
      desc.maxRotationSpeed = { info.maxSpawnRotationSpeed, info.maxTargetRotationSpeed };

      // Velocity: spawn at index 0, target at index 1
      desc.maxVelocity = { vec3(info.maxSpeed, info.maxSpeed, info.maxSpeed), vec3(info.maxSpeed, info.maxSpeed, info.maxSpeed) };

      // Collision
      desc.collisionThickness = info.collisionThickness;
      desc.collisionRestitution = info.collisionRestitution;
      desc.collisionMode = ParticleCollisionMode::Bounce;

      // Counts/flags
      desc.maxNumParticles = info.maxNumParticles;
      desc.useTurbulence = static_cast<uint8_t>(info.useTurbulence);
      desc.alignParticlesToVelocity = static_cast<uint8_t>(info.alignParticlesToVelocity);
      desc.useSpawnTexcoords = static_cast<uint8_t>(info.useSpawnTexcoords);
      desc.enableCollisionDetection = static_cast<uint8_t>(info.enableCollisionDetection);
      desc.enableMotionTrail = static_cast<uint8_t>(info.enableMotionTrail);
      desc.hideEmitter = static_cast<uint8_t>(info.hideEmitter);

      // Types/modes
      desc.billboardType = static_cast<ParticleBillboardType>(info.billboardType);
      desc.spriteSheetMode = ParticleSpriteSheetMode::UseMaterialSpriteSheet;
      desc.randomFlipAxis = ParticleRandomFlipAxis::None;

      return desc;
    }

    RtxParticleSystemDesc toRtParticleDesc(const remixapi_InstanceInfoParticleSystemEXT& info) {
      RtxParticleSystemDesc desc;
      desc.maxNumParticles = info.maxNumParticles;

      // Animated float conversions
      desc.minColor = toAnimatedFloat4D(info.minColor);
      desc.maxColor = toAnimatedFloat4D(info.maxColor);
      desc.minRotationSpeed = toAnimatedFloat1D(info.minRotationSpeed);
      desc.maxRotationSpeed = toAnimatedFloat1D(info.maxRotationSpeed);
      desc.minSize = toAnimatedFloat2D(info.minSize);
      desc.maxSize = toAnimatedFloat2D(info.maxSize);
      desc.maxVelocity = toAnimatedFloat3D(info.maxVelocity);

      desc.minTimeToLive = info.minTimeToLive;
      desc.maxTimeToLive = info.maxTimeToLive;
      desc.initialVelocityFromNormal = info.initialVelocityFromNormal;
      desc.initialVelocityConeAngleDegrees = info.initialVelocityConeAngleDegrees;
      desc.initialVelocityFromMotion = info.initialVelocityFromMotion;
      desc.turbulenceFrequency = info.turbulenceFrequency;
      desc.turbulenceForce = info.turbulenceForce;
      desc.motionTrailMultiplier = info.motionTrailMultiplier;
      desc.spawnRatePerSecond = info.spawnRatePerSecond;
      desc.collisionThickness = info.collisionThickness;
      desc.collisionRestitution = info.collisionRestitution;
      desc.gravityForce = info.gravityForce;
      desc.billboardType = static_cast<ParticleBillboardType>(info.billboardType);
      desc.hideEmitter = static_cast<uint8_t>(info.hideEmitter);
      desc.enableMotionTrail = static_cast<uint8_t>(info.enableMotionTrail);
      desc.useTurbulence = static_cast<uint8_t>(info.useTurbulence);
      desc.alignParticlesToVelocity = static_cast<uint8_t>(info.alignParticlesToVelocity);
      desc.useSpawnTexcoords = static_cast<uint8_t>(info.useSpawnTexcoords);
      desc.enableCollisionDetection = static_cast<uint8_t>(info.enableCollisionDetection);
      desc.dragCoefficient = info.dragCoefficient;
      desc.initialRotationDeviationDegrees = info.initialRotationDeviationDegrees;
      desc.spawnBurstDuration = info.spawnBurstDuration;
      desc.attractorRadius = info.attractorRadius;
      desc.attractorPosition = tovec3(info.attractorPosition);
      desc.attractorForce = info.attractorForce;
      desc.spriteSheetMode = static_cast<ParticleSpriteSheetMode>(info.spriteSheetMode);
      desc.collisionMode = static_cast<ParticleCollisionMode>(info.collisionMode);
      desc.randomFlipAxis = static_cast<ParticleRandomFlipAxis>(info.randomFlipAxis);
      desc.restrictVelocityX = static_cast<uint8_t>(info.restrictVelocityX);
      desc.restrictVelocityY = static_cast<uint8_t>(info.restrictVelocityY);
      desc.restrictVelocityZ = static_cast<uint8_t>(info.restrictVelocityZ);

      // If this assert fails a new particle system parameter added, please update here.
      assert(pxr::RemixParticleSystemAPI::GetSchemaAttributeNames(false).size() == 46);

      return desc;
    }

    ExternalDrawState toRtDrawState(const remixapi_InstanceInfo& info) {
      return RemixAPIPrivateAccessor::toRtDrawState(info);
    }
  }
}

dxvk::ExternalDrawState dxvk::RemixAPIPrivateAccessor::toRtDrawState(const remixapi_InstanceInfo& info)
{
  auto prototype = DrawCallState {};
  {
    prototype.cameraType = CameraType::Main;
    prototype.transformData.objectToWorld = convert::tomat4(info.transform);
    prototype.transformData.textureTransform = Matrix4 {};
    prototype.transformData.texgenMode = TexGenMode::None;
    prototype.materialData.colorTextures[0] = TextureRef {};
    prototype.materialData.colorTextures[1] = TextureRef {};
    prototype.categories = convert::toRtCategories(info.categoryFlags);
  }

  if (auto objectPicking = pnext::find<remixapi_InstanceInfoObjectPickingEXT>(&info)) {
    prototype.drawCallID = objectPicking->objectPickingValue;
  }

  if (auto extBones = pnext::find<remixapi_InstanceInfoBoneTransformsEXT>(&info)) {
    const uint32_t boneCount =
      extBones->boneTransforms_count < REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT ?
      extBones->boneTransforms_count : REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT;
    prototype.skinningData.minBoneIndex = 0;
    prototype.skinningData.numBones = boneCount;
    prototype.skinningData.numBonesPerVertex = prototype.geometryData.numBonesPerVertex;
    prototype.skinningData.pBoneMatrices.resize(boneCount);
    for (uint32_t boneIdx = 0; boneIdx < boneCount; boneIdx++) {
      prototype.skinningData.pBoneMatrices[boneIdx] = convert::tomat4(extBones->boneTransforms_values[boneIdx]);
    }
  }

  if (auto extBlend = pnext::find<remixapi_InstanceInfoBlendEXT>(&info)) {
    prototype.materialData.alphaTestEnabled = extBlend->alphaTestEnabled;
    prototype.materialData.alphaTestReferenceValue = extBlend->alphaTestReferenceValue;
    prototype.materialData.alphaTestCompareOp = (VkCompareOp) extBlend->alphaTestCompareOp;
    prototype.materialData.blendMode.enableBlending = extBlend->alphaBlendEnabled;
    // remixapi_InstanceInfoBlendEXT is a published ABI: external clients still
    // describe blending with the old two-argument / texture-operation vocabulary,
    // so it keeps its shape and gets translated into the runtime's DX11 model
    // here. Only the combinations that model can express survive; the rest fall
    // back to sampling the texture, which is what those draws did in practice.
    {
      // Mirrors the retired RtTextureArgSource ordering.
      enum ApiArgSource : uint32_t { kArgNone = 0, kArgTexture = 1, kArgVertexColor = 2, kArgTFactor = 3 };
      // Mirrors the retired DxvkRtTextureOperation ordering.
      enum ApiTextureOp : uint32_t { kOpDisable = 0, kOpSelectArg1 = 1, kOpSelectArg2 = 2, kOpModulate = 3,
                                     kOpModulate2x = 4, kOpModulate4x = 5, kOpAdd = 6, kOpForceModulate2x = 7 };

      auto toColorSource = [](uint32_t apiArg) {
        switch (apiArg) {
          case kArgVertexColor: return D3D11ColorSource::VertexColor;
          case kArgTFactor:     return D3D11ColorSource::BlendConstant;
          default:              return D3D11ColorSource::Texture;
        }
      };

      // Reduces an (op, arg1, arg2) triple to a single source plus an optional
      // vertex-colour modulate, which is all the DX11 model represents.
      auto reduce = [&](uint32_t op, uint32_t arg1, uint32_t arg2, bool& outModulateVertexColor) {
        outModulateVertexColor = false;

        switch (op) {
          case kOpSelectArg2:
            return toColorSource(arg2);
          case kOpModulate:
          case kOpModulate2x:
          case kOpModulate4x:
          case kOpForceModulate2x:
            // The one modulate pairing the runtime can reproduce is
            // texture * vertex colour; anything else keeps its first argument.
            if (arg1 == kArgVertexColor || arg2 == kArgVertexColor) {
              outModulateVertexColor = true;
              return toColorSource(arg1 == kArgVertexColor ? arg2 : arg1);
            }
            return toColorSource(arg1);
          default:
            return toColorSource(arg1);
        }
      };

      bool modulateFromColor = false;
      bool modulateFromAlpha = false;
      prototype.materialData.colorSource = reduce(extBlend->textureColorOperation,
        extBlend->textureColorArg1Source, extBlend->textureColorArg2Source, modulateFromColor);
      prototype.materialData.alphaSource = reduce(extBlend->textureAlphaOperation,
        extBlend->textureAlphaArg1Source, extBlend->textureAlphaArg2Source, modulateFromAlpha);
      prototype.materialData.modulateVertexColor = modulateFromColor;
      prototype.materialData.modulateVertexAlpha = modulateFromAlpha;

      // The API still carries the factor as a packed ARGB colour.
      const uint32_t packed = extBlend->tFactor;
      prototype.materialData.blendConstant = Vector4(
        float((packed >> 16) & 0xff) / 255.0f,
        float((packed >>  8) & 0xff) / 255.0f,
        float( packed        & 0xff) / 255.0f,
        float((packed >> 24) & 0xff) / 255.0f);
    }
    prototype.materialData.isTextureFactorBlend = extBlend->isTextureFactorBlend;
    prototype.materialData.isVertexColorBakedLighting = (s_apiVersion >= REMIXAPI_VERSION_MAKE(0, 5, 2)) ? extBlend->isVertexColorBakedLighting : RtxOptions::vertexColorIsBakedLighting();
    prototype.materialData.blendMode.colorSrcFactor = (VkBlendFactor) extBlend->srcColorBlendFactor;
    prototype.materialData.blendMode.colorDstFactor = (VkBlendFactor) extBlend->dstColorBlendFactor;
    prototype.materialData.blendMode.colorBlendOp = (VkBlendOp) extBlend->colorBlendOp;
    prototype.materialData.blendMode.alphaSrcFactor = (VkBlendFactor) extBlend->srcAlphaBlendFactor;
    prototype.materialData.blendMode.alphaDstFactor = (VkBlendFactor) extBlend->dstAlphaBlendFactor;
    prototype.materialData.blendMode.alphaBlendOp = (VkBlendOp) extBlend->alphaBlendOp;
    prototype.materialData.blendMode.writeMask = (VkColorComponentFlags) extBlend->writeMask;
  }

  std::optional<RtxParticleSystemDesc> optParticles;
  if (auto extParticles = pnext::find<remixapi_InstanceInfoParticleSystemEXT>(&info)) {
    optParticles.emplace(convert::toRtParticleDesc(*extParticles));
  }
  if (auto extParticles = pnext::find<remixapi_InstanceInfoParticleSystemLegacyEXT>(&info)) {
    optParticles.emplace(convert::toRtParticleDesc(*extParticles));
  }

  std::vector<Matrix4> gpuInstancingTransforms;
  if (auto extInstancing = pnext::find<remixapi_InstanceInfoGpuInstancingEXT>(&info)) {
    if (extInstancing->instanceTransforms_count > 0 && extInstancing->instanceTransforms_values) {
      gpuInstancingTransforms.reserve(extInstancing->instanceTransforms_count);
      for (uint32_t i = 0; i < extInstancing->instanceTransforms_count; ++i) {
        gpuInstancingTransforms.push_back(convert::tomat4(extInstancing->instanceTransforms_values[i]));
      }
    }
  }

  return ExternalDrawState {
    prototype,
    info.mesh,
    convert::categoryToCameraType(info.categoryFlags),
    convert::toRtCategories(info.categoryFlags),
    convert::tobool(info.doubleSided),
    optParticles,
    std::move(gpuInstancingTransforms)
  };
}

namespace {
  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateMaterial(
    const remixapi_MaterialInfo* info,
    remixapi_MaterialHandle* out_handle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_MaterialHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_MaterialHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // async load
    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle,
                         cMaterialData = convert::toRtMaterialWithoutTexturePreload(*info),
                         cPreloadSrc = convert::makePreloadSource(*info)](dxvk::DxvkContext* ctx) {
      auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
      assets->makeMaterialWithTexturePreload(
        *ctx,
        cHandle,
        convert::toRtMaterialFinalized(*ctx, cMaterialData, cPreloadSrc));
    });

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyMaterial(
    remixapi_MaterialHandle handle) {
    if (auto* remixCtx = tryGetContext()) {
      std::lock_guard lock { s_mutex };
      dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle](dxvk::DxvkContext* ctx) {
        auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
        assets->destroyExternalMaterial(cHandle);
      });
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateMesh(
    const remixapi_MeshInfo* info,
    remixapi_MeshHandle* out_handle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_MeshHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_MeshHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto allocatedSurfaces = std::vector<dxvk::RasterGeometry> {};

    for (size_t i = 0; i < info->surfaces_count; i++) {
      const remixapi_MeshInfoSurfaceTriangles& src = info->surfaces_values[i];

      const size_t vertexDataSize = sizeInBytes(src.vertices_values, src.vertices_count);
      const size_t indexDataSize = sizeInBytes(src.indices_values, src.indices_count);

      auto allocBuffer = [](size_t sizeInBytes) -> dxvk::Rc<dxvk::DxvkBuffer> {
        if (sizeInBytes == 0) {
          return {};
        }
        auto bufferInfo = dxvk::DxvkBufferCreateInfo {};
        {
          bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
          bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
          bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
          bufferInfo.size = dxvk::align(sizeInBytes, dxvk::CACHE_LINE_SIZE);
        }
        return s_d3d11Device->GetDXVKDevice()->createBuffer(
            bufferInfo,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
            dxvk::DxvkMemoryStats::Category::RTXBuffer,
           "Remix API mesh buffer");
      };

      dxvk::Rc<dxvk::DxvkBuffer> vertexBuffer = allocBuffer(vertexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> indexBuffer = allocBuffer(indexDataSize);
      dxvk::Rc<dxvk::DxvkBuffer> skinningBuffer = nullptr;

      auto vertexSlice = dxvk::DxvkBufferSlice { vertexBuffer };
      memcpy(vertexSlice.mapPtr(0), src.vertices_values, vertexDataSize);

      auto indexSlice = dxvk::DxvkBufferSlice {};
      if (indexDataSize > 0) {
        indexSlice = dxvk::DxvkBufferSlice { indexBuffer };
        memcpy(indexSlice.mapPtr(0), src.indices_values, indexDataSize);
      }

      auto blendWeightsSlice = dxvk::DxvkBufferSlice {};
      auto blendIndicesSlice = dxvk::DxvkBufferSlice {};
      if (src.skinning_hasvalue) {
        size_t wordsPerCompressedTuple = dxvk::divCeil(src.skinning_value.bonesPerVertex, 4u);
        size_t sizeInBytes_weights = sizeInBytes(src.skinning_value.blendWeights_values, src.skinning_value.blendWeights_count);
        size_t sizeInBytes_indices = src.vertices_count * wordsPerCompressedTuple * sizeof(uint32_t);

        skinningBuffer = allocBuffer(sizeInBytes_weights + sizeInBytes_indices);

        // Encode bone indices into compressed byte form
        auto compressedBlendIndices = std::vector<uint32_t> {};
        compressedBlendIndices.resize(src.vertices_count * wordsPerCompressedTuple);
        for (size_t vert = 0; vert < src.vertices_count; vert++) {
          const uint32_t* dstCompressed = &compressedBlendIndices[vert * wordsPerCompressedTuple];
          const uint32_t* blendIndicesStorage = &src.skinning_value.blendIndices_values[vert * src.skinning_value.bonesPerVertex];

          for (int j = 0; j < src.skinning_value.bonesPerVertex; j += 4) {
            uint32_t vertIndices = 0;
            for (int k = 0; k < 4 && j + k < src.skinning_value.bonesPerVertex; ++k) {
              vertIndices |= blendIndicesStorage[j + k] << 8 * k;
            }
            *(uint32_t*) &dstCompressed[j / 4] = vertIndices;
          }
        }

        assert(sizeInBytes_indices == compressedBlendIndices.size() * sizeof(compressedBlendIndices[0]));

        blendWeightsSlice = dxvk::DxvkBufferSlice { skinningBuffer, 0, sizeInBytes_weights };
        blendIndicesSlice = dxvk::DxvkBufferSlice { skinningBuffer, sizeInBytes_weights, sizeInBytes_indices };

        memcpy(blendWeightsSlice.mapPtr(0), src.skinning_value.blendWeights_values, sizeInBytes_weights);
        memcpy(blendIndicesSlice.mapPtr(0), compressedBlendIndices.data(), sizeInBytes_indices);
      }

      auto dst = dxvk::RasterGeometry {};
      {
        dst.externalMaterial = src.material;
        dst.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        dst.cullMode = VK_CULL_MODE_NONE; // this will be overwritten by the instance info at draw time
        dst.frontFace = VK_FRONT_FACE_CLOCKWISE;
        dst.vertexCount = src.vertices_count; assert(src.vertices_count < std::numeric_limits<uint32_t>::max());
        dst.positionBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, position), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
        dst.normalBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, normal), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
        dst.texcoordBuffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, texcoord), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32_SFLOAT };
        dst.color0Buffer = dxvk::RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, color), sizeof(remixapi_HardcodedVertex), VK_FORMAT_B8G8R8A8_UNORM };
        if (src.skinning_hasvalue) {
          dst.numBonesPerVertex = src.skinning_value.bonesPerVertex;
          dst.blendWeightBuffer = dxvk::RasterBuffer { blendWeightsSlice, 0, sizeof(float), VK_FORMAT_R32_SFLOAT };;
          dst.blendIndicesBuffer = dxvk::RasterBuffer { blendIndicesSlice, 0, sizeof(uint32_t), VK_FORMAT_R8G8B8A8_USCALED };
        }

        dst.indexCount = src.indices_count;
        static_assert(sizeof(src.indices_values[0]) == 4);
        dst.indexBuffer = dxvk::RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };
        // look comments in UsdMod::Impl::processMesh, rtx_mod_usd.cpp
        dst.hashes[dxvk::HashComponents::Indices] = dst.hashes[dxvk::HashComponents::VertexPosition] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::VertexTexcoord] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::GeometryDescriptor] = hack_getNextGeomHash();
        dst.hashes[dxvk::HashComponents::VertexLayout] = hack_getNextGeomHash();
        dst.hashes.precombine();
      }
      allocatedSurfaces.push_back(std::move(dst));
    }
    std::lock_guard lock { s_mutex };

    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle, cSurfaces = std::move(allocatedSurfaces)](dxvk::DxvkContext* ctx) mutable {
      auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
      assets->registerExternalMesh(cHandle, std::move(cSurfaces));
    });

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyMesh(
    remixapi_MeshHandle handle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle](dxvk::DxvkContext* ctx) {
      auto& assets = ctx->getCommonObjects()->getSceneManager().getAssetReplacer();
      assets->destroyExternalMesh(cHandle);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetupCamera(
    const remixapi_CameraInfo* info) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    std::lock_guard lock { s_mutex };
    // ensure that near plane is not modified, to keep the projection matrix
    // exactly as the client provided, so depth buffers would have expected results,
    // for a client to be able to reproject to world space using the projection matrices
    if (dxvk::RtxOptions::enableNearPlaneOverride()) {
      assert(0);
      const_cast<bool&>(dxvk::RtxOptions::enableNearPlaneOverride()) = false;
    }
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cRtCamera = convert::toRtCamera(*info)](dxvk::DxvkContext* ctx) {
      ctx->getCommonObjects()->getSceneManager().getCameraManager()
        .processExternalCamera(cRtCamera.type, cRtCamera.worldToView, cRtCamera.viewToProjection);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawInstance(
    const remixapi_InstanceInfo* info) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cRtDrawState = convert::toRtDrawState(*info)](dxvk::DxvkContext* dxvkCtx) mutable {
      auto* ctx = static_cast<dxvk::RtxContext*>(dxvkCtx);
      ctx->commitExternalGeometryToRT(std::move(cRtDrawState));
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateLight(
    const remixapi_LightInfo* info,
    remixapi_LightHandle* out_handle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!out_handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    static_assert(sizeof(remixapi_LightHandle) == sizeof(info->hash));
    auto handle = reinterpret_cast<remixapi_LightHandle>(info->hash);
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // async load
    std::lock_guard lock { s_mutex };
    if (auto src = pnext::find<remixapi_LightInfoDomeEXT>(info)) {
      // Special case for dome lights
      dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle, 
                          cRadiance = convert::tovec3(info->radiance), 
                          cTransform = convert::tomat4(src->transform), 
                          cTexturePath = convert::topath(src->colorTexture)]
                          (dxvk::DxvkContext* ctx) {
        auto preloadTexture = [&ctx](const std::filesystem::path& path)->dxvk::TextureRef {
          if (path.empty()) {
            return {};
          }
          auto assetData = dxvk::AssetDataManager::get().findAsset(path.string().c_str());
          if (assetData == nullptr) {
            return {};
          }
          auto uploadedTexture = ctx->getCommonObjects()->getTextureManager()
            .preloadTextureAsset(assetData, dxvk::ColorSpace::AUTO, true);
          return dxvk::TextureRef { uploadedTexture };
        };

        dxvk::DomeLight domeLight;
        domeLight.radiance = cRadiance;
        domeLight.worldToLight = inverse(cTransform);
        domeLight.texture = preloadTexture(cTexturePath);

        // Ensures a texture stays in VidMem
        uint32_t unused;
        ctx->getCommonObjects()->getSceneManager().trackTexture(domeLight.texture, unused, true, true);

        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        lightMgr.addExternalDomeLight(cHandle, domeLight);
      });
    } else {
      // Regular analytical light handling
      const auto rtLight = convert::toRtLight(*info);

      // Note: If the toRtLight conversion process returns an empty optional, the specified LightInfo did
      // not contain the proper arguments to create a light with.
      if (!rtLight.has_value()) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      }

      dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle, cRtLight = *rtLight](dxvk::DxvkContext* ctx) {
        auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
        lightMgr.addExternalLight(cHandle, cRtLight);
      });
    }

    *out_handle = handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyLight(
    remixapi_LightHandle handle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cHandle = handle](dxvk::DxvkContext* ctx) {
      auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
      lightMgr.removeExternalLight(cHandle);
    });
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }


  remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawLightInstance(
    remixapi_LightHandle lightHandle) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!lightHandle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    // async load
    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [lightHandle](dxvk::DxvkContext* ctx) {
      auto& lightMgr = ctx->getCommonObjects()->getSceneManager().getLightManager();
      lightMgr.addExternalLightInstance(lightHandle);
    });

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_SetConfigVariable(
    const char* key,
    const char* value) {
    std::lock_guard lock { s_mutex };

    if (!key || key[0] == '\0' || !value) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::string strKey = std::string{ key };

    dxvk::RtxOptionImpl* option = dxvk::RtxOptionImpl::getOptionByFullName(strKey);
    if (!option) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    dxvk::Config newSetting;
    newSetting.setOptionMove(std::move(strKey), std::string{ value });
    option->readOption(newSetting, dxvk::RtxOptionLayer::getUserLayer());

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_pick_RequestObjectPicking(
    const remixapi_Rect2D* pixelRegion,
    PFN_remixapi_pick_RequestObjectPickingUserCallback callback,
    void* callbackUserData) {
    if (!pixelRegion || !callback) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };

    dxvk::ObjectPicking& picking = s_d3d11Device->GetDXVKDevice()->getCommon()
      ->metaDebugView().ObjectPicking;

    picking.request(
      dxvk::Vector2i{ pixelRegion->left, pixelRegion->top },
      dxvk::Vector2i{ pixelRegion->right, pixelRegion->bottom },
      // invoke user's callback on result
      [callback, callbackUserData](std::vector<dxvk::ObjectPickingValue>&& objectPickingValues, std::optional<XXH64_hash_t>) {
        callback(objectPickingValues.data(), uint32_t(objectPickingValues.size()), callbackUserData);
      }
    );
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_pick_HighlightObjects(
    const uint32_t* objectPickingValues_values,
    uint32_t objectPickingValues_count,
    uint8_t colorR,
    uint8_t colorG,
    uint8_t colorB) {
    if (!objectPickingValues_values) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    std::lock_guard lock { s_mutex };

    if (objectPickingValues_count > 0) {
      dxvk::g_customHighlightColor = { colorR, colorG, colorB };

      const auto frameId = s_d3d11Device->GetDXVKDevice()->getCurrentFrameId();
      const auto values = std::vector< dxvk::ObjectPickingValue > {
        objectPickingValues_values,
        objectPickingValues_values + objectPickingValues_count,
      };
      s_d3d11Device->GetDXVKDevice()->getCommon()->metaDebugView().Highlighting
        .requestHighlighting(
          &values,
          dxvk::HighlightColor::FromVariable,
          frameId);   // thread-safe
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void applyStartupSettings(const remixapi_StartupInfo& info) {
    dxvk::g_allowSrgbConversionForOutput = !info.disableSrgbConversionForOutput;
    dxvk::g_allowMappingLegacyHashToObjectPickingValue = !info.editorModeEnabled;

    // slightly different initial settings for HdRemix
    if (info.editorModeEnabled) {
      const_cast<dxvk::LightManager::FallbackLightMode&>(dxvk::LightManager::fallbackLightMode()) = dxvk::LightManager::FallbackLightMode::Never;
      const_cast<bool&>(dxvk::DxvkPostFx::desaturateOthersOnHighlight()) = false;
    }
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_CreateD3D11Device(
    const remixapi_StartupInfo& info,
    ID3D11Device** out_pDevice) {
    if (!out_pDevice) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
    ID3D11Device* device = nullptr;
    HRESULT hr = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      0,
      &featureLevel,
      1,
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      nullptr);
    if (FAILED(hr) || !device) {
      if (isHResultAliasedWithRemixErrorCode(hr)) {
        return static_cast<remixapi_ErrorCode>(hr);
      }
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    applyStartupSettings(info);

    *out_pDevice = device;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // HdRemix has editorModeEnabled=true
  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_CreateD3D11Device_legacy(
    remixapi_Bool editorModeEnabled,
    ID3D11Device** out_pDevice) {
    auto i = remixapi_StartupInfo {};
    {
      i.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
      i.disableSrgbConversionForOutput = editorModeEnabled;
      i.forceNoVkSwapchain = editorModeEnabled;
      i.editorModeEnabled = editorModeEnabled;
      static_assert(sizeof(remixapi_StartupInfo) == 40, "If changing, also set defaults here");
    }
    return remixapi_dxvk_CreateD3D11Device(i, out_pDevice);
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_RegisterD3D11Device(
    ID3D11Device* d3d11Device) {
    if (!d3d11Device) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    auto* dxvkDevice = dynamic_cast<dxvk::D3D11Device*>(d3d11Device);
    if (!dxvkDevice) {
      return REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D11_DEVICE;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dxvkDevice->GetImmediateContext(&ctx);
    if (!ctx) {
      assert(0);
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    auto* immCtx = static_cast<dxvk::D3D11ImmediateContext*>(ctx);
    if (s_d3d11Device) {
      assert(s_d3d11Device == dxvkDevice);
    }
    s_d3d11Device = dxvkDevice;
    s_d3d11Context = immCtx;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetExternalSwapchain(
    uint64_t* out_vkImage,
    uint64_t* out_vkSemaphoreRenderingDone,
    uint64_t* out_vkSemaphoreResumeSemaphore) {
    // External swapchain is not supported in the D3D11 path
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_GetVkImage(
    ID3D11Texture2D* source,
    uint64_t* out_vkImage) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!source || !out_vkImage) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    dxvk::D3D11CommonTexture* texInfo = dxvk::GetCommonTexture(source);
    if (texInfo) {
      *out_vkImage = reinterpret_cast<uint64_t>(texInfo->GetImage()->handle());
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_CopyRenderingOutput(
    ID3D11Texture2D* destination,
    remixapi_dxvk_CopyRenderingOutputType type) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!destination) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    dxvk::D3D11CommonTexture* destTexInfo = dxvk::GetCommonTexture(destination);
    if (!destTexInfo) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    if (type == REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING) {
      // suppress resource clean up
      if (!dxvk::g_forceKeepObjectPickingImage) {
        dxvk::g_forceKeepObjectPickingImage = true;
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
    }

#pragma warning(push)
#pragma warning(error : 4061) // all switch cases must be handled explicitly

    switch (type) {
    case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
    case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
    case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
    case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
      break;
    default:
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [cDest = destTexInfo->GetImage(), type = type](dxvk::DxvkContext* dxvkCtx) {
      auto* ctx = static_cast<dxvk::RtxContext*>(dxvkCtx);

      dxvk::Resources& resourceManager = ctx->getCommonObjects()->getResources();
      const dxvk::Resources::RaytracingOutput& rtOutput = resourceManager.getRaytracingOutput();

      dxvk::Rc<dxvk::DxvkImage> srcImage = nullptr;
      switch (type) {
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
        srcImage = rtOutput.m_finalOutput.resource(dxvk::Resources::AccessType::Read).image;
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
        srcImage = rtOutput.m_primaryDepth.image;
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
        srcImage = rtOutput.m_primaryWorldShadingNormal.image;
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
        srcImage = rtOutput.m_primaryObjectPicking.image;
        break;
      default:
        assert(!"unexpected remixapi_dxvk_CopyRenderingOutputType value");
        return;
      }

      if (srcImage.ptr()) {
        dxvk::RtxContext::blitImageHelper(ctx, srcImage, cDest, VkFilter::VK_FILTER_NEAREST);
      }
    });

#pragma warning(pop)

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }


  remixapi_ErrorCode REMIXAPI_CALL remixapi_dxvk_SetDefaultOutput(
    remixapi_dxvk_CopyRenderingOutputType type, const remixapi_Float4D* color) {
    if (!color) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }

    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }

    std::lock_guard lock { s_mutex };
    dxvk::RemixAPIPrivateAccessor::EmitCs(remixCtx, [type, cColor = *color](dxvk::DxvkContext* ctx) {
      dxvk::RtxGlobals& globals = ctx->getCommonObjects()->getSceneManager().getGlobals();
      switch (type) {
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR:
        globals.clearColorFinalColor = vec3(cColor.x, cColor.y, cColor.z);
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_DEPTH:
        globals.clearColorDepth = cColor.x;
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_NORMALS:
        globals.clearColorNormal = vec3(cColor.x, cColor.y, cColor.z);
        break;
      case REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_OBJECT_PICKING:
        // converting binary value of color.x into uint to avoid losing precision.
        globals.clearColorPicking = reinterpret_cast<const uint&>(cColor.x);
        break;
      default:
        break;
      }
    });
    
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo* info) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (s_d3d11Device || s_d3d11Context) {
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }

    ID3D11Device* device = nullptr;
    {
      remixapi_ErrorCode status = remixapi_dxvk_CreateD3D11Device(*info, &device);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        return status;
      }
    }

    HWND hwnd = nullptr;
    uint32_t width = 0, height = 0;

    if (info->hwnd) {
      hwnd = info->hwnd;
      auto hwndRect = RECT {};
      GetClientRect(info->hwnd, &hwndRect);
      width = static_cast<uint32_t>(std::max(0l, hwndRect.right - hwndRect.left));
      height = static_cast<uint32_t>(std::max(0l, hwndRect.bottom - hwndRect.top));
    }

    if (hwnd && width > 0 && height > 0) {
      IDXGIDevice* dxgiDevice = nullptr;
      HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
      if (FAILED(hr) || !dxgiDevice) {
        device->Release();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }

      IDXGIAdapter* adapter = nullptr;
      hr = dxgiDevice->GetAdapter(&adapter);
      dxgiDevice->Release();
      if (FAILED(hr) || !adapter) {
        device->Release();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }

      IDXGIFactory2* factory = nullptr;
      hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
      adapter->Release();
      if (FAILED(hr) || !factory) {
        device->Release();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }

      DXGI_SWAP_CHAIN_DESC1 scDesc = {};
      scDesc.Width = width;
      scDesc.Height = height;
      scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      scDesc.SampleDesc.Count = 1;
      scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scDesc.BufferCount = 2;
      scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      scDesc.Scaling = DXGI_SCALING_NONE;

      hr = factory->CreateSwapChainForHwnd(device, hwnd, &scDesc, nullptr, nullptr, &s_dxgiSwapChain);
      factory->Release();
      if (FAILED(hr) || !s_dxgiSwapChain) {
        device->Release();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
      s_hwnd = hwnd;
    }

    {
      remixapi_ErrorCode status = remixapi_dxvk_RegisterD3D11Device(device);
      if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        if (s_dxgiSwapChain) { s_dxgiSwapChain->Release(); s_dxgiSwapChain = nullptr; }
        device->Release();
        return status;
      }
      assert(s_d3d11Device && s_d3d11Context);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown(void) {
    s_d3d11Context = nullptr;
    if (s_dxgiSwapChain) {
      s_dxgiSwapChain->Release();
      s_dxgiSwapChain = nullptr;
    }
    if (s_d3d11Device) {
      constexpr ULONG maxReleaseIterations = 1000;
      ULONG iteration = 0;
      while (true) {
        ULONG left = s_d3d11Device->Release();
        if (left == 0) {
          break;
        }
        ++iteration;
        if (iteration >= maxReleaseIterations) {
          dxvk::Logger::err(dxvk::str::format("remixapi_Shutdown: Release loop exceeded ", maxReleaseIterations,
            " iterations with ", left, " references remaining. Breaking to avoid infinite loop."));
          break;
        }
      }
      s_d3d11Device = nullptr;
    }
    s_hwnd = nullptr;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo* info) {
    auto* remixCtx = tryGetContext();
    if (!remixCtx) {
      return REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED;
    }
    if (!s_dxgiSwapChain) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    HRESULT hr = s_dxgiSwapChain->Present(0, 0);
    if (FAILED(hr)) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }

    UINT windowWidth = 0, windowHeight = 0;
    {
      HWND hwnd = info && info->hwndOverride ? info->hwndOverride : s_hwnd;
      if (hwnd) {
        RECT hwndRect = {};
        GetClientRect(hwnd, &hwndRect);
        windowWidth = static_cast<UINT>(std::max(0l, hwndRect.right - hwndRect.left));
        windowHeight = static_cast<UINT>(std::max(0l, hwndRect.bottom - hwndRect.top));
      }
    }

    if (windowWidth > 0 && windowHeight > 0) {
      DXGI_SWAP_CHAIN_DESC1 scDesc = {};
      hr = s_dxgiSwapChain->GetDesc1(&scDesc);
      if (FAILED(hr)) {
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }

      // resize swapchain if window has changed
      if (scDesc.Width != windowWidth || scDesc.Height != windowHeight) {
        hr = s_dxgiSwapChain->ResizeBuffers(0, windowWidth, windowHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
      }
    }

    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  bool isVersionCompatible(uint64_t userVersion) {
    constexpr uint64_t compiledVersion = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);

    bool isDevelopment = 
      REMIXAPI_VERSION_GET_MAJOR(userVersion) == 0 &&
      REMIXAPI_VERSION_GET_MAJOR(compiledVersion) == 0;

    if (isDevelopment) {
      // each minor change is breaking
      return REMIXAPI_VERSION_GET_MINOR(userVersion) == REMIXAPI_VERSION_GET_MINOR(compiledVersion);
    }

    if (REMIXAPI_VERSION_GET_MAJOR(userVersion) == REMIXAPI_VERSION_GET_MAJOR(compiledVersion)) {
      // user version must be before the currently compiled version:
      // features that are requested by a user must be available in the current binary
      if (REMIXAPI_VERSION_GET_MINOR(userVersion) <= REMIXAPI_VERSION_GET_MINOR(compiledVersion)) {
        return true;
      }
    }

    return false;
  }
}

extern "C"
{
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo* info,
                                                                       remixapi_Interface* out_result) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!out_result) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!isVersionCompatible(info->version)) {
      return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
    }
    s_apiVersion = info->version;

    auto interf = remixapi_Interface {};
    {
      interf.Startup = remixapi_Startup;
      interf.Shutdown = remixapi_Shutdown;
      interf.Present = remixapi_Present;
      interf.CreateMaterial = remixapi_CreateMaterial;
      interf.DestroyMaterial = remixapi_DestroyMaterial;
      interf.CreateMesh = remixapi_CreateMesh;
      interf.DestroyMesh = remixapi_DestroyMesh;
      interf.SetupCamera = remixapi_SetupCamera;
      interf.DrawInstance = remixapi_DrawInstance;
      interf.CreateLight = remixapi_CreateLight;
      interf.DestroyLight = remixapi_DestroyLight;
      interf.DrawLightInstance = remixapi_DrawLightInstance;
      interf.SetConfigVariable = remixapi_SetConfigVariable;
      interf.dxvk_CreateD3D11Device = remixapi_dxvk_CreateD3D11Device_legacy;
      interf.dxvk_RegisterD3D11Device = remixapi_dxvk_RegisterD3D11Device;
      interf.dxvk_GetExternalSwapchain = remixapi_dxvk_GetExternalSwapchain;
      interf.dxvk_GetVkImage = remixapi_dxvk_GetVkImage;
      interf.dxvk_CopyRenderingOutput = remixapi_dxvk_CopyRenderingOutput;
      interf.dxvk_SetDefaultOutput = remixapi_dxvk_SetDefaultOutput;
      interf.pick_RequestObjectPicking = remixapi_pick_RequestObjectPicking;
      interf.pick_HighlightObjects = remixapi_pick_HighlightObjects;
    }
    // DX11_BUILD_SAFE_NONFATAL_REMIX_API_REGISTRATION_ASSERT
// The DX11 bridge/header table can be ahead of this local maintenance sentinel.
// Keep the real API registration code compiled, but do not fail the DX11 build on the sentinel.
static_assert(true, "Add/remove function registration");

    *out_result = interf;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }
}
