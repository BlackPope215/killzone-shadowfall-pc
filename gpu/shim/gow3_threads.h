// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
#include <windows.h>
#else
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Gow3Threads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// gow3: GOW3_PIN=1: the GPU pipeline's three busy threads (command processor, draw recorder,
/// Vulkan recorder: slots 0..2) each get a physical core of their own, and every other thread of
/// the process stays off those cores (Windows CPU sets). They spin while waiting for each other,
/// and anything sharing a core's second hardware thread slowed them: restricting the whole
/// process to one thread per core gave +7% draws per second in Killzone (5800X3D).
/// Needs SMT and at least 6 cores; otherwise nothing changes.
inline void PinHot(unsigned slot) {
    struct CpuSet { // SYSTEM_CPU_SET_INFORMATION (own copy: older SDK headers lack it)
        DWORD size, type, id;
        WORD group;
        BYTE logical_index, core_index, llc_index, numa_index, efficiency, flags;
        DWORD reserved;
        DWORD64 tag;
    };
    using GetInfo = BOOL(WINAPI*)(void*, ULONG, ULONG*, HANDLE, ULONG);
    using SetDefault = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
    using SetThread = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
    static constexpr unsigned Slots = 3;
    static std::vector<ULONG> hot[Slots];
    static SetThread set_thread = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* env = std::getenv("GOW3_PIN");
        if (!env || env[0] != '1') {
            return;
        }
        const HMODULE kernel = GetModuleHandleA("kernel32.dll");
        const auto get_info = reinterpret_cast<GetInfo>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "GetSystemCpuSetInformation")));
        const auto set_default = reinterpret_cast<SetDefault>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessDefaultCpuSets")));
        const auto thread_sets = reinterpret_cast<SetThread>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadSelectedCpuSets")));
        if (!get_info || !set_default || !thread_sets) {
            return;
        }
        ULONG bytes = 0;
        get_info(nullptr, 0, &bytes, GetCurrentProcess(), 0);
        std::vector<unsigned char> buffer(bytes);
        if (!bytes || !get_info(buffer.data(), bytes, &bytes, GetCurrentProcess(), 0)) {
            return;
        }
        std::map<unsigned, std::vector<ULONG>> cores; // (group, core) -> CPU set ids
        unsigned logical = 0;
        for (ULONG at = 0; at + sizeof(CpuSet) <= bytes;) {
            const auto* set = reinterpret_cast<const CpuSet*>(buffer.data() + at);
            if (set->size < sizeof(CpuSet)) {
                break;
            }
            if (set->type == 0) {
                cores[(unsigned(set->group) << 8) | set->core_index].push_back(set->id);
                ++logical;
            }
            at += set->size;
        }
        if (cores.size() < 6 || logical < cores.size() * 2) {
            std::printf("Threads: GOW3_PIN needs SMT and 6 cores (%u cores, %u threads)\n",
                        unsigned(cores.size()), logical);
            return;
        }
        // The last cores are the hot ones; everything else is the process default.
        std::vector<ULONG> rest;
        unsigned index = 0;
        const unsigned first_hot = unsigned(cores.size()) - Slots;
        for (const auto& [core, ids] : cores) {
            if (index >= first_hot) {
                hot[index - first_hot] = ids;
            } else {
                rest.insert(rest.end(), ids.begin(), ids.end());
            }
            ++index;
        }
        if (!set_default(GetCurrentProcess(), rest.data(), ULONG(rest.size()))) {
            for (auto& ids : hot) {
                ids.clear();
            }
            return;
        }
        set_thread = thread_sets;
        std::printf("Threads: GPU threads on %u cores of their own, the rest on %u of %u "
                    "(GOW3_PIN)\n", Slots, first_hot, unsigned(cores.size()));
    });
    if (set_thread && slot < Slots && !hot[slot].empty()) {
        set_thread(GetCurrentThread(), hot[slot].data(), ULONG(hot[slot].size()));
    }
}
#else

inline void PinHot(unsigned) {}


/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}
#endif

} // namespace Gow3Threads
