// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <boost/container/small_vector.hpp>
#include "gow3_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "gow3_guest_image.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/page_state.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    using PageState = VideoCore::PageState;

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    // gow3: GOW3_FAULT_PROFILE=1 prints, every 5 s, the guest instructions whose faults on GPU
    // tracked pages cost the most time (game code that waits for the GPU; from iss_pc). Raw
    // addresses: subtract "Guest image base" (game.log) for guest offsets.
    struct FaultSite {
        u64 count = 0, ns = 0, caller = 0;
        std::array<u64, 4> guest_callers{};
        VAddr last = 0;
        bool write = false;
    };
    static void ProfileFault(void* context, VAddr addr, bool write, u64 ns) {
        static const bool enabled = [] {
            const char* e = std::getenv("GOW3_FAULT_PROFILE");
            return e && *e == '1';
        }();
        if (!enabled) {
            return;
        }
        static std::mutex mutex;
        static std::unordered_map<u64, FaultSite> sites;
        static auto window = std::chrono::steady_clock::now();
        const u64 rip = reinterpret_cast<u64>(Common::GetRip(context));
#ifdef _WIN32
        const u64 rsp = static_cast<EXCEPTION_POINTERS*>(context)->ContextRecord->Rsp;
#else
        const u64 rsp =
            static_cast<u64>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
#endif
        // The return address at the top of the stack (right for leaf routines such as memcpy).
        const u64 caller = rsp ? *reinterpret_cast<const u64*>(rsp) : 0;
        // Deeper callers: the first guest-image addresses found in the next 64 stack words (a
        // heuristic; names the game code that called a library copy routine).
        std::array<u64, 4> guest_callers{};
        if (rsp) {
            const auto* stack = reinterpret_cast<const u64*>(rsp);
            for (u32 i = 1, found = 0; i < 64 && found < guest_callers.size(); ++i) {
                if (Gow3GuestImage::Offset(stack[i]) != ~u64{0}) {
                    guest_callers[found++] = stack[i];
                }
            }
        }
        std::scoped_lock lock{mutex};
        auto& site = sites[rip];
        ++site.count;
        site.ns += ns;
        site.last = addr;
        site.write = write;
        site.caller = caller;
        site.guest_callers = guest_callers;
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(5)) {
            return;
        }
        window = now;
        std::vector<std::pair<u64, FaultSite>> top(sites.begin(), sites.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        // Guest offsets where the address is in the guest image, else the host address.
        const auto name = [](u64 address) {
            const u64 offset = Gow3GuestImage::Offset(address);
            return static_cast<unsigned long long>(offset != ~u64{0} ? offset : address);
        };
        std::printf("Fault profile (5 s; guest offsets):");
        for (size_t i = 0; i < top.size() && i < 10; ++i) {
            const auto& s = top[i].second;
            std::printf(" [%s %#llx<-%#llx x%llu %.1f ms @%#llx stack", s.write ? "W" : "R",
                        name(top[i].first), name(s.caller),
                        static_cast<unsigned long long>(s.count), s.ns / 1e6,
                        static_cast<unsigned long long>(s.last));
            for (const u64 c : s.guest_callers) {
                if (c) {
                    std::printf(" %#llx", name(c));
                }
            }
            std::printf("]");
        }
        std::printf("\n");
        std::fflush(stdout);
        sites.clear();
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto start = std::chrono::steady_clock::now();
        const bool handled = HandleGuestFault(context, fault_address);
        ProfileFault(context, reinterpret_cast<VAddr>(fault_address), Common::IsWriteError(context),
                     static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count()));
        return handled;
    }

    // gow3: GOW3_NOWAIT_WRITES=<offset>[,<offset>...] (guest offsets, hex; from iss_pc): guest
    // instructions whose write faults take the page over without reading the GPU data back
    // (BufferCache::NoWaitWrite). For pages the game refills while it never reads what the GPU
    // wrote there.
    static bool IsNoWaitWriteSite(void* context) {
        static const std::vector<u64> sites = [] {
            std::vector<u64> list;
            const char* e = std::getenv("GOW3_NOWAIT_WRITES");
            for (const char* p = e; p && *p;) {
                char* end = nullptr;
                const u64 offset = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(offset);
                p = *end ? end + 1 : end;
            }
            if (!list.empty()) {
                std::printf("No-wait writes: %zu guest instruction(s)\n", list.size());
            }
            return list;
        }();
        if (sites.empty()) {
            return false;
        }
        const u64 offset = Gow3GuestImage::Offset(reinterpret_cast<u64>(Common::GetRip(context)));
        return std::find(sites.begin(), sites.end(), offset) != sites.end();
    }

    static bool WorkerFaultSkip() {
        static const bool on = [] {
            const char* env = std::getenv("GOW3_PREP_FAULT_SKIP");
            return !env || env[0] != '0';
        }();
        return on;
    }

    static bool HandleGuestFault(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // gow3: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            Gow3Stats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        } else if (runtime_fault_recover && Vulkan::DrawPreparation::OnWorkerThread() &&
                   WorkerFaultSkip()) {
            // gow3 (from SpartanPort 21db7f29, Supermedo/bloodborne_pc a39f457a5): a
            // draw-preparation worker reads ahead of the GPU thread while holding the shader
            // cache's lock, which the GPU thread takes to add a shader. Serving the fault
            // would drain the draw pipe and download from the GPU: waiting for the GPU thread
            // while blocking it. The worker goes back to its recovery point (src/probe.c) and
            // the GPU thread prepares that draw itself. GOW3_PREP_FAULT_SKIP=0: served as before.
            static std::atomic<u64> skipped{0};
            static std::atomic<s64> last_report{0};
            const u64 count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
            const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            s64 last = last_report.load(std::memory_order_relaxed);
            if (now - last >= 5000 &&
                last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
                std::printf("Draw preparation: %llu worker faults on GPU-tracked pages left to "
                            "the GPU thread in all\n",
                            static_cast<unsigned long long>(count));
            }
            return false;
        }
        if (Common::IsWriteError(context)) {
            Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
            const bool nowait = !is_gpu_thread && IsNoWaitWriteSite(context);
            return rasterizer->OnWriteFault(addr, is_gpu_thread, nowait);
        } else {
            Gow3Stats::read_faults.fetch_add(1, std::memory_order_relaxed);
            Gow3Stats::Timer timer{Gow3Stats::t_read_faults};
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    // gow3 (from SpartanPort 4763068 / tlou-remastered-pc 08bb8ea): watcher counts allocated per
    // lock region on first use. The whole 40-bit address space as one array was 512 MiB, zeroed
    // at startup, for a game that tracks a few GiB of it. Every access here holds the region's
    // lock; the compare-exchange only guards two regions' first uses racing for one slot.
    class PageStates {
    public:
        PageStates() = default;
        PageStates(const PageStates&) = delete;
        PageStates& operator=(const PageStates&) = delete;
        ~PageStates() {
            for (auto& slot : chunks) {
                delete[] slot.load(std::memory_order_relaxed);
            }
        }
        PageState& operator[](size_t page) {
            std::atomic<PageState*>& slot = chunks[page / PAGES_PER_LOCK];
            PageState* chunk = slot.load(std::memory_order_acquire);
            if (!chunk) [[unlikely]] {
                PageState* fresh = new PageState[PAGES_PER_LOCK]{};
                if (slot.compare_exchange_strong(chunk, fresh, std::memory_order_acq_rel)) {
                    chunk = fresh;
                } else {
                    delete[] fresh;
                }
            }
            return chunk[page % PAGES_PER_LOCK];
        }

    private:
        std::array<std::atomic<PageState*>, NUM_ADDRESS_LOCKS> chunks{};
    };
    PageStates cached_pages;
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// gow3: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. GOW3_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("gow3:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                Gow3Stats::Timer timer{Gow3Stats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (std::getenv("GOW3_UFFD") && std::getenv("GOW3_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
