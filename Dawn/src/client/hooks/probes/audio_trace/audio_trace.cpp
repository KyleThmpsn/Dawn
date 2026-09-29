/**
 * The game's Wwise audio pipeline for the IDs you list: bank loads, the sound and node objects a
 * bank holds, PCM headers, streams, file opens and event posts.
 * Every hook forwards the native call unchanged and queues a line; a thread of its own writes
 * them, so the audio threads never wait on the disk.
 *
 * Turn on: `"audio_trace": true` in the `client` section of Dawn\settings.json, and
 * Dawn\audio_trace_watch.txt in place before the game starts. The file is read once, at startup.
 * Watch file: one Wwise ID per line as exactly eight hex digits, naming a bank, sound, media,
 * event or file. Blank lines are skipped. Up to 16384 IDs and 144 KiB. For example:
 *   1A2B3C4D
 * Anything else refuses the whole file: no hook attaches, and Dawn\logs\dawn.log gets an
 * `ev=audio_trace stage=watch result=fail` line with the reason, and the line number when one
 * line is at fault.
 * Output: Dawn\audio_trace.log, with one `stage=` line per record. It is recreated once the watch
 * file and the game code checks pass, so after a refusal the file from an earlier launch remains.
 * Otherwise Dawn\logs\dawn.log gets only `ev=audio_trace stage=install`: an info line when it
 * installs, which needs `core.logging.levels.client` at `info` or `debug`, and a warn line with
 * the reason when it cannot.
 * Limits: every record for a listed ID is kept, as are failures and PCM attempts. Other
 * successful records keep the first eight of each kind as stock examples. At most 4096 records
 * per kind; a streamed PCM record may add a `stage=pcm_header` line.
 */

#include "audio_trace.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../hooking/call_gate.h"
#include "../../../hooking/detour.h"
#include "../detach.h"

