/**
 * Guard-page memory watch for locating the code that reads or writes one value.
 *
 * Hardware breakpoints and debugger attach both halt threads, and this client does not survive a
 * halted thread (BAP timeouts and the per-frame fibers). A guarded page instead costs the touching
 * thread two exceptions per touch and nothing else, so it can run for seconds inside a live
 * session. Logging happens on the polling thread, never inside the handler, because the log lock
 * may be held by the faulting thread. Each address keeps its first few hits in full and only
 * counts the rest, so a value written every frame cannot flood the log or the Dawn folder.
 *
 * The first touch of a guarded page faults on the touching thread. The handler records the touch
 * when it lands on a watched value, single-steps that one instruction, and guards the page again.
 * Other threads keep running throughout.
 *
 * Turn on: `"selector_watch": true` in the `client` section of Dawn\settings.json, then create
 * Dawn\selector_watch.txt. The file is re-read every 250 ms. Changing the addresses in it re-arms
 * the watch and resets its counts; re-saving the same addresses does neither. Deleting it
 * disarms everything.
 * Watch file: up to four hex addresses, separated by spaces or new lines, with or without `0x`.
 * Each is the address of a 4-byte value in the game's memory. For example:
 *   0x000001F2A3B4C5D0
 * An address outside committed memory is skipped with a `stage=arm result=not_committed` warn
 * line and tried again only when the addresses change.
 * Output: `ev=selector_watch` lines in Dawn\logs\dawn.log. Each touch writes `stage=hit` with the
 * instruction, its image offset and code bytes, and `write=1` for a write or 0 for a read; then
 * `stage=regs`, `stage=stack` and `stage=deep`, and a raw copy of the stack to
 * Dawn\selector_stack_<n>.bin. `stage=armed` reports the counts every 5 seconds while anything is
 * armed. These are info lines, so `core.logging.levels.client` must be `info` or `debug`.
 * Limits: 16 touches in full per address, the rest counted. At most 64 distinct pages per
 * session; past that, a new page is refused with `result=page_table_full`.
 */

#include "selector_watch.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../diagnostics/module_range.h"

namespace dawn::client::hooks::probes::selector_watch {
namespace {

constexpr std::size_t kMaxTargets = 4;
constexpr std::size_t kRingCapacity = 64;
constexpr std::size_t kStackWords = 48;
constexpr std::size_t kDeepWords = 24;
constexpr std::size_t kDeepStackBytes = 0x2000;
constexpr std::size_t kCodeBytes = 32;
constexpr std::uintptr_t kPageMask = ~static_cast<std::uintptr_t>(0xFFF);
constexpr DWORD kPollMs = 250;
/** Hits recorded in full per armed address, each with a raw stack file; later hits are counted. */
constexpr std::uint32_t kHitsPerTarget = 16;
/** Pages guarded at any point this session, so a fault already in flight when its page is
    disarmed is still recognised as ours. */
constexpr std::size_t kKnownPageCapacity = 64;
/** How often the armed state is written while anything is armed. */
constexpr std::uint64_t kStatusIntervalMs = 5'000;
/** Time uninstall waits for the polling thread to leave its loop. */
constexpr DWORD kJoinMs = 2'000;

struct Target {
    std::uintptr_t address{};
    std::uintptr_t page{};
    DWORD baseProtect{};
    bool armed{};
};

struct Hit {
    std::uint32_t thread{};
    std::uint32_t write{};
    std::uintptr_t address{};
    std::uintptr_t rip{};
    std::uintptr_t rsp{};
    std::array<std::uintptr_t, 16> regs{};
    std::array<std::uintptr_t, kStackWords> stack{};
    std::array<std::uintptr_t, kDeepWords> deep{};
    std::array<std::uint32_t, kDeepWords> deepOffsets{};
    std::uint32_t deepCount{};
    /** Raw copy of the stack from rsp, so callee-saved spills can be read offline. */
    std::array<std::uint8_t, kDeepStackBytes> raw{};
    std::uint32_t rawBytes{};
};

std::atomic_bool g_installed{false};
/** False while targets are being replaced or after uninstall; no page is re-armed then. */
std::atomic_bool g_watching{false};
PVOID g_handler{};
HANDLE g_thread{};
HANDLE g_stop{};
std::array<Target, kMaxTargets> g_targets{};
std::atomic_size_t g_targetCount{0};
std::array<std::atomic_uint32_t, kMaxTargets> g_captured{};
std::array<std::atomic_uint64_t, kMaxTargets> g_suppressed{};
std::array<std::atomic<std::uintptr_t>, kKnownPageCapacity> g_knownPages{};
std::atomic_size_t g_knownPageCount{0};
/** Two threads can fault on two watched pages at once; only one writes a ring slot at a time. */
SRWLOCK g_ringLock{SRWLOCK_INIT};
std::array<Hit, kRingCapacity> g_ring{};
std::atomic_size_t g_ringWrite{0};
std::atomic_size_t g_ringRead{0};
std::atomic_uint64_t g_faultsSeen{0};
std::atomic_uint64_t g_contended{0};
thread_local std::uintptr_t t_rearmPage = 0;
diagnostics::ModuleRange g_image{};

/** Guard state of one page, read from the OS rather than assumed. */
bool page_protect(std::uintptr_t page, DWORD& protect) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    protect = info.Protect & ~static_cast<DWORD>(PAGE_GUARD);
    return true;
}

bool arm_page(std::uintptr_t page, DWORD baseProtect) noexcept {
    DWORD previous = 0;
    return VirtualProtect(reinterpret_cast<LPVOID>(page), 0x1000, baseProtect | PAGE_GUARD, &previous)
           != FALSE;
}

bool disarm_page(std::uintptr_t page, DWORD baseProtect) noexcept {
    DWORD previous = 0;
    return VirtualProtect(reinterpret_cast<LPVOID>(page), 0x1000, baseProtect, &previous) != FALSE;
}

/** Index of the target whose page holds the address, or kMaxTargets. */
std::size_t target_for_page(std::uintptr_t page) noexcept {
    const std::size_t count = g_targetCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_targets[i].armed && g_targets[i].page == page) {
            return i;
        }
    }
    return kMaxTargets;
}

