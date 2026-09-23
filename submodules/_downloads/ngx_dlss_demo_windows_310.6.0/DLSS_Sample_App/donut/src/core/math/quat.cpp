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
#include <donut/core/math/math.h>

namespace donut::math
{
	// Quaternion implementations

	quat rotationQuat(float3_arg axis, float radians)
	{
		// Note: assumes axis is normalized
		float sinHalfTheta = sinf(0.5f * radians);
		float cosHalfTheta = cosf(0.5f * radians);

		return makequat(cosHalfTheta, axis * sinHalfTheta);
	}

	quat rotationQuat(float3_arg euler)
	{
		float sinHalfX = sinf(0.5f * euler.x);
		float cosHalfX = cosf(0.5f * euler.x);
		float sinHalfY = sinf(0.5f * euler.y);
		float cosHalfY = cosf(0.5f * euler.y);
		float sinHalfZ = sinf(0.5f * euler.z);
		float cosHalfZ = cosf(0.5f * euler.z);

		quat quatX = makequat(cosHalfX, sinHalfX, 0, 0);
		quat quatY = makequat(cosHalfY, 0, sinHalfY, 0);
		quat quatZ = makequat(cosHalfZ, 0, 0, sinHalfZ);

		// Note: multiplication order for quats is like column-vector convention
		return quatZ * quatY * quatX;
	}

	quat slerp(quat_arg a, quat_arg b, float u)
	{
		float theta = acosf(dot(a, b));
		return (a * sinf((1.0f - u) * theta) + b * sinf(u * theta)) / sinf(theta);
	}
}