namespace dawn::client::hooks::probes::audio_trace {
namespace {
using Open = std::uint32_t(__fastcall*)(void*, std::uint32_t, std::uint32_t,
                                      std::uint32_t, void*, bool);
using Header = std::uint32_t(__fastcall*)(void*, void*);
using Hirc = std::uint32_t(__fastcall*)(void*, void*, std::uint32_t);
using Memory = std::uint32_t(__fastcall*)(void*);
using Stream = std::uint32_t(__fastcall*)(void*, const void*);
using Post = std::uint32_t(__fastcall*)(std::uint32_t, std::uint64_t, std::uint32_t,
                                      void*, void*, const void*, std::uint32_t);
using AutoStream = std::uint32_t(__fastcall*)(void*, std::uint32_t, void*, const void*,
                                            const void*, void**, bool);
using FileOpen = std::uint32_t(__fastcall*)(void*, std::uint32_t, std::uint32_t,
                                          void*, bool*, void*);
using Sound = std::uint32_t(__fastcall*)(void*, const void*, std::uint32_t, void*, bool);
using Node = std::uint32_t(__fastcall*)(void*, const void**, std::uint32_t*, bool);

SRWLOCK g_lifecycle{SRWLOCK_INIT};
hooking::CallGate g_calls;
std::array<hooking::detour::Handle, 10> g_handles{};
std::atomic<Open> g_open{};
std::atomic<Header> g_header{};
std::atomic<Hirc> g_hirc{};
std::atomic<Memory> g_memory{};
std::atomic<Stream> g_stream{};
std::atomic<Post> g_post{};
std::atomic<AutoStream> g_auto{};
std::atomic<FileOpen> g_fileOpen{};
std::atomic<Sound> g_sound{};
std::atomic<Node> g_node{};
HANDLE g_file{INVALID_HANDLE_VALUE}, g_stop{}, g_worker{};

/** Longest formatted record; every producer formats into a buffer no larger than this. */
constexpr std::size_t kLineCapacity = 384;
/** Records waiting for the writer. At one drain per interval this absorbs about 40,000 lines a second. */
constexpr std::size_t kQueueCapacity = 4096;
constexpr DWORD kDrainIntervalMs = 100;
/** Records the writer copies out per lock hold, then writes with one call. */
constexpr std::size_t kBatchLines = 32;

/** One formatted record waiting for the writer thread. */
struct Line {
    std::uint16_t length{};
    std::array<char, kLineCapacity> text{};
};
SRWLOCK g_queueLock{SRWLOCK_INIT};
std::array<Line, kQueueCapacity> g_queue{};
std::size_t g_head{}, g_count{};
std::atomic<std::uint64_t> g_queued{}, g_dropped{};
/** Only the writer thread touches this. */
std::array<char, kBatchLines * kLineCapacity> g_batch{};

std::array<std::uint32_t, 16384> g_watch{};
std::size_t g_watchCount{};
std::array<std::atomic_uint32_t, 10> g_seen{};
std::array<std::atomic_uint32_t, 10> g_written{};
thread_local std::uint32_t g_bank{};
thread_local std::uint32_t g_loading{};

/** Prefixes and signatures checked against the captured native executable.
 * These are whole function entries, never the interior format-check blocks.
 * Observation always calls the original exactly once and returns its result unchanged.
 */
struct Target {
    std::uintptr_t rva;
    std::array<unsigned char, 16> prefix;
};
constexpr std::array<Target, 10> kTargets{{
    {0x19CFB80, {0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x57}},
    {0x19D3E90, {0x48,0x89,0x5C,0x24,0x18,0x55,0x56,0x57,0x48,0x83,0xEC,0x20,0x41,0xB8,0x08,0x00}},
    {0x19D4730, {0x48,0x89,0x5C,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}},
    {0x1A05200, {0x40,0x53,0x55,0x41,0x56,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x48,0x8B,0x41,0x18}},
    {0x1A057C0, {0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x10,0x49,0x89,0x6B,0x18,0x56,0x41,0x56,0x41,0x57}},
    {0x19B3370, {0x48,0x89,0x74,0x24,0x20,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30,0x4C}},
    {0x1A3A030, {0x40,0x56,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x0F,0x57,0xC0,0x4D,0x8B,0xF1}},
    {0x10DC580, {0x40,0x53,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8B,0x84,0x24,0x90,0x00,0x00}},
    {0x1A13A50, {0x48,0x8B,0xC4,0x44,0x89,0x40,0x18,0x53,0x48,0x83,0xEC,0x60,0x48,0x83,0xC2,0x04}},
    {0x19DB2F0, {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83}},
}};

bool copy(const void* source, void* destination, std::size_t size) noexcept {
    __try {
        if (source == nullptr) return false;
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<class T> T read(const void* pointer, std::size_t offset) noexcept {
    T value{};
    if (pointer != nullptr) {
        (void)copy(static_cast<const unsigned char*>(pointer) + offset, &value, sizeof(value));
    }
    return value;
}

bool watched(std::uint32_t id) noexcept {
    return std::binary_search(g_watch.begin(), g_watch.begin() + g_watchCount, id);
}

/**
 * Queues one formatted record for the writer thread. The hooked audio thread never touches the
 * file: it waits at most for another producer's single-record copy or the writer's batch copy.
 * A full queue drops the record and counts it, and the stop line reports the count.
 */
void line(const char* text) noexcept {
    const std::size_t length = (std::min)(std::strlen(text), kLineCapacity);
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_count == g_queue.size()) {
        ReleaseSRWLockExclusive(&g_queueLock);
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Line& slot = g_queue[(g_head + g_count) % g_queue.size()];
    std::memcpy(slot.text.data(), text, length);
    slot.length = static_cast<std::uint16_t>(length);
    ++g_count;
    ReleaseSRWLockExclusive(&g_queueLock);
    g_queued.fetch_add(1, std::memory_order_relaxed);
}

/** Writer thread only: empties the queue in batches, one file write per batch. */
void drain() noexcept {
    for (;;) {
        std::size_t used = 0;
        AcquireSRWLockExclusive(&g_queueLock);
        const std::size_t count = (std::min)(kBatchLines, g_count);
        for (std::size_t i = 0; i < count; ++i) {
            const Line& slot = g_queue[(g_head + i) % g_queue.size()];
            std::memcpy(g_batch.data() + used, slot.text.data(), slot.length);
            used += slot.length;
        }
        g_head = (g_head + count) % g_queue.size();
        g_count -= count;
        ReleaseSRWLockExclusive(&g_queueLock);
        if (count == 0) return;
        DWORD written{};
        (void)WriteFile(g_file, g_batch.data(), static_cast<DWORD>(used), &written, nullptr);
    }
}

DWORD WINAPI writer(void*) noexcept {
    while (WaitForSingleObject(g_stop, kDrainIntervalMs) == WAIT_TIMEOUT) drain();
    drain();
    return 0;
}

/** Stops the writer after it empties the queue, then closes the trace with a summary line. */
void close_output() noexcept {
    const bool started = g_worker != nullptr;
    if (g_worker) {
        SetEvent(g_stop);
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    if (g_file == INVALID_HANDLE_VALUE) return;
    if (started) {
        char summary[128]{};
        const int length = std::snprintf(summary, sizeof(summary), "stage=stop queued=%llu dropped=%llu\n",
                                         static_cast<unsigned long long>(g_queued.load()),
                                         static_cast<unsigned long long>(g_dropped.load()));
        DWORD written{};
        if (length > 0) (void)WriteFile(g_file, summary, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
}

/**
 * Keep successful stock examples, all watched records, PCM attempts and bounded failures.
 * Each caller decides `interesting` from the record's own IDs. An object record is never kept
 * for the bank it arrived in: a watched bank holds hundreds of stock objects, and keeping them
 * all used up the cap before any watched object loaded.
 */
bool record(std::size_t kind, bool interesting, std::uint32_t result) noexcept {
    const auto ordinal = g_seen[kind].fetch_add(1, std::memory_order_relaxed);
    if (ordinal >= 8 && (kind < 3 || kind >= 5) && result == 1 && !interesting) return false;
    return g_written[kind].fetch_add(1, std::memory_order_relaxed) < 4096;
}

std::uint32_t __fastcall open_body(void* self, std::uint32_t id, std::uint32_t offset,
                                  std::uint32_t codec, void* custom, bool language) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_open)(self, id, offset, codec, custom, language);
    if (call.accepts_side_effects()) {
        g_bank = id;
        if (record(0, watched(id), result)) {
            char text[320]{};
            std::snprintf(text, sizeof(text),
                "tick=%llu stage=bank_open id=%08X watched=%u result=%u offset=%u codec=%u language=%u block=%u\n",
                GetTickCount64(), id, watched(id) ? 1U : 0U, result, offset, codec,
                language ? 1U : 0U, read<std::uint32_t>(self, 0x18));
            line(text);
        }
    }
    return result;
}

std::uint32_t __fastcall header_body(void* self, void* output) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_header)(self, output);
    const auto id = read<std::uint32_t>(output, 4);
    if (call.accepts_side_effects() && record(1, watched(id), result)) {
        char text[256]{};
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=bank_header id=%08X requested=%08X watched=%u result=%u version=%u language=%u\n",
            GetTickCount64(), id, g_bank, watched(id) ? 1U : 0U, result,
            read<std::uint32_t>(output, 0), read<std::uint32_t>(output, 8));
        line(text);
    }
    return result;
}

std::uint32_t __fastcall hirc_body(void* self, void* usage, std::uint32_t id) {
    hooking::CallGate::Scope call(g_calls);
    const auto previous = g_loading;
    g_loading = id;
    const auto result = hooking::await_original(g_hirc)(self, usage, id);
    g_loading = previous;
    if (call.accepts_side_effects() && record(2, watched(id), result)) {
        char text[192]{};
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=bank_objects id=%08X requested=%08X watched=%u result=%u loaded=%u reserved=%u\n",
            GetTickCount64(), id, g_bank, watched(id) ? 1U : 0U, result,
            read<std::uint32_t>(usage, 0x68), read<std::uint32_t>(usage, 0x6c));
        line(text);
    }
    return result;
}

/** A version-113 sound object: its ID, the source plugin, one stream-type byte, then the media ID. */
constexpr std::size_t kSoundMediaOffset = 9;

std::uint32_t __fastcall sound_body(void* self, const void* data, std::uint32_t size,
                                   void* usage, bool partial) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_sound)(self, data, size, usage, partial);
    if (call.accepts_side_effects()) {
        const auto id = read<std::uint32_t>(data, 0);
        const auto media = read<std::uint32_t>(data, kSoundMediaOffset);
        const bool interesting = watched(id) || watched(media);
        if (record(8, interesting, result)) {
            char text[288]{};
            std::snprintf(text, sizeof(text),
                "tick=%llu stage=sound_object bank=%08X id=%08X result=%u size=%u plugin=%08X partial=%u media=%08X watched=%u\n",
                GetTickCount64(), g_loading, id, result, size,
                read<std::uint32_t>(data, 4), partial ? 1U : 0U, media, interesting ? 1U : 0U);
            line(text);
        }
    }
    return result;
}

