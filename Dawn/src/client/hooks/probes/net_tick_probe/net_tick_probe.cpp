/**
 * Whether the game's networking tick is stuck, and whether the callback pump still runs.
 * The tick guards itself with a one-byte latch that is set while a tick runs. A latch held set
 * across checks means a tick never finished; a pump that stops means the main loop did.
 *
 * Turn on: `"stall_trace": true` in the `client` section of Dawn\settings.json.
 * Output: `ev=net_tick` lines in Dawn\logs\dawn.log: `stage=resolve` once, then `stage=sample` on
 * the first check and whenever a held latch or a stopped pump starts or ends. They are info
 * lines, so `core.logging.levels.client` must be `info` or `debug`.
 * Limits: checked every 2 seconds from the stall probe's thread. The latch counts as held after
 * three checks in a row read it set.
 */

#include "net_tick_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../patterns/image_scan.h"
#include "../../../patterns/signature_text.h"
#include "../stall_probe/stall_probe.h"

namespace dawn::client::hooks::probes::net_tick_probe {
namespace {

/**
 * The guarded entry of the game's whole networking tick.
 * It reads an enable byte, then calls the re-entrancy test and returns when that test is true.
 * Both RIP-relative operands, the call and both jump displacements are wildcarded.
 */
inline constexpr std::string_view kTickGuardText =
    "48 83 EC 28 80 3D ? ? ? ? 00 0F 84 ? ? ? ? E8 ? ? ? ? 84 C0 0F 85";
/** Compiled length of the tick-guard signature, counted from its text at build time. */
inline constexpr std::size_t kTickGuardSize = patterns::signature_length(kTickGuardText);
constinit const std::array<patterns::PatternByte, kTickGuardSize> kTickGuard =
    patterns::signature<kTickGuardSize>(kTickGuardText);

/** Displacement of the call to the re-entrancy test, and the instruction after it. */
constexpr std::size_t kTestCallOperand = 18;
constexpr std::size_t kTestCallNext = 22;

/** The test is `movzx eax, byte ptr [rip+disp]` then `retn`, so its shape is checkable. */
constexpr std::byte kMovzxEaxByte0{0x0F};
constexpr std::byte kMovzxEaxByte1{0xB6};
constexpr std::byte kMovzxEaxByte2{0x05};
constexpr std::byte kRetn{0xC3};
/** Displacement of the latch inside the test, and the instruction after it. */
constexpr std::size_t kLatchOperand = 3;
constexpr std::size_t kLatchNext = 7;
/** Byte after the whole test, which must be the return. */
constexpr std::size_t kTestReturn = 7;

/** How often the latch and the pump are checked. */
constexpr std::uint64_t kSampleIntervalMs = 2'000;
/**
 * The latch is set for as long as one networking tick runs, so a single check lands on it now and
 * then in healthy play. Only this many checks in a row reading it set count as a held latch.
 */
constexpr unsigned kHeldSamples = 3;

/** Set once by `prepare`; until then `sample` returns at once. */
std::atomic<bool> g_enabled{};
std::atomic<bool> g_resolved{};
std::atomic<const std::uint8_t*> g_latch{};
std::atomic<std::uint64_t> g_dueTick{};

/** The previous check. Only the caller that wins the due-tick exchange touches it. */
struct Checked final {
    bool done{};
    unsigned latch{};
    /** Checks in a row, this one included, that read the same set latch. */
    unsigned latchRun{};
    std::uint64_t pumpCalls{};
    bool pumpStopped{};
};
Checked g_previous{};

/** Resolves the latch address from two decoded operands. Runs at most once. */
void resolve_once() noexcept {
    if (g_resolved.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::byte* const guard = patterns::scan_main_image_unique(kTickGuard, "net_tick_guard");
    if (guard == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=net_tick stage=resolve result=fail reason=signature");
        return;
    }
    const std::byte* const test =
        patterns::resolve_relative(guard + kTestCallOperand, guard + kTestCallNext);
    // The second derivation reads an operand out of whatever the call landed on, so the shape of
    // that function is checked before its bytes are trusted as an operand.
    if (test == nullptr || test[0] != kMovzxEaxByte0 || test[1] != kMovzxEaxByte1
        || test[2] != kMovzxEaxByte2 || test[kTestReturn] != kRetn) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=net_tick stage=resolve result=fail reason=shape");
        return;
    }
    const auto* const latch = reinterpret_cast<const std::uint8_t*>(
        patterns::resolve_relative(test + kLatchOperand, test + kLatchNext));
    if (latch == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=net_tick stage=resolve result=fail reason=operand");
        return;
    }
    g_latch.store(latch, std::memory_order_release);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=net_tick stage=resolve result=ok latch=0x%p",
                                      static_cast<const void*>(latch));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Enables sampling and resolves the latch during activation. */
void prepare() noexcept {
    resolve_once();
    g_enabled.store(true, std::memory_order_release);
}

/** Reports the networking-tick latch and the pump, but only when a held latch or a stopped pump
    starts or ends. */
void sample() noexcept {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint8_t* const latch = g_latch.load(std::memory_order_acquire);
    if (latch == nullptr) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    std::uint64_t due = g_dueTick.load(std::memory_order_relaxed);
    if (now < due
        || !g_dueTick.compare_exchange_strong(
            due, now + kSampleIntervalMs, std::memory_order_relaxed)) {
        return;
    }
    // A healthy session writes the first check and then nothing; a held latch or a stopped pump
    // is written once when it starts and once when it ends.
    const unsigned value = *latch;
    const std::uint64_t pumpCalls = stall_probe::pump_count();
    // Measured against zero on the first check, so a pump the game has not started reads stopped.
    const bool pumpStopped = pumpCalls == g_previous.pumpCalls;
    const unsigned latchRun = value == 0 ? 0U
                              : g_previous.done && value == g_previous.latch ? g_previous.latchRun + 1U
                                                                              : 1U;
    const bool latchHeld = latchRun >= kHeldSamples;
    const bool changed = !g_previous.done || pumpStopped != g_previous.pumpStopped
                         || latchHeld != (g_previous.latchRun >= kHeldSamples);
    g_previous = {true, value, (std::min)(latchRun, kHeldSamples), pumpCalls, pumpStopped};
    if (!changed) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=net_tick stage=sample latch=%u latch_held=%u pump=%s "
                                      "pump_calls=%llu",
                                      value, latchHeld ? 1U : 0U,
                                      pumpStopped ? "stopped" : "moving",
                                      static_cast<unsigned long long>(pumpCalls));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace dawn::client::hooks::probes::net_tick_probe
