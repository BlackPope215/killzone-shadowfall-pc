// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace Vulkan {

/// gow3 (Killzone): flips presented so far (VideoOutDriver::Flip); the frame clock of
/// PendingDrawPolicy.
inline std::atomic<std::uint64_t> gow3_flip_count{0};

/// gow3 (Killzone): what a draw does when its shaders or pipeline are still compiling.
///
/// Waiting (PipelineCache::WaitForPending) stalls the GPU thread for the whole compile (~200 ms
/// each on a first play: the stutter of the first missions). Dropping the draw instead is
/// invisible when its target is redrawn every frame (scene, lighting, post-processing: the draw
/// shows up a few frames later), but not for caches drawn once: Killzone renders each glyph of
/// its text once into a 512x512 atlas, and a dropped glyph stays missing for the session.
///
/// So a pending draw is dropped only when its target is "live": drawn in at least `streak`
/// frames in a row (a gap of up to 2 flips is tolerated: the GPU thread runs ahead of the
/// presented frame) and at least `min_width` pixels wide. Everything else waits as before.
/// Depth-only draws (shadow maps) always wait: Killzone updates its shadow maps incrementally
/// as the camera moves, so a dropped caster left a dark band that followed the player. Only
/// full-resolution targets (at least 90% of the widest target drawn lately: G-buffer, light
/// accumulation) may drop: the half-resolution ones (ambient occlusion, volumetrics,
/// reflections) blend in earlier frames, so a dropped draw left dark cuts for a while.
/// GOW3_SMART_PENDING=0 always waits; GOW3_PENDING_STREAK / GOW3_PENDING_MIN_WIDTH tune it;
/// GOW3_PENDING_DROP_DEPTH=1 lets depth-only draws drop too.
/// Used on the GPU command thread only.
class PendingDrawPolicy {
public:
    PendingDrawPolicy() {
        enabled = Env("GOW3_SMART_PENDING", 1) != 0;
        streak_needed = static_cast<std::uint32_t>(Env("GOW3_PENDING_STREAK", 12));
        min_width = static_cast<std::uint32_t>(Env("GOW3_PENDING_MIN_WIDTH", 640));
        drop_depth = Env("GOW3_PENDING_DROP_DEPTH", 0) != 0;
        widest_any = Env("GOW3_PENDING_WIDEST_ANY", 0) != 0;
    }

    /// Records a draw into `target` (color or depth address, `width` pixels) during `frame`.
    /// `depth`: the draw also has a depth buffer (scene passes).
    void Note(std::uint64_t target, std::uint32_t width, bool color, std::uint64_t frame,
              bool depth = true) {
        if (!target) {
            return;
        }
        // The widest color target of the last ~10 s (600 flips) is the render resolution
        // (shadow maps, depth only, can be wider). gow3: only scene passes count, those with a
        // depth buffer as well: the UI composed at the output size (2560 wide for a 1920 scene)
        // made every full-resolution target look like a 75% one, so no pending draw was ever
        // dropped and each new shader stalled the game ~450 ms ("waited 473 ms for a compile:
        // target 0x220490000, 1920 px wide, drawn 50437 frames in a row").
        // Independent of the display: the output size (1920, 2560, 3440, 3840...) never enters
        // the comparison, only the scene passes do, so FHD, QHD, 4K and ultrawide behave alike.
        // GOW3_PENDING_WIDEST_ANY=1 counts every color target again.
        if (color && (depth || widest_any) && (width >= widest || frame - widest_frame > 600)) {
            widest = width;
            widest_frame = frame;
        }
        Seen& s = seen[target];
        if (s.streak && frame == s.last) {
            return;
        }
        s.streak = s.streak && frame > s.last && frame - s.last <= 2 ? s.streak + 1 : 1;
        s.last = frame;
        if (seen.size() > 4096) {  // targets come and go with levels: forget the stale ones
            for (auto it = seen.begin(); it != seen.end();) {
                it = frame - it->second.last > 600 ? seen.erase(it) : std::next(it);
            }
        }
    }

