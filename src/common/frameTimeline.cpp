#include "common/frameTimeline.h"
#include "common/hostPath.h"
#include "common/timer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Common::FrameTimeline {
namespace {
constexpr size_t Capacity = 1048576;
constexpr uint32_t FrameLimit = 300;
struct Record {
    uint64_t time, frame, submission, tick, a, b, c, d;
    uint32_t tid;
    Kind kind;
};
struct State {
    std::vector<Record> records;
    std::atomic<size_t> head {0}, dropped {0};
    std::atomic<uint32_t> writers {0}, frames {0};
    std::mutex lifecycle;
    std::jthread worker;
    uint64_t frequency = 0, origin = 0;
    bool started = false;
};
// The quick_exit path skips normal destructors. Keep state valid until process
// exit, and explicitly stop/join the writer there as well as on GPU shutdown.
State& Get() { static State* state = new State; return *state; }
const char* KindName(Kind kind) {
    static constexpr const char* names[] = {
        "thread_name", "enqueue", "suspend_enter", "suspend_return", "suspend_release",
        "slice_begin", "slice_end", "cpu_frame_end", "frame_boundary",
        "flip_link", "flip_ready", "flip_presented", "cpu_flip_call",
        "native_submit", "native_begin", "native_end", "calibration", "native_unavailable",
        "wait_reg_mem_blocked", "wait_reg_mem_passed", "wait_flip_blocked", "wait_flip_passed",
        "equeue_begin", "equeue_end", "equeue_event", "event_trigger",
        "vblank_begin", "vblank_end", "priority_begin", "priority_end",
        "priority_drain_begin", "priority_drain_end", "label_write", "clock_publish",
        "frame_native_begin", "frame_native_end", "frame_cpu_complete", "native_wait_begin", "native_wait_end"
    };
    return names[static_cast<size_t>(kind)];
}
void JsonString(FILE* file, const char* text) {
    std::fputc('"', file);
    for (; *text; ++text) {
        const auto ch = static_cast<unsigned char>(*text);
        if (ch == '"' || ch == '\\') std::fputc('\\', file);
        if (ch < 32) std::fprintf(file, "\\u%04x", unsigned(ch));
        else std::fputc(ch, file);
    }
    std::fputc('"', file);
}
void Save(const char* reason) {
    auto& state = Get();
    active.store(false, std::memory_order_seq_cst);
    while (state.writers.load(std::memory_order_seq_cst)) std::this_thread::yield();
    const auto size = std::min(state.head.load(), Capacity);
    state.records.resize(size);
    struct Frame {
        uint64_t enqueue = 0, suspend = 0, begin = 0, end = 0, native_begin = 0, native_end = 0;
        uint64_t ready = 0, present = 0, native_count = 0, native_submits = 0, t6_suspend = 0;
        uint64_t queued = 0, completed = 0, boundary = 0;
        uint64_t flip_count = 0;
    };
    std::map<uint64_t, Frame> frames;
    std::map<uint64_t, uint64_t> flips;
    std::set<uint64_t> flip_frames;
    uint64_t first_boundary = UINT64_MAX, last_boundary = 0;
    auto first = [](uint64_t& dst, uint64_t t) { if (!dst || t < dst) dst = t; };
    for (const auto& r: state.records) {
        if (r.kind == Kind::FlipLink) { flips[r.a] = r.frame; flip_frames.insert(r.frame); }
        if (r.kind == Kind::FrameBoundary) {
            first_boundary = std::min(first_boundary, r.frame);
            last_boundary = std::max(last_boundary, r.frame);
        }
    }
    const auto interior = [&](uint64_t id) { return id > first_boundary && id <= last_boundary; };
    for (const auto& [request, frame]: flips) ++frames[frame].flip_count;
    // Resolve request IDs independently of event arrival order. Native results
    // are harvested later but carry their calibrated execution time.
    for (auto& r: state.records) {
        if (r.kind == Kind::NativeBegin || r.kind == Kind::NativeEnd) r.tid = 0; // Native GPU lane, not query-harvesting CPU TID.
        if ((r.kind == Kind::FlipReady || r.kind == Kind::FlipPresented) && flips.contains(r.a)) r.frame = flips[r.a];
        if (r.kind == Kind::SuspendReturn && r.a) frames[r.a].t6_suspend = r.time;
        if (!r.frame) continue;
        auto& f = frames[r.frame];
        switch (r.kind) {
        case Kind::Enqueue: first(f.enqueue, r.time); ++f.queued; break;
        case Kind::SuspendEnter: first(f.suspend, r.time); break;
        case Kind::SliceBegin: first(f.begin, r.time); break;
        case Kind::SliceEnd: f.end = std::max(f.end, r.time); f.completed += r.a != 0; break;
        case Kind::FrameBoundary: f.boundary = r.time; break;
        case Kind::NativeSubmit: ++f.native_submits; break;
        case Kind::NativeBegin: first(f.native_begin, r.time); ++f.native_count; break;
        case Kind::NativeEnd: f.native_end = std::max(f.native_end, r.time); break;
        case Kind::FlipReady: f.ready = std::max(f.ready, r.time); break;
        case Kind::FlipPresented: f.present = std::max(f.present, r.time); break;
        default: break;
        }
    }
    for (const auto& [id, f]: frames) {
        if (f.boundary && f.queued == f.completed && f.end) {
            state.records.push_back({f.end, id, 0, 0, 0, 0, 0, 0, 0, Kind::FrameCpuComplete});
        }
        if (f.native_begin && f.native_end) {
            state.records.push_back({f.native_begin, id, 0, 0, 0, 0, 0, 0, 0, Kind::FrameNativeBegin});
            state.records.push_back({f.native_end, id, 0, 0, 0, 0, 0, 0, 0, Kind::FrameNativeEnd});
        }
    }
    std::stable_sort(state.records.begin(), state.records.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    const char* configured = std::getenv("KYTY_FRAME_TIMELINE_FILE");
    const auto path = NativeHostPath(configured && *configured ? configured : "frame-timeline.jsonl");
#if defined(_WIN32)
    FILE* file = _wfopen(path.c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) { std::fprintf(stderr, "[FRAME_TIMELINE] cannot open output file\n"); return; }
    std::fprintf(file, "{\"type\":\"header\",\"schema\":1,\"clock\":\"QPC\",\"frequency\":%llu,\"origin\":%llu,"
        "\"frame_boundaries\":%u,\"events\":%zu,\"dropped\":%zu,\"reason\":\"%s\","
        "\"first_boundary\":%llu,\"last_boundary\":%llu,"
        "\"inflight_scope\":\"interior epochs only; ready/present count epochs with a linked flip; native observed envelopes\"}\n",
        (unsigned long long)state.frequency, (unsigned long long)state.origin, state.frames.load(), size, state.dropped.load(), reason,
        (unsigned long long)first_boundary, (unsigned long long)last_boundary);
    const auto ms = [&](uint64_t t) { return t ? (static_cast<long double>(t) - state.origin) * 1000.0 / state.frequency : -1.0L; };
    std::set<uint64_t> enqueued, cpu_done, ready, presented, native, cpu_pending, ready_pending, present_pending;
    std::set<uint64_t> ready_requests, presented_requests;
    std::map<uint64_t, uint64_t> ready_counts, presented_counts;
    for (const auto& r: state.records) {
        if (interior(r.frame)) {
            if (r.kind == Kind::Enqueue && enqueued.insert(r.frame).second) {
                if (!cpu_done.contains(r.frame)) cpu_pending.insert(r.frame);
                if (flip_frames.contains(r.frame) && !ready.contains(r.frame)) ready_pending.insert(r.frame);
                if (flip_frames.contains(r.frame) && !presented.contains(r.frame)) present_pending.insert(r.frame);
            }
            if (r.kind == Kind::FrameCpuComplete) { cpu_done.insert(r.frame); cpu_pending.erase(r.frame); }
            if (r.kind == Kind::FlipReady && ready_requests.insert(r.a).second && ++ready_counts[r.frame] == frames[r.frame].flip_count) {
                ready.insert(r.frame); ready_pending.erase(r.frame);
            }
            if (r.kind == Kind::FlipPresented && presented_requests.insert(r.a).second && ++presented_counts[r.frame] == frames[r.frame].flip_count) {
                presented.insert(r.frame); present_pending.erase(r.frame);
            }
            if (r.kind == Kind::FrameNativeBegin) native.insert(r.frame);
            if (r.kind == Kind::FrameNativeEnd) native.erase(r.frame);
        }
        std::fprintf(file, "{\"type\":\"event\",\"event\":\"%s\",\"ms\":%.6f,\"tid\":%u,\"frame\":%llu,"
            "\"submission\":%llu,\"tick\":%llu,\"a\":%llu,\"b\":%llu,\"c\":%llu,\"d\":%llu,"
            "\"cpu_pending\":%zu,\"native_inflight\":%zu,\"ready_pending\":%zu,\"present_pending\":%zu",
            KindName(r.kind), double(ms(r.time)), r.tid, (unsigned long long)r.frame, (unsigned long long)r.submission,
            (unsigned long long)r.tick, (unsigned long long)r.a, (unsigned long long)r.b, (unsigned long long)r.c,
            (unsigned long long)r.d, cpu_pending.size(), native.size(), ready_pending.size(), present_pending.size());
        if (r.kind == Kind::ThreadName) {
            char name[33] {}; std::memcpy(name, &r.a, 32);
            std::fputs(",\"name\":", file); JsonString(file, name);
        }
        std::fputs("}\n", file);
    }
    for (const auto& [id, f]: frames) {
        std::fprintf(file, "{\"type\":\"frame\",\"frame\":%llu,\"t0_enqueue_ms\":%.6f,\"t0_suspend_ms\":%.6f,"
            "\"t1_ms\":%.6f,\"t2_ms\":%.6f,\"t3_ms\":%.6f,\"t4_ms\":%.6f,\"t5_ms\":%.6f,"
            "\"present_ms\":%.6f,\"t6_suspend_return_ms\":%.6f,\"native_buffers\":%llu,\"native_submits\":%llu,"
            "\"queued\":%llu,\"completed\":%llu,\"flips\":%llu,\"boundary_seen\":%u,\"interior\":%u,\"native_coverage_complete\":%u}\n", (unsigned long long)id,
            double(ms(f.enqueue)), double(ms(f.suspend)), double(ms(f.begin)),
            double(ms(f.boundary && f.queued == f.completed ? f.end : 0)),
            double(ms(f.native_begin)), double(ms(f.native_end)), double(ms(f.ready)), double(ms(f.present)),
            double(ms(f.t6_suspend)), (unsigned long long)f.native_count, (unsigned long long)f.native_submits,
            (unsigned long long)f.queued, (unsigned long long)f.completed, (unsigned long long)f.flip_count, unsigned(f.boundary != 0), unsigned(interior(id)),
            unsigned(interior(id) && f.queued == f.completed && f.native_count && f.native_count == f.native_submits));
    }
    const bool ok = std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    std::fprintf(stderr, "[FRAME_TIMELINE] saved=%u frames=%u events=%zu dropped=%zu reason=%s\n",
        unsigned(ok && closed), state.frames.load(), size, state.dropped.load(), reason);
}
void Run(std::stop_token stop) {
    auto& state = Get();
    const auto trigger = NativeHostPath("frame-timeline.trigger");
    std::error_code error;
    const auto old_stamp = std::filesystem::last_write_time(trigger, error);
    const bool existed = !error;
    std::fprintf(stderr, "[FRAME_TIMELINE] armed trigger=frame-timeline.trigger frames=300 existing_marker_ignored=1\n");
    while (!stop.stop_requested()) {
        const auto stamp = std::filesystem::last_write_time(trigger, error);
        if (!error && (!existed || stamp != old_stamp)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop.stop_requested()) return;
    // Preallocate before exposing the capture to producers. No file I/O,
    // allocation or diagnostic mutex on the guest/GPU/completion hot paths.
    state.records.resize(Capacity);
    state.frequency = Timer::QueryPerformanceFrequency();
    state.origin = Now();
    active.store(true, std::memory_order_seq_cst);
    std::fprintf(stderr, "[FRAME_TIMELINE] capturing\n");
    uint64_t drain_start = 0;
    const char* reason = "shutdown";
    while (!stop.stop_requested()) {
        const auto now = Now();
        if (!drain_start && state.frames.load() >= FrameLimit) drain_start = now;
        if (drain_start && now - drain_start >= state.frequency * 2) { reason = "300_frames_plus_2s_drain"; break; }
        if (now - state.origin >= state.frequency * 45) { reason = "45s_timeout"; break; }
        if (state.head.load() >= Capacity) { reason = "event_capacity"; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Save(reason);
}
} // namespace
bool Enabled() {
    static const bool enabled = [] { const auto* s = std::getenv("KYTY_FRAME_TIMELINE"); return s && std::strcmp(s, "1") == 0; }();
    return enabled;
}
uint64_t Now() { return Timer::QueryPerformanceCounter(); }
void Start() {
    auto& state = Get();
    std::lock_guard lock(state.lifecycle);
    if (state.started) return;
    state.started = true;
    std::at_quick_exit(Stop);
    state.worker = std::jthread(Run);
}
void Stop() {
    auto& state = Get();
    std::lock_guard lock(state.lifecycle);
    if (!state.worker.joinable()) return;
    state.worker.request_stop();
    state.worker.join();
}
void Emit(Kind kind, uint64_t frame, uint64_t submission, uint64_t tick,
          uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t at) {
    auto& state = Get();
    state.writers.fetch_add(1, std::memory_order_seq_cst);
    if (active.load(std::memory_order_seq_cst)) {
        const auto slot = state.head.fetch_add(1, std::memory_order_relaxed);
        if (slot < Capacity) {
#if defined(_WIN32)
            const uint32_t tid = GetCurrentThreadId();
#else
            const uint32_t tid = static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
            state.records[slot] = {at ? at : Now(), frame, submission, tick, a, b, c, d, tid, kind};
            if (kind == Kind::FrameBoundary) state.frames.fetch_add(1, std::memory_order_relaxed);
        } else state.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    state.writers.fetch_sub(1, std::memory_order_seq_cst);
}
void NameImpl(const char* name) {
    if (!Recording()) return;
    if (thread_named) return;
    thread_named = true;
    uint64_t words[4] {};
    std::memcpy(words, name, std::min(std::strlen(name), size_t{31}));
    Event(Kind::ThreadName, 0, 0, 0, words[0], words[1], words[2], words[3]);
}
} // namespace Common::FrameTimeline
