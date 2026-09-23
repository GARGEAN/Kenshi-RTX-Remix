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

#ifndef TAA_CB_H
#define TAA_CB_H

struct TemporalAntiAliasingConstants
{
    float4x4 reprojectionMatrix;

    float2 previousViewOrigin;
    float2 previousViewSize;

    float2 viewOrigin;
    float2 viewSize;

    float2 sourceTextureSizeInv;
    float clampingFactor;
    float newFrameWeight;

    uint3 padding;
    uint stencilMask;
};

#endif // TAA_CB_H