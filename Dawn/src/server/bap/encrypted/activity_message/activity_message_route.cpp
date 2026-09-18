#include "../../../../state/activity/Newlight/launchpad/runtime.h"
#include "activity_message_route.h"
#include "lost_sector_rewards.h"
#include "../push/activity/native_activity_publisher.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "adventure_start_route.h"
#include "omega_roster_readiness.h"
#include "omega_monitor_edges.h"
#include "omega_opening_intake.h"
#include "../../../../state/activity/coo/omega_adapter.h"
#include "../../../runtime/activity/native_activity_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../core/settings/settings.h"
#include "../../../../middleware/bap/activity_message/activity_client_identity_parser.h"
#include "../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../middleware/bap/activity_message/activity_high_water_validator.h"
#include "../../../../middleware/bap/activity_message/activity_join_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_membership_acknowledgement_parser.h"
#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../../../../middleware/bap/activity_message/activity_state_refresh_parser.h"
#include "../../../../middleware/bap/activity_message/client_authoritative_data.h"
#include "../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../middleware/bap/activity_message/incident.h"
#include "../../../../middleware/bap/activity_message/loot_pickup.h"
#include "../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../middleware/bap/activity_message/tower_watch_cue_manifest.h"
#include "../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../state/activity/omega_ending.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/activity/strike_pact/runtime.h"
#include "../../../../state/activity/strike_bond/runtime.h"
#include "../../../../state/activity/coo/native_player_trigger.h"
#include "../../../../state/activity/omega/omega_progression.h"
#include "../../../../state/build_data/scenarios/cue_graph_manifest_exporter.h"
#include "membership/activity_membership_route.h"
#include "../diagnostics/omega_trace.h"
#include "middleware/bap/activity_message/activity_entity_slot_request_parser.h"
#include "patch_epoch/activity_patch_epoch_route.h"
#include "festival_pickups.h"
#include "forest_chest_rewards.h"
#include "forest_loot_pickups.h"

namespace dawn::server::bap::encrypted::activity_message {
namespace {

using omega_roster_readiness::exact_body;
using omega_roster_readiness::exact_omega_initial_report;

namespace service = middleware::bap::activity_message;
namespace authority = service::entity_authority;
namespace client_keepalive = service::client_keepalive;
namespace high_water = service::high_water;
namespace epoch_message = service::patch_epoch;
namespace tower_watch = service::tower_watch;

/** Activity message type 3 starts the client join transaction. */
constexpr std::uint32_t kJoinRequestMessageType = 3;
/** Enough bytes to distinguish the two activity-start requests without flooding the log. */
constexpr std::size_t kActivityStartPreviewBytes = 64;
/** Holds one observed sense body while bounding an unexpected production-sized update. */
constexpr std::size_t kSenseUpdateCaptureBytes = 320;
constexpr std::string_view kOmegaDestination = "mission_scot";
/** Exact opening-volume entered edge recovered from D00142CF/type-30/index-20. */
constexpr std::uint32_t kOmegaOpeningRegistry = 0xD00142CFU;
constexpr std::uint8_t kOmegaOpeningSlotType = 30;
constexpr std::uint16_t kOmegaOpeningSlotIndex = 20;
/** Exact authored player-monitor entered edge at the Infinite Forest entrance. */
constexpr std::uint16_t kOmegaForestEntranceSlotIndex = 24;
/** Observed Scene body, excluding its group terminator; never a progression predicate. */
constexpr std::uint8_t kOmegaSceneSlotType = 43;
constexpr std::uint16_t kOmegaSceneSlotIndex = 1;
constexpr std::uint32_t kOmegaSceneObservationBits = 139;
constexpr std::uint64_t kOmegaSceneObservationFirst = 0xC07607CB084F2555ULL;
constexpr std::uint64_t kOmegaSceneObservationSecond = 0x4A0AB3190D600000ULL;
constexpr std::uint64_t kOmegaSceneObservationThird = 0x5ULL;
constexpr std::uint32_t kOmegaSceneObservationNextRevision = 5;

/** One row per Client-sent message this route accepts but has no state to change for. */
struct AcceptedMessage {
    std::uint32_t type;
    const char* name;
};

/**
 * The Client senders that carry no work for this host. Each is one-way, so accepting is the whole
 * contract. The names are the binary's own, so a log line says what arrived.
 */
constexpr std::array<AcceptedMessage, 12> kAcceptedMessages{{
    {8, "request_activity_host"},
    {13, "request_peer_reservation"},
    {14, "release_peer_reservation"},
    {15, "peer_leave_request"},
    {34, "process_debug_command"},
    {37, "connectivity_failure"},
    {39, "send_client_heartbeat"},
    {43, "bug_claw"},
    {46, "report_lag_switch"},
    {47, "connection_quality_report"},
    {48, "speculative_migration"},
    {50, "refresh_inspirations"},
}};

/** Dawn's legacy FNV-like diagnostic hash over the complete packet. */
[[nodiscard]] std::uint64_t payload_hash(std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte byte : payload) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] bool same_epoch(const service::patch_epoch::PatchEpoch& left,
                              const service::patch_epoch::PatchEpoch& right) noexcept {
    return left.first == right.first && left.second == right.second;
}

[[nodiscard]] bool omega_destination(state::activity::ActivityInstanceKey activity) noexcept {
    state::activity::destination::DestinationSelection selection{};
    if (!state::activity::destination::snapshot(activity, selection)) {
        return false;
    }
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    return name == kOmegaDestination;
}

[[nodiscard]] bool towerfall_destination(
    state::activity::ActivityInstanceKey activity) noexcept {
    state::activity::destination::DestinationSelection selection{};
    if (!state::activity::destination::snapshot(activity, selection)) {
        return false;
    }
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    return name == tower_watch::kPackage;
}

[[nodiscard]] bool exact_tower_watch_roster(
    const service::sense_update::SenseUpdate& update) noexcept {
    if (!update.hasRosterAcknowledgement || update.topLevelRosterCount < 2U
        || update.rosterEntryCount < update.topLevelRosterCount) {
        return false;
    }
    bool runtime = false;
    bool root = false;
    bool local = false;
    for (std::size_t index = 0U; index < update.rosterEntryCount; ++index) {
        const service::sense_update::RosterEntry& entry = update.rosterEntries[index];
        if (!entry.active || entry.state != 0x83U) {
            continue;
        }
        runtime = runtime
                  || (entry.bubble < 0
                      && entry.registryKey == tower_watch::kMissionRuntimeRegistry);
        root = root
               || (entry.bubble < 0
                   && entry.registryKey == tower_watch::kRootCueRegistry);
        local = local || entry.registryKey == tower_watch::kTowerWatchRegistry;
    }
    return runtime && root && local;
}

/** Applies Tower Watch's three-beat manifest to decoded native sense transitions. */
void observe_tower_watch(Session& session,
                         const service::sense_update::SenseUpdate& update,
                         std::uint64_t sequence,
                         std::uint64_t packetHash) noexcept {
    ActivitySensorObservation& observation = session.activity.sensorObservation;
    bool changed = false;
    bool report = false;
    const char* event = "none";
    if (exact_tower_watch_roster(update) && !observation.towerWatchRosterReady) {
        observation.towerWatchRosterReady = true;
        changed = true;
        report = true;
        event = "roster_ready";
    }
    for (std::size_t index = 0U; index < update.objectCount; ++index) {
        const service::sense_update::SenseObject& object = update.objects[index];
        if (object.registryKey != tower_watch::kTowerWatchRegistry) {
            continue;
        }
        if (object.slotType == tower_watch::kStartupChoreographyType
            && object.slotIndex == tower_watch::kStartupChoreographyIndex
            && object.bodyHash == tower_watch::kStartupChoreographyHash) {
            std::array<char, 320> startupLine{};
            const int startupWritten = std::snprintf(
                startupLine.data(),
                startupLine.size(),
                "ev=tower_watch_executor stage=wall_trigger_probe result=rejected "
                "reason=startup_choreography registry=0x%08X type=%u index=%u "
                "packet=%llu body_hash=0x%016llX would_fire=0",
                object.registryKey,
                static_cast<unsigned>(object.slotType),
                static_cast<unsigned>(object.slotIndex),
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(object.bodyHash));
            if (startupWritten > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {startupLine.data(),
                                  (std::min)(static_cast<std::size_t>(startupWritten),
                                             startupLine.size() - 1U)});
            }
        }
        if (object.registryKey == tower_watch::kWallApproachCandidateRegistry
            && object.slotType == tower_watch::kWallApproachCandidateType
            && object.slotIndex == tower_watch::kWallApproachCandidateIndex) {
            const std::uint64_t now = GetTickCount64();
            const bool first = !observation.towerWatchApproachSeen;
            const std::uint64_t previous = observation.towerWatchApproachBodyHash;
            if (first) {
                observation.towerWatchApproachSeen = true;
                observation.towerWatchApproachBaselineHash = object.bodyHash;
            }
            observation.towerWatchApproachBodyHash = object.bodyHash;
            const bool transitioned = !first && object.bodyHash != previous;
            const bool armed = observation.towerWatchOpeningCommittedTick != 0U
                               && observation.towerWatchPublishedStage
                                      == tower_watch::value(tower_watch::Stage::opening);
            const std::uint64_t elapsed = observation.towerWatchOpeningCommittedTick != 0U
                                              ? now - observation.towerWatchOpeningCommittedTick
                                              : 0U;
            std::array<char, 640> candidateLine{};
            const int candidateWritten = std::snprintf(
                candidateLine.data(),
                candidateLine.size(),
                "ev=tower_watch_executor stage=wall_trigger_probe result=%s "
                "registry=0x%08X type=%u index=%u packet=%llu "
                "elapsed_since_opening_ms=%llu body_bits=%u "
                "body_hash=0x%016llX previous_hash=0x%016llX "
                "baseline_hash=0x%016llX body_first=0x%016llX "
                "body_second=0x%016llX armed=%u transitioned=%u would_fire=%u",
                first ? "first_sample" : (transitioned ? "transition" : "repeat"),
                object.registryKey,
                static_cast<unsigned>(object.slotType),
                static_cast<unsigned>(object.slotIndex),
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(elapsed),
                object.bodyBits,
                static_cast<unsigned long long>(object.bodyHash),
                static_cast<unsigned long long>(previous),
                static_cast<unsigned long long>(observation.towerWatchApproachBaselineHash),
                static_cast<unsigned long long>(object.bodyFirst),
                static_cast<unsigned long long>(object.bodySecond),
                armed ? 1U : 0U,
                transitioned ? 1U : 0U,
                armed && transitioned ? 1U : 0U);
            if (candidateWritten > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {candidateLine.data(),
                                  (std::min)(static_cast<std::size_t>(candidateWritten),
                                             candidateLine.size() - 1U)});
            }
        }
        if (object.slotType == tower_watch::kEncounterSenseType
            && object.slotIndex == tower_watch::kEncounterSenseIndex) {
            if (!observation.towerWatchEncounterSeen) {
                observation.towerWatchEncounterSeen = true;
                observation.towerWatchEncounterBaselineHash = object.bodyHash;
                observation.towerWatchEncounterBodyHash = object.bodyHash;
                event = "encounter_baseline";
                report = true;
            } else if (object.bodyHash != observation.towerWatchEncounterBodyHash) {
                observation.towerWatchEncounterBodyHash = object.bodyHash;
                if (observation.towerWatchBreachSeen
                    && object.bodyHash
                           != observation.towerWatchEncounterBaselineHash) {
                    observation.towerWatchEncounterActive = true;
                    event = "encounter_active";
                    report = true;
                } else if (observation.towerWatchEncounterActive
                           && object.bodyHash
                                  == observation.towerWatchEncounterBaselineHash) {
                    observation.towerWatchEncounterChanged = true;
                    changed = true;
                    event = "encounter_clear";
                    report = true;
                }
            }
        }
    }
    if (changed) {
        session.activity.keepaliveDueTick = 0U;
    }
    if (report) {
        std::array<char, 384> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=tower_watch_executor stage=observe result=%s event=%s packet=%llu "
            "packet_hash=0x%016llX roster=%u breach=%u encounter_seen=%u "
            "encounter_active=%u encounter_cleared=%u published=%u outbound=%s",
            changed ? "latched" : "baseline",
            event,
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            observation.towerWatchRosterReady ? 1U : 0U,
            observation.towerWatchBreachSeen ? 1U : 0U,
            observation.towerWatchEncounterSeen ? 1U : 0U,
            observation.towerWatchEncounterActive ? 1U : 0U,
            observation.towerWatchEncounterChanged ? 1U : 0U,
            static_cast<unsigned>(observation.towerWatchPublishedStage),
            changed ? "authority_queued" : "none");
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(written),
                                         line.size() - 1U)});
        }
    }
}

