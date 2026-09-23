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

#ifndef MATERIAL_CB_H
#define MATERIAL_CB_H

static const int SpecularType_None = 0;
static const int SpecularType_Scalar = 1;
static const int SpecularType_Color = 2;
static const int SpecularType_MetalRough = 3;

struct MaterialConstants
{
    float3  diffuseColor;
    int     useDiffuseTexture;

    float3  specularColor;
    int     specularTextureType;

    float3  emissiveColor;
    int     useEmissiveTexture;

    float   roughness;
    float   opacity;
    int     useNormalsTexture;
    int     materialID;
};

#endif // MATERIAL_CB_H