// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: FSR Upscaling 4.1 through AMD's FSR SDK 2.4 for Vulkan. The signed DLLs
// (amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll) are loaded at run time from
// the executable's folder (GOW3_FFX_DIR overrides); the SDK picks the newest upscaler the GPU and
// driver support (FSR 4.1.2 INT8 on RX 7000 with AMD Software 26.10.1 or later, FP8 on RX 9000,
// else FSR 3.1.6). Kept in its own translation unit: the SDK headers share names with the
// FSR-Vulkan ones vk_fsr4.cpp uses.

#pragma once

#include <memory>
#include <string>
#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Fsr41Sdk {
public:
    Fsr41Sdk(const Instance& instance, Scheduler& scheduler);
    ~Fsr41Sdk();

    /// Records the upscale into `frame.cmdbuf`; every image is in layout General before and
    /// after. False when it cannot run (Problem() says why).
    bool Record(const Fsr4Upscaler::Frame& frame);

    [[nodiscard]] const std::string& Problem() const noexcept;
    /// The DLLs are missing or refuse this device: the SDK will not run in this session.
    [[nodiscard]] bool Fatal() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
