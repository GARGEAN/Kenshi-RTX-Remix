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

#ifndef _WIN32

// declare internal implementation of concurrency::task_group

#include <functional>

namespace concurrency
{
	// minimal serialized implementation of concurrency::task_group
	template<class _Function>
	class task_handle;
	
	class task_group
	{
		public:
		task_group();
		void run(std::function<void(void)> f);
		void wait();
                void cancel();
	};
}

#endif // _WIN32
