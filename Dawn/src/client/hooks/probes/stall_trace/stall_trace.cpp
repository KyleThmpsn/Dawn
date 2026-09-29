/**
 * Every thread's stack at the moment the watchdog first reports a stalled job.
 * The watchdog names the stalled job but not what the rest of the game is doing. On the first
 * "hitch detected ... stalled" message of a run, this suspends each thread in turn, copies its
 * instruction pointer and stack, and resumes it; lines are written only after the resume, so
 * nothing is read from a running thread.
 *
 * Turn on: `"stall_trace": true` in the `client` section of Dawn\settings.json.
 * Output: `ev=stall_trace` lines in Dawn\logs\dawn.log: `stage=begin` with the watchdog's text,
 * `stage=loader` with the world tag loader's state, `stage=thread` and `stage=stack` for every
 * thread, then `stage=end`. They are warn lines, so they show at the default log level.
 * Limits: three traces per session. Each briefly suspends every game thread, which can perturb
 * the stall it measures.
 */

#include "stall_trace.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../bootflow/loader_diagnostics.h"
#include "../../../process/freeze/client_process_freeze.h"

namespace dawn::client::hooks::probes::stall_trace {
namespace {

/** A stall repeats the same hitch forever; a few traces are enough to compare. */
constexpr std::uint32_t kMaximumTraces = 3;
/** Stack bytes copied per thread while it is suspended. */
constexpr std::size_t kStackBytes = 0x4000;
/** Module-relative values reported from the raw stack scan of each thread. */
constexpr std::size_t kScanFrames = 40;
/** Modules large enough to name every image the client loads. */
constexpr std::size_t kModuleCapacity = 256;
/** `ThreadQuerySetWin32StartAddress` information class of NtQueryInformationThread. */
constexpr int kThreadStartAddressClass = 9;

using QueryThread = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

struct Module final {
    std::uintptr_t base{};
    std::uintptr_t end{};
    std::array<char, 24> name{};
};

std::atomic_uint32_t g_traces{};
std::atomic_bool g_busy{};
/** When the watchdog last reported a hitch, so the stall probe can leave a reported stall alone. */
std::atomic_uint64_t g_lastHitch{};
/** Only one trace runs at a time (`g_busy`), so a single copy buffer suffices. */
std::array<std::uint64_t, kStackBytes / sizeof(std::uint64_t)> g_stack{};
std::array<Module, kModuleCapacity> g_modules{};
std::size_t g_moduleCount{};

[[nodiscard]] bool contains(std::string_view text, std::string_view needle) noexcept {
    return text.find(needle) != std::string_view::npos;
}

/** Snapshot of loaded images, taken while no thread is suspended. */
void load_modules() noexcept {
    g_moduleCount = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const auto mainImage = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    HMODULE self = nullptr;
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&load_modules), &self);
    for (BOOL more = Module32FirstW(snapshot, &entry); more && g_moduleCount < g_modules.size();
         more = Module32NextW(snapshot, &entry)) {
        Module& module = g_modules[g_moduleCount++];
        module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        module.end = module.base + entry.modBaseSize;
        module.name = {};
        if (module.base == mainImage) {
            std::memcpy(module.name.data(), "exe", 3);
            continue;
        }
        if (entry.hModule == self) {
            std::memcpy(module.name.data(), "dawn", 4);
            continue;
        }
        std::size_t used = 0;
        for (const wchar_t* cursor = entry.szModule;
             *cursor != L'\0' && *cursor != L'.' && used + 1 < module.name.size(); ++cursor) {
            const wchar_t value = *cursor;
            module.name[used++] = value < 0x80
                                      ? static_cast<char>(value >= L'A' && value <= L'Z'
                                                              ? value + (L'a' - L'A')
                                                              : value)
                                      : '?';
        }
    }
    CloseHandle(snapshot);
}

[[nodiscard]] const Module* module_of(std::uint64_t address) noexcept {
    for (std::size_t index = 0; index < g_moduleCount; ++index) {
        if (address >= g_modules[index].base && address < g_modules[index].end) {
            return &g_modules[index];
        }
    }
    return nullptr;
}

/** Appends `module+offset` (or a raw address) to a line, keeping it terminated. */
void append_address(char* line, std::size_t capacity, std::size_t& used, std::uint64_t address,
                    bool first) noexcept {
    if (used + 40U >= capacity) {
        return;
    }
    const Module* module = module_of(address);
    const int written =
        module != nullptr
            ? std::snprintf(line + used, capacity - used, "%s%s+%llX", first ? "" : ",",
                            module->name.data(),
                            static_cast<unsigned long long>(address - module->base))
            : std::snprintf(line + used, capacity - used, "%s%llX", first ? "" : ",",
                            static_cast<unsigned long long>(address));
    if (written > 0) {
        used += (std::min)(static_cast<std::size_t>(written), capacity - used - 1U);
    }
}

/** Copies stack memory that may be unmapped past the committed region. */
[[nodiscard]] std::size_t copy_stack(std::uint64_t rsp) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(rsp), &region, sizeof(region)) == 0) {
        return 0;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    const std::size_t available = end > rsp ? static_cast<std::size_t>(end - rsp) : 0U;
    const std::size_t bytes = (std::min)(available, kStackBytes) & ~std::size_t{7};
    __try {
        std::memcpy(g_stack.data(), reinterpret_cast<const void*>(rsp), bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return bytes;
}

/** Suspends one thread just long enough to read its registers and copy its stack. */
[[nodiscard]] bool capture(HANDLE thread, CONTEXT& context, std::size_t& stackBytes) noexcept {
    stackBytes = 0;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        return false;
    }
    context = {};
    context.ContextFlags = CONTEXT_FULL;
    const bool read = GetThreadContext(thread, &context) != FALSE;
    if (read) {
        stackBytes = copy_stack(context.Rsp);
    }
    (void)ResumeThread(thread);
    return read;
}

void write_line(const char* line, std::size_t used) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, {line, used});
}

