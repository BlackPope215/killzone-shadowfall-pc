// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gow3: GOW3_STREAMOUT_DIAG=1 describes the stream-out geometry draws the port skips, without
// changing anything: the stream-out registers, the copy shader's instructions (numeric opcodes,
// shader_recompiler/frontend/opcodes.h), the buffer descriptors it loads from user data, which
// later draws read those buffers, DRAW_INDEX_AUTO draws that take their vertex count from a
// stream-out fill level, and how many stream-out draws are skipped per second.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "core/memory.h"
#include "shader_recompiler/frontend/decode.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan::StreamoutDiag {

// vk_streamout_watch.cpp: CPU accesses to the stream-out buffer (guard page).
void WatchArm(VAddr begin, VAddr end);
void WatchReport();

inline VAddr& WatchBegin() {
    static VAddr v = 0;
    return v;
}
inline VAddr& WatchEnd() {
    static VAddr v = 0;
    return v;
}

inline bool On() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_STREAMOUT_DIAG");
        const bool v = env && env[0] == '1';
        if (v) {
            std::printf("GPU: stream-out diagnostics on (GOW3_STREAMOUT_DIAG)\n");
        }
        return v;
    }();
    return on;
}

struct Range {
    VAddr begin, end;
    u32 buffer_index;
    u64 copy_shader;
};

inline std::mutex& Mutex() {
    static std::mutex m;
    return m;
}
inline std::vector<Range>& Ranges() {
    static std::vector<Range> r;
    return r;
}

inline void Rate() {
    static u64 skipped = 0;
    static auto window = std::chrono::steady_clock::now();
    ++skipped;
    const auto now = std::chrono::steady_clock::now();
    const bool report = now - window >= std::chrono::seconds(10);
    if (!report && WatchBegin()) {
        WatchArm(WatchBegin(), WatchEnd()); // re-arm: a guard fires once
    }
    if (report) {
        std::printf("GPU: stream-out: %.1f skipped pipeline selections per second\n",
                    double(skipped) / std::chrono::duration<double>(now - window).count());
        // What the buffers hold now (nothing writes them while stream out is skipped, unless
        // the game itself does): the first vertices as floats.
        for (const auto& r : Ranges()) {
            if (!Core::Memory::Instance()->IsValidGpuMapping(r.begin, 64)) {
                continue;
            }
            const auto* f = reinterpret_cast<const float*>(r.begin);
            std::printf("  buffer %llx now: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n",
                        static_cast<unsigned long long>(r.begin), f[0], f[1], f[2], f[3], f[4],
                        f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        }
        if (WatchBegin()) {
            WatchReport();
            WatchArm(WatchBegin(), WatchEnd());
        }
        skipped = 0;
        window = now;
    }
}

/// A stream-out draw that is skipped: described once per copy shader.
inline void Describe(const AmdGpu::Regs& regs) {
    if (!On()) {
        return;
    }
    std::scoped_lock lock{Mutex()};
    Rate();
    const auto& vs = regs.vs_program;
    const u32* code = vs.Address<const u32*>();
    const u64 key = reinterpret_cast<u64>(code);
    static std::unordered_set<u64> seen;
    if (!code || seen.size() >= 16 || !seen.insert(key).second) {
        return;
    }
    auto* memory = Core::Memory::Instance();
    const auto& ra = regs.reg_array;
    std::printf("GPU: stream-out draw #%zu: copy shader %p, config %08x, buffer config %08x, "
                "primitive %u, %u indices, %u instances, ps %s, gs %p, es %p\n",
                seen.size(), static_cast<const void*>(code), regs.vgt_strmout_config.raw,
                ra[0xA2E6], u32(regs.primitive_type), regs.num_indices,
                regs.num_instances.NumInstances(),
                regs.ps_program.Address<const u32*>() ? "yes" : "no",
                static_cast<const void*>(regs.gs_program.Address<const u32*>()),
                static_cast<const void*>(regs.es_program.Address<const u32*>()));
    for (u32 b = 0; b < 4; ++b) {
        const u32 base = 0xA2B4 + 4 * b;
        std::printf("  buffer %u: size %u dwords, vertex stride %u dwords, offset %u dwords, "
                    "streamout offset reg %u\n",
                    b, ra[base], ra[base + 1], ra[base + 3], ra[base + 2]);
    }
    std::printf("  copy shader user data:");
    for (u32 i = 0; i < 16; ++i) {
        std::printf(" %08x", vs.user_data[i]);
    }
    std::printf("\n");
    Shader::Gcn::GcnCodeSlice slice{code, code + 1024};
    Shader::Gcn::GcnDecodeContext decoder;
    for (u32 n = 0; n < 200 && !slice.atEnd(); ++n) {
        const auto inst = decoder.decodeInstruction(slice);
        std::printf("  [%3u] op %4u enc %2u", n, u32(inst.opcode), u32(inst.encoding));
        for (u32 d = 0; d < inst.dst_count && d < 2; ++d) {
            std::printf(" d%u=%u:%u", d, u32(inst.dst[d].field), inst.dst[d].code);
        }
        for (u32 s = 0; s < inst.src_count && s < 4; ++s) {
            std::printf(" s%u=%u:%u", s, u32(inst.src[s].field), inst.src[s].code);
        }
        if (inst.opcode >= Shader::Gcn::Opcode::S_LOAD_DWORDX4 &&
            inst.opcode <= Shader::Gcn::Opcode::S_LOAD_DWORDX16) {
            const auto& smrd = inst.control.smrd;
            std::printf(" smrd off %u imm %u", u32(smrd.offset), u32(smrd.imm));
            const u32 sbase = inst.src[0].code * 2; // the SBASE field counts register pairs
            if (smrd.imm && sbase + 1 < 16) {
                const VAddr table = (VAddr(vs.user_data[sbase]) |
                                     (VAddr(vs.user_data[sbase + 1]) << 32)) &
                                    0xFFFFFFFFFFFFULL;
                const VAddr at = table + smrd.offset * 4;
                if (table && memory->IsValidGpuMapping(at, 16)) {
                    const auto* vsharp = reinterpret_cast<const u32*>(at);
                    // V#: base address is 44 bits (word 1 bits 0-11); stream-out buffers set
                    // num_records to the maximum, the real size is VGT_STRMOUT_BUFFER_SIZE.
                    const VAddr addr = VAddr(vsharp[0]) | (VAddr(vsharp[1] & 0xFFF) << 32);
                    const u32 stride = (vsharp[1] >> 16) & 0x3FFF;
                    const u32 records = vsharp[2];
                    u64 bytes = std::min<u64>(u64(records) * (stride ? stride : 1),
                                              u64(1) << 20);
                    if (stride && stride == ra[0xA2B5] * 4 && ra[0xA2B4]) {
                        bytes = u64(ra[0xA2B4]) * 4; // stream-out buffer 0: its real size
                        WatchBegin() = addr;
                        WatchEnd() = addr + bytes;
                        std::printf("  watching CPU accesses to %llx..%llx\n",
                                    static_cast<unsigned long long>(addr),
                                    static_cast<unsigned long long>(addr + bytes));
                    }
                    std::printf(" -> V# %llx stride %u records %u (%llu bytes)",
                                static_cast<unsigned long long>(addr), stride, records,
                                static_cast<unsigned long long>(bytes));
                    if (addr && bytes && Ranges().size() < 64) {
                        Ranges().push_back({addr, addr + bytes, n, key});
                    }
                }
            }
        }
        std::printf("\n");
        if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
            break;
        }
    }
    std::fflush(stdout);
}