    /// True when a pending draw into `target` (`width` pixels wide; `color`: it writes a color
    /// buffer, else only depth) may be dropped.
    [[nodiscard]] bool MayDrop(std::uint64_t target, std::uint32_t width, bool color) const {
        // No scene pass seen yet (menus, loading): nothing to compare with, so wait.
        if (!enabled || !target || !widest || width < min_width || (!color && !drop_depth) ||
            std::uint64_t(width) * 10 < std::uint64_t(widest) * 9) {
            return false;
        }
        const auto it = seen.find(target);
        return it != seen.end() && it->second.streak >= streak_needed;
    }

    /// Statistics for the log line (Report).
    void CountDropped(std::uint64_t target, std::uint32_t width) {
        ++dropped;
        Drops& d = dropped_by_target[target];
        ++d.count;
        d.width = width;
    }
    /// A draw into `target` waited `ms` for a compile; waits of 100 ms or more are logged one
    /// by one (what to tune when a stutter remains).
    void CountWait(double ms, std::uint64_t target = 0, std::uint32_t width = 0) {
        ++waits;
        wait_ms += ms;
        worst_ms = ms > worst_ms ? ms : worst_ms;
        if (ms >= 100.0 && long_waits_logged < 200) {
            ++long_waits_logged;
            const auto it = seen.find(target);
            std::printf("Shaders: waited %.0f ms for a compile: target %#llx, %u px wide, drawn %u "
                        "frames in a row\n",
                        ms, static_cast<unsigned long long>(target), width,
                        it != seen.end() ? it->second.streak : 0u);
            std::fflush(stdout);
        }
    }
    /// Every 5 s with activity: "Shaders: ... draws skipped while compiling ...".
    void Report() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::seconds(5) || (!dropped && !waits)) {
            return;
        }
        last_report = now;
        // The three targets with the most drops: where to look when something flickers.
        char top[160] = "";
        for (int n = 0, used = 0; n < 3 && !dropped_by_target.empty(); ++n) {
            auto best = dropped_by_target.begin();
            for (auto it = dropped_by_target.begin(); it != dropped_by_target.end(); ++it) {
                best = it->second.count > best->second.count ? it : best;
            }
            used += std::snprintf(top + used, sizeof(top) - used, "%s%#llx %upx x%u", n ? ", " : "; top: ",
                                  static_cast<unsigned long long>(best->first), best->second.width,
                                  best->second.count);
            dropped_by_target.erase(best);
            if (used >= int(sizeof(top))) {
                break;
            }
        }
        std::printf("Shaders: %u draws skipped while compiling (live targets), %u waits for a "
                    "compile (%.0f ms in all, worst %.0f ms), scene width %u%s\n",
                    dropped, waits, wait_ms, worst_ms, widest, top);
        std::fflush(stdout);
        dropped_by_target.clear();
        dropped = waits = 0;
        wait_ms = worst_ms = 0.0;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

private:
    bool widest_any = false;
    struct Seen {
        std::uint64_t last = 0;
        std::uint32_t streak = 0;
    };
    struct Drops {
        std::uint32_t count = 0, width = 0;
    };
    static long Env(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v && *v ? std::strtol(v, nullptr, 10) : fallback;
    }

    bool enabled = true;
    std::uint32_t streak_needed = 12;
    std::uint32_t min_width = 640;
    bool drop_depth = false;
    std::uint32_t widest = 0;
    std::uint64_t widest_frame = 0;
    std::unordered_map<std::uint64_t, Seen> seen;
    std::unordered_map<std::uint64_t, Drops> dropped_by_target;
    std::uint32_t dropped = 0, waits = 0, long_waits_logged = 0;
    double wait_ms = 0.0, worst_ms = 0.0;
    std::chrono::steady_clock::time_point last_report{};
};

} // namespace Vulkan
