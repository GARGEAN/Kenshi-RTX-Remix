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

#ifndef GBUFFER_CB_H
#define GBUFFER_CB_H

#include "view_cb.h"

struct GBufferFillConstants
{
    PlanarViewConstants leftView;
    PlanarViewConstants leftViewPrev;
    PlanarViewConstants rightView;
    PlanarViewConstants rightViewPrev;
};

#endif // GBUFFER_CB_H