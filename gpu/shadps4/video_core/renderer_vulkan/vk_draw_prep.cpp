// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "gow3_threads.h"
#include "gow3_toggles.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include <xxhash.h>

namespace Vulkan {

namespace {

bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    using AmdGpu::PM4ItOpcode;
    return opcode == PM4ItOpcode::DrawIndex2 || opcode == PM4ItOpcode::DrawIndexOffset2 ||
           opcode == PM4ItOpcode::DrawIndexAuto;
}

/// Walks the type-3 packets of a command buffer (type-2 padding skipped).
template <typename Func>
void ForEachPacket(std::span<const u32> commands, Func&& func) {
    for (size_t at = 0; at < commands.size();) {
        const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t words = header->type3.NumWords() + 1;
        if (at + words > commands.size()) {
            return;
        }
        func(header);
        at += words;
    }
}

/// gow3: GOW3_PREP_CE=0: workers read guest memory as it is (constant engine dumps the GPU
/// thread has not made yet are missing: about half the Killzone draws were then unusable).
bool CeSimulation() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PREP_CE");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// Appends a constant engine buffer, its indirect buffers inlined (their memory is final when
/// the game submits).
void AppendCe(std::vector<u32>& out, std::span<const u32> commands, int depth) {
    ForEachPacket(commands, [&](const AmdGpu::PM4Header* header) {
        const u32 words = header->type3.NumWords() + 1;
        if (header->type3.opcode == AmdGpu::PM4ItOpcode::IndirectBufferConst) {
            const auto* ib = reinterpret_cast<const AmdGpu::PM4CmdIndirectBuffer*>(header);
            if (depth < 4 && ib->ib_size > 0) {
                AppendCe(out, {ib->Address<const u32>(), ib->ib_size}, depth + 1);
            }
            return;
        }
        const auto* first = reinterpret_cast<const u32*>(header);
        out.insert(out.end(), first, first + words);
    });
}

/// The constant engine of one submission, as Liverpool::ProcessCeUpdate runs it: from the
/// start until it waits for the draw engine, resumed when the draw engine waits for it.
struct CeSim {
    DrawPreparation::CeHeap& heap;
    DrawPreparation::Submission& submission;
    size_t at = 0;
    u32 ce_count = 0;
    bool done = false;

