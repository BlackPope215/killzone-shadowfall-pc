// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gow3 (Killzone): GOW3_OCCLUSION=real runs the game's occlusion queries on the GPU.
//
// The game brackets small test draws (the light flare's area) with two ZPASS_DONE events: the
// first at address A, the second at A + 8. It then draws the flare under SET_PREDICATION on A:
// only when the test draws passed the depth test. The port ignored both, so every flare was
// drawn, also behind walls.
//
// Here each test draw gets a Vulkan occlusion query (inside its own render pass, so a query
// never spans a pass). When the GPU has finished the frame, the query results are summed per
// address A. A predicated draw is skipped when the last known result for its address says so.
// The result is a few frames old (no wait); an address without a result counts as visible.
// The same results go into the guest's query memory, for code that reads them on the CPU.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class OcclusionQueries {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_OCCLUSION");
            const bool on = env && (env[0] == 'r' || env[0] == 'R');
            if (on) {
                std::printf("GPU: occlusion queries run on the GPU (GOW3_OCCLUSION=real)\n");
            }
            return on;
        }();
        return enabled;
    }

    static constexpr u32 PoolSize = 16384;

    /// Recording side. Returns false when the pool cannot be made (feature off).
    bool Init(vk::Device dev, bool host_reset) {
        if (pool || failed) {
            return bool(pool);
        }
        if (!host_reset) {
            std::printf("GPU: occlusion queries unavailable (no host query reset)\n");
            failed = true;
            return false;
        }
        device = dev;
        const auto [result, created] = device.createQueryPool(
            {.queryType = vk::QueryType::eOcclusion, .queryCount = PoolSize});
        if (result != vk::Result::eSuccess) {
            std::printf("GPU: occlusion query pool failed (%d)\n", int(result));
            failed = true;
            return false;
        }
        pool = created;
        device.resetQueryPool(pool, 0, PoolSize);
        return true;
    }

    vk::QueryPool Pool() const {
        return pool;
    }

    /// Recording side: a ZPASS_DONE event. The first opens a test at `addr`, the one at
    /// addr + 8 closes it.
    void OnZpass(VAddr addr, u64 tick) {
        ++stats.events;
        if (open && addr == open_addr + 8) {
            Close(tick);
            return;
        }
        if (open) {
            ++stats.unpaired;
            Close(tick);
        }
        open = true;
        open_addr = addr;
        open_queries.clear();
        open_complete = true;
    }

    /// Recording side, for a draw about to be recorded: the query to wrap it in, or -1.
    s32 QueryForDraw() {
        if (!open || !pool) {
            return -1;
        }
        if (head - tail >= PoolSize) {
            open_complete = false;
            ++stats.pool_full;
            return -1;
        }
        const u32 index = u32(head % PoolSize);
        ++head;
        open_queries.push_back(index);
        ++stats.test_draws;
        return s32(index);
    }

    /// Recording side: SET_PREDICATION. `op` 0 clears it, 1 is ZPASS on `addr`.
    void SetPredicate(VAddr addr, u32 op, bool draw_visible) {
        ++stats.predicates;
        if (op != 1) {
            pred_active = false;
            return;
        }
        pred_active = true;
        pred_addr = addr;
        pred_draw_visible = draw_visible;
    }

    /// Recording side: whether the predicate in effect drops this draw.
    bool SkipDraw() {
        if (!pred_active) {
            return false;
        }
        u64 visible = 0;
        if (!Lookup(pred_addr, visible)) {
            ++stats.pred_unknown;
            return false;
        }
        const bool skip = (visible != 0) != pred_draw_visible;
        ++(skip ? stats.pred_skipped : stats.pred_drawn);
        return skip;
    }

    /// Recording side: reads the tests the GPU has finished. `is_free(tick)`: done on the GPU.
    template <typename IsFree>
    void Collect(IsFree&& is_free) {
        while (!done.empty() && is_free(done.front().tick)) {
            Record& rec = done.front();
            u64 sum = 0;
            bool ok = rec.complete;
            for (const u32 q : rec.queries) {
                u64 value = 0;
                const vk::Result r = device.getQueryPoolResults(
                    pool, q, 1, sizeof(value), &value, sizeof(value),
                    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
                if (r != vk::Result::eSuccess) {
                    ok = false;
                }
                sum += value;
                device.resetQueryPool(pool, q, 1);
            }
            tail += rec.queries.size();
            if (ok) {
                std::scoped_lock lock{mutex};
                results[rec.addr] = sum;
                ++(sum ? stats.visible : stats.hidden);
            }
            done.pop_front();
        }
        PrintStats();
    }

    /// Any thread: the last result for a test at `addr`.
    bool Lookup(VAddr addr, u64& visible) {
        std::scoped_lock lock{mutex};
        const auto it = results.find(addr);
        if (it == results.end()) {
            return false;
        }
        visible = it->second;
        return true;
    }

private:
    struct Record {
        VAddr addr;
        u64 tick;
        bool complete;
        std::vector<u32> queries;
    };

    void Close(u64 tick) {
        open = false;
        if (open_queries.empty()) {
            // No test draw was recorded (filtered or still compiling): no result.
            ++stats.empty_tests;
            return;
        }
        done.push_back({open_addr, tick, open_complete, std::move(open_queries)});
        open_queries = {};
    }

    void PrintStats() {
        const auto now = std::chrono::steady_clock::now();
        if (now - stats.window < std::chrono::seconds(10)) {
            return;
        }
        if (stats.events || stats.predicates) {
            std::printf("GPU: occlusion: %llu events, %llu test draws, %llu visible / %llu hidden "
                        "results, %llu empty tests, %llu unpaired; predicates %llu: %llu drawn, "
                        "%llu skipped, %llu without result; pool full %llu\n",
                        U(stats.events), U(stats.test_draws), U(stats.visible), U(stats.hidden),
                        U(stats.empty_tests), U(stats.unpaired), U(stats.predicates),
                        U(stats.pred_drawn), U(stats.pred_skipped), U(stats.pred_unknown),
                        U(stats.pool_full));
        }
        stats = Stats{};
        stats.window = now;
    }
    static unsigned long long U(u64 v) {
        return static_cast<unsigned long long>(v);
    }

    vk::Device device;
    vk::QueryPool pool;
    bool failed = false;

    bool open = false;
    VAddr open_addr = 0;
    bool open_complete = true;
    std::vector<u32> open_queries;
    std::deque<Record> done;
    u64 head = 0;
    u64 tail = 0;

    bool pred_active = false;
    VAddr pred_addr = 0;
    bool pred_draw_visible = true;

    std::mutex mutex;
    std::unordered_map<VAddr, u64> results;

    struct Stats {
        u64 events = 0, test_draws = 0, visible = 0, hidden = 0, empty_tests = 0, unpaired = 0;
        u64 predicates = 0, pred_drawn = 0, pred_skipped = 0, pred_unknown = 0, pool_full = 0;
        std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    } stats;
};

} // namespace Vulkan
