#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Diagnostic only. Registration occurs on thread startup, never in an exception.
// Fixed, non-reused rows keep sampler identities safe across exits/TID reuse.
namespace Common::GuestCpuProfile {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
enum class Part : size_t { Other, MappedLookup, Buffer, Texture, ImageHeap, TrackerLookup,
    TrackingLock, TextureLock, ImageHeapLock, PageLock, MapLock, VirtualProtect, TextureLookup, BdaLock, ReadbackWait, Count };
inline constexpr std::array PartNames {"other", "mapped_lookup", "buffer", "texture", "image_heap",
    "tracker_lookup", "tracking_lock", "texture_lock", "image_heap_lock", "page_lock", "map_lock", "virtual_protect",
    "texture_lookup", "bda_lock", "readback_wait"};
inline constexpr size_t Parts = static_cast<size_t>(Part::Count);
static_assert(Parts == PartNames.size());
struct Totals {
    uint64_t calls = 0, ticks = 0, detail_calls = 0, detail_ticks = 0, unhandled = 0, dropped = 0;
    std::array<uint64_t, Parts> parts {}, events {};
};
// Exact fault counts, not uploaded bytes or sampled addresses. One producer per
// thread; fixed open addressing, no eviction, allocation, lock or I/O in Add.
// A bucket covers 64 KiB, with a separate counter for every 4 KiB page.
template <size_t Slots = 2048>
struct FaultPages {
    static_assert(Slots > 0);
    struct Bucket { std::atomic<uint64_t> tag {0}; std::array<std::atomic<uint64_t>, 16> calls {}; };
    std::array<Bucket, Slots> buckets {};
    std::atomic<uint64_t> dropped {0};
    static uint64_t Hash(uint64_t x) {
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    void Add(uint64_t address) {
        const auto tag = (address >> 16) + 1;
        const auto first = Hash(tag) % Slots;
        for (size_t n = 0; n < Slots && n < 16; ++n) {
            auto& bucket = buckets[(first + n) % Slots];
            auto key = bucket.tag.load(std::memory_order_acquire);
            if (!key) { bucket.tag.store(tag, std::memory_order_release); key = tag; }
            if (key != tag) continue;
            bucket.calls[(address >> 12) & 15].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    template <typename Func> void ForEach(Func&& func) const {
        for (const auto& bucket: buckets) {
            const auto tag = bucket.tag.load(std::memory_order_acquire);
            if (!tag) continue;
            for (size_t i = 0; i < 16; ++i) {
                const auto calls = bucket.calls[i].load(std::memory_order_relaxed);
                if (calls) func(((tag - 1) << 16) + i * 4096, calls);
            }
        }
    }
};
struct Row {
    std::atomic<bool> ready {false}, alive {false};
    uint32_t tid = 0;
    uint64_t creation = 0, guest_low = 0, guest_high = 0, host_low = 0, host_high = 0;
    char name[96] {};
    std::atomic<uint64_t> sequence {0};
    std::atomic<uint64_t> calls {0}, ticks {0}, detail_calls {0}, detail_ticks {0}, unhandled {0}, dropped {0};
    std::array<std::atomic<uint64_t>, Parts> parts {}, events {};
    std::unique_ptr<FaultPages<>> pages; // allocated at registration, before ready
};
inline std::array<Row, 1024> rows;
inline std::atomic<size_t> next_row {0};
inline std::atomic<uint64_t> overflow {0}, epoch {0};
inline std::atomic<bool> recording {false};
inline thread_local Row* self = nullptr;
inline thread_local unsigned detail_sequence = 0;
inline bool EnvOn(const char* name) { const auto* v = std::getenv(name); return v && std::strcmp(v, "1") == 0; }
inline bool FaultEnabled() { static const bool value = EnvOn("KYTY_WRITE_FAULT_PROFILE"); return value; }
inline bool AddressesEnabled() { static const bool value = EnvOn("KYTY_WRITE_FAULT_ADDRESSES"); return value; }
struct BufferOwners {
    struct Range { uint64_t base = 0, size = 0; };
    std::array<Range, 8192> ranges;
    size_t count = 0, dropped = 0;
    uint64_t captured_ticks = 0;
    std::atomic<uint64_t> requested {0}, published {0};
    // GPU lane only, in a frame boundary. Immutable after release publication
    // until the reporting worker requests a NEW window, after finishing reads.
    void Add(uint64_t base, uint64_t size) {
        if (count < ranges.size()) ranges[count++] = {base, size}; else ++dropped;
    }
};
inline bool ContainsFaultPage(uint64_t base, uint64_t size, uint64_t page) {
    return page >= base && page - base <= size && size - (page - base) >= 4096;
}
inline std::array<BufferOwners, 2> buffer_owners; // beginning / end of the same window
inline unsigned GuestCount() {
    static const unsigned count = [] {
        const auto* value = std::getenv("KYTY_GUEST_THREAD_SAMPLER");
        if (!value) return 0u;
        char* end = nullptr; const auto n = std::strtoul(value, &end, 10);
        return end != value && !*end && n > 0 ? static_cast<unsigned>(n > 8 ? 8 : n) : 0u;
    }();
    return count;
}
inline unsigned DetailPeriod() {
    static const unsigned period = [] {
        const auto* value = std::getenv("KYTY_WRITE_FAULT_DETAIL_PERIOD");
        if (!value) return 64u;
        char* end = nullptr; const auto n = std::strtoul(value, &end, 10);
        return end != value && !*end && n <= 65536 ? static_cast<unsigned>(n) : 64u;
    }();
    return period; // 0 = entry/exit clocks only, 1 = every fault's detailed stages.
}
inline uint64_t Now() {
#if defined(_WIN32)
    LARGE_INTEGER value {}; QueryPerformanceCounter(&value); return value.QuadPart;
#else
    return 0;
#endif
}
inline uint64_t Frequency() {
#if defined(_WIN32)
    LARGE_INTEGER value {}; QueryPerformanceFrequency(&value); return value.QuadPart;
#else
    return 1;
#endif
}
inline uint64_t FileTimeTicks(uint32_t low, uint32_t high) { return uint64_t{low} | (uint64_t{high} << 32); }
inline void Register(const char* name, uint64_t guest_low, uint64_t guest_size) {
#if defined(_WIN32)
    if (self || (!FaultEnabled() && !GuestCount())) return;
    (void)DetailPeriod();
    const auto index = next_row.fetch_add(1);
    if (index >= rows.size()) { ++overflow; return; }
    auto& row = rows[index];
    row.tid = GetCurrentThreadId();
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) { ++overflow; return; }
    row.creation = FileTimeTicks(created.dwLowDateTime, created.dwHighDateTime);
    row.guest_low = guest_low; row.guest_high = guest_low + guest_size;
    if (FaultEnabled() && AddressesEnabled()) row.pages = std::make_unique<FaultPages<>>();
    ULONG_PTR low = 0, high = 0; GetCurrentThreadStackLimits(&low, &high);
    MEMORY_BASIC_INFORMATION memory {};
    if (VirtualQuery(&memory, &memory, sizeof(memory))) low = reinterpret_cast<uintptr_t>(memory.AllocationBase);
    row.host_low = low; row.host_high = high;
    // Keep log lines parseable even for arbitrary guest names.
    for (size_t i = 0; name && name[i] && i + 1 < sizeof(row.name); ++i)
        row.name[i] = name[i] >= ' ' && name[i] != '"' && name[i] != '\\' ? name[i] : '_';
    self = &row;
    row.alive.store(true); row.ready.store(true, std::memory_order_release);
#endif
}
inline void Unregister() { if (self) self->alive.store(false); self = nullptr; }

struct GuestModule {
    std::atomic<bool> ready {false}, alive {false};
    uint64_t base = 0, size = 0;
    char name[128] {};
};
inline std::array<GuestModule, 512> modules;
inline std::atomic<size_t> next_module {0};
inline void RegisterModule(uint64_t base, uint64_t size, const char* name) {
    if (!GuestCount() || !size) return;
    const auto index = next_module.fetch_add(1);
    if (index >= modules.size()) return;
    auto& m = modules[index]; m.base = base; m.size = size;
    for (size_t i = 0; name && name[i] && i + 1 < sizeof(m.name); ++i)
        m.name[i] = name[i] >= ' ' && name[i] != '"' && name[i] != '\\' ? name[i] : '_';
    m.alive = true; m.ready.store(true, std::memory_order_release);
}
inline void UnregisterModule(uint64_t base) {
    if (!GuestCount()) return;
    for (auto& m: modules) if (m.ready.load(std::memory_order_acquire) && m.base == base) m.alive = false;
}

struct Frame {
    Part part = Part::Other;
    uint64_t last = 0;
    std::array<uint64_t, Parts> parts {}, events {};
};
inline thread_local Frame* current = nullptr;
inline thread_local std::array<Frame, 8> scratch;
inline thread_local size_t detail_depth = 0;
class Stage {
public:
    explicit Stage(Part part): frame(current), part(part) {
        if (!frame) return;
        previous = frame->part; const auto now = Now();
        frame->parts[static_cast<size_t>(previous)] += now - frame->last;
        ++frame->events[static_cast<size_t>(part)]; frame->part = part; frame->last = now;
    }
    void End() {
        if (!frame) return;
        const auto now = Now(); frame->parts[static_cast<size_t>(part)] += now - frame->last;
        frame->part = previous; frame->last = now; frame = nullptr;
    }
    ~Stage() { End(); }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
private:
    Frame* frame;
    Part part, previous = Part::Other;
};
class Fault {
public:
    explicit Fault(bool write, uint64_t address = 0): address(address) {
        if (!write || !self || !recording.load(std::memory_order_relaxed)) return;
        row = self; generation = epoch.load(); start = Now();
        const auto period = DetailPeriod(); detail = period && (++detail_sequence % period == 0) && detail_depth < scratch.size();
        if (detail) { previous = current; frame = &scratch[detail_depth++]; *frame = {}; frame->last = start; current = frame; }
    }
    void Handled() { handled = true; }
    ~Fault() {
        if (!row) return;
        const auto end = Now();
        if (detail) { frame->parts[static_cast<size_t>(frame->part)] += end - frame->last; current = previous; --detail_depth; }
        if (generation != epoch.load() || !recording.load()) { ++row->dropped; return; }
        row->sequence.fetch_add(1, std::memory_order_acq_rel);
        if (handled) {
            ++row->calls; row->ticks.fetch_add(end - start);
            if (row->pages) row->pages->Add(address);
            if (detail) {
                ++row->detail_calls; row->detail_ticks.fetch_add(end - start);
                for (size_t i = 0; i < Parts; ++i) { row->parts[i].fetch_add(frame->parts[i]); row->events[i].fetch_add(frame->events[i]); }
            }
        } else ++row->unhandled;
        row->sequence.fetch_add(1, std::memory_order_release);
    }
    Fault(const Fault&) = delete;
    Fault& operator=(const Fault&) = delete;
private:
    Row* row = nullptr;
    uint64_t generation = 0, start = 0;
    uint64_t address = 0;
    bool detail = false, handled = false;
    Frame* frame = nullptr;
    Frame* previous = nullptr;
};
inline bool Read(const Row& row, Totals& result) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto before = row.sequence.load(std::memory_order_acquire);
        if (before & 1) continue;
        result = {row.calls.load(), row.ticks.load(), row.detail_calls.load(), row.detail_ticks.load(), row.unhandled.load(), row.dropped.load()};
        for (size_t i = 0; i < Parts; ++i) { result.parts[i] = row.parts[i].load(); result.events[i] = row.events[i].load(); }
        if (before == row.sequence.load(std::memory_order_acquire)) return true;
    }
    return false;
}
inline Totals Difference(const Totals& end, const Totals& begin) {
    Totals r {end.calls - begin.calls, end.ticks - begin.ticks, end.detail_calls - begin.detail_calls,
        end.detail_ticks - begin.detail_ticks, end.unhandled - begin.unhandled, end.dropped - begin.dropped};
    for (size_t i = 0; i < Parts; ++i) { r.parts[i] = end.parts[i] - begin.parts[i]; r.events[i] = end.events[i] - begin.events[i]; }
    return r;
}
} // namespace Common::GuestCpuProfile
