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

#ifndef VIEW_CB_H
#define VIEW_CB_H

struct PlanarViewConstants
{
    float4x4    matWorldToView;
    float4x4    matViewToClip;
    float4x4    matWorldToClip;
    float4x4    matClipToView;
    float4x4    matViewToWorld;
    float4x4    matClipToWorld;

    float2      viewportOrigin;
    float2      viewportSize;

    float2      viewportSizeInv;
    float2      pixelOffset;

    float2      clipToWindowScale;
    float2      clipToWindowBias;

    float2      windowToClipScale;
    float2      windowToClipBias;

    float4      cameraDirectionOrPosition;
};

#endif // VIEW_CB_H