[[nodiscard]] bool exact_omega_opening(
    const service::sense_update::SenseUpdate& update) noexcept {
    return omega_monitor_edges::entered(update,kOmegaOpeningSlotIndex);
}

[[nodiscard]] bool exact_omega_forest_entrance(
    const service::sense_update::SenseUpdate& update) noexcept {
    return omega_monitor_edges::entered(update,kOmegaForestEntranceSlotIndex);
}

void report_roster_entries(const service::sense_update::SenseUpdate& update,
                           std::uint64_t sequence,
                           std::uint64_t packetHash) noexcept {
    for (std::size_t index = 0; index < update.rosterEntryCount; ++index) {
        const service::sense_update::RosterEntry& entry = update.rosterEntries[index];
        std::array<char, 256> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=sensor_roster_ack result=entry packet=%llu hash=0x%016llX "
            "ordinal=%zu scope=%s bubble=%d key=0x%08X active=%u state=0x%02X",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            index,
            entry.bubble < 0 ? "top" : "bubble",
            static_cast<int>(entry.bubble),
            entry.registryKey,
            entry.active ? 1U : 0U,
            static_cast<unsigned>(entry.state));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

void report_sense_entries(const service::sense_update::SenseUpdate& update,
                          std::uint64_t sequence,
                          std::uint64_t packetHash) noexcept {
    for (std::size_t index = 0; index < update.groupCount; ++index) {
        const service::sense_update::SenseGroup& group = update.groups[index];
        std::array<char, 256> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=sensor_sense_group result=decoded packet=%llu "
            "hash=0x%016llX ordinal=%zu key=0x%08X group_bits=%u objects=%u",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            index,
            group.registryKey,
            group.bodyBits,
            static_cast<unsigned>(group.objectCount));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    for (std::size_t index = 0; index < update.objectCount; ++index) {
        const service::sense_update::SenseObject& object = update.objects[index];
        std::array<char, 512> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=sensor_sense_entry result=decoded packet=%llu "
            "packet_hash=0x%016llX ordinal=%zu group=%u object=%u key=0x%08X "
            "slot=%u/%u body_bits=%u body=0x%016llX,0x%016llX,0x%016llX "
            "body_hash=0x%016llX body_set_count=%u revision=%u root_delta=%u",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            index,
            static_cast<unsigned>(object.groupOrdinal),
            static_cast<unsigned>(object.objectOrdinal),
            object.registryKey,
            static_cast<unsigned>(object.slotType),
            static_cast<unsigned>(object.slotIndex),
            object.bodyBits,
            static_cast<unsigned long long>(object.bodyFirst),
            static_cast<unsigned long long>(object.bodySecond),
            static_cast<unsigned long long>(object.bodyThird),
            static_cast<unsigned long long>(object.bodyHash),
            static_cast<unsigned>(object.bodySetBitCount),object.revision,object.hasDelta?1U:0U);
        if (written > 0 && static_cast<std::size_t>(written)<line.size()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

constexpr std::array<std::uint8_t, 3> kAuthoredRootCueTypes{{68U, 11U, 53U}};
constexpr std::array<const char*, 3> kAuthoredRootCueNames{{
    "directive",
    "dialogue",
    "music",
}};

[[nodiscard]] int authored_root_cue_index(std::uint8_t slotType) noexcept {
    for (std::size_t index = 0; index < kAuthoredRootCueTypes.size(); ++index) {
        if (kAuthoredRootCueTypes[index] == slotType) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

[[nodiscard]] bool top_level_key(const ActivitySensorObservation& observation,
                                 std::uint32_t key) noexcept {
    for (std::size_t index = 0; index < observation.authoredTopLevelKeyCount; ++index) {
        if (observation.authoredTopLevelKeys[index] == key) {
            return true;
        }
    }
    return false;
}

/** Tracks mission-root directive/dialogue/music bodies without naming a particular mission key. */
void report_authored_root_transitions(
    Session& session,
    const service::sense_update::SenseUpdate& update,
    std::uint64_t sequence,
    std::uint64_t packetHash) noexcept {
    ActivitySensorObservation& observation = session.activity.sensorObservation;
    if (update.hasRosterAcknowledgement) {
        std::array<std::uint32_t, 8> keys{};
        const std::size_t count = (std::min)(
            static_cast<std::size_t>(update.topLevelRosterCount), keys.size());
        for (std::size_t index = 0; index < count; ++index) {
            keys[index] = update.rosterEntries[index].registryKey;
        }
        bool changed = count != observation.authoredTopLevelKeyCount;
        for (std::size_t index = 0; !changed && index < count; ++index) {
            changed = keys[index] != observation.authoredTopLevelKeys[index];
        }
        if (changed) {
            observation.authoredTopLevelKeys = keys;
            observation.authoredTopLevelKeyCount = static_cast<std::uint8_t>(count);
            observation.authoredRootRegistryKey = 0;
            observation.authoredRootCueBodyHashes = {};
            observation.authoredRootCueBodyBits = {};
            observation.authoredRootCueSlotIndexes = {};
            observation.authoredRootCueSeen = {};

            std::array<char, 384> line{};
            int written = std::snprintf(
                line.data(), line.size(),
                "ev=activity stage=authored_root_roster result=changed packet=%llu "
                "packet_hash=0x%016llX top=%zu keys=",
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(packetHash),
                count);
            for (std::size_t index = 0;
                 index < count && written > 0
                 && static_cast<std::size_t>(written) < line.size();
                 ++index) {
                written += std::snprintf(
                    line.data() + written,
                    line.size() - static_cast<std::size_t>(written),
                    "%s0x%08X",
                    index == 0 ? "" : ",",
                    keys[index]);
            }
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }

    for (std::size_t objectIndex = 0; objectIndex < update.objectCount; ++objectIndex) {
        const service::sense_update::SenseObject& object = update.objects[objectIndex];
        const int cueIndexValue = authored_root_cue_index(object.slotType);
        if (cueIndexValue < 0 || !top_level_key(observation, object.registryKey)) {
            continue;
        }
        if (observation.authoredRootRegistryKey == 0) {
            observation.authoredRootRegistryKey = object.registryKey;
        } else if (observation.authoredRootRegistryKey != object.registryKey) {
            continue;
        }
        const std::size_t cueIndex = static_cast<std::size_t>(cueIndexValue);
        const bool seen = observation.authoredRootCueSeen[cueIndex];
        const std::uint64_t previousHash = observation.authoredRootCueBodyHashes[cueIndex];
        const std::uint32_t previousBits = observation.authoredRootCueBodyBits[cueIndex];
        const std::uint16_t previousSlot = observation.authoredRootCueSlotIndexes[cueIndex];
        const bool changed = seen
                             && (previousHash != object.bodyHash
                                 || previousBits != object.bodyBits
                                 || previousSlot != object.slotIndex);
        observation.authoredRootCueSeen[cueIndex] = true;
        observation.authoredRootCueBodyHashes[cueIndex] = object.bodyHash;
        observation.authoredRootCueBodyBits[cueIndex] = object.bodyBits;
        observation.authoredRootCueSlotIndexes[cueIndex] = object.slotIndex;

        std::array<char, 512> line{};
        const int written = std::snprintf(
            line.data(), line.size(),
            "ev=activity stage=authored_root_transition result=%s packet=%llu "
            "packet_hash=0x%016llX key=0x%08X cue=%s slot=%u/%u "
            "body_bits=%u body_hash=0x%016llX previous_slot=%u previous_bits=%u "
            "previous_hash=0x%016llX set_count=%u width_source=%s",
            !seen ? "initial" : (changed ? "changed" : "unchanged"),
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            object.registryKey,
            kAuthoredRootCueNames[cueIndex],
            static_cast<unsigned>(object.slotType),
            static_cast<unsigned>(object.slotIndex),
            object.bodyBits,
            static_cast<unsigned long long>(object.bodyHash),
            static_cast<unsigned>(previousSlot),
            previousBits,
            static_cast<unsigned long long>(previousHash),
            static_cast<unsigned>(object.bodySetBitCount),
            object.inferredBodyWidth ? "envelope" : "schema");
        if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/**
 * Decodes the recovered type-6 subset and commits Omega's bounded opening transition. The route
 * never emits a reply or constructs scene/spawner state; the periodic authority publisher turns
 * the committed edge into the normal slot-18/35 update.
 */
void report_sense_update(Session& session, const service::Request& request) noexcept {
    const std::span<const std::byte> payload = request.payload;
    service::sense_update::SenseUpdate update{};
    std::size_t consumedBits = 0;
    const bool parsed = (omega_destination(session.activity.instance) ? service::sense_update::parse_omega_sense_update : service::sense_update::parse_sense_update)(payload, update, consumedBits);
    const std::size_t captureBytes = (std::min)(payload.size(), kSenseUpdateCaptureBytes);
    const std::uint64_t packetHash = payload_hash(payload);
    if (session.activitySensorPacketSequence
        != (std::numeric_limits<std::uint64_t>::max)()) {
        ++session.activitySensorPacketSequence;
    }
    const std::uint64_t sequence = session.activitySensorPacketSequence;

    const bool handleBound = static_cast<bool>(session.activity.instance)
                             && request.accountHandle == session.activity.instance.sessionId;
    if (parsed && handleBound && update.hasRosterAcknowledgement) {
        state::activity::destination::DestinationSelection selection{};
        if (state::activity::destination::snapshot(session.activity.instance, selection)) {
            const std::string_view activity(
                reinterpret_cast<const char*>(selection.packageName.data()),
                selection.packageNameLength);
            state::build_data::scenarios::export_cue_observation_mapping(activity, update);
        }
    }
    if(parsed && handleBound && session.activityPatchEpochSeen && same_epoch(update.epoch,session.activityPatchEpoch)
        && state::activity::newlight::launchpad::native_run()) {
        for(std::size_t i=0;i<update.objectCount;++i) {
            const auto& o=update.objects[i];
            if(o.hasCombatantOutput) {state::activity::newlight::launchpad::observe_actor(o.registryKey,o.slotType,o.slotIndex,o.combatantOutput);}
            if(o.hasSquadOutput) {state::activity::newlight::launchpad::observe_source(o.registryKey,o.slotType,o.slotIndex,o.squadOutput);}
            if(o.hasObjectOutput) {state::activity::newlight::launchpad::observe_use(o.registryKey,o.slotType,o.slotIndex,o.objectOutput);}
            if(o.hasPassengerOutput) {state::activity::newlight::launchpad::observe_passenger(o.registryKey,o.slotType,o.slotIndex,o.passengerOutput);}
            if(o.hasDeviceOutput) {state::activity::newlight::launchpad::observe_device(o.registryKey,o.slotType,o.slotIndex,o.deviceOutput);}
        }
    }
    // The native task evaluator answers a published combat objective inside the squad's own sense
    // body. Nothing else reports it, so this is the only place the strike learns which authored
    // task groups its squads can reach.
    if (parsed && handleBound && session.activityPatchEpochSeen
        && same_epoch(update.epoch, session.activityPatchEpoch)
        && state::activity::strike_pact::native_run() != 0) {
        for (std::size_t index = 0; index < update.objectCount; ++index) {
            const service::sense_update::SenseObject& object = update.objects[index];
            // Every squad delta is forwarded, not only the ones carrying a cost. The revision, the
            // costs and the initialized latch each arrive in their own report, and the strike
            // merges them; dropping a report because it restates only one of the three loses it.
            if (!object.hasSquadOutput) { continue; }
            state::activity::strike_pact::TaskCosts costs{};
            costs.mask = object.squadOutput.costMask;
            costs.revision = object.squadOutput.revision;
            costs.hasRevision = object.squadOutput.hasRevision;
            costs.initialized = object.squadOutput.initialized;
            for (std::size_t group = 0; group < costs.cost.size(); ++group) {
                costs.cost[group] = object.squadOutput.cost[group];
            }
            state::activity::strike_pact::observe_costs(object.registryKey, object.slotIndex, costs);
        }
    }
    if (parsed && handleBound && session.activityPatchEpochSeen
        && same_epoch(update.epoch, session.activityPatchEpoch)
        && state::activity::strike_bond::native_run() != 0) {
        for (std::size_t index = 0; index < update.objectCount; ++index) {
            const service::sense_update::SenseObject& object = update.objects[index];
            // Every squad delta is forwarded, not only the ones carrying a cost. The revision, the
            // costs and the initialized latch each arrive in their own report, and the strike
            // merges them; dropping a report because it restates only one of the three loses it.
            if (!object.hasSquadOutput) { continue; }
            state::activity::coo::TaskCosts costs{};
            costs.mask = object.squadOutput.costMask;
            costs.revision = object.squadOutput.revision;
            costs.hasRevision = object.squadOutput.hasRevision;
            costs.initialized = object.squadOutput.initialized;
            for (std::size_t group = 0; group < costs.cost.size(); ++group) {
                costs.cost[group] = object.squadOutput.cost[group];
            }
            state::activity::strike_bond::observe_costs(object.registryKey, object.slotIndex, costs);
        }
    }
    const bool omegaSelected = handleBound && omega_destination(session.activity.instance);
    const bool towerfallSelected = handleBound
                                   && towerfall_destination(session.activity.instance);
    const bool syntheticStageMachine =
        core::settings::get().omegaExperiments.syntheticStageMachine;
    const bool sceneAuthority = syntheticStageMachine
                                && core::settings::get().omegaExperiments.sceneAuthority;
    const bool epochBound = parsed && session.activityPatchEpochSeen
                            && same_epoch(update.epoch, session.activityPatchEpoch);
    if(handleBound && epochBound && !session.activity.joinedForeignSession && session.activity.lineage
        && session.activity.lineage.bound==session.activity.instance) {
        const auto owner=session.activity.lineage.source;
        state::activity::membership::RegionSnapshotInputs useInputs{};
        const bool useBound=state::activity::membership::snapshot_region_inputs(
            session.activity.instance,owner,{},useInputs);
        const auto useRegion=useBound?push::activity::native_publisher::runtime_region(
            session.activity.advertisedRegion,useInputs.sourceMembership.currentRegion.index):-1;
        const auto useBubble=push::activity::native_publisher::population_prefetch_bubble(useRegion);
        for(std::size_t index=0;index<update.objectCount;++index) {
            const auto& object=update.objects[index];
            if(object.hasObjectOutput && object.slotType==4)
                lost_sector_rewards::receive(session,owner,useBubble,
                    object.registryKey,object.slotType,object.slotIndex,object.objectOutput);
        }
    }
    if(handleBound && epochBound) {
        const auto run=state::activity::strike_pact::native_run();
        if(run!=0) {
            for(std::size_t index=0;index<update.objectCount;++index) {
                const auto& object=update.objects[index];
                if(object.slotType==37 && object.hasForestGeneratorState) {
                    state::activity::strike_pact::observe_generator(run,object.registryKey,object.slotIndex,
                        object.forestSeed,object.forestActive32);
                }
                if(object.slotType==43 && object.hasSceneOutput) {
                    state::activity::strike_pact::observe_scene(run,object.registryKey,object.slotIndex,object.sceneOutput);
                }
                if(object.slotType==1 && object.hasSquadOutput) {
                    state::activity::strike_pact::observe_squad(run,object.registryKey,object.slotIndex,object.squadOutput);
                }
                if(object.slotType==2 && object.hasCombatantOutput) {
                    state::activity::strike_pact::observe_combatant(run,object.registryKey,
                        object.slotIndex,object.combatantOutput);
                }
            }
        }
    }
    if(handleBound && epochBound) {
        const auto run=state::activity::strike_bond::native_run();
        if(run!=0) {
            for(std::size_t index=0;index<update.objectCount;++index) {
                const auto& object=update.objects[index];
                if(object.slotType==37 && object.hasForestGeneratorState) {
                    state::activity::strike_bond::observe_generator(run,object.registryKey,object.slotIndex,
                        object.forestSeed,object.forestActive32);
                }
                if(object.slotType==43 && object.hasSceneOutput) {
                    state::activity::strike_bond::observe_scene(run,object.registryKey,object.slotIndex,object.sceneOutput);
                }
                if(object.slotType==1 && object.hasSquadOutput) {
                    state::activity::strike_bond::observe_squad(run,object.registryKey,object.slotIndex,object.squadOutput);
                }
                if(object.slotType==2 && object.hasCombatantOutput) {
                    state::activity::strike_bond::observe_combatant(run,object.registryKey,
                        object.slotIndex,object.combatantOutput);
                }
            }
        }
    }
    if(handleBound && epochBound && state::activity::strike_pact::native_run()!=0) {
        for(std::size_t index=0;index<update.objectCount;++index) {
            const auto& object=update.objects[index];
            if(object.slotType!=30 || !object.hasMonitorOutput) { continue; }
            const auto& monitor=object.monitorOutput;
            state::activity::strike_pact::observe_monitor(object.registryKey,object.slotIndex,
                monitor.any,monitor.count,monitor.value);
        }
    }
    const bool destinationBound = parsed && epochBound && omegaSelected;
    if(parsed && handleBound && epochBound && session.activity.lineage
        && session.activity.lineage.bound==session.activity.instance
        && session.activity.advertisedRegion>=0 && session.activity.advertisedRegion%8==0) {
        ::dawn::server::runtime::activity::native_activity::observe(
            session.activity.lineage.source,
            static_cast<std::uint32_t>(session.activity.advertisedRegion/8),update);
    }
    const char* validation = !parsed                 ? "decode"
                             : !handleBound          ? "session"
                             : !session.activityPatchEpochSeen ? "no_epoch"
                             : !epochBound           ? "epoch"
                             : !destinationBound     ? "destination"
                                                     : "ok";
    const bool rosterReady = destinationBound
                             && exact_omega_initial_report(update);
    // TEMP diagnostic: when an acknowledgement-bearing report fails readiness, dump its shape so
    // the exact-match tables can be corrected from real values instead of guesses.
    if (!rosterReady && destinationBound && update.hasRosterAcknowledgement) {
        static std::uint32_t s_ackDumps = 0;
        if (s_ackDumps < 3U) {
            ++s_ackDumps;
            std::array<char, 480> ackLine{};
            int ackLength = std::snprintf(
                ackLine.data(), ackLine.size(),
                "ev=omega_ack_dump top=%u entries=%u blocks=%u groups=%u objects=%u rows=",
                static_cast<unsigned>(update.topLevelRosterCount),
                static_cast<unsigned>(update.rosterEntryCount),
                static_cast<unsigned>(update.bubbleBlockCount),
                static_cast<unsigned>(update.groupCount),
                static_cast<unsigned>(update.objectCount));
            const std::size_t rows = (std::min)(static_cast<std::size_t>(update.rosterEntryCount),
                                                std::size_t{10});
            for (std::size_t index = 0; index < rows; ++index) {
                const service::sense_update::RosterEntry& entry = update.rosterEntries[index];
                ackLength += std::snprintf(
                    ackLine.data() + ackLength,
                    ackLine.size() - static_cast<std::size_t>(ackLength),
                    "%08X/b%d/a%u/s%02X,", entry.registryKey,
                    static_cast<int>(entry.bubble), entry.active ? 1U : 0U,
                    static_cast<unsigned>(entry.state));
            }
            if (ackLength > 0) {
                core::log::write(core::log::Channel::server, core::log::Level::info,
                                 {ackLine.data(), static_cast<std::size_t>(ackLength)});
            }
        }
    }
    const bool opening = parsed && exact_omega_opening(update);
    // Observing the native entrance advances presentation and Forest readiness.
    // It does not issue a transport mutation and must not depend on that retired experiment.
    const bool forestEntrance = parsed && exact_omega_forest_entrance(update);
    bool sceneObservation = false;
    if (sceneAuthority && destinationBound) {
        for (std::size_t index = 0; index < update.objectCount; ++index) {
            const service::sense_update::SenseObject& object = update.objects[index];
            if (object.registryKey == kOmegaOpeningRegistry
                && object.slotType == kOmegaSceneSlotType
                && object.slotIndex == kOmegaSceneSlotIndex
                && exact_body(object,
                              kOmegaSceneObservationBits,
                              kOmegaSceneObservationFirst,
                              kOmegaSceneObservationSecond,
                              kOmegaSceneObservationThird)) {
                sceneObservation = true;
                break;
            }
        }
    }
    const bool observerReset = parsed && handleBound && epochBound
                               && (!destinationBound
                                   || (update.hasRosterAcknowledgement && !rosterReady));
    const char* resetReason = !observerReset       ? "none"
                              : !destinationBound ? "destination"
                                                  : "roster";
    const bool cooOpening = omega_destination(session.activity.instance)
        && !session.activity.joinedForeignSession
        && state::activity::coo::omega::select(state::activity::mission_run_generation(),
            core::settings::get().omegaExperiments.cooExecutor);
    bool becameReady = false;
    const char* transition = "none";
    const char* forestResult = "none";
    if (cooOpening) {
        const auto intake = omega_opening_intake::capture(
            session.activity.sensorObservation.omegaOpeningExecutor,
            {session.activity.key.generation.value, sequence, parsed, handleBound,
                epochBound, destinationBound}, update);
        if (intake.queued) { session.activity.keepaliveDueTick = 0; }
        transition = intake.overflow ? "queue_overflow" : intake.queued ? "queued" : "none";
        forestResult = forestEntrance ? transition : "none";
    } else {
        if (observerReset) {
            // A bound teardown/replacement report invalidates both ordering and dedupe state. Preserve
            // only the packet ordinal so the diagnostic stream remains monotonic on this connection.
            session.activity.sensorObservation = {};
            session.activity.omegaOpeningStage =
                middleware::bap::activity_message::sensor_auth_update::kOmegaOpeningStageNone;
        }
        becameReady = rosterReady && !session.activity.sensorObservation.omegaRosterReady;
        if (rosterReady) {
            session.activity.sensorObservation.omegaRosterReady = true;
        }
        if (becameReady) {
            // This is a new authored graph. Retirements from the prior graph cannot satisfy it.
            session.activity.sensorObservation.omegaIkoraLattice.reset();
            session.activity.sensorObservation.omegaIkoraLattice.begin(
                session.activity.key.generation.value);
            session.activity.sensorObservation.omegaSceneHandoffArmed = false;
            session.activity.sensorObservation.omegaSceneCompleted = false;
            session.activity.sensorObservation.omegaForestEntranceTriggered = false;
            session.activity.sensorObservation.omegaPortalTransportConfirmed = false;
            session.activity.sensorObservation.omegaForestEntranceAuthorityPublished = false;
            session.activity.keepaliveDueTick = 0;
        }
        transition = observerReset ? "reset" : "none";
        if (opening) {
            if (!destinationBound) {
                transition = "rejected";
            } else if (!session.activity.sensorObservation.omegaRosterReady) {
                transition = "not_ready";
            } else if (session.activity.sensorObservation.omegaOpeningTriggered) {
                transition = "duplicate";
            } else {
                session.activity.sensorObservation.omegaOpeningTriggered = true;
                // Wake the normal authority publisher. Delivery, retry and nonce ownership remain in
                // the keepalive path rather than turning type 6 into a request/reply command.
                session.activity.keepaliveDueTick = 0;
                transition = "latched";
            }
        }
        // Reconstructed host join: the actual native Ikora selector output releases only the
        // GUID-bound near portal gate. Never infer this edge from time, position, or C7 submission.
        if (destinationBound && session.activity.sensorObservation.omegaRosterReady
            && session.activity.sensorObservation.omegaOpeningTriggered) {
            namespace lattice = state::activity::omega_ikora_lattice;
            for (std::size_t index = 0; index < update.objectCount; ++index) {
                lattice::Scene scene{};
                if (!lattice::extract_scene(update.objects[index], scene)) { continue; }
                const auto result = session.activity.sensorObservation.omegaIkoraLattice.observe(
                    session.activity.key.generation.value, true, scene);
                if (result == lattice::Observation::released) {
                    session.activity.keepaliveDueTick = 0;
                    std::array<char, 240> line{};
                    const int length = std::snprintf(line.data(), line.size(),
                        "ev=omega_lattice stage=host_release key=0xD00142CF slot=23/16 "
                        "scene_generation=0x%08X scene_revision=%u event=0x792AAA50 "
                        "position=0 position_revision=2 snap=0 policy=reconstructed",
                        scene.generationWire, scene.revision);
                    if (length > 0 && static_cast<std::size_t>(length) < line.size()) {
                        core::log::write(core::log::Channel::server, core::log::Level::info,
                            {line.data(), static_cast<std::size_t>(length)});
                    }
                }
            }
        }
        forestResult = "none";
        if (forestEntrance) {
            if (!destinationBound) {
                forestResult = "rejected";
            } else if (!session.activity.sensorObservation.omegaRosterReady) {
                // v1: roster readiness alone arms the latch. The opening-authority-published
                // ordering clause returns once the stage machine is the ordering authority
                // (it only sets at the synthetic Triggered stage, which the baseline never
                // reaches).
                forestResult = "not_ready";
            } else if (session.activity.sensorObservation.omegaForestEntranceTriggered) {
                forestResult = "duplicate";
            } else {
                session.activity.sensorObservation.omegaForestEntranceTriggered = true;
                session.activity.keepaliveDueTick = 0;
                forestResult = "latched";
                // The initial player hash and exact native carrier now own portal contact.
                // Keep progression observation, but never replace native arrival/facing with
                // a body-position write. No timeout or inactive-carrier fallback is scheduled.
                const auto portalPlan = session.activity.sensorObservation.omegaIkoraLattice.plan(
                    session.activity.key.generation.value, !state::activity::omega_authority_quiesced());
                const bool carrierArmed = portalPlan.active && portalPlan.position.revision == 2;
                std::array<char, 224> portalLine{};
                const int portalLength = std::snprintf(portalLine.data(),portalLine.size(),
                    "ev=omega_portal stage=entry path=native_only result=%s "
                    "carrier=BA5F26EF/4/0 carrier_armed=%u fallback=none arrival_verified=0",
                    carrierArmed ? "await_native_arrival" : "carrier_not_armed",carrierArmed?1U:0U);
                if (portalLength > 0 && static_cast<std::size_t>(portalLength) < portalLine.size()) {
                    core::log::write(core::log::Channel::server,core::log::Level::info,
                        {portalLine.data(),static_cast<std::size_t>(portalLength)});
                }
            }
        }
    }
    const bool latched = std::string_view(transition) == "latched";
    const bool forestLatched = std::string_view(forestResult) == "latched";
    const char* hostAction = forestLatched ? "latch_forest_entrance"
                             : latched      ? "start_opening_scene"
                                            : "none";
    const char* observerResult = forestEntrance ? forestResult : transition;

    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity stage=sensor_sense_update result=%s parse=%s validation=%s "
        "observer=%s observer_reset=%s host_action=%s packet=%llu bytes=%zu "
        "consumed_bits=%zu padding_bits=%u "
        "epoch=0x%016llX,0x%016llX hash=0x%016llX roster=%u roster_entries=%u "
        "top=%u bubbles=%u groups=%u objects=%u capture_bytes=%zu hex=",
        parsed ? "decoded" : "invalid",
        parsed ? "recovered" : "fail",
        validation,
        transition,
        resetReason,
        hostAction,
        static_cast<unsigned long long>(sequence),
        payload.size(),
        consumedBits,
        static_cast<unsigned>(update.paddingBits),
        static_cast<unsigned long long>(update.epoch.first),
        static_cast<unsigned long long>(update.epoch.second),
        static_cast<unsigned long long>(packetHash),
        update.hasRosterAcknowledgement ? 1U : 0U,
        static_cast<unsigned>(update.rosterEntryCount),
        static_cast<unsigned>(update.topLevelRosterCount),
        static_cast<unsigned>(update.bubbleBlockCount),
        static_cast<unsigned>(update.groupCount),
        static_cast<unsigned>(update.objectCount),
        captureBytes);
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    for (std::size_t index = 0;
         index < captureBytes && static_cast<std::size_t>(written) + 2U < line.size();
         ++index) {
        const unsigned value = std::to_integer<unsigned char>(payload[index]);
        line[static_cast<std::size_t>(written++)] = kHex[value >> 4U];
        line[static_cast<std::size_t>(written++)] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server,
                     parsed ? core::log::Level::info : core::log::Level::warn,
                     {line.data(), static_cast<std::size_t>(written)});
    if (omegaSelected) {
        ::dawn::server::bap::encrypted::diagnostics::omega_trace::record_sense(
            session.activity.instance.sessionId,
            sequence,
            packetHash,
            validation,
            observerResult,
            hostAction,
            session.activity.sensorObservation.omegaRosterReady,
            session.activity.sensorObservation.omegaOpeningTriggered,
            parsed,
            update,
            payload);
    }
    if (!parsed) {
        return;
    }
    if (towerfallSelected && epochBound) {
        observe_tower_watch(session, update, sequence, packetHash);
    }
    report_roster_entries(update, sequence, packetHash);
    report_sense_entries(update, sequence, packetHash);
    report_authored_root_transitions(session, update, sequence, packetHash);

    if (becameReady) {
        std::array<char, 384> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=activity stage=omega_opening_observer result=ready event=roster_ack "
            "validation=%s packet=%llu packet_hash=0x%016llX session=0x%016llX "
            "bootstrap_type1_bits=%u ready=1 triggered=%u proposed=none "
            "host_action=none outbound=none",
            validation,
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            static_cast<unsigned long long>(session.activity.instance.sessionId),
            update.objects[2].bodyBits,
            session.activity.sensorObservation.omegaOpeningTriggered ? 1U : 0U);
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              static_cast<std::size_t>(transitionWritten)});
        }
    }
    if (opening) {
        std::array<char, 448> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=activity stage=omega_opening_observer result=%s event=volume_edge "
            "validation=%s packet=%llu packet_hash=0x%016llX session=0x%016llX "
            "key=0xD00142CF slot=30/20 ready=%u triggered=%u proposed=%s "
            "host_action=%s outbound=%s",
            transition,
            validation,
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            static_cast<unsigned long long>(session.activity.instance.sessionId),
            session.activity.sensorObservation.omegaRosterReady ? 1U : 0U,
            session.activity.sensorObservation.omegaOpeningTriggered ? 1U : 0U,
            latched ? "none_to_baseline" : "none",
            hostAction,
            latched ? "authority_queued" : "none");
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              static_cast<std::size_t>(transitionWritten)});
        }
    }
    if (forestEntrance) {
        std::array<char, 448> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=activity stage=omega_forest_entrance result=%s event=player_monitor_edge "
            "validation=%s packet=%llu packet_hash=0x%016llX session=0x%016llX "
            "key=0xD00142CF slot=30/24 opening_delivered=%u scene_completed=%u "
            "proposed=%s host_action=%s outbound=%s",
            forestResult,
            validation,
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(packetHash),
            static_cast<unsigned long long>(session.activity.instance.sessionId),
            session.activity.sensorObservation.omegaOpeningAuthorityPublished ? 1U : 0U,
            session.activity.sensorObservation.omegaSceneCompleted ? 1U : 0U,
            forestLatched ? "settled_to_state_4" : "none",
            hostAction,
            forestLatched ? "authority_queued" : "none");
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              static_cast<std::size_t>(transitionWritten)});
        }
    }
    if (sceneObservation) {
        std::array<char, 448> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=activity stage=omega_scene_observation result=observed key=0xD00142CF "
            "slot=43/1 body_bits=140 body=0x%016llX,0x%016llX,0x%016llX "
            "next_revision=%u trigger=%u state2_delivered=%u retirement_reset=no "
            "host_action=none mutation=observe_only",
            static_cast<unsigned long long>(kOmegaSceneObservationFirst),
            static_cast<unsigned long long>(kOmegaSceneObservationSecond),
            static_cast<unsigned long long>(kOmegaSceneObservationThird),
            kOmegaSceneObservationNextRevision,
            session.activity.sensorObservation.omegaOpeningTriggered ? 1U : 0U,
            session.activity.sensorObservation.omegaOpeningAuthorityPublished ? 1U : 0U);
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              static_cast<std::size_t>(transitionWritten)});
        }
    }
}