std::uint32_t __fastcall node_body(void* self, const void** cursor, std::uint32_t* size, bool partial) {
    hooking::CallGate::Scope call(g_calls);
    const auto start = read<std::uintptr_t>(cursor, 0);
    const auto result = hooking::await_original(g_node)(self, cursor, size, partial);
    const auto id = read<std::uint32_t>(self, 0x10);
    if (call.accepts_side_effects() && record(9, watched(id), result)) {
        char text[384]{};
        const auto begin = reinterpret_cast<const void*>(start);
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=node_object bank=%08X id=%08X result=%u consumed=%llu remaining=%u prefix=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X watched=%u\n",
            GetTickCount64(), g_loading, id, result,
            static_cast<unsigned long long>(read<std::uintptr_t>(cursor, 0) - start),
            read<std::uint32_t>(size, 0), read<std::uint32_t>(begin, 0), read<std::uint32_t>(begin, 4),
            read<std::uint32_t>(begin, 8), read<std::uint32_t>(begin, 12), read<std::uint32_t>(begin, 16),
            read<std::uint32_t>(begin, 20), read<std::uint32_t>(begin, 24), read<std::uint32_t>(begin, 28),
            watched(id) ? 1U : 0U);
        line(text);
    }
    return result;
}

void pcm(std::size_t kind, void* self, std::uint32_t result) noexcept {
    if (!record(kind, false, result)) return;
    const auto context = read<const void*>(self, 0x18);
    char text[384]{};
    std::snprintf(text, sizeof(text),
        "tick=%llu stage=%s result=%u frames=%u data_bytes=%u data_offset=%u loop_start=%u loop_end=%u rate=%u channel_config=%08X format=%08X\n",
        GetTickCount64(), kind == 3 ? "pcm_memory" : "pcm_stream", result,
        read<std::uint32_t>(self, 0x28), read<std::uint32_t>(self, 0x30),
        read<std::uint32_t>(self, 0x34), read<std::uint32_t>(self, 0x38),
        read<std::uint32_t>(self, 0x3C), read<std::uint32_t>(context, 0x120),
        read<std::uint32_t>(context, 0x124), read<std::uint32_t>(context, 0x128));
    line(text);
}

