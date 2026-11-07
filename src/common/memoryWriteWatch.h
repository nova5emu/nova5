#pragma once

// Opt-in forensic watch, armed by the Wolverine sound trace only. No GPU/kernel
// locks or address-space queries in the exception handler: backing writers may
// already hold those locks. This is a diagnostic, not a coherency mechanism.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "common/termination.h"
#endif

namespace Common::MemoryWriteWatch {
#if defined(_WIN32)
inline constexpr uint64_t Page = 0x1000, Bytes = 64;
struct Alias {
    uint64_t target = 0, page = 0;
    std::atomic<DWORD> original {0};
    std::atomic<bool> owned {false};
};
inline Alias aliases[2]; // guest view, backing view
inline uint64_t baseline[Bytes / 8] {};
inline std::atomic<bool> active {false}, changed {false};
inline std::atomic<unsigned> writes {0};
inline PVOID handler = nullptr;
struct Step {
    bool pending = false, had_tf = false;
    unsigned opened = 0;
    uint64_t address = 0;
    CONTEXT before {};
};
inline thread_local Step step;

inline bool Overlaps(uint64_t a, uint64_t size, uint64_t b, uint64_t length) {
    return size && length && (a <= b ? b - a < size : a - b < length);
}
inline bool Read(uint64_t address, void* data, size_t size) {
    SIZE_T received = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, size, &received) &&
           received == size;
}
inline DWORD Protection(uint64_t address) {
    MEMORY_BASIC_INFORMATION info {};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) && info.State == MEM_COMMIT
               ? info.Protect : 0;
}
inline DWORD ReadOnly(DWORD original) {
    return original == PAGE_EXECUTE_READWRITE ? PAGE_EXECUTE_READ : PAGE_READONLY;
}
inline void Restore(unsigned index) {
    auto& a = aliases[index];
    if (a.owned.exchange(false) && Protection(a.page) == ReadOnly(a.original)) {
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(a.page), Page, a.original, &ignored);
    }
}
inline void Disarm() {
    active.store(false, std::memory_order_release);
    Restore(0);
    Restore(1);
}
inline void PrintHostStack() {
    void* frames[24] {};
    const auto count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    std::printf("[MEM_WRITE_WATCH] host stack (exe=%p):", GetModuleHandleA(nullptr));
    for (unsigned i = 0; i < count; ++i) std::printf(" %p", frames[i]);
    std::printf("\n");
}
inline void PrintWriter(const CONTEXT& c, uint64_t address, const char* phase) {
    char where[300] {};
    Termination::Describe(c.Rip, where, sizeof(where));
    std::printf("[MEM_WRITE_WATCH] %s thread=%lu pc=%s address=0x%llx writes=%u\n",
                phase, GetCurrentThreadId(), where, (unsigned long long)address, writes.load());
    std::printf("  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx rsp=%llx\n"
                "  r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx flags=%lx\n",
                c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15, c.EFlags);
    uint8_t code[32] {};
    if (Read(c.Rip, code, sizeof(code))) {
        std::printf("  code:");
        for (auto byte: code) std::printf(" %02x", byte);
        std::printf("\n");
    }
    // Include the caller of a leaf memset/memcpy and bounded guest frame pointers.
    uint64_t stack[24] {};
    if (Read(c.Rsp, stack, sizeof(stack))) {
        std::printf("  stack:");
        for (auto word: stack) std::printf(" %llx", (unsigned long long)word);
        std::printf("\n");
    }
    uint64_t frame = c.Rbp;
    for (unsigned depth = 0; depth < 12; ++depth) {
        uint64_t pair[2] {};
        if (frame < c.Rsp || frame - c.Rsp > 0x100000 || !Read(frame, pair, sizeof(pair))) break;
        Termination::Describe(pair[1], where, sizeof(where));
        std::printf("  frame[%u]=0x%llx return=%s\n", depth, (unsigned long long)frame, where);
        if (pair[0] <= frame) break;
        frame = pair[0];
    }
    PrintHostStack();
    std::fflush(stdout);
}
inline LONG WINAPI OnException(PEXCEPTION_POINTERS e) {
    auto& c = *e->ContextRecord;
    const auto& record = *e->ExceptionRecord;
    if (record.ExceptionCode == EXCEPTION_SINGLE_STEP && step.pending) {
        const auto opened = step.opened;
        step.pending = false;
        if (!step.had_tf) c.EFlags &= ~DWORD {0x100};
        for (unsigned i = 0; i < 2; ++i) {
            if ((opened & (1u << i)) && aliases[i].owned.load()) {
                DWORD ignored = 0;
                if (Protection(aliases[i].page) != aliases[i].original ||
                    !VirtualProtect(reinterpret_cast<void*>(aliases[i].page), Page,
                                    ReadOnly(aliases[i].original), &ignored)) {
                    aliases[i].owned.store(false);
                    std::printf("[MEM_WRITE_WATCH] lost alias=%u protection while single-stepping\n", i);
                }
            }
        }
        uint64_t now[Bytes / 8] {};
        if (active.load() && Read(aliases[1].target, now, sizeof(now)) &&
            std::memcmp(now, baseline, sizeof(now)) != 0) {
            changed.store(true);
            PrintWriter(step.before, step.address, "CHANGED after instruction");
            std::printf("[MEM_WRITE_WATCH] before / after:");
            for (unsigned i = 0; i < Bytes / 8; ++i)
                std::printf(" %llx/%llx", (unsigned long long)baseline[i], (unsigned long long)now[i]);
            std::printf("\n");
            Disarm();
        }
        if (writes.load() >= 65536 && active.load()) {
            std::printf("[MEM_WRITE_WATCH] stopped: 65536 writes without observing a table change\n");
            Disarm();
        }
        std::fflush(stdout);
        return step.had_tf ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!active.load(std::memory_order_acquire) || record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record.NumberParameters < 2 || record.ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = record.ExceptionInformation[1];
    for (unsigned i = 0; i < 2; ++i) {
        auto& a = aliases[i];
        if (!a.owned.load() || address < a.page || address - a.page >= Page ||
            Protection(a.page) != ReadOnly(a.original)) continue;
        if (!step.pending) {
            uint64_t now[Bytes / 8] {};
            if (!Read(aliases[1].target, now, sizeof(now)) || std::memcmp(now, baseline, sizeof(now)) != 0) {
                PrintWriter(c, address, "table changed or unreadable BEFORE this write; writer not established");
                Disarm();
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            step.before = c;
            step.address = address;
            step.opened = 0;
            step.had_tf = (c.EFlags & 0x100) != 0;
        }
        DWORD ignored = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(a.page), Page, a.original, &ignored))
            return EXCEPTION_CONTINUE_SEARCH;
        step.pending = true;
        step.opened |= 1u << i;
        c.EFlags |= 0x100;
        if (writes.fetch_add(1) < 4) PrintWriter(step.before, address, "page write (before)");
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline bool Arm(uint64_t guest, uint64_t backing) {
    if (active.load() || step.pending || guest == 0 || backing == 0 ||
        guest == backing || (guest & (Page - 1)) + Bytes > Page ||
        (backing & (Page - 1)) + Bytes > Page) return false;
    if (handler == nullptr) handler = AddVectoredExceptionHandler(1, OnException);
    if (handler == nullptr) return false;
    uint64_t guest_bytes[Bytes / 8] {};
    if (!Read(guest, guest_bytes, sizeof(guest_bytes)) || !Read(backing, baseline, sizeof(baseline)) ||
        std::memcmp(guest_bytes, baseline, sizeof(baseline)) != 0) return false;
    for (unsigned i = 0; i < 2; ++i) {
        auto& a = aliases[i];
        a.target = i == 0 ? guest : backing;
        a.page = a.target & ~(Page - 1);
        a.original = Protection(a.page);
        if (a.original != PAGE_READWRITE && a.original != PAGE_EXECUTE_READWRITE) return false;
    }
    writes.store(0);
    changed.store(false);
    // Publish before protecting: another thread can immediately hit the watch.
    active.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 2; ++i) {
        DWORD ignored = 0;
        aliases[i].owned.store(true);
        if (!VirtualProtect(reinterpret_cast<void*>(aliases[i].page), Page, ReadOnly(aliases[i].original), &ignored)) {
            aliases[i].owned.store(false);
            Disarm();
            return false;
        }
    }
    std::printf("[MEM_WRITE_WATCH] armed guest=0x%llx backing=0x%llx bytes=%llu\n",
                (unsigned long long)guest, (unsigned long long)backing, (unsigned long long)Bytes);
    std::fflush(stdout);
    return true;
}

// Called before backing writes and VM mutations. VM mutation drops only the
// affected alias's watch so it never restores an obsolete protection on a remap.
// The stable backing alias remains watched if the guest view changes protection.
inline void Notify(const char* operation, uint64_t address, uint64_t size, bool changes_mapping = false,
                   uint64_t detail = 0) {
    if (!active.load(std::memory_order_acquire)) return;
    bool hit = false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& a = aliases[i];
        if (!Overlaps(address, size, a.page, Page)) continue;
        hit = true;
        std::printf("[MEM_WRITE_WATCH] operation=%s address=0x%llx size=0x%llx alias=%u exact=%u detail=0x%llx\n",
                    operation, (unsigned long long)address, (unsigned long long)size, i,
                    unsigned(Overlaps(address, size, a.target, Bytes)), (unsigned long long)detail);
        if (changes_mapping) {
            Restore(i);
            std::printf("[MEM_WRITE_WATCH] alias=%u disarmed before VM change; other alias remains watched\n", i);
        }
    }
    if (hit) { PrintHostStack(); std::fflush(stdout); }
}
struct ProtectionScope {
    uint64_t address, size;
    ProtectionScope(uint64_t address, uint64_t size): address(address), size(size) {
        Notify("ProtectView", address, size, true);
    }
    ~ProtectionScope() {
        if (!active.load(std::memory_order_acquire) || !Overlaps(address, size, aliases[0].page, Page)) return;
        auto& a = aliases[0];
        const auto mode = Protection(a.page);
        // Never weaken another owner's read/no-access protection. When that owner
        // makes it writable again, resume observing the next CPU write ourselves.
        if (mode != PAGE_READWRITE && mode != PAGE_EXECUTE_READWRITE) return;
        a.original = mode;
        a.owned.store(true);
        DWORD ignored = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(a.page), Page, ReadOnly(mode), &ignored)) {
            a.owned.store(false);
            std::printf("[MEM_WRITE_WATCH] failed to rearm guest alias after ProtectView\n");
        } else {
            std::printf("[MEM_WRITE_WATCH] guest alias rearmed after ProtectView\n");
        }
        std::fflush(stdout);
    }
};
inline void Report() {
    std::printf("[MEM_WRITE_WATCH] summary active=%u changed=%u writes=%u guest-watch=%u backing-watch=%u\n",
                unsigned(active.load()), unsigned(changed.load()), writes.load(),
                unsigned(aliases[0].owned.load()), unsigned(aliases[1].owned.load()));
    std::fflush(stdout);
}
#else
inline void Notify(const char*, uint64_t, uint64_t, bool = false, uint64_t = 0) {}
struct ProtectionScope { ProtectionScope(uint64_t, uint64_t) {} };
#endif
} // namespace Common::MemoryWriteWatch