/** @return The binary name for one accepted message type, or nullptr when it is not one. */
[[nodiscard]] const char* accepted_name(std::uint32_t messageType) noexcept {
    const auto row = std::find_if(kAcceptedMessages.begin(),
                                  kAcceptedMessages.end(),
                                  [messageType](const AcceptedMessage& candidate) noexcept {
                                      return candidate.type == messageType;
                                  });
    return row == kAcceptedMessages.end() ? nullptr : row->name;
}

/**
 * Records one accepted message that changes no host state.
 * @param messageType Activity message type from the envelope.
 * @param name Binary name for that type.
 * @param payload Whole owned payload. Activity-start messages include a bounded hexadecimal
 * preview so their still-unknown fields can be recovered from a controlled launch.
 */
void report_accepted(std::uint32_t messageType,
                     const char* name,
                     std::span<const std::byte> payload) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=activity stage=message result=accept type=%u name=%s "
                                "bytes=%zu",
                                messageType,
                                name,
                                payload.size());
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    const bool activityStart = messageType == 8 || messageType == 11;
    if (activityStart) {
        const std::size_t preview =
            (std::min)(payload.size(), kActivityStartPreviewBytes);
        int appended = std::snprintf(line.data() + written,
                                     line.size() - static_cast<std::size_t>(written),
                                     " preview=");
        if (appended <= 0
            || static_cast<std::size_t>(appended)
                   >= line.size() - static_cast<std::size_t>(written)) {
            return;
        }
        written += appended;
        constexpr char kHex[] = "0123456789ABCDEF";
        for (std::size_t index = 0;
             index < preview && static_cast<std::size_t>(written) + 2 < line.size();
            ++index) {
            const unsigned value = std::to_integer<unsigned char>(payload[index]);
            line[static_cast<std::size_t>(written)] = kHex[value >> 4U];
            ++written;
            line[static_cast<std::size_t>(written)] = kHex[value & 0xFU];
            ++written;
        }
    }
    core::log::write(core::log::Channel::server,
                     activityStart ? core::log::Level::info : core::log::Level::debug,
                     {line.data(), static_cast<std::size_t>(written)});
}

