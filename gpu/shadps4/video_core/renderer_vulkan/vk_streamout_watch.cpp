// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_STREAMOUT_DIAG=1: which CPU code touches the stream-out buffer (vk_streamout_diag.h).
// The buffer's page gets PAGE_GUARD on top of its current protection; the first access after
// each arming raises STATUS_GUARD_PAGE_VIOLATION (the system clears the guard and the access
// then completes normally). Accesses inside the buffer are counted per instruction address,
// with the offset into the allocation that holds the code (the game image or this program).

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common/types.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Vulkan::StreamoutDiag {

namespace {
std::atomic<u64> watch_begin{0}, watch_end{0};
std::once_flag handler_once;

struct Hit {
    u64 rip;
    u64 base;
    u64 address;
    u32 kind; // 0 read, 1 write
    u32 count;
    DWORD thread;
};
constexpr u32 MaxHits = 32;
Hit hits[MaxHits];
std::atomic<u32> num_hits{0};
std::atomic<u64> guard_hits{0}, outside_hits{0};

LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 address = record->ExceptionInformation[1];
    const u64 begin = watch_begin.load(std::memory_order_relaxed);
    const u64 end = watch_end.load(std::memory_order_relaxed);
    const u64 page = begin & ~u64(0xFFF);
    if (!begin || address < page || address >= ((end + 0xFFF) & ~u64(0xFFF))) {
        return EXCEPTION_CONTINUE_SEARCH; // someone else's guard page
    }
    ++guard_hits;
    if (address < begin || address >= end) {
        ++outside_hits; // other data on the same page
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const u64 rip = u64(record->ExceptionAddress);
    const u32 kind = u32(record->ExceptionInformation[0] == 1);
    const u32 n = num_hits.load(std::memory_order_acquire);
    for (u32 i = 0; i < n; ++i) {
        if (hits[i].rip == rip && hits[i].kind == kind) {
            ++hits[i].count;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (n < MaxHits) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(rip), &mbi, sizeof(mbi));
        hits[n] = {rip, u64(mbi.AllocationBase), address, kind, 1, GetCurrentThreadId()};
        num_hits.store(n + 1, std::memory_order_release);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

void WatchArm(VAddr begin, VAddr end) {
    std::call_once(handler_once, [] { AddVectoredExceptionHandler(1, GuardHandler); });
    watch_begin = begin;
    watch_end = end;
    const u64 page = begin & ~u64(0xFFF);
    const u64 last = (end + 0xFFF) & ~u64(0xFFF);
    for (u64 p = page; p < last; p += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & (PAGE_NOACCESS))) {
            continue;
        }
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(p), 0x1000, mbi.Protect | PAGE_GUARD, &old);
    }
}

void WatchReport() {
    const u32 n = num_hits.load(std::memory_order_acquire);
    std::printf("  CPU accesses to the stream-out buffer: %llu guard hits, %llu on other data of "
                "the page, %u code sites\n",
                static_cast<unsigned long long>(guard_hits.load()),
                static_cast<unsigned long long>(outside_hits.load()), n);
    const u64 self = u64(GetModuleHandleW(nullptr));
    for (u32 i = 0; i < n; ++i) {
        const auto& h = hits[i];
        std::printf("    %s at %llx (%s+0x%llx), %u times, first at %llx, thread %lu\n",
                    h.kind ? "write" : "read", static_cast<unsigned long long>(h.rip),
                    h.base == self ? "port" : "alloc", static_cast<unsigned long long>(h.rip - h.base),
                    h.count, static_cast<unsigned long long>(h.address),
                    static_cast<unsigned long>(h.thread));
    }
}

} // namespace Vulkan::StreamoutDiag

#else
namespace Vulkan::StreamoutDiag {
void WatchArm(VAddr, VAddr) {}
void WatchReport() {}
} // namespace Vulkan::StreamoutDiag
#endif