    /// Runs until it blocks or ends; true if it moved.
    bool Run(u32 de_count, u32 draw_ordinal) {
        const auto& commands = submission.ce_commands;
        const size_t start = at;
        while (at < commands.size()) {
            const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
            if (header->type == 2) {
                ++at;
                continue;
            }
            if (header->type != 3) {
                break;
            }
            const size_t words = header->type3.NumWords() + 1;
            if (at + words > commands.size()) {
                break;
            }
            const auto* body = reinterpret_cast<const u32*>(header) + 1;
            switch (header->type3.opcode) {
            case AmdGpu::PM4ItOpcode::WriteConstRam: {
                const auto* write = reinterpret_cast<const AmdGpu::PM4WriteConstRam*>(header);
                if (size_t(write->Offset()) + write->Size() <= heap.size()) {
                    std::memcpy(heap.data() + write->Offset(), &write->data, write->Size());
                }
                break;
            }
            case AmdGpu::PM4ItOpcode::DumpConstRam: {
                const auto* dump = reinterpret_cast<const AmdGpu::PM4DumpConstRam*>(header);
                const u32 size = dump->Size();
                if (size && size_t(dump->Offset()) + size <= heap.size()) {
                    const u32 offset = u32(submission.dump_bytes.size());
                    submission.dump_bytes.insert(submission.dump_bytes.end(),
                                                 heap.data() + dump->Offset(),
                                                 heap.data() + dump->Offset() + size);
                    submission.dumps.push_back(
                        {dump->Address<u64>(), size, draw_ordinal, offset});
                    g_gow3_prep_ce_dumps.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_gow3_prep_ce_bad.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            case AmdGpu::PM4ItOpcode::IncrementCeCounter:
                ++ce_count;
                break;
            case AmdGpu::PM4ItOpcode::WaitOnDeCounterDiff:
                if (u32(de_count - ce_count) >= body[0]) {
                    return at != start; // blocked until the draw engine moves on
                }
                break;
            default:
                break;
            }
            at += words;
        }
        done = true;
        return at != start;
    }
};

/// gow3: the pending dumps of a worker's draw, indexed by 64-byte block (a draw's tables are
/// walked with dozens of reads against hundreds of dumps: a linear search starved the workers).
struct DumpIndex {
    static constexpr u32 Shift = 6;
    std::unordered_map<u64, u32> head; ///< block -> newest entry covering it (+1; 0 = none)
    std::vector<std::pair<u32, u32>> links; ///< (entry, next link + 1) per block covered

    void Clear() {
        head.clear();
        links.clear();
    }
    void Add(const Shader::SrtOverlayEntry& e, u32 index) {
        for (u64 block = e.address >> Shift; block <= (e.address + e.size - 1) >> Shift; ++block) {
            u32& first = head[block];
            links.push_back({index, first});
            first = u32(links.size());
        }
    }
    static u32 Lookup(const Shader::SrtOverlay& overlay, u64 address, u32 size, u8* out) {
        const auto& self = *static_cast<const DumpIndex*>(overlay.context);
        const u32 all = (1u << size) - 1;
        u32 mask = 0;
        for (u64 block = address >> Shift; block <= (address + size - 1) >> Shift && mask != all;
             ++block) {
            const auto it = self.head.find(block);
            if (it == self.head.end()) {
                continue;
            }
            // Newest first: a byte takes the first entry that covers it.
            for (u32 link = it->second; link && mask != all; link = self.links[link - 1].second) {
                const auto& e = overlay.entries[self.links[link - 1].first];
                const u64 lo = std::max<u64>(address, e.address);
                const u64 hi = std::min<u64>(address + size, e.address + e.size);
                for (u64 at = lo; at < hi; ++at) {
                    const u32 bit = 1u << u32(at - address);
                    if (!(mask & bit)) {
                        out[at - address] = e.data[at - e.address];
                        mask |= bit;
                    }
                }
            }
        }
        return mask;
    }
};

u32 DefaultWorkerCount() {
    if (const char* env = std::getenv("GOW3_PREP_WORKERS")) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 16));
    }
    // Workers are SCHED_IDLE and claim whole buffers, so more of them only use more idle
    // cores: half the hardware threads (Steam Deck 4, 16-thread desktop 8).
    return std::clamp<u32>(Gow3Threads::Available() / 2, 1, 8);
}

/// Flattened user data of one submission: chunks never move, so the GPU thread can read a
/// prepared draw's data while the worker keeps appending.
struct FlatArena {
    static constexpr size_t ChunkWords = 16 * 1024;
    std::vector<std::unique_ptr<u32[]>> chunks;
    size_t used = ChunkWords;

    const u32* Append(const std::vector<u32>& data) {
        return AppendArray(data.data(), data.size());
    }

    /// Trivially copyable elements, 8-byte aligned (sharps hold u64 bit fields).
    template <typename T>
    const T* AppendArray(const T* data, size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
        const size_t words = (count * sizeof(T) + sizeof(u32) - 1) / sizeof(u32);
        used = (used + 1) & ~size_t(1);
        if (chunks.empty() || words > ChunkWords - std::min(used, ChunkWords)) {
            chunks.push_back(std::make_unique<u32[]>(std::max(ChunkWords, words)));
            used = 0;
        }
        u32* dst = chunks.back().get() + used;
        if (count) {
            std::memcpy(dst, data, count * sizeof(T));
        }
        used += words;
        return reinterpret_cast<const T*>(dst);
    }
};

