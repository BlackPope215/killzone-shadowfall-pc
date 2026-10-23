// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::UiComposition {

enum class Background { None, Copy, Temporal };

// UI composition must not require scene depth/camera or an active FSR context.
constexpr Background Choose(bool scaled, bool ui_draw, bool scene_ready, bool fsr_active) {
    if (!scaled || !ui_draw) {
        return Background::None;
    }
    return scene_ready && fsr_active ? Background::Temporal : Background::Copy;
}

inline bool NativeViewport(float width, float height) {
    return std::abs(std::abs(width) - 1920.0f) < 0.5f &&
           std::abs(std::abs(height) - 1080.0f) < 0.5f;
}

// Scaleform draws, including the first stencil/movie pass. A native-size viewport
// alone also matches every fullscreen post pass when guest targets stay at 1080p.
constexpr bool MovieShader(uint64_t hash) {
    return hash == 0x34e8a281 || hash == 0x81d336ce || hash == 0x09957251 ||
           hash == 0x24042a9b || hash == 0xa400228b;
}

inline std::array<float, 2> Scale(uint32_t guest_width, uint32_t guest_height,
                                uint32_t output_width, uint32_t output_height,
                                bool native_coordinates) {
    return {float(output_width) / float(native_coordinates ? 1920 : guest_width),
            float(output_height) / float(native_coordinates ? 1080 : guest_height)};
}

// Killzone ultrawide: an output wider (or taller) than 16:9 by more than 1% gets the UI in native
// 1920x1080 coordinates (HUD, menus, movies) at one uniform scale, centred: a 16:9 area in the
// middle instead of stretched. Returns {scale x, scale y, offset x, offset y} in output pixels.
inline std::array<float, 4> Fit(uint32_t guest_width, uint32_t guest_height, uint32_t output_width,
                                uint32_t output_height, bool native_coordinates) {
    const auto scale = Scale(guest_width, guest_height, output_width, output_height, native_coordinates);
    const float aspect = float(output_width) / float(output_height);
    if (!native_coordinates || std::abs(aspect / (16.0f / 9.0f) - 1.0f) <= 0.01f) {
        return {scale[0], scale[1], 0.0f, 0.0f};
    }
    const float s = std::min(scale[0], scale[1]);
    return {s, s, std::round((float(output_width) - 1920.0f * s) * 0.5f),
            std::round((float(output_height) - 1080.0f * s) * 0.5f)};
}

} // namespace Vulkan::UiComposition
