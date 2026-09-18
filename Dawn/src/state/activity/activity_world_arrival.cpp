#include <Windows.h>

#include <atomic>

#include "runtime.h"

namespace dawn::state::activity {
namespace {

/**
 * How far the client has got through the current destination load.
 * The spawn gate writes and reads this, so it is an atomic, not lock-guarded State. It is one
 * observed value and no transaction depends on it.
 */
std::atomic<WorldPhase> g_phase{WorldPhase::idle};
/** Tick the current load started on. Read only while the phase is transitioning. */
std::atomic<std::uint64_t> g_transitionTick{};
/**
 * Armed after the opening has reached in-world, then retained through later phase transitions.
 * Seeding before this point advances the active mission phase while the opening slice is still
 * instantiating and filters out its phase-zero player citizen.
 */
std::atomic<bool> g_missionSeedArmed{false};
/** A return off-destination ends the run; ordinary region transitions retain this value. */
std::atomic<std::uint64_t> g_missionRunGeneration{1};
/** Set only after native observation resolves the current instance's mission-state storage. */
std::atomic<bool> g_missionAuthorityRuntimeInitialized{false};
/** Set once the native type-7 request is issued; silences Omega authority emission. */
std::atomic<bool> g_omegaAuthorityQuiesced{false};
/**
 * Omega forest-entrance arrival latch. Unlike the per-instance latches above, this survives
 * activity teardown on purpose: the entrance sense sets it in the mission instance and the NEXT
 * mission_scot launch consumes it to arrive inside the forest bubble (region 88).
 */
std::atomic<bool> g_omegaForestArrivalPending{false};
/** Native type-53 record-0 acknowledgement that orders Tower Watch's breach beat. */
std::atomic<bool> g_towerWatchOpeningDialogueProcessed{false};

} // namespace

/** Records how far the current destination load has got. */
void note_world_phase(WorldPhase phase) noexcept {
    const WorldPhase previous = g_phase.load(std::memory_order_relaxed);
    // Stamped before the phase, so a reader that sees transitioning never reads a stale tick.
    if (phase == WorldPhase::transitioning && previous != WorldPhase::transitioning) {
        g_transitionTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    if (phase == WorldPhase::arrived) {
        g_missionSeedArmed.store(true, std::memory_order_relaxed);
    } else if (phase == WorldPhase::idle) {
        if (previous != WorldPhase::idle) {
            g_missionRunGeneration.fetch_add(1, std::memory_order_release);
        }
        g_missionSeedArmed.store(false, std::memory_order_relaxed);
        g_omegaAuthorityQuiesced.store(false, std::memory_order_relaxed);
        g_towerWatchOpeningDialogueProcessed.store(false, std::memory_order_relaxed);
    }
    g_phase.store(phase, std::memory_order_relaxed);
}

/** @return How far the client has got through the current destination load. */
WorldPhase world_phase() noexcept {
    return g_phase.load(std::memory_order_relaxed);
}

/** @return Whether the current destination has reached in-world at least once. */
bool mission_seed_armed() noexcept {
    return g_missionSeedArmed.load(std::memory_order_relaxed);
}

/** Server route: the forest-entrance sense arms the next-launch forest arrival. */
void note_omega_forest_arrival_pending() noexcept {
    g_omegaForestArrivalPending.store(true, std::memory_order_release);
}

/** Session creation: one-shot consume; the launch that eats this arrives in the forest. */
bool consume_omega_forest_arrival_pending() noexcept {
    return g_omegaForestArrivalPending.exchange(false, std::memory_order_acq_rel);
}

std::uint64_t mission_run_generation() noexcept {
    return g_missionRunGeneration.load(std::memory_order_acquire);
}

/** Client: record the first native acknowledgement of Tower Watch's opening Ghost line. */
bool mark_tower_watch_opening_dialogue_processed() noexcept {
    return !g_towerWatchOpeningDialogueProcessed.exchange(true, std::memory_order_acq_rel);
}

/** Host: retain the acknowledgement until the world returns to idle. */
bool tower_watch_opening_dialogue_processed() noexcept {
    return g_towerWatchOpeningDialogueProcessed.load(std::memory_order_acquire);
}

/** Server publication paths read this before arming any Omega body. */
bool omega_authority_quiesced() noexcept {
    return g_omegaAuthorityQuiesced.load(std::memory_order_acquire);
}

/** Latches the client's successful native mission-storage observation. */
bool acknowledge_mission_authority_runtime_initialized() noexcept {
    return !g_missionAuthorityRuntimeInitialized.exchange(true, std::memory_order_acq_rel);
}

/** Clears the type-18 acknowledgement for a newly allocated activity instance. */
void reset_mission_authority_runtime_initialization() noexcept {
    g_missionAuthorityRuntimeInitialized.store(false, std::memory_order_release);
}

/** @return Whether the current activity instance has applied its type-18 storage runtime. */
bool mission_authority_runtime_initialized() noexcept {
    return g_missionAuthorityRuntimeInitialized.load(std::memory_order_acquire);
}

/** @return Milliseconds since the running load started, or zero when none is running. */
std::uint64_t world_transition_age() noexcept {
    if (g_phase.load(std::memory_order_relaxed) != WorldPhase::transitioning) {
        return 0;
    }
    const std::uint64_t started = g_transitionTick.load(std::memory_order_relaxed);
    const std::uint64_t now = GetTickCount64();
    return now > started ? now - started : 0;
}

} // namespace dawn::state::activity