/**
 * Checks one incident and reports its verdict. Nothing relays msg 19 yet, so a pass changes
 * nothing. A failure is named because a bad target index would crash the Client if it were sent on.
 * @param request Validated owned svc8 envelope.
 */
/** @return The FNV-1 name of an SObject row the movie raises incidents for, else empty. */
[[nodiscard]] const char* incident_name(std::uint32_t target) noexcept {
    namespace ending = state::activity::omega_ending;
    switch (target) {
    case ending::kCinematicStartedIncident: return "cinematic_started";
    case ending::kCinematicSkipIncident: return "cinematic_skip";
    case ending::kCinematicFinishedIncident: return "cinematic_finished";
    default: return "";
    }
}

/**
 * Incident targets whose raw body is dumped verbatim, so a wire layout that Dawn cannot decode yet
 * can still be read back from the log. 3539 is the native placed-loot report: its Haunted Forest
 * arm is 82 bytes, two more than the Tower arm, and the extra 16 bits have not been located.
 */
constexpr std::array<std::uint32_t,1> kPayloadDumpTargets{
    service::loot_pickup::kIncidentTarget};

/**
 * Dumps per activity incarnation.
 * [owner] Not authored. One opened chest per branch is the expected rate, so eight covers a whole
 * run while bounding the log growth of a debug-level hex dump.
 */
