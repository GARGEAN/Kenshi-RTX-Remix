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

#ifndef VOXELIZATION_CB_H
#define VOXELIZATION_CB_H

#include <donut/shaders/light_cb.h>

#define FORWARD_MAX_LIGHTS 16
#define FORWARD_MAX_SHADOWS 16

struct VoxelizationViewConstants
{
    float4x4    matWorldToClip;
};

struct VoxelizationLightConstants
{
    float2      shadowMapTextureSize;
    float2      shadowMapTextureSizeInv;
    float4      ambientColorTop;
    float4      ambientColorBottom;

    uint3       padding;
    uint        numLights;

    LightConstants lights[FORWARD_MAX_LIGHTS];
    ShadowConstants shadows[FORWARD_MAX_SHADOWS];
};

#endif // VOXELIZATION_CB_H