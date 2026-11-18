// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device);

/// gow3: writes `value` as the default of the 32-bit float specialization constant `spec_id` in
/// SPIR-V `code`, for a pipeline made without specialization info (ImGui's). False when the code
/// has no such constant (then it is unchanged).
bool SetSpecConstantDefault(std::span<u32> code, u32 spec_id, float value);

} // namespace Vulkan