/// Reads the stage's resource sharps from the worker's flattened user data, so the GPU thread
/// only binds them (the sharp fetches and texture-description hashing were ~7% of it).
void PrepareResources(const Shader::Info& info, PreparedStage& stage, FlatArena& arena) {
    thread_local std::vector<AmdGpu::Image> images;
    thread_local std::vector<u64> hashes;
    thread_local std::vector<AmdGpu::Sampler> samplers;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    images.clear();
    hashes.clear();
    samplers.clear();
    buffers.clear();
    for (const auto& res : info.images) {
        images.push_back(res.GetSharp(info));
        hashes.push_back(ImageDescHash(images.back(), res));
    }
    for (const auto& res : info.samplers) {
        samplers.push_back(res.GetSharp(info));
    }
    for (const auto& res : info.buffers) {
        buffers.push_back(res.IsSpecial() ? AmdGpu::Buffer::Null() : res.GetSharp(info));
    }
    stage.image_sharps = arena.AppendArray(images.data(), images.size());
    stage.image_hashes = arena.AppendArray(hashes.data(), hashes.size());
    stage.sampler_sharps = arena.AppendArray(samplers.data(), samplers.size());
    stage.buffer_sharps = arena.AppendArray(buffers.data(), buffers.size());
    stage.num_images = static_cast<u32>(images.size());
    stage.num_samplers = static_cast<u32>(samplers.size());
    stage.num_buffers = static_cast<u32>(buffers.size());
}

/// Rasterizer::BindVertexBuffers' pure part (GraphicsPipeline::GetVertexInputs for the dynamic
/// vertex input path, and the merge of the stream memory into ranges).
void PrepareVertexInputs(const Shader::Info& vs, const Shader::Gcn::FetchShaderData& fetch,
                         const AmdGpu::Regs& regs, PreparedVertexInputs& out, FlatArena& arena) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    thread_local std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    thread_local std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    thread_local std::vector<PreparedVertexInputs::Range> ranges, merged;
    thread_local std::vector<u8> range_index;
    attributes.clear();
    bindings.clear();
    buffers.clear();
    ranges.clear();
    merged.clear();
    range_index.clear();
    for (const auto& attrib : fetch.attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs);
        // The V# may come from user data the guest is still writing: an unknown format leaves
        // the inputs to the GPU thread, which reads them once the draw is submitted (the
        // assertion in SurfaceFormat stopped the process here).
        const vk::Format format =
            LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (format == vk::Format::eUndefined &&
            buffer.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid) {
            out.valid = false;
            return;
        }
        attributes.push_back({
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = format,
            .offset = 0,
        });
        bindings.push_back({
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
            .divisor = step_rate == InstanceIdType::OverStepRate0 ? regs.vgt_instance_step_rate_0
                       : step_rate == InstanceIdType::OverStepRate1
                           ? regs.vgt_instance_step_rate_1
                           : 1u,
        });
        buffers.push_back(buffer);
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
        }
    }
    std::ranges::sort(ranges, {}, &PreparedVertexInputs::Range::base);
    for (const auto& range : ranges) {
        if (merged.empty() || merged.back().end < range.base) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }
    for (const auto& buffer : buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto it = std::ranges::find_if(merged, [&](const auto& range) {
                return buffer.base_address >= range.base && buffer.base_address < range.end;
            });
            range_index.push_back(static_cast<u8>(it - merged.begin()));
        }
    }
    out.attributes = arena.AppendArray(attributes.data(), attributes.size());
    out.bindings = arena.AppendArray(bindings.data(), bindings.size());
    out.buffers = arena.AppendArray(buffers.data(), buffers.size());
    out.range_index = arena.AppendArray(range_index.data(), range_index.size());
    out.ranges = arena.AppendArray(merged.data(), merged.size());
    out.count = static_cast<u32>(attributes.size());
    out.num_ranges = static_cast<u32>(merged.size());
    out.buffers_hash =
        buffers.empty() ? 0 : XXH3_64bits(buffers.data(), buffers.size() * sizeof(AmdGpu::Buffer));
    out.valid = true;
}

} // namespace