/** @return True when this watch has guarded the page at some point this session. */
bool known_page(std::uintptr_t page) noexcept {
    const std::size_t count = g_knownPageCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_knownPages[i].load(std::memory_order_relaxed) == page) {
            return true;
        }
    }
    return false;
}

/**
 * Polling thread only: remembers a page before it is first guarded.
 * @return False when the table is full and the page is not in it; such a page must not be
 * guarded, or a fault in flight when it is disarmed would reach the game unhandled.
 */
[[nodiscard]] bool remember_page(std::uintptr_t page) noexcept {
    if (known_page(page)) {
        return true;
    }
    const std::size_t count = g_knownPageCount.load(std::memory_order_relaxed);
    if (count == g_knownPages.size()) {
        return false;
    }
    g_knownPages[count].store(page, std::memory_order_relaxed);
    g_knownPageCount.store(count + 1, std::memory_order_release);
    return true;
}

/** Handler only: copies one hit into the ring, or drops it rather than wait for another writer. */
void capture(std::uintptr_t address, const EXCEPTION_RECORD& record, const CONTEXT& context) noexcept {
    if (TryAcquireSRWLockExclusive(&g_ringLock) == FALSE) {
        g_contended.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::size_t slot = g_ringWrite.load(std::memory_order_relaxed);
    if (slot - g_ringRead.load(std::memory_order_acquire) < kRingCapacity) {
        Hit& hit = g_ring[slot % kRingCapacity];
        hit.thread = GetCurrentThreadId();
        hit.write = static_cast<std::uint32_t>(record.ExceptionInformation[0]);
        hit.address = address;
        hit.rip = static_cast<std::uintptr_t>(context.Rip);
        hit.rsp = static_cast<std::uintptr_t>(context.Rsp);
        hit.regs = {context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp,
                    context.Rsi, context.Rdi, context.R8, context.R9, context.R10, context.R11,
                    context.R12, context.R13, context.R14, context.R15};
        // Every stack read stops at the end of the thread's committed stack region, so none can
        // fault inside this handler, even a touch made close to the top of a stack.
        const auto rsp = static_cast<std::uintptr_t>(context.Rsp);
        std::uintptr_t stackEnd = rsp;
        MEMORY_BASIC_INFORMATION stackInfo{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(rsp), &stackInfo, sizeof(stackInfo)) == sizeof(stackInfo)
            && stackInfo.State == MEM_COMMIT) {
            stackEnd = reinterpret_cast<std::uintptr_t>(stackInfo.BaseAddress) + stackInfo.RegionSize;
        }
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(rsp);
        const std::size_t words = (std::min)(kStackWords, static_cast<std::size_t>((stackEnd - rsp) / 8));
        hit.stack = {};
        for (std::size_t w = 0; w < words; ++w) {
            hit.stack[w] = stack[w];
        }
        hit.deepCount = 0;
        hit.rawBytes = 0;
        const std::uintptr_t limit = (std::min)(stackEnd, rsp + kDeepStackBytes);
        if (limit > rsp) {
            hit.rawBytes = static_cast<std::uint32_t>(limit - rsp);
            std::memcpy(hit.raw.data(), reinterpret_cast<const void*>(rsp), hit.rawBytes);
        }
        // Deeper walk for return-address candidates: only qwords inside the main image.
        const std::uintptr_t imageBase = g_image.base;
        const std::uintptr_t imageEnd = g_image.end;
        for (std::uintptr_t at = rsp + kStackWords * 8;
             imageBase != 0 && at + 8 <= limit && hit.deepCount < kDeepWords; at += 8) {
            const std::uintptr_t value = *reinterpret_cast<const std::uintptr_t*>(at);
            if (value >= imageBase && value < imageEnd) {
                hit.deep[hit.deepCount] = value;
                hit.deepOffsets[hit.deepCount] = static_cast<std::uint32_t>(at - rsp);
                ++hit.deepCount;
            }
        }
        g_ringWrite.store(slot + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_ringLock);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* info) noexcept {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    EXCEPTION_RECORD& record = *info->ExceptionRecord;
    CONTEXT& context = *info->ContextRecord;
    if (record.ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        if (record.NumberParameters < 2) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto address = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
        const std::uintptr_t page = address & kPageMask;
        const std::size_t index = target_for_page(page);
        if (index == kMaxTargets) {
            // A page this watch has since disarmed. The OS already lifted the guard for this
            // touch, so the access simply runs again; passing it on would crash the game.
            return known_page(page) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
        }
        g_faultsSeen.fetch_add(1, std::memory_order_relaxed);
        // Record a touch that lands on any watched value in this page, then single-step and
        // re-arm. A target past its budget is only counted, which also skips the stack copy.
        const std::size_t count = g_targetCount.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i) {
            const Target& target = g_targets[i];
            if (!target.armed || target.page != page) {
                continue;
            }
            if (address + 8 <= target.address || address >= target.address + 4) {
                continue;
            }
            if (g_captured[i].fetch_add(1, std::memory_order_relaxed) >= kHitsPerTarget) {
                g_suppressed[i].fetch_add(1, std::memory_order_relaxed);
            } else {
                capture(address, record, context);
            }
            break;
        }
        t_rearmPage = page;
        context.EFlags |= 0x100; // trap flag: one instruction, then re-arm
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record.ExceptionCode == STATUS_SINGLE_STEP && t_rearmPage != 0) {
        const std::uintptr_t page = t_rearmPage;
        t_rearmPage = 0;
        const std::size_t index = target_for_page(page);
        if (index != kMaxTargets && g_watching.load(std::memory_order_acquire)) {
            (void)arm_page(page, g_targets[index].baseProtect);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void disarm_all() noexcept {
    g_watching.store(false, std::memory_order_release);
    const std::size_t count = g_targetCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_targets[i].armed) {
            (void)disarm_page(g_targets[i].page, g_targets[i].baseProtect);
            g_targets[i].armed = false;
        }
    }
    g_targetCount.store(0, std::memory_order_release);
}

/** Reads up to kMaxTargets hex addresses from the watch file. @return Count, or -1 when absent. */
int read_targets(const core::path::Buffer& file, std::array<std::uintptr_t, kMaxTargets>& out) noexcept {
    FILE* handle = nullptr;
    if (_wfopen_s(&handle, file.chars.data(), L"rb") != 0 || handle == nullptr) {
        return -1;
    }
    std::array<char, 512> text{};
    const std::size_t got = std::fread(text.data(), 1, text.size() - 1, handle);
    std::fclose(handle);
    text[got] = '\0';
    int count = 0;
    const char* cursor = text.data();
    while (*cursor != '\0' && count < static_cast<int>(kMaxTargets)) {
        while (*cursor == ' ' || *cursor == '\r' || *cursor == '\n' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        char* end = nullptr;
        const unsigned long long value = std::strtoull(cursor, &end, 16);
        if (end == cursor) {
            break;
        }
        if (value != 0) {
            out[static_cast<std::size_t>(count)] = static_cast<std::uintptr_t>(value);
            ++count;
        }
        cursor = end;
    }
    return count;
}

/** Writes one hit's raw stack beside the log as `selector_stack_<sequence>.bin`. */
void dump_raw_stack(const Hit& hit, std::size_t sequence) noexcept {
    if (hit.rawBytes == 0) {
        return;
    }
    std::array<wchar_t, 64> name{};
    const int n = std::swprintf(name.data(), name.size(), L"selector_stack_%zu.bin", sequence);
    core::path::Buffer file{};
    if (n <= 0 || !core::path::artifact_file({name.data(), static_cast<std::size_t>(n)}, file)) {
        return;
    }
    FILE* handle = nullptr;
    if (_wfopen_s(&handle, file.chars.data(), L"wb") != 0 || handle == nullptr) {
        return;
    }
    (void)std::fwrite(hit.raw.data(), 1, hit.rawBytes, handle);
    (void)std::fclose(handle);
}

void flush_hits(const diagnostics::ModuleRange& image) noexcept {
    while (g_ringRead.load(std::memory_order_relaxed) < g_ringWrite.load(std::memory_order_acquire)) {
        const std::size_t sequence = g_ringRead.load(std::memory_order_relaxed);
        const Hit hit = g_ring[sequence % kRingCapacity];
        g_ringRead.store(sequence + 1, std::memory_order_release);
        dump_raw_stack(hit, sequence);
        std::array<char, core::log::kLineCapacity> line{};
        int used = std::snprintf(line.data(), line.size(),
                                 "ev=selector_watch stage=hit seq=%zu tid=%u write=%u addr=0x%llX rip=0x%llX",
                                 sequence, hit.thread, hit.write,
                                 static_cast<unsigned long long>(hit.address),
                                 static_cast<unsigned long long>(hit.rip));
        if (used < 0) {
            continue;
        }
        auto append = [&](const char* format, unsigned long long value) {
            if (used < 0 || static_cast<std::size_t>(used) >= line.size()) {
                return;
            }
            const int more = std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), format, value);
            if (more > 0) {
                used += more;
            }
        };
        if (diagnostics::contains(image, hit.rip)) {
            append(" rva=0x%llX", static_cast<unsigned long long>(hit.rip - image.base));
        }
        for (std::size_t w = 0; w < kStackWords; ++w) {
            if (diagnostics::contains(image, hit.stack[w])) {
                append(" ret=0x%llX", static_cast<unsigned long long>(hit.stack[w] - image.base));
            }
        }
        // Code bytes around the instruction pointer, for offline disassembly.
        std::array<std::uint8_t, kCodeBytes> code{};
        const std::uintptr_t codeStart = hit.rip - kCodeBytes / 2;
        MEMORY_BASIC_INFORMATION info{};
        // The whole copy must sit inside the one readable region queried, not only its start.
        if (VirtualQuery(reinterpret_cast<LPCVOID>(codeStart), &info, sizeof(info)) == sizeof(info)
            && info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0
            && codeStart + code.size()
                   <= reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
            std::memcpy(code.data(), reinterpret_cast<const void*>(codeStart), code.size());
            if (used > 0 && static_cast<std::size_t>(used) + 8 + code.size() * 2 < line.size()) {
                used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), " code=");
                for (std::uint8_t byte : code) {
                    used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used), "%02X", byte);
                }
            }
        }
        if (used > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {line.data(), (std::min)(static_cast<std::size_t>(used), line.size() - 1)});
        }
        static constexpr const char* kRegNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                                      "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        std::array<char, core::log::kLineCapacity> regs{};
        int n = std::snprintf(regs.data(), regs.size(), "ev=selector_watch stage=regs tid=%u rip=0x%llX",
                              hit.thread, static_cast<unsigned long long>(hit.rip));
        for (std::size_t r = 0; r < 16 && n > 0 && static_cast<std::size_t>(n) < regs.size(); ++r) {
            const int more = std::snprintf(regs.data() + n, regs.size() - static_cast<std::size_t>(n), " %s=%llX",
                                           kRegNames[r], static_cast<unsigned long long>(hit.regs[r]));
            if (more > 0) { n += more; }
        }
        if (n > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {regs.data(), (std::min)(static_cast<std::size_t>(n), regs.size() - 1)});
        }
        std::array<char, core::log::kLineCapacity> stack{};
        n = std::snprintf(stack.data(), stack.size(), "ev=selector_watch stage=stack tid=%u rsp=0x%llX s=",
                          hit.thread, static_cast<unsigned long long>(hit.rsp));
        for (std::size_t w = 0; w < kStackWords && n > 0 && static_cast<std::size_t>(n) + 18 < stack.size(); ++w) {
            const int more = std::snprintf(stack.data() + n, stack.size() - static_cast<std::size_t>(n), "%llX,",
                                           static_cast<unsigned long long>(hit.stack[w]));
            if (more > 0) { n += more; }
        }
        if (n > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {stack.data(), (std::min)(static_cast<std::size_t>(n), stack.size() - 1)});
        }
        std::array<char, core::log::kLineCapacity> deep{};
        n = std::snprintf(deep.data(), deep.size(), "ev=selector_watch stage=deep tid=%u rip=0x%llX d=",
                          hit.thread, static_cast<unsigned long long>(hit.rip));
        for (std::uint32_t w = 0; w < hit.deepCount && n > 0 && static_cast<std::size_t>(n) + 24 < deep.size(); ++w) {
            const int more = std::snprintf(deep.data() + n, deep.size() - static_cast<std::size_t>(n), "%X:%llX,",
                                           hit.deepOffsets[w], static_cast<unsigned long long>(hit.deep[w] - image.base));
            if (more > 0) { n += more; }
        }
        if (n > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {deep.data(), (std::min)(static_cast<std::size_t>(n), deep.size() - 1)});
        }
    }
}

