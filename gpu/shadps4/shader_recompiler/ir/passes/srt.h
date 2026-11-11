// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

/// gow3: guest memory as it will be when the GPU thread reaches a draw: writes not done yet
/// (constant engine dumps ahead of it), oldest first. Draw-preparation workers walk a draw's
/// resource tables through it (OverlayWalker), on their own thread.
struct SrtOverlayEntry {
    u64 address;
    u32 size;
    const u8* data;
};
struct SrtOverlay {
    const SrtOverlayEntry* entries{};
    size_t count{};
    u64 lo{~0ull}, hi{}; ///< bounds of all entries
};
extern thread_local const SrtOverlay* g_srt_overlay;
/// The same walker with every guest memory read taken through g_srt_overlay; null when its
/// code cannot be translated (then the plain walker is used).
PFN_SrtWalker OverlayWalker(PFN_SrtWalker walker, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
