// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_FB_COPY, feedback-loop snapshots.
//
// Some Killzone passes draw into G-buffer targets while sampling the same targets (decal boxes,
// pass "fb 1" in the frame analyzer). Vulkan only defines such a read for the texel a fragment
// writes itself; filtered or neighbouring reads race with the other fragments of the draw. On
// RDNA3 (RX 7000) that race shows up as flashing coloured dots and purple edge dashes; NVIDIA
// happens to return the old values. With a snapshot the pass samples a copy of the targets made
// just before the draw, which is what the PS4 effectively read.
//
// GOW3_FB_COPY unset: snapshot for the pixel shaders known to need it (DefaultHashes).
// GOW3_FB_COPY=0 / off: never.  GOW3_FB_COPY=all: every feedback-loop draw.
// GOW3_FB_COPY=<hex hashes>: these pixel shaders (lower 32 bits, as the frame analyzer prints).
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "common/types.h"

namespace Vulkan::FeedbackCopy {

/// Pixel shaders found to need a snapshot (bisected with GOW3_SKIP_FILE on an RX 7900 XT).
inline constexpr u32 DefaultHashes[] = {
    0x53ee1598, // decal box writing all five G-buffer targets (bedroom lamp, frame pass #62)
};

struct Config {
    bool all = false;
    std::unordered_set<u32> hashes;
};

inline const Config& Get() {
    static const Config config = [] {
        Config c;
        const char* env = std::getenv("GOW3_FB_COPY");
        std::string value = env ? env : "";
        for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (value == "0" || value == "off" || value == "no") {
            std::printf("GPU: feedback-loop snapshots off (GOW3_FB_COPY)\n");
            return c;
        }
        if (value == "all" || value == "1") {
            c.all = true;
            std::printf("GPU: feedback-loop snapshots for every feedback-loop draw\n");
            return c;
        }
        if (value.empty()) {
            for (const u32 h : DefaultHashes) c.hashes.insert(h);
        } else {
            std::string token;
            auto flush = [&] {
                if (!token.empty()) {
                    if (token.size() > 8) token = token.substr(token.size() - 8);
                    c.hashes.insert(static_cast<u32>(std::strtoul(token.c_str(), nullptr, 16)));
                    token.clear();
                }
            };
            for (const char ch : value) {
                if (std::isxdigit(static_cast<unsigned char>(ch))) token += ch;
                else if (ch == 'x' && token == "0") token.clear();
                else flush();
            }
            flush();
        }
        std::printf("GPU: feedback-loop snapshots for %zu pixel shader(s)\n", c.hashes.size());
        return c;
    }();
    return config;
}

inline bool Wanted(u64 ps_hash) {
    const auto& c = Get();
    return c.all || (!c.hashes.empty() && c.hashes.contains(static_cast<u32>(ps_hash)));
}

} // namespace Vulkan::FeedbackCopy