std::uint32_t __fastcall memory_body(void* self) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_memory)(self);
    if (call.accepts_side_effects()) pcm(3, self, result);
    return result;
}

std::uint32_t __fastcall stream_body(void* self, const void* header) {
    hooking::CallGate::Scope call(g_calls);
    const auto available = read<std::uint32_t>(self, 0x68);
    std::array<unsigned char, 80> prefix{};
    const auto bytes = (std::min)(static_cast<std::size_t>(available), prefix.size());
    const bool captured = call.accepts_side_effects() && copy(header, prefix.data(), bytes);
    const auto result = hooking::await_original(g_stream)(self, header);
    if (call.accepts_side_effects()) {
        pcm(4, self, result);
        if (captured && g_written[4].load(std::memory_order_relaxed) <= 4096) {
            constexpr char digits[] = "0123456789ABCDEF";
            char hex[161]{};
            for (std::size_t i = 0; i < bytes; ++i) {
                hex[i * 2] = digits[prefix[i] >> 4];
                hex[i * 2 + 1] = digits[prefix[i] & 15];
            }
            char text[320]{};
            std::snprintf(text, sizeof(text),
                "tick=%llu stage=pcm_header result=%u available=%u prefix=%s\n",
                GetTickCount64(), result, available, hex);
            line(text);
        }
    }
    return result;
}