/// A buffer bound by a later draw: reported once per shader pair when it overlaps a buffer a
/// copy shader loaded.
inline void CheckRead(VAddr address, u64 size, u64 shader_hash, u64 stage, const char* what) {
    if (!On() || !size) {
        return;
    }
    std::scoped_lock lock{Mutex()};
    for (const auto& r : Ranges()) {
        if (address < r.end && r.begin < address + size) {
            static std::unordered_set<u64> reported;
            const u64 key = (shader_hash << 8) ^ stage ^ (u64(r.begin) << 7);
            if (reported.size() < 64 && reported.insert(key).second) {
                std::printf("GPU: stream-out buffer %llx (copy shader %llx, load %u) read as %s "
                            "by shader %08x (stage %u)\n",
                            static_cast<unsigned long long>(r.begin),
                            static_cast<unsigned long long>(r.copy_shader), r.buffer_index, what,
                            u32(shader_hash), u32(stage));
            }
            return;
        }
    }
}

/// DRAW_INDEX_AUTO: USE_OPAQUE (bit 6 of the draw initiator) takes the vertex count from a
/// stream-out buffer's filled size.
inline void NoteDrawAuto(u32 initiator, u32 count) {
    if (!On() || !((initiator >> 6) & 1)) {
        return;
    }
    static u32 logged = 0;
    if (logged < 8) {
        ++logged;
        std::printf("GPU: DRAW_INDEX_AUTO with USE_OPAQUE (vertex count from stream out), "
                    "count field %u\n",
                    count);
    }
}

} // namespace Vulkan::StreamoutDiag