constexpr unsigned kPayloadDumpsPerRun = 8;

/** Capped hex dump of one watched incident body. Never changes any retained state. */
void report_incident_payload(const service::incident::Incident& parsed,
    state::activity::ActivityInstanceKey owner) noexcept {
    if(!parsed.hasPayload || parsed.payloadLength==0
        || std::find(kPayloadDumpTargets.begin(),kPayloadDumpTargets.end(),parsed.primaryTarget)
            ==kPayloadDumpTargets.end())return;
    // BAP's route lock serializes every caller, exactly as the pickup rings assume.
    static std::uint64_t dumpRun{};
    static unsigned dumped{};
    const std::uint64_t run=owner.incarnation.value;
    if(dumpRun!=run) { dumpRun=run;dumped=0U; }
    if(dumped>=kPayloadDumpsPerRun)return;
    ++dumped;
    std::array<char,core::log::kLineCapacity> line{};
    const int header=std::snprintf(line.data(),line.size(),
        "ev=activity stage=incident_payload target=%u bytes=%u hex=",
        parsed.primaryTarget,parsed.payloadLength);
    if(header<=0 || static_cast<std::size_t>(header)>=line.size())return;
    auto cursor=static_cast<std::size_t>(header);
    const auto bytes=(std::min)(static_cast<std::size_t>(parsed.payloadLength),
        parsed.payload.size());
    for(std::size_t index=0;index<bytes && cursor+3U<line.size();++index) {
        const int step=std::snprintf(line.data()+cursor,line.size()-cursor,"%02X",
            static_cast<unsigned>(static_cast<std::uint8_t>(parsed.payload[index])));
        if(step<=0)break;
        cursor+=static_cast<std::size_t>(step);
    }
    core::log::write(core::log::Channel::server,core::log::Level::debug,{line.data(),cursor});
}