DrawPreparation::DrawPreparation(PipelineCache& pipeline_cache_)
    : pipeline_cache{pipeline_cache_} {
    worker_count = DefaultWorkerCount();
    if (worker_count) {
        scanner = std::jthread([this](std::stop_token stop) { ScannerLoop(stop); });
    }
    for (u32 i = 0; i < worker_count; ++i) {
        workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    }
    std::printf("GPU: draw preparation workers: %u (+1 scanner, %u hardware threads)\n",
                worker_count, Gow3Threads::Available());
}

DrawPreparation::~DrawPreparation() {
    for (auto& worker : workers) {
        worker.request_stop();
    }
    if (scanner.joinable()) {
        scanner.request_stop();
    }
    cv.notify_all();
    workers.clear();
    scanner = {};
}

std::shared_ptr<DrawPreparation::Submission> DrawPreparation::Build(
    std::span<const u32> commands, std::span<const u32> ce_commands) {
    if (!Enabled()) {
        return {};
    }
    auto submission = std::make_shared<Submission>();
    submission->commands.assign(commands.begin(), commands.end());
    if (CeSimulation() && !ce_commands.empty()) {
        AppendCe(submission->ce_commands, ce_commands, 0);
    }
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
        submission->num_draws += IsDirectDraw(header->type3.opcode);
    });
    submission->draws = std::make_unique<PreparedDraw[]>(submission->num_draws);
    return submission;
}

void DrawPreparation::Enqueue(u64 seq, std::shared_ptr<Submission> submission) {
    if (!submission) {
        return;
    }
    submission->seq = seq;
    {
        std::scoped_lock lk{mutex};
        submissions.push_back(std::move(submission));
    }
    cv.notify_all();
}

void DrawPreparation::BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum,
                                      const u8* ce_heap) {
    current_draw = 0;
    current.reset();
    if (!Enabled()) {
        return;
    }
    gpu_seq.store(seq, std::memory_order_release);
    std::scoped_lock lk{mutex};
    if (!baseline_ready) {
        initial_regs = std::make_unique<AmdGpu::Regs>(regs);
        tail_regs = std::make_unique<AmdGpu::Regs>(regs);
        initial_checksum = tail_checksum = reg_checksum;
        baseline_seq = tail_seq = seq;
        tail_heap = std::make_unique<CeHeap>();
        ce_known = ce_heap != nullptr;
        if (ce_heap) {
            std::memcpy(tail_heap->data(), ce_heap, CeHeapSize);
        }
        baseline_ready = true;
        cv.notify_all();
    }
    // Idle helpers can starve on a busy CPU. Rather than queueing without bound, restart
    // them from the GPU thread's own state (one register copy) once the scanner lags.
    constexpr u64 MaxScanLag = 64;
    if (!submissions.empty() && seq > tail_seq + MaxScanLag &&
        submissions.front()->seq < seq && !submissions.front()->scanned.load()) {
        *tail_regs = regs;
        tail_seq = seq;
        tail_checksum = reg_checksum;
        if (ce_heap && tail_heap) {
            std::memcpy(tail_heap->data(), ce_heap, CeHeapSize);
        }
        while (!submissions.empty() && submissions.front()->seq < seq) {
            submissions.pop_front();
        }
        ++rebases;
        cv.notify_all();
    }
    if (!submissions.empty() && seq >= submissions.front()->seq &&
        seq - submissions.front()->seq < submissions.size()) {
        current = submissions[seq - submissions.front()->seq];
    }
}

void DrawPreparation::EndSubmission() {
    if (current) {
        current->gpu_done.store(true, std::memory_order_release);
        current.reset();
    }
    if (Enabled()) {
        Collect();
    }
}

const PreparedDraw* DrawPreparation::NextDraw() {
    if (!current || current_draw >= current->num_draws) {
        return nullptr;
    }
    const PreparedDraw* draw = &current->draws[current_draw++];
    if (Gow3Toggle::Disabled(Gow3Toggle::DrawPreparation)) {
        return nullptr;
    }
    return draw;
}

