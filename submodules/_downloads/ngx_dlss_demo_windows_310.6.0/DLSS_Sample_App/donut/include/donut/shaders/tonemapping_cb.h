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

#ifndef DEPTH_CB_H
#define DEPTH_CB_H

struct ToneMappingConstants
{
    uint2 viewOrigin;
    uint2 viewSize;

    float logLuminanceScale;
    float logLuminanceBias;
    float histogramLowPercentile;
    float histogramHighPercentile;

    float eyeAdaptationSpeedUp;
    float eyeAdaptationSpeedDown;
    float minAdaptedLuminance;
    float maxAdaptedLuminance;

    float frameTime;
    float exposureScale;
    float whitePointInvSquared;
    uint sourceSlice;

    float2 colorLUTTextureSize;
    float2 colorLUTTextureSizeInv;
};

#endif // DEPTH_CB_H