void report_incident(const service::Request& request,bool liveBinding,
    state::activity::ActivityInstanceKey owner,const Session& session) noexcept {
    namespace incident = service::incident;
    namespace ending = state::activity::omega_ending;
    incident::Incident parsed;
    const incident::Verdict verdict = incident::validate(request.payload, parsed);
    namespace player_trigger=state::activity::coo::native_player_trigger;
    if(liveBinding && verdict==incident::Verdict::accepted && parsed.hasPayload
        && parsed.primaryTarget==player_trigger::kIncident) {
        player_trigger::Receipt receipt{};
        if(player_trigger::decode(std::span(parsed.payload).first(parsed.payloadLength),receipt)
            && owner) {
            static_cast<void>(::dawn::server::runtime::activity::native_activity::observe_player_trigger(owner,receipt));
            // pf_reward_chest carries no interaction controller, so pt_reward_chest is the only
            // authored "reached the chest" signal. Nothing is armed here; the trigger is native.
            forest_chest_rewards::receive(session,owner,receipt);
        }
    }
    if(liveBinding && verdict==incident::Verdict::accepted && parsed.hasPayload
        && parsed.primaryTarget==player_trigger::kIncident) {
        const auto run=state::activity::strike_pact::native_run();
        player_trigger::Receipt receipt{};
        if(run!=0 && player_trigger::decode(std::span(parsed.payload).first(parsed.payloadLength),receipt)) {
            state::activity::strike_pact::observe_player_trigger(run,receipt.registry,
                static_cast<std::uint16_t>(receipt.slot));
        }
    }
    if(liveBinding && verdict==incident::Verdict::accepted && parsed.hasPayload
        && parsed.primaryTarget==player_trigger::kIncident) {
        const auto run=state::activity::strike_bond::native_run();
        player_trigger::Receipt receipt{};
        if(run!=0 && player_trigger::decode(std::span(parsed.payload).first(parsed.payloadLength),receipt)) {
            state::activity::strike_bond::observe_player_trigger(run,receipt.registry,
                static_cast<std::uint16_t>(receipt.slot));
        }
    }
    if(verdict==incident::Verdict::accepted)report_incident_payload(parsed,owner);
    // A skip prompt only raises this incident; playback stops when the host publishes the
    // stop authority, which the ending runtime does for a movie it is currently playing.
    const bool skipRequested = verdict == incident::Verdict::accepted
                               && parsed.primaryTarget == ending::kCinematicSkipIncident;
    state::activity::newlight::launchpad::cinematics::Incident movie{};
    middleware::encoding::bits::Reader movieReader(request.payload);
    const bool launchpadAccepted=liveBinding && verdict==incident::Verdict::accepted && parsed.hasPayload
        && state::activity::newlight::launchpad::cinematics::decode(movieReader,movie)
        && state::activity::newlight::launchpad::observe_cinematic(movie);
    const bool skipAccepted=skipRequested && (launchpadAccepted || ending::request_skip(state::activity::mission_run_generation()));
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=incident result=%s target=%u name=%s "
                                      "extra=%u selector=%u payload=%u skip=%s",
                                      incident::verdict_name(verdict),
                                      parsed.primaryTarget,
                                      incident_name(parsed.primaryTarget),
                                      parsed.extraTargetCount,
                                      static_cast<unsigned>(parsed.hasCompressedSelector),
                                      parsed.payloadLength,
                                      skipRequested ? (skipAccepted ? "stop" : "ignored") : "-");
    if (written <= 0) {
        return;
    }
    const auto level = verdict != incident::Verdict::accepted ? core::log::Level::warn
                       : skipRequested                       ? core::log::Level::info
                                                             : core::log::Level::debug;
    core::log::write(
        core::log::Channel::server, level, {line.data(), static_cast<std::size_t>(written)});
}

