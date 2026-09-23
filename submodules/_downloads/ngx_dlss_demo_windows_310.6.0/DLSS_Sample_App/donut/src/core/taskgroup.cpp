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
#if __linux__

#include <functional>
#include <donut/core/taskgroup.h>

// super-minimal (synchronous) implementation
concurrency::task_group::task_group() { }
void concurrency::task_group::run(std::function<void(void)> f) {f();}
void concurrency::task_group::wait() { /**/ }
void concurrency::task_group::cancel() { /**/ }

#endif // __linux__