/** The numeric event entry has seven arguments. Its public wrapper builds the
 * external-source descriptor and passes it as argument six. The native result
 * is a playing ID, with zero indicating failure, not an AKRESULT.
 */
std::uint32_t __fastcall post_body(std::uint32_t id, std::uint64_t object,
                                  std::uint32_t flags, void* callback, void* cookie,
                                  const void* external, std::uint32_t playing) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_post)(id, object, flags, callback, cookie, external, playing);
    if (call.accepts_side_effects() && record(5, watched(id), result != 0 ? 1U : 0U)) {
        char text[288]{};
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=event_post id=%08X watched=%u object=%016llX flags=%08X requested_playing=%u playing=%u success=%u\n",
            GetTickCount64(), id, watched(id) ? 1U : 0U,
            static_cast<unsigned long long>(object), flags, playing, result, result != 0 ? 1U : 0U);
        line(text);
    }
    return result;
}

/** Numeric CreateAuto and the game's file resolver, validated through the live
 * stream-manager vtables. Observe lookup failures before a codec is created.
 */
std::uint32_t __fastcall auto_body(void* self, std::uint32_t id, void* flags,
                                  const void* heuristics, const void* buffers,
                                  void** output, bool synchronous) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_auto)(self, id, flags, heuristics, buffers, output, synchronous);
    if (call.accepts_side_effects() && record(6, watched(id), result)) {
        char text[288]{};
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=stream_create id=%08X watched=%u result=%u company=%u codec=%u language=%u stream=%p\n",
            GetTickCount64(), id, watched(id) ? 1U : 0U, result,
            read<std::uint32_t>(flags, 0), read<std::uint32_t>(flags, 4),
            read<unsigned char>(flags, 0x18), read<void*>(output, 0));
        line(text);
    }
    return result;
}

std::uint32_t __fastcall file_open_body(void* self, std::uint32_t id, std::uint32_t mode,
                                       void* flags, bool* synchronous, void* descriptor) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_fileOpen)(self, id, mode, flags, synchronous, descriptor);
    if (call.accepts_side_effects() && record(7, watched(id), result)) {
        char text[320]{};
        std::snprintf(text, sizeof(text),
            "tick=%llu stage=file_open id=%08X watched=%u result=%u mode=%u company=%u codec=%u language=%u synchronous=%u bytes=%llu device=%u\n",
            GetTickCount64(), id, watched(id) ? 1U : 0U, result, mode,
            read<std::uint32_t>(flags, 0), read<std::uint32_t>(flags, 4),
            read<unsigned char>(flags, 0x18), read<unsigned char>(synchronous, 0),
            static_cast<unsigned long long>(read<std::uint64_t>(descriptor, 0)),
            read<std::uint32_t>(descriptor, 0x20));
        line(text);
    }
    return result;
}