/** Every hit counted past its address's budget, across the armed targets. */
std::uint64_t suppressed_total() noexcept {
    std::uint64_t total = 0;
    for (const auto& count : g_suppressed) {
        total += count.load(std::memory_order_relaxed);
    }
    return total;
}

/** Addresses whose page is guarded now. Polling thread only. */
std::size_t armed_count() noexcept {
    std::size_t armed = 0;
    const std::size_t count = g_targetCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        armed += g_targets[i].armed ? 1U : 0U;
    }
    return armed;
}

/** Replaces the armed targets with the ones the file names. Polling thread only. */
void rearm(const std::array<std::uintptr_t, kMaxTargets>& wanted, std::size_t wantedCount) noexcept {
    disarm_all();
    for (std::size_t i = 0; i < kMaxTargets; ++i) {
        g_captured[i].store(0, std::memory_order_relaxed);
        g_suppressed[i].store(0, std::memory_order_relaxed);
    }
    std::size_t armed = 0;
    for (std::size_t i = 0; i < wantedCount; ++i) {
        Target& target = g_targets[armed];
        target.address = wanted[i];
        target.page = wanted[i] & kPageMask;
        target.armed = false;
        if (!page_protect(target.page, target.baseProtect)) {
            core::log::writef(core::log::Channel::client, core::log::Level::warn,
                              "ev=selector_watch stage=arm result=not_committed addr=0x%llX",
                              static_cast<unsigned long long>(wanted[i]));
            continue;
        }
        if (!remember_page(target.page)) {
            core::log::writef(core::log::Channel::client, core::log::Level::warn,
                              "ev=selector_watch stage=arm result=page_table_full addr=0x%llX",
                              static_cast<unsigned long long>(wanted[i]));
            continue;
        }
        target.armed = true;
        ++armed;
    }
    g_targetCount.store(armed, std::memory_order_release);
    g_watching.store(armed != 0, std::memory_order_release);
    for (std::size_t i = 0; i < armed; ++i) {
        if (!arm_page(g_targets[i].page, g_targets[i].baseProtect)) {
            g_targets[i].armed = false;
            core::log::writef(core::log::Channel::client, core::log::Level::warn,
                              "ev=selector_watch stage=arm result=protect_failed addr=0x%llX",
                              static_cast<unsigned long long>(g_targets[i].address));
        } else {
            core::log::writef(core::log::Channel::client, core::log::Level::info,
                              "ev=selector_watch stage=arm result=ok addr=0x%llX page=0x%llX protect=0x%X "
                              "hits_logged_max=%u",
                              static_cast<unsigned long long>(g_targets[i].address),
                              static_cast<unsigned long long>(g_targets[i].page),
                              static_cast<unsigned>(g_targets[i].baseProtect), kHitsPerTarget);
        }
    }
    if (wantedCount == 0) {
        core::log::write(core::log::Channel::client, core::log::Level::info,
                         "ev=selector_watch stage=disarm result=ok");
    }
}