void DrawPreparation::Collect() {
    std::scoped_lock lk{mutex};
    while (!submissions.empty()) {
        const auto& front = submissions.front();
        if (!baseline_ready || !front->gpu_done.load(std::memory_order_acquire)) {
            break;
        }
        if (front->seq >= baseline_seq) {
            // Its delta moves the restart state to the start of the next buffer.
            if (!front->scanned.load(std::memory_order_acquire)) {
                break;
            }
            front->delta.Apply(*tail_regs);
            tail_seq = front->seq + 1;
            tail_checksum = front->end_checksum;
        }
        submissions.pop_front();
    }
}

void DrawPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("gow3:DrawScan");
    Gow3Threads::MakeBackground();
    std::unique_ptr<AmdGpu::Regs> regs;
    std::unique_ptr<CeHeap> heap;
    u64 checksum = 0;
    u64 next_seq = 0;
    {
        std::unique_lock lk{mutex};
        cv.wait(lk, stop, [&] { return baseline_ready; });
        if (stop.stop_requested()) {
            return;
        }
        regs = std::make_unique<AmdGpu::Regs>(*initial_regs);
        checksum = initial_checksum;
        next_seq = baseline_seq;
        heap = std::make_unique<CeHeap>(*tail_heap);
    }
    AmdGpu::RegDirty dirty;
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, stop, [&] {
                return next_seq < tail_seq ||
                       (!submissions.empty() && submissions.back()->seq >= next_seq);
            });
            if (stop.stop_requested()) {
                return;
            }
            if (next_seq < tail_seq) {
                // Rebased by the GPU thread: continue from its state.
                *regs = *tail_regs;
                checksum = tail_checksum;
                next_seq = tail_seq;
                *heap = *tail_heap;
                continue;
            }
            // Unscanned buffers are only collected by a rebase, so `next_seq` is still queued.
            submission = submissions[next_seq - submissions.front()->seq];
        }
        submission->start_checksum = checksum;
        dirty.Clear();
        CeSim ce{*heap, *submission};
        u32 de_count = 0, draw_ordinal = 0;
        const bool simulate = ce_known && !submission->ce_commands.empty();
        if (simulate) {
            ce.Run(de_count, draw_ordinal);
        }
        ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, header, checksum, &dirty);
            if (!simulate) {
                return;
            }
            switch (header->type3.opcode) {
            case AmdGpu::PM4ItOpcode::IncrementDeCounter:
                ++de_count;
                break;
            case AmdGpu::PM4ItOpcode::WaitOnCeCounter:
                while (ce.ce_count <= de_count && !ce.done && ce.Run(de_count, draw_ordinal)) {
                }
                break;
            default:
                draw_ordinal += IsDirectDraw(header->type3.opcode);
                break;
            }
        });
        if (simulate) {
            // What remains runs when the GPU thread reaches the end (drawn by nothing here).
            while (!ce.done && ce.Run(de_count, draw_ordinal)) {
            }
        }
        submission->delta.Capture(*regs, dirty);
        submission->end_checksum = checksum;
        {
            std::scoped_lock lk{mutex};
            submission->scanned.store(true, std::memory_order_release);
        }
        ++next_seq;
        cv.notify_all();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 index) {
    Common::SetCurrentThreadName(("gow3:DrawPrep" + std::to_string(index)).c_str());
    Gow3Threads::MakeBackground();
    auto regs = std::make_unique<AmdGpu::Regs>();
    constexpr u64 NoPosition = ~0ull;
    u64 position = NoPosition; ///< `regs` holds the state at the start of this buffer
    PipelineSelection sel{};
    sel.regs = regs.get();
    PrepWorker prep_worker{};
    sel.worker = &prep_worker;
    FlatArena arena;
    std::vector<std::shared_ptr<Submission>> path;
    // gow3: constant engine dumps between the GPU thread and the draw being prepared.
    std::vector<std::shared_ptr<Submission>> ce_sources;
    std::vector<Shader::SrtOverlayEntry> ce_entries;
    Shader::SrtOverlay ce_overlay{};
    DumpIndex ce_index;

    // The nearest scanned buffer the GPU thread has not reached and no worker has claimed.
    const auto claimable = [&]() -> std::shared_ptr<Submission> {
        const u64 gpu = gpu_seq.load(std::memory_order_acquire);
        for (const auto& submission : submissions) {
            if (submission->seq >= gpu && submission->seq >= tail_seq &&
                submission->scanned.load(std::memory_order_acquire) &&
                !submission->claimed.load(std::memory_order_relaxed)) {
                return submission;
            }
        }
        return {};
    };

    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> target;
        {
            std::unique_lock lk{mutex};
            // gpu_seq moves without the lock: keep the candidate the predicate found.
            cv.wait(lk, stop, [&] {
                target = baseline_ready ? claimable() : nullptr;
                return target != nullptr;
            });
            if (stop.stop_requested() || !target) {
                return;
            }
            target->claimed.store(true, std::memory_order_relaxed);
            // Buffers between this worker's state and the target: their deltas. A position
            // already collected (or past the target) restarts from the queue's tail state.
            if (position == NoPosition || position < tail_seq || position > target->seq) {
                *regs = *tail_regs;
                position = tail_seq;
            }
            const u64 front = submissions.front()->seq;
            path.clear();
            for (u64 seq = position; seq < target->seq; ++seq) {
                path.push_back(submissions[seq - front]);
            }
            // Earlier buffers the GPU thread has not finished: all their dumps come first.
            ce_sources.clear();
            const u64 gpu = gpu_seq.load(std::memory_order_acquire);
            for (u64 seq = std::max(gpu, front); seq < target->seq; ++seq) {
                ce_sources.push_back(submissions[seq - front]);
            }
        }
        ce_entries.clear();
        ce_index.Clear();
        ce_overlay = {};
        ce_overlay.lookup = &DumpIndex::Lookup;
        ce_overlay.context = &ce_index;
        const auto add_dump = [&](const Submission& from, const Submission::Dump& dump) {
            ce_entries.push_back({dump.address, dump.size, from.dump_bytes.data() + dump.offset});
            ce_index.Add(ce_entries.back(), u32(ce_entries.size() - 1));
            ce_overlay.lo = std::min(ce_overlay.lo, dump.address);
            ce_overlay.hi = std::max(ce_overlay.hi, dump.address + dump.size);
        };
        for (const auto& source : ce_sources) {
            for (const auto& dump : source->dumps) {
                add_dump(*source, dump);
            }
        }
        size_t next_dump = 0;
        for (const auto& step : path) {
            step->delta.Apply(*regs);
        }
        path.clear();
        u64 checksum = target->start_checksum;
        u32 ordinal = 0;
        arena = FlatArena{};
        ForEachPacket(target->commands, [&](const AmdGpu::PM4Header* header) {
            AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(*regs, header, checksum);
            if (!IsDirectDraw(header->type3.opcode)) {
                return;
            }
            // Past the GPU thread the draws are no longer worth preparing.
            if (ordinal < target->num_draws &&
                target->seq >= gpu_seq.load(std::memory_order_acquire)) {
                auto& draw = target->draws[ordinal];
                prep_worker.stages.clear();
                prep_worker.failed = false;
                sel.draw_indirect_params = {};
                while (next_dump < target->dumps.size() &&
                       target->dumps[next_dump].visible_from <= ordinal) {
                    add_dump(*target, target->dumps[next_dump++]);
                }
                ce_overlay.entries = ce_entries.data();
                ce_overlay.count = ce_entries.size();
                Shader::g_srt_overlay = ce_overlay.count ? &ce_overlay : nullptr;
                const bool prepared_ok = pipeline_cache.PrepareGraphicsPipeline(sel);
                Shader::g_srt_overlay = nullptr;
                if (prepared_ok) {
                    draw.reg_checksum = checksum;
                    draw.key = sel.graphics_key;
                    draw.num_stages = static_cast<u32>(prep_worker.stages.size());
                    for (u32 i = 0; i < draw.num_stages; ++i) {
                        const auto& stage = prep_worker.stages[i];
                        draw.stages[i] = {
                            .program = stage.program,
                            .hash = stage.hash,
                            .hw_stage = stage.hw_stage,
                            .pgm_base = stage.pgm_base,
                            .flat = arena.Append(*stage.flat),
                            .flat_size = static_cast<u32>(stage.flat->size()),
                        };
                        PrepareResources(prep_worker.infos.at(stage.program), draw.stages[i],
                                         arena);
                    }
                    draw.vertex.valid = false;
                    const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
                    if (vs && sel.fetch_shader) {
                        PrepareVertexInputs(*vs, *sel.fetch_shader, *regs, draw.vertex, arena);
                    }
                    draw.state.store(PreparedDraw::Ready, std::memory_order_release);
                } else {
                    draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
                }
            }
            ++ordinal;
        });
        // The arena's chunks live as long as the submission.
        target->flat_chunks = std::move(arena.chunks);
        position = target->seq + 1;
    }
}

