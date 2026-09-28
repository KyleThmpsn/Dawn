/**
 * Every effect the game applies or removes, perk spawns, weapon fire sources and projectiles,
 * logged as they happen. This file holds the trace's lifecycle and its line writer; effects.cpp,
 * spawn.cpp and projectile.cpp hold the hooks. Every hook formats on the thread it runs on and
 * queues the line; one thread of its own hands queued lines to the Dawn log, so a burst of
 * launches never waits on the disk.
 *
 * Turn on: `"effect_trace": true` in the `client` section of Dawn\settings.json.
 * Output: `ev=effect_trace` lines in Dawn\logs\dawn.log. `stage=effect` is one effect applied or
 * removed, with its kind, the definition tag that owns it and the owner; `stage=effect_definition`
 * and `stage=effect_node` dump a definition or node the first time it appears. `stage=spawn` and
 * `stage=spawn_asset` describe one perk spawn request. `stage=fire_source` describes a weapon the
 * first time it fires, and `stage=fire_summary` counts its shots every 5 seconds.
 * `stage=launch` is one projectile and `stage=flight` its early flight. `stage=region` and
 * `stage=raw` dump a source or projectile type in full the first time it appears. They are info
 * lines, so `core.logging.levels.client` must be `info` or `debug`.
 * Limits: eight spawn requests per graph and 16 effect lines per definition and action every 10
 * seconds. The rest are counted in a `stage=spawn_suppressed` or `stage=effect_suppressed` line,
 * written when those 10 seconds end or at shutdown. Flight is sampled at 50, 150, 400 and 1000
 * ms. A full queue drops lines and reports how many at shutdown.
 */

#include "effect_trace.h"
#include "internal.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>

#include "../../../../core/logging/log.h"

namespace dawn::client::hooks::probes::effect_trace {
namespace {

/** Lines waiting for the writer. At one drain per interval this absorbs a heavy firefight. */
constexpr std::size_t kQueueCapacity = 2048;
/** Also the flight sampling cadence, so the earliest sample lands close to its target age. */
constexpr DWORD kDrainIntervalMs = 25;
/** Lines the writer copies out per lock hold. */
constexpr std::size_t kBatchLines = 32;
/** How often the writer closes out spawn budgets whose interval has ended. */
constexpr std::uint64_t kBudgetServiceMs = 1'000;

struct Slot final {
    std::uint16_t length{};
    std::array<char, kLineCapacity> text{};
};

SRWLOCK g_lifecycle{SRWLOCK_INIT};
SRWLOCK g_queueLock{SRWLOCK_INIT};
std::array<Slot, kQueueCapacity> g_queue{};
std::size_t g_head{}, g_count{};
/** Only the writer thread touches this. */
std::array<Slot, kBatchLines> g_batch{};
std::atomic_bool g_accepting{};
std::atomic<std::uint64_t> g_dropped{};
HANDLE g_stop{}, g_worker{};
bool g_installed{};

/** Writer thread only: empties the queue into the Dawn log in batches. */
void drain() noexcept {
    for (;;) {
        AcquireSRWLockExclusive(&g_queueLock);
        const std::size_t count = (std::min)(kBatchLines, g_count);
        for (std::size_t i = 0; i < count; ++i) {
            g_batch[i] = g_queue[(g_head + i) % g_queue.size()];
        }
        g_head = (g_head + count) % g_queue.size();
        g_count -= count;
        ReleaseSRWLockExclusive(&g_queueLock);
        if (count == 0) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {g_batch[i].text.data(), g_batch[i].length});
        }
    }
}

DWORD WINAPI writer(LPVOID) noexcept {
    std::uint64_t nextBudgets = 0;
    while (WaitForSingleObject(g_stop, kDrainIntervalMs) == WAIT_TIMEOUT) {
        const std::uint64_t now = GetTickCount64();
        service_flights(now);
        if (now >= nextBudgets) {
            service_spawn_budgets(now, false);
            service_effect_budgets(now, false);
            nextBudgets = now + kBudgetServiceMs;
        }
        drain();
    }
    drain();
    return 0;
}

[[nodiscard]] bool start_writer() noexcept {
    g_head = g_count = 0;
    g_dropped.store(0, std::memory_order_relaxed);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_worker = g_stop != nullptr ? CreateThread(nullptr, 0, &writer, nullptr, 0, nullptr) : nullptr;
    if (g_worker == nullptr) {
        if (g_stop != nullptr) {
            CloseHandle(g_stop);
            g_stop = nullptr;
        }
        return false;
    }
    g_accepting.store(true, std::memory_order_release);
    return true;
}

/** Stops accepting, lets the writer empty the queue, and reports any lines a full queue dropped. */
void stop_writer() noexcept {
    g_accepting.store(false, std::memory_order_release);
    if (g_worker != nullptr) {
        SetEvent(g_stop);
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_stop != nullptr) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    const auto dropped = g_dropped.load(std::memory_order_relaxed);
    if (dropped != 0) {
        core::log::writef(core::log::Channel::client, core::log::Level::warn,
                          "ev=effect_trace stage=writer result=dropped lines=%llu",
                          static_cast<unsigned long long>(dropped));
    }
}

} // namespace

void emit(std::string_view text) noexcept {
    if (!g_accepting.load(std::memory_order_acquire)) {
        return;
    }
    const std::size_t length = (std::min)(text.size(), kLineCapacity);
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_count == g_queue.size()) {
        ReleaseSRWLockExclusive(&g_queueLock);
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Slot& slot = g_queue[(g_head + g_count) % g_queue.size()];
    std::memcpy(slot.text.data(), text.data(), length);
    slot.length = static_cast<std::uint16_t>(length);
    ++g_count;
    ReleaseSRWLockExclusive(&g_queueLock);
}

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_installed) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    if (!start_writer()) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        core::log::write(core::log::Channel::client, core::log::Level::warn,
                         "ev=effect_trace stage=install result=fail reason=writer_thread");
        return false;
    }
    const bool spawn = install_spawn();
    const bool projectiles = install_projectiles();
    const bool effects = install_effects();
    g_installed = spawn || projectiles || effects;
    if (!g_installed) {
        stop_writer();
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    core::log::writef(core::log::Channel::client,
                      g_installed ? core::log::Level::info : core::log::Level::warn,
                      "ev=effect_trace stage=install result=%s spawn=%u projectiles=%u effects=%u "
                      "mode=read_only writer=background", g_installed ? "ok" : "fail",
                      spawn ? 1U : 0U, projectiles ? 1U : 0U, effects ? 1U : 0U);
    return g_installed;
}

bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (!g_installed) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    // Every hook set must leave before the writer stops, or a late hook would queue into nothing.
    if (!uninstall_effects() || !uninstall_projectiles() || !uninstall_spawn()) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return false;
    }
    flush_flights();
    const std::uint64_t now = GetTickCount64();
    service_spawn_budgets(now, true);
    service_effect_budgets(now, true);
    stop_writer();
    g_installed = false;
    ReleaseSRWLockExclusive(&g_lifecycle);
    return true;
}

} // namespace dawn::client::hooks::probes::effect_trace
