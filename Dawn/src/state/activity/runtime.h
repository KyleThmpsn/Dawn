#pragma once

#include <cstdint>

#include "definition.h"
#include "entity_slots/runtime.h"

namespace dawn::state::activity {

/**
 * Prepares one allocation with State's fixed default destination, without changing State.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the picked record and allocator revisions can be committed.
 */
[[nodiscard]] bool prepare_session(std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Prepares an allocation that may atomically replace one exact caller-owned record.
 * A present replacement is the only occupied slot this transaction may reuse.
 */
[[nodiscard]] bool prepare_session(ActivityInstanceKey replaces,
                                   std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Prepares one allocation with an explicit checked scalar destination.
 * @param selection Caller-owned destination, copied into the read-only allocation plan.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the destination and allocator snapshot can be committed together.
 */
[[nodiscard]] bool prepare_session(const destination::DestinationSelection& selection,
                                   std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/** Prepares an explicit-destination allocation with an optional exact replacement. */
[[nodiscard]] bool prepare_session(const destination::DestinationSelection& selection,
                                   ActivityInstanceKey replaces,
                                   std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Commits one prepared activity-session allocation when its revisions still match.
 * @param allocation Prepared plan. Always cleared before this function returns.
 * @return True when the allocation committed in one step.
 */
[[nodiscard]] bool commit(PendingAllocation& allocation) noexcept;

/**
 * Frees one committed activity-session record.
 * Nothing else clears one. A host that allocates per region must release them, or the table
 * fills and later allocations fail closed.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when a record held that id and is now free.
 */
bool release_session(std::uint64_t sessionId) noexcept;

/**
 * Frees a committed record only when both its session id and incarnation still match.
 * @param key Exact key captured from the record's committed lifetime.
 * @return True when that exact lifetime was retired.
 */
bool release_session(ActivityInstanceKey key) noexcept;

/**
 * Retires exactly one activity lifetime without allowing revision exhaustion to strand it.
 * Repeating the same retirement is safe and reports alreadyRetired.
 */
[[nodiscard]] RetireResult retire_session_exact(ActivityInstanceKey key) noexcept;

/**
 * Tests whether a nonzero activity session id is still in the fixed-size table.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the id is in a committed record.
 */
[[nodiscard]] bool contains(std::uint64_t sessionId) noexcept;

/** @return True only while the exact activity incarnation remains committed. */
[[nodiscard]] bool contains(ActivityInstanceKey key) noexcept;

/**
 * Captures the exact incarnation for one current session id.
 * @param sessionId Nonzero protocol session id.
 * @param output Cleared first, then receives the exact key.
 */
[[nodiscard]] bool snapshot_instance_key(std::uint64_t sessionId,
                                         ActivityInstanceKey& output) noexcept;

/**
 * Captures the current host-region generation for an exact activity incarnation.
 * @param activity Exact committed activity key.
 * @param output Cleared first, then receives the exact region key.
 */
[[nodiscard]] bool snapshot_host_region_key(ActivityInstanceKey activity,
                                            HostRegionKey& output) noexcept;

/**
 * Tests whether a committed activity session id has finished a join.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the current record has a committed join revision.
 */
[[nodiscard]] bool is_joined(std::uint64_t sessionId) noexcept;

/**
 * Returns the newest joined activity session in the bounded table.
 * @return Its public session id, or zero while no join is committed.
 */
[[nodiscard]] std::uint64_t newest_joined_session() noexcept;

/** @return The exact most recently created joined activity lifetime, or an absent key. */
[[nodiscard]] ActivityInstanceKey newest_joined_activity() noexcept;

/** How far the client has got through the current destination load. */
enum class WorldPhase : std::uint8_t {
    /** No destination load is running. Orbit sits here, and the spawn is never held. */
    idle,
    /** The step that arms the black fade has started and the in-world step has not been reached. */
    transitioning,
    /** The in-world step is entered, so the fade is armed and a spawn now releases it. */
    arrived,
};

/**
 * Records how far the current destination load has got.
 * Entering transitioning from any other phase resets the load's start tick.
 * @param phase Phase the client's own boot-flow step maps to.
 */
void note_world_phase(WorldPhase phase) noexcept;

/** @return How far the client has got through the current destination load. */
[[nodiscard]] WorldPhase world_phase() noexcept;

/**
 * @return Whether authored mission bodies may be seeded. This becomes true on the first in-world
 * entry and remains true through mission phase transitions until the client returns to orbit.
 */
[[nodiscard]] bool mission_seed_armed() noexcept;

/** Run identity advances on return to idle/orbit and survives transitions within a run. */
[[nodiscard]] std::uint64_t mission_run_generation() noexcept;

/**
 * Server-side forest-entrance latch -> client game-thread transition request. The server sets
 * this when the Omega forest-entrance monitor sense latches (portal_mutation experiment); the
 * client's frame poll consumes it and installs the native type-7 pending request.
 */


/**
 * The game thread issued the native type-7 request: every Omega authority emission must stop
 * immediately so no dialogue/scene body applies into components the slice teardown is freeing
 * (the index-heap double-free). Cleared with the seed latch on the next idle world phase.
 */

/** @return True once the transition was issued; Omega authority emission must be silent. */
[[nodiscard]] bool omega_authority_quiesced() noexcept;

/**
 * The forest-entrance sense arms the next mission_scot launch to arrive inside the forest
 * bubble (region 88) through the initial-slice-set path. Survives activity teardown.
 */
void note_omega_forest_arrival_pending() noexcept;

/** @return True exactly once; the consuming launch applies the forest arrival override. */
[[nodiscard]] bool consume_omega_forest_arrival_pending() noexcept;

/**
 * Client dialogue consumer -> host Tower Watch progression latch. The native type-53 scanner
 * sets this only after opening record 0 changes from unprocessed to processed.
 * @return True only when this world lifetime records the edge for the first time.
 */
[[nodiscard]] bool mark_tower_watch_opening_dialogue_processed() noexcept;

/** @return Whether Tower Watch's opening Ghost record was processed in this world lifetime. */
[[nodiscard]] bool tower_watch_opening_dialogue_processed() noexcept;

/**
 * Latches that a native observer resolved the constructed mission-state storage.
 * Omega's active arrival owner requires both type 18 and type 35 in the loaded world.
 * This proves authority materialization only; it does not prove a mission executor is running.
 * @return True only for the first acknowledgement in the current activity instance.
 */
[[nodiscard]] bool acknowledge_mission_authority_runtime_initialized() noexcept;

/** Clears the type-18 acknowledgement at a new activity-instance boundary. */
void reset_mission_authority_runtime_initialization() noexcept;

/** @return Whether type-18 mission-state storage has been applied for this activity instance. */
[[nodiscard]] bool mission_authority_runtime_initialized() noexcept;

/** @return Milliseconds since the running load started, or zero when none is running. */
[[nodiscard]] std::uint64_t world_transition_age() noexcept;

} // namespace dawn::state::activity