void DrawPreparation::Count(bool was_used) {
    ++(was_used ? used : unused);
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    // The clock is read every 1024 draws: per draw it was 8% of the GPU thread.
    if (!stats || ((used + unused) & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    std::printf("Draw preparation: %llu prepared pipelines used, %llu not (%u workers, "
                "%llu scanner rebases)\n",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(unused),
                worker_count, static_cast<unsigned long long>(rebases));
    const auto& m = g_gow3_prep_miss;
    std::printf("Draw preparation misses: pending %llu, unavailable %llu, registers %llu, "
                "program %llu, user data %llu, pipeline %llu, not prepared %llu; "
                "waited for %llu (%llu became ready)\n",
                (unsigned long long)m[0], (unsigned long long)m[1], (unsigned long long)m[2],
                (unsigned long long)m[3], (unsigned long long)m[4], (unsigned long long)m[5],
                (unsigned long long)m[6], (unsigned long long)g_gow3_prep_waited,
                (unsigned long long)g_gow3_prep_wait_hits);
    {
        std::array<std::pair<u32, u32>, 6> top{};
        for (u32 line = 0; line < g_gow3_prep_fail_lines.size(); ++line) {
            const u32 n = g_gow3_prep_fail_lines[line].exchange(0, std::memory_order_relaxed);
            if (n > top.back().second) {
                top.back() = {line, n};
                std::sort(top.begin(), top.end(), [](auto a, auto b) { return a.second > b.second; });
            }
        }
        std::printf("Draw preparation gives up at vk_pipeline_cache.cpp lines:");
        for (const auto& [line, n] : top) {
            if (n) std::printf(" %u x%u", line, n);
        }
        const auto& f = g_gow3_prep_flat_stage;
        std::printf("; user data differs by hw stage: ps %llu vs %llu gs %llu es %llu hs %llu ls %llu\n",
                    (unsigned long long)f[0], (unsigned long long)f[1], (unsigned long long)f[2],
                    (unsigned long long)f[3], (unsigned long long)f[4], (unsigned long long)f[5]);
        g_gow3_prep_flat_stage.fill(0);
        std::printf("Draw preparation: %llu constant engine dumps simulated, %llu not (%s)\n",
                    (unsigned long long)g_gow3_prep_ce_dumps.exchange(0),
                    (unsigned long long)g_gow3_prep_ce_bad.exchange(0),
                    CeSimulation() ? "GOW3_PREP_CE on" : "GOW3_PREP_CE=0");
    }
    g_gow3_prep_miss.fill(0);
    g_gow3_prep_waited = g_gow3_prep_wait_hits = 0;
    used = unused = 0;
}

} // namespace Vulkan