DWORD WINAPI poll_thread(LPVOID) noexcept {
    core::path::Buffer file{};
    if (!core::path::artifact_file(L"selector_watch.txt", file)) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=selector_watch stage=install result=no_path");
        return 0;
    }
    diagnostics::ModuleRange image{};
    (void)diagnostics::module_range(GetModuleHandleW(nullptr), image);
    g_image = image;
    std::array<std::uintptr_t, kMaxTargets> wanted{};
    std::array<std::uintptr_t, kMaxTargets> current{};
    std::size_t currentCount = 0;
    std::uint64_t lastReport = 0;
    while (WaitForSingleObject(g_stop, kPollMs) == WAIT_TIMEOUT) {
        flush_hits(image);
        const int count = read_targets(file, wanted);
        const std::size_t wantedCount = count < 0 ? 0 : static_cast<std::size_t>(count);
        bool same = wantedCount == currentCount;
        for (std::size_t i = 0; same && i < wantedCount; ++i) {
            same = wanted[i] == current[i];
        }
        if (same) {
            const std::uint64_t now = GetTickCount64();
            const std::size_t armed = armed_count();
            if (armed != 0 && now - lastReport >= kStatusIntervalMs) {
                lastReport = now;
                core::log::writef(core::log::Channel::client, core::log::Level::info,
                                  "ev=selector_watch stage=armed targets=%u faults=%llu "
                                  "hits_suppressed=%llu hits_contended=%llu",
                                  static_cast<unsigned>(armed),
                                  static_cast<unsigned long long>(g_faultsSeen.load(std::memory_order_relaxed)),
                                  static_cast<unsigned long long>(suppressed_total()),
                                  static_cast<unsigned long long>(g_contended.load(std::memory_order_relaxed)));
            }
            continue;
        }
        rearm(wanted, wantedCount);
        currentCount = wantedCount;
        current = wanted;
    }
    disarm_all();
    flush_hits(image);
    return 0;
}

} // namespace

bool install() noexcept {
    if (g_installed.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    // Registered once and never removed: a thread still single-stepping after its guard fault
    // must find the handler, so uninstall stops the watch but leaves the handler in place.
    if (g_handler == nullptr) {
        g_handler = AddVectoredExceptionHandler(1, &handler);
    }
    if (g_handler == nullptr) {
        g_installed.store(false, std::memory_order_release);
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=selector_watch stage=install result=handler_failed");
        return false;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop != nullptr ? CreateThread(nullptr, 0, &poll_thread, nullptr, 0, nullptr) : nullptr;
    if (g_thread == nullptr) {
        if (g_stop != nullptr) {
            CloseHandle(g_stop);
            g_stop = nullptr;
        }
        g_installed.store(false, std::memory_order_release);
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=selector_watch stage=install result=thread_failed");
        return false;
    }
    core::log::write(core::log::Channel::client, core::log::Level::info,
                     "ev=selector_watch stage=install result=ok");
    return true;
}

bool uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    SetEvent(g_stop);
    const bool joined = WaitForSingleObject(g_thread, kJoinMs) == WAIT_OBJECT_0;
    CloseHandle(g_thread);
    g_thread = nullptr;
    if (joined) {
        CloseHandle(g_stop);
    } else {
        // The thread disarms on its way out; if it has not left yet, no page may stay guarded.
        disarm_all();
    }
    g_stop = nullptr;
    g_installed.store(false, std::memory_order_release);
    core::log::write(core::log::Channel::client, core::log::Level::info,
                     "ev=selector_watch stage=uninstall result=ok handler=retained");
    return true;
}

} // namespace dawn::client::hooks::probes::selector_watch