/**
 * Reports one activity message the route did not stage, naming its type.
 * Every inbound activity message is one-way, so nothing here can jam the Client's reply ring. An
 * unnamed drop is invisible, and membership waits on the identity message.
 * @param messageType Activity message type from the envelope.
 * @param accountHandle Handle the envelope carried.
 * @param reason Short name of the step that declined.
 */
void report_message(std::uint32_t messageType,
                    std::uint64_t accountHandle,
                    const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=message result=skip type=%u "
                                      "handle=0x%llX reason=%s",
                                      messageType,
                                      static_cast<unsigned long long>(accountHandle),
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Prepares the joined State and the whole initial lease mask as one mutation.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives join scalars and the chosen lease mask.
 * @return True when the fixed join payload and current State can stage together.
 */
[[nodiscard]] bool prepare_join(const service::Request& request, ActivityPlan& plan) noexcept {
    service::JoinRequest parsed;
    // The client takes the low slots and the server keeps the reserve above them.
    const std::size_t reserve =
        core::settings::server::gameplay::effective_reserve(core::settings::get().server.gameplay);
    const std::size_t granted = state::activity::entity_slots::kSlotCount - reserve;
    if (!service::join_request::parse_join_request(request.payload, parsed)
        || parsed.sessionId != request.accountHandle
        || !state::activity::entity_slots::prepare_join(
            parsed.sessionId, parsed.memberKey, granted, reserve, plan.entitySlotMutation)) {
        return false;
    }
    plan.correlation = parsed.correlation;
    plan.instanceKey = plan.entitySlotMutation.instanceKey;
    plan.sessionId = parsed.sessionId;
    plan.joinCharacterSoid = parsed.characterSoid;
    plan.delivery = Delivery::joinNotifications;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/**
 * Prepares only currently free slots for one positive client request.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen lease mask.
 * @return True for a valid positive request, including an exhausted zero-mask grant.
 */
[[nodiscard]] bool prepare_grant(state::activity::ActivityInstanceKey key,
                                 const service::Request& request,
                                 ActivityPlan& plan) noexcept {
    std::int32_t requested = 0;
    if (!static_cast<bool>(key) || request.accountHandle != key.sessionId
        || !service::entity_slot_request::parse_entity_slot_request(request.payload, requested)
        || requested <= 0
        || !state::activity::entity_slots::prepare_grant(
            key, static_cast<std::size_t>(requested), plan.entitySlotMutation)) {
        return false;
    }
    plan.instanceKey = plan.entitySlotMutation.instanceKey;
    plan.sessionId = key.sessionId;
    plan.delivery = Delivery::entitySlotNotification;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

/** @return How many slots one authority mask names. */
[[nodiscard]] std::size_t
mask_slot_count(const service::entity_slots::EntitySlotMask& mask) noexcept {
    std::size_t slots = 0;
    for (const std::byte value : mask) {
        slots += static_cast<std::size_t>(std::popcount(std::to_integer<unsigned char>(value)));
    }
    return slots;
}

/** @return Lowest selected logical slot, or the fixed slot-count sentinel when empty. */
[[nodiscard]] std::size_t
first_mask_slot(const service::entity_slots::EntitySlotMask& mask) noexcept {
    for (std::size_t byteIndex = 0; byteIndex < mask.size(); ++byteIndex) {
        const unsigned value = std::to_integer<unsigned char>(mask[byteIndex]);
        if (value != 0) {
            return byteIndex * service::entity_slots::kBitsPerMaskByte
                   + static_cast<std::size_t>(std::countr_zero(value));
        }
    }
    return service::entity_slots::kSlotCount;
}

/** @return Highest selected logical slot, or the fixed slot-count sentinel when empty. */
[[nodiscard]] std::size_t
last_mask_slot(const service::entity_slots::EntitySlotMask& mask) noexcept {
    for (std::size_t byteIndex = mask.size(); byteIndex > 0; --byteIndex) {
        const unsigned value = std::to_integer<unsigned char>(mask[byteIndex - 1]);
        if (value != 0) {
            return (byteIndex - 1) * service::entity_slots::kBitsPerMaskByte
                   + static_cast<std::size_t>(std::bit_width(value) - 1U);
        }
    }
    return service::entity_slots::kSlotCount;
}

/**
 * Reports one msg 26 or msg 33. Neither returns a lease. Msg 21 does.
 * @param request Validated owned svc8 envelope.
 * @param expectReason True for msg 26, which trails a 3-bit reason after the mask.
 * @return True when the fixed body for that message type decodes.
 */
[[nodiscard]] bool report_authority_release(const service::Request& request,
                                            bool expectReason) noexcept {
    authority::Release decoded;
    const bool parsed = expectReason ? authority::parse_abandon(request.payload, decoded)
                                     : authority::parse_abdicate(request.payload, decoded);
    if (!parsed) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=authority result=noted type=%u "
                                      "selector=%u reason=%d slots=%zu first=%zu last=%zu",
                                      request.messageType,
                                      static_cast<unsigned>(decoded.selector),
                                      decoded.hasReason ? decoded.reason : 0,
                                      mask_slot_count(decoded.mask),
                                      first_mask_slot(decoded.mask),
                                      last_mask_slot(decoded.mask));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Reports one msg 29, 31 or 32 answer. This host sends no msg 28 or msg 30, so an answer here is
 * the Client reconciling on its own. Nothing is staged.
 * @param request Validated owned svc8 envelope.
 * @return True when the body for that message type decodes.
 */
[[nodiscard]] bool report_query_answer(const service::Request& request) noexcept {
    namespace authority = service::entity_authority;
    authority::QueryAnswer answer;
    if (!authority::parse_query_answer(request.messageType, request.payload, answer)) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=authority result=ok type=%u corr=0x%08X "
                                      "selector=%d mask=%u",
                                      request.messageType,
                                      answer.correlation,
                                      answer.hasSelector ? static_cast<int>(answer.selector) : -1,
                                      static_cast<unsigned>(answer.hasMask));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Reports one msg 27 purge request. The host does not answer it: the reply is msg 25, whose
 * consumer asserts unless the epoch is one above the Client's own, and nothing here tracks that.
 * @param request Validated owned svc8 envelope.
 * @return True when the fixed body is present.
 */
[[nodiscard]] bool report_request_purge(const service::Request& request) noexcept {
    std::int32_t reason = 0;
    if (!service::entity_authority::parse_request_purge(request.payload, reason)) {
        return false;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=activity stage=purge result=noted reason=%d", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Prepares only the slots that are both held and in the returned mask.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the chosen release mask.
 * @return True when the exact mask decodes and its session can stage a release.
 */
[[nodiscard]] bool prepare_release(state::activity::ActivityInstanceKey key,
                                   const service::Request& request,
                                   ActivityPlan& plan) noexcept {
    service::entity_slots::EntitySlotMask decoded{};
    if (!static_cast<bool>(key) || request.accountHandle != key.sessionId
        || !service::entity_slots::decode_entity_slots(request.payload, decoded)) {
        return false;
    }
    state::activity::entity_slots::LeaseMask returned{};
    std::copy(decoded.begin(), decoded.end(), returned.begin());
    if (!state::activity::entity_slots::prepare_release(
            key, returned, plan.entitySlotMutation)) {
        return false;
    }
    plan.instanceKey = plan.entitySlotMutation.instanceKey;
    plan.sessionId = key.sessionId;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::entitySlots;
    return true;
}

} // namespace

/** Routes one svc8 activity message and prepares any supported push transaction. */
bool process(Session& session,
             std::span<const std::byte> requestBody,
             ActivityPlan& plan,
             bool& hasTransaction) noexcept {
    plan = {};
    hasTransaction = false;

    service::Request request;
    if (!service::parse_request(requestBody, request)) {
        report_message(0, 0, "parse");
        return false;
    }
    const bool requiresLiveBinding = request.messageType == epoch_message::kMessageType
        || request.messageType == service::adventure_start::kMessageType
        || request.messageType == service::sense_update::kMessageType
        || request.messageType == service::entity_slot_request::kMessageType
        || request.messageType == service::entity_slots::kRequestMessageType
        || request.messageType == service::state_refresh::kMessageType
        || request.messageType == service::client_identity::kMessageType
        || request.messageType == service::client_authoritative_data::kMessageType
        || request.messageType == service::membership_acknowledgement::kMessageType;
    const bool hasBoundHandle = request.messageType == epoch_message::kMessageType
                                    ? (request.accountHandle == 0
                                       || request.accountHandle
                                              == session.activity.instance.sessionId)
                                    : request.accountHandle
                                          == session.activity.instance.sessionId;
    if (requiresLiveBinding
        && (!lifecycle::activity_binding_is_current(session) || !hasBoundHandle
            || !state::activity::contains(session.activity.instance))) {
        report_message(request.messageType, request.accountHandle, "binding");
        return true;
    }
    // Dispatch is on message type alone, and every handler keys off the envelope's own handle, so
    // nothing has to be bound first. The join request carries the session in the first place, and
    // it arrives on a link that has allocated nothing.
    bool prepared = false;
    if (request.messageType == service::adventure_start::kMessageType) {
        prepared = adventure_start::prepare(session.activity.instance,request,plan);
    } else if (request.messageType == epoch_message::kMessageType) {
        // Type 52 alone carries a zero handle, so its session is the one this link allocated.
        prepared = patch_epoch::prepare(session.activity.instance, request, plan);
    } else if (request.messageType == service::sense_update::kMessageType) {
        // This is the client's observable response to the authoritative sensor roster. Capture it
        // before acknowledging the one-way message so its exact object state can be recovered.
        report_sense_update(session, request);
        return true;
    } else if (request.messageType == high_water::kMessageType
               || request.messageType == client_keepalive::kMessageType) {
        // Both are one-way notices with nothing to answer.
        return true;
    } else if (request.messageType == kJoinRequestMessageType) {
        prepared = prepare_join(request, plan);
    } else if (request.messageType == service::entity_slot_request::kMessageType) {
        prepared = prepare_grant(session.activity.instance, request, plan);
    } else if (request.messageType == service::entity_slots::kRequestMessageType) {
        prepared = prepare_release(session.activity.instance, request, plan);
    } else if (request.messageType == service::state_refresh::kMessageType) {
        prepared = membership::prepare_refresh(session.activity.instance, request, plan);
    } else if (request.messageType == service::client_identity::kMessageType) {
        prepared = membership::prepare_identity(session.activity.instance, request, plan);
    } else if (request.messageType == service::client_authoritative_data::kMessageType) {
        prepared = membership::prepare_authoritative(session.activity.instance, request, plan);
    } else if (request.messageType == service::membership_acknowledgement::kMessageType) {
        prepared = membership::prepare_acknowledgement(session.activity.instance, request, plan);
    } else if (request.messageType == authority::kAbandonMessageType) {
        if (!report_authority_release(request, true)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == authority::kAbdicateMessageType) {
        if (!report_authority_release(request, false)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == service::incident::kMessageType) {
        const bool liveBinding = hasBoundHandle && lifecycle::activity_binding_is_current(session)
                                 && state::activity::contains(session.activity.instance)
                                 && session.activityPatchEpochSeen;
        if (liveBinding) {
            festival_pickups::receive(session, request);
            // Same guard, same message: the Forest arm of the same placed-loot report. The two
            // rings are destination-exclusive, so exactly one of them can ever queue a claim.
            forest_loot_pickups::receive(session, request);
        }
        report_incident(request, liveBinding, session.activity.instance, session);
        return true;
    } else if (request.messageType == authority::kRequestPurgeMessageType) {
        if (!report_request_purge(request)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (request.messageType == authority::kResetAcknowledgementMessageType
               || request.messageType == authority::kQueryPerBubbleMessageType
               || request.messageType == authority::kQueryResponseMessageType) {
        if (!report_query_answer(request)) {
            report_message(request.messageType, request.accountHandle, "parse");
        }
        return true;
    } else if (const char* name = accepted_name(request.messageType); name != nullptr) {
        // One-way with nothing to change here. Accepting is the whole contract.
        report_accepted(request.messageType, name, request.payload);
        return true;
    } else {
        // Later message handlers are independent. An owned envelope is a safe no-op.
        report_message(request.messageType, request.accountHandle, "unhandled");
        return true;
    }
    // A message that cannot be staged is reported and dropped. Failing the frame would leave the
    // Client's pending ring jammed.
    if (!prepared) {
        report_message(request.messageType, request.accountHandle, "prepare");
        plan = {};
        return true;
    }
    hasTransaction = true;
    return true;
}

} // namespace dawn::server::bap::encrypted::activity_message
