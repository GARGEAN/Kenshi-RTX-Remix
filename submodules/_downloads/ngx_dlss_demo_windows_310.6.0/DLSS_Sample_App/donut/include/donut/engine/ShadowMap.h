/*
 * SPDX-FileCopyrightText: Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */
#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

struct ShadowConstants;

namespace donut::engine
{
    class IShadowMap
    {
    public:
        virtual dm::float4x4 GetWorldToUvzwMatrix() const = 0;
        virtual const class ICompositeView& GetView() const = 0;
        virtual nvrhi::ITexture* GetTexture() const = 0;
        virtual uint32_t GetNumberOfCascades() const = 0;
        virtual const IShadowMap* GetCascade(uint32_t index) const = 0;
        virtual uint32_t GetNumberOfPerObjectShadows() const = 0;
        virtual const IShadowMap* GetPerObjectShadow(uint32_t index) const = 0;
        virtual dm::int2 GetTextureSize() const = 0;
        virtual dm::box2 GetUVRange() const = 0;
        virtual dm::float2 GetFadeRangeInTexels() const = 0;
        virtual bool IsLitOutOfBounds() const = 0;
        virtual void FillShadowConstants(ShadowConstants& constants) const = 0;
    };
}