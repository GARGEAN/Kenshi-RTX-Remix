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

#ifndef SSAO_CB_H
#define SSAO_CB_H

struct SsaoConstants
{
    float4x4    matClipToView;
    float4x4    matWorldToView;

    float2      randomOffset;
    float2      windowToClipScale;

    float2      windowToClipBias;
    float2      clipToWindowScale;

    float2      clipToWindowBias;
    float2      radiusWorldToClip;

    float       amount;
    float       invBackgroundViewDepth;
    float       radiusWorld;
    float       surfaceBias;

    float3      padding;
    float       powerExponent;
};

#endif // SSAO_CB_H