bool idle() noexcept { return g_calls.idle(); }

/** Logs why the watch file was refused; the trace then stays detached. */
void refuse(const char* reason, std::size_t lineNumber) noexcept {
    core::log::writef(core::log::Channel::client, core::log::Level::warn,
                      "ev=audio_trace stage=watch result=fail reason=%s line=%zu", reason, lineNumber);
}

/**
 * Wwise IDs to follow, eight hex digits per line.
 * @return True when the file holds at least one ID and nothing else. A missing file is silent;
 * any other refusal is logged with its reason and line.
 */
bool watch_file() noexcept {
    core::path::Buffer path;
    if (!core::path::artifact_file(L"audio_trace_watch.txt", path)
        || GetFileAttributesW(path.chars.data()) == INVALID_FILE_ATTRIBUTES) return false;
    const HANDLE input = CreateFileW(path.chars.data(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        refuse("unreadable", 0);
        return false;
    }
    // Static: the file can be large, and this runs on the game's thread during activation.
    static std::array<char, 147457> text{};
    LARGE_INTEGER size{};
    DWORD bytes{};
    const bool measured = GetFileSizeEx(input, &size) != FALSE;
    // A file past the buffer is refused whole rather than read up to a line it cuts in half.
    const bool fits = measured && size.QuadPart >= 0
                      && static_cast<std::uint64_t>(size.QuadPart) < text.size();
    const bool ok = fits
                    && ReadFile(input, text.data(), static_cast<DWORD>(text.size() - 1), &bytes, nullptr) != FALSE;
    CloseHandle(input);
    if (!ok) {
        refuse(measured && !fits ? "too_large" : "unreadable", 0);
        return false;
    }
    text[bytes] = '\0';
    g_watchCount = 0;
    std::uint32_t value{};
    std::size_t digits{};
    std::size_t lineNumber = 1;
    for (std::size_t i = 0; i <= bytes; ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit >= 0) {
            if (++digits > 8) {
                refuse("digits", lineNumber);
                return false;
            }
            value = value * 16U + static_cast<std::uint32_t>(digit);
        } else if (c == '\r' || c == '\n' || c == 0) {
            if (digits != 0) {
                if (digits != 8) {
                    refuse("digits", lineNumber);
                    return false;
                }
                if (g_watchCount == g_watch.size()) {
                    refuse("too_many", lineNumber);
                    return false;
                }
                g_watch[g_watchCount++] = value;
                value = 0;
                digits = 0;
            }
            if (c == '\n') ++lineNumber;
        } else {
            refuse("syntax", lineNumber);
            return false;
        }
    }
    if (g_watchCount == 0) {
        refuse("no_ids", 0);
        return false;
    }
    std::sort(g_watch.begin(), g_watch.begin() + g_watchCount);
    return true;
}
} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_handles[0].attached) {
        const bool accepting = g_calls.accepting();
        ReleaseSRWLockExclusive(&g_lifecycle);
        return accepting;
    }
    if (!watch_file()) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::array replacements{reinterpret_cast<void*>(&open_body), reinterpret_cast<void*>(&header_body),
        reinterpret_cast<void*>(&hirc_body), reinterpret_cast<void*>(&memory_body), reinterpret_cast<void*>(&stream_body),
        reinterpret_cast<void*>(&post_body), reinterpret_cast<void*>(&auto_body),
        reinterpret_cast<void*>(&file_open_body), reinterpret_cast<void*>(&sound_body),
        reinterpret_cast<void*>(&node_body)};
    std::array<hooking::detour::Spec, 10> specs{};
    for (std::size_t i = 0; i < kTargets.size(); ++i) {
        std::array<unsigned char, 16> bytes{};
        void* target = reinterpret_cast<void*>(base + kTargets[i].rva);
        if (!copy(target, bytes.data(), bytes.size()) || bytes != kTargets[i].prefix) {
            core::log::writef(core::log::Channel::client, core::log::Level::warn,
                             "ev=audio_trace stage=install result=fail reason=prefix rva=%llX",
                             static_cast<unsigned long long>(kTargets[i].rva));
            ReleaseSRWLockExclusive(&g_lifecycle);
            return false;
        }
        specs[i] = {target, replacements[i]};
    }
    core::path::Buffer path;
    if (core::path::artifact_file(L"audio_trace.log", path)) {
        g_file = CreateFileW(path.chars.data(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_file == INVALID_HANDLE_VALUE) {
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=audio_trace stage=install result=fail reason=log_file");
        ReleaseSRWLockExclusive(&g_lifecycle);
        return false;
    }
    g_head = g_count = 0;
    g_queued.store(0, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = g_stop != nullptr ? CreateThread(nullptr, 0, writer, nullptr, 0, nullptr) : nullptr;
    if (g_worker == nullptr) {
        close_output();
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=audio_trace stage=install result=fail reason=writer_thread");
        ReleaseSRWLockExclusive(&g_lifecycle);
        return false;
    }
    g_calls.quiesce();
    for (auto& count : g_seen) count.store(0, std::memory_order_relaxed);
    for (auto& count : g_written) count.store(0, std::memory_order_relaxed);
    const bool installed = hooking::detour::install(specs, g_handles);
    if (installed) {
        hooking::publish_original(g_open, reinterpret_cast<Open>(g_handles[0].original));
        hooking::publish_original(g_header, reinterpret_cast<Header>(g_handles[1].original));
        hooking::publish_original(g_hirc, reinterpret_cast<Hirc>(g_handles[2].original));
        hooking::publish_original(g_memory, reinterpret_cast<Memory>(g_handles[3].original));
        hooking::publish_original(g_stream, reinterpret_cast<Stream>(g_handles[4].original));
        hooking::publish_original(g_post, reinterpret_cast<Post>(g_handles[5].original));
        hooking::publish_original(g_auto, reinterpret_cast<AutoStream>(g_handles[6].original));
        hooking::publish_original(g_fileOpen, reinterpret_cast<FileOpen>(g_handles[7].original));
        hooking::publish_original(g_sound, reinterpret_cast<Sound>(g_handles[8].original));
        hooking::publish_original(g_node, reinterpret_cast<Node>(g_handles[9].original));
        line("stage=install result=ok version=7 hooks=10 native_forwarding=unchanged result_success=1 writer=background object_filter=object_or_media\n");
        g_calls.accept();
    } else {
        close_output();
    }
    core::log::writef(core::log::Channel::client,
                     installed ? core::log::Level::info : core::log::Level::warn,
                     "ev=audio_trace stage=install result=%s%s watch_ids=%zu",
                     installed ? "ok" : "fail", installed ? "" : " reason=detour", g_watchCount);
    ReleaseSRWLockExclusive(&g_lifecycle);
    return installed;
}

bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    g_calls.quiesce();
    if (!g_handles[0].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&open_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&header_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&hirc_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&memory_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&stream_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&post_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&auto_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&file_open_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&sound_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&node_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&hooking::call_gate_detail::enter)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&hooking::call_gate_detail::leave)},
    };
    const bool removed = probes::detach(g_handles, protectedEntries, &idle)
                         == hooking::detour::UninstallResult::removed;
    if (removed) {
        g_open.store(nullptr); g_header.store(nullptr); g_hirc.store(nullptr);
        g_memory.store(nullptr); g_stream.store(nullptr); g_post.store(nullptr);
        g_auto.store(nullptr); g_fileOpen.store(nullptr);
        g_sound.store(nullptr); g_node.store(nullptr);
        line("stage=uninstall result=ok\n");
        close_output();
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    return removed;
}
} // namespace dawn::client::hooks::probes::audio_trace
