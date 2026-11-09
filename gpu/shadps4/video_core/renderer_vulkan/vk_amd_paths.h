// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_AMD_PATHS_OFF=mixed,shaderext (or `all`). Runs the shared core without the device
// extensions only AMD drivers expose, so a tester on an RDNA2 card (#65) can tell an AMD-only
// code path of the port from the driver itself:
//   mixed      VK_AMD_mixed_attachment_samples is not enabled: passes whose colour and depth
//              sample counts differ get equal counts, as on Intel (vk_pipeline_cache.cpp)
//   shaderext  VK_AMD_shader_explicit_vertex_parameter, VK_AMD_gcn_shader and
//              VK_AMD_shader_trinary_minmax are not enabled: fragment shaders use
//              VK_KHR_fragment_shader_barycentric and the generic cube and min/max code
//              (changes the SPIR-V; folded into the shader binary cache version)

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Vulkan::AmdPathsOff {

struct Flags {
    bool mixed{};
    bool shaderext{};

    bool Any() const {
        return mixed || shaderext;
    }
    bool operator==(const Flags&) const = default;
};

/// Tokens separated by commas or spaces, case-insensitive; `all` or `1` turn both on, unknown
/// tokens are ignored.
inline Flags Parse(const char* text) {
    Flags flags;
    if (!text) {
        return flags;
    }
    std::string_view rest{text};
    while (!rest.empty()) {
        const auto end = rest.find_first_of(", ");
        std::string token{rest.substr(0, end)};
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        for (auto& c : token) {
            c = char(std::tolower(static_cast<unsigned char>(c)));
        }
        if (token == "all" || token == "1") {
            flags = Flags{.mixed = true, .shaderext = true};
        } else if (token == "mixed") {
            flags.mixed = true;
        } else if (token == "shaderext") {
            flags.shaderext = true;
        }
    }
    return flags;
}

/// The line a tester's last_run.log must contain; empty when nothing is on.
inline std::string Marker(Flags flags) {
    if (!flags.Any()) {
        return {};
    }
    std::string line = "GPU: amd paths off:";
    if (flags.mixed) {
        line += " mixed";
    }
    if (flags.shaderext) {
        line += " shaderext";
    }
    return line;
}

/// GOW3_AMD_PATHS_OFF, read once; the marker goes to stdout (last_run.log) the first time.
inline Flags Current() {
    static const Flags flags = [] {
        const Flags parsed = Parse(std::getenv("GOW3_AMD_PATHS_OFF"));
        if (parsed.Any()) {
            std::printf("%s\n", Marker(parsed).c_str());
        }
        return parsed;
    }();
    return flags;
}

/// gow3: GOW3_MANUAL_INTERP=1 interpolates the fragment inputs in the shader from the per-vertex
/// values, with the I/J the guest code computed (V_INTERP_P1/P2), as on NVIDIA. Without it the
/// AMD path guesses one qualifier (center/centroid/sample) per attribute from the VGPR holding
/// I/J and lets the hardware interpolate, which is wrong when the guest edits I/J or uses two
/// kinds for one attribute (stray colours along triangle edges). 0 forces the hardware path.
/// -1: not set. Changes the SPIR-V (folded into the shader binary cache version).
inline int ManualInterp() {
    static const int value = [] {
        const char* v = std::getenv("GOW3_MANUAL_INTERP");
        if (!v || !*v) {
            return -1;
        }
        const int on = v[0] == '1' ? 1 : 0;
        std::printf("GPU: manual interpolation %s (GOW3_MANUAL_INTERP)\n", on ? "on" : "off");
        return on;
    }();
    return value;
}

/// gow3: GOW3_LDS_BARRIERS_ALL=1 gives every GPU the LDS barriers only NVIDIA gets
/// (needs_lds_barriers): without a barrier the driver compiler may reorder one lane's
/// shared-memory store past another lane's load in a 64-thread compute workgroup, which leaves a
/// tile of pixels wrong for a frame. Changes the SPIR-V (folded into the shader cache version).
inline bool LdsBarriersAll() {
    static const bool on = [] {
        const char* v = std::getenv("GOW3_LDS_BARRIERS_ALL");
        const bool enabled = v && v[0] == '1';
        if (enabled) {
            std::printf("GPU: LDS barriers on every GPU (GOW3_LDS_BARRIERS_ALL)\n");
        }
        return enabled;
    }();
    return on;
}

} // namespace Vulkan::AmdPathsOff
