/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef NGX_ADJUST_CB_H
#define NGX_ADJUST_CB_H

struct NGXAdjustmentConstants
{
    bool isHDR;
    float postExposureScale;
    float postBlackLevel;
    float postWhiteLevel;
    float postGamma;
};

#endif // NGX_ADJUST_CB_H