/** Thread description (if the client named it) and start routine, read without suspension. */
void describe(HANDLE thread, QueryThread query, char* name, std::size_t nameCapacity,
              std::uint64_t& start) noexcept {
    name[0] = '\0';
    start = 0;
    if (query != nullptr) {
        PVOID address = nullptr;
        if (query(thread, kThreadStartAddressClass, &address, sizeof(address), nullptr) >= 0) {
            start = reinterpret_cast<std::uint64_t>(address);
        }
    }
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
        std::size_t used = 0;
        for (const wchar_t* cursor = description; *cursor != L'\0' && used + 1 < nameCapacity;
             ++cursor) {
            const wchar_t value = *cursor;
            name[used++] = value >= 0x20 && value < 0x7F && value != L'"'
                               ? static_cast<char>(value) : '?';
        }
        name[used] = '\0';
        LocalFree(description);
    }
}

void trace_thread(DWORD threadId, QueryThread query, std::uint32_t trace) noexcept {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                         | THREAD_QUERY_INFORMATION,
                                     FALSE, threadId);
    if (thread == nullptr) {
        return;
    }
    CONTEXT context{};
    std::size_t stackBytes = 0;
    const bool captured = capture(thread, context, stackBytes);
    std::array<char, 96> name{};
    std::uint64_t start = 0;
    describe(thread, query, name.data(), name.size(), start);
    CloseHandle(thread);
    if (!captured) {
        return;
    }

    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(), line.size(),
                                "ev=stall_trace stage=thread trace=%u tid=%lu name=\"%s\" start=",
                                trace, static_cast<unsigned long>(threadId), name.data());
    if (written <= 0) {
        return;
    }
    std::size_t used = (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
    append_address(line.data(), line.size(), used, start, true);
    // Only what was read while the thread was suspended is reported: its instruction pointer
    // here, and the copied stack below. A call chain walked after the resume could be wrong.
    written = std::snprintf(line.data() + used, line.size() - used, " rip=");
    used += written > 0 ? (std::min)(static_cast<std::size_t>(written), line.size() - used - 1U) : 0U;
    append_address(line.data(), line.size(), used, context.Rip, true);
    line[used] = '\0';
    write_line(line.data(), used);

    written = std::snprintf(line.data(), line.size(),
                            "ev=stall_trace stage=stack trace=%u tid=%lu bytes=%zu values=",
                            trace, static_cast<unsigned long>(threadId), stackBytes);
    if (written <= 0) {
        return;
    }
    used = (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
    std::size_t reported = 0;
    for (std::size_t index = 0; index < stackBytes / sizeof(std::uint64_t)
                                && reported < kScanFrames; ++index) {
        const std::uint64_t value = g_stack[index];
        const Module* module = module_of(value);
        if (module == nullptr || std::strcmp(module->name.data(), "ntdll") == 0) {
            continue;
        }
        append_address(line.data(), line.size(), used, value, reported == 0);
        ++reported;
    }
    line[used] = '\0';
    write_line(line.data(), used);
}

void trace(std::uint32_t number, const char* text) noexcept {
    std::array<char, 320> head{};
    const int written = std::snprintf(head.data(), head.size(),
                                      "ev=stall_trace stage=begin trace=%u text=%.240s", number,
                                      text);
    if (written > 0) {
        write_line(head.data(), (std::min)(static_cast<std::size_t>(written), head.size() - 1U));
    }
    bootflow::report_loader_diagnostics("hitch");
    load_modules();
    const auto query = reinterpret_cast<QueryThread>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    // Thread ids are collected first so no Toolhelp call runs while a thread is suspended.
    std::array<DWORD, 512> threads{};
    std::size_t count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && count < threads.size();
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
            threads[count++] = entry.th32ThreadID;
        }
    }
    CloseHandle(snapshot);
    // A hook install suspends threads too; two suspenders at once can freeze each other, so the
    // dump claims the process as its only suspender.
    process::freeze::enter_exclusive();
    for (std::size_t index = 0; index < count; ++index) {
        trace_thread(threads[index], query, number);
    }
    process::freeze::leave_exclusive();
    std::array<char, 96> tail{};
    const int ended = std::snprintf(tail.data(), tail.size(),
                                    "ev=stall_trace stage=end trace=%u threads=%zu", number,
                                    count);
    if (ended > 0) {
        write_line(tail.data(), (std::min)(static_cast<std::size_t>(ended), tail.size() - 1U));
    }
}

} // namespace

void observe(const char* text, std::uint32_t repeats) noexcept {
    if (text == nullptr || !core::settings::get().client.stallTrace) {
        return;
    }
    const std::string_view message{text};
    if (!contains(message, "hitch detected")) {
        return;
    }
    g_lastHitch.store(GetTickCount64(), std::memory_order_relaxed);
    if (repeats != 1U || !contains(message, "stalled")) {
        return;
    }
    if (g_traces.load(std::memory_order_relaxed) >= kMaximumTraces
        || g_busy.exchange(true, std::memory_order_acquire)) {
        return;
    }
    const std::uint32_t number = g_traces.fetch_add(1, std::memory_order_relaxed) + 1U;
    trace(number, text);
    g_busy.store(false, std::memory_order_release);
}

std::uint64_t last_hitch_tick() noexcept {
    return g_lastHitch.load(std::memory_order_relaxed);
}

} // namespace dawn::client::hooks::probes::stall_trace
