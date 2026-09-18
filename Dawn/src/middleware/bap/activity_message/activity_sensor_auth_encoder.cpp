#include <algorithm>

#include "sensor_auth_update.h"
#include "native/roster_lifetime_wire.h"
#include "../../../state/activity/coo/native_presentation_authority.h"
#include "../../../state/activity/omega_intro_rules.h"
#include "../../../state/activity/omega_portal_entry.h"
#include "../../../state/activity/omega_ikora_lattice.h"
#include "../../../state/activity/omega_crown_respawn_authority.h"
#include "../../../state/activity/omega_enemy_crown_catalog.h"
#include "../../../state/activity/omega_lair_full_roster_catalog.h"
#include "../../../state/activity/omega_ending_rules.h"

namespace dawn::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;
namespace portal = state::activity::omega_portal_entry;
namespace lattice = state::activity::omega_ikora_lattice;
namespace crown = state::activity::omega_crown_respawn;
namespace music = state::activity::omega_music;

[[nodiscard]] bool publishes_music(const Snapshot& snapshot) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaMusicPresent;
}

/** The type-13 slot type, which is the only one that may carry the player key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** Native-owned mission state omitted from Omega's deltas after its one initialization packet. */
constexpr std::uint8_t kSlotTypeActivityScript = 18;
constexpr std::uint8_t kSlotTypeMissionDirector = 35;
/** Omega's authored activity-script and mission-director registry. */
constexpr std::uint32_t kOmegaMissionRuntimeRegistry = 0x4786C0E0U;
/** Client-backed scene_ikora_opens_portal authority record. */
constexpr std::uint32_t kOmegaOpeningRegistry = 0xD00142CFU;
/** Client-backed lighthouse_teleport authority record. */
constexpr std::uint32_t kOmegaTeleportRegistry = 0xBA5F26EFU;
constexpr std::uint8_t kOmegaSceneSlotType = 43;
constexpr std::uint16_t kOmegaSceneSlotIndex = 1;
/** Ghost pre-roll dialogue slot (type 53) in the global sensor group; body from bodies.cpp. */
constexpr std::uint32_t kOmegaDialogueRegistry = 0x82FB58B7U;
/** Infinite Forest map-generator group (bubble 11), amended into the roster activity-wide. */
constexpr std::uint32_t kOmegaForestGeneratorRegistry = 0x2763EC97U;
constexpr std::uint32_t kOmegaLairRegistry = state::activity::omega_presentation::kIntroRegistry;
constexpr std::uint8_t kOmegaDialogueSlotType = 53;
constexpr std::uint16_t kOmegaDialogueSlotIndex = 2;
/** Objective directive slot (type 68) in the same mission registry group; body from bodies.cpp. */
constexpr std::uint8_t kOmegaDirectiveSlotType = 68;
constexpr std::uint16_t kOmegaDirectiveSlotIndex = 0;
constexpr std::uint8_t kOmegaPortalVisualSlotType = 4;
constexpr std::uint16_t kOmegaTeleportSlotIndex = 0;
constexpr std::uint8_t kOmegaPortalGateSlotType = 23;
constexpr std::uint16_t kOmegaPortalGateSlotIndex = 1;
constexpr std::uint16_t kOmegaPortalVisualFirstIndex = 2;
constexpr std::uint16_t kOmegaPortalVisualLastIndex = 4;
constexpr std::uint16_t kOmegaGateControllerSlotIndex = 16;
constexpr std::uint8_t kOmegaEngagementSlotType = 70;
constexpr std::uint16_t kOmegaEngagementSlotIndex = 17;
constexpr std::uint8_t kOmegaMonitorSlotType = 30;
constexpr std::uint16_t kOmegaOpeningMonitorSlotIndex = 20;
constexpr std::uint16_t kOmegaEntranceMonitorSlotIndex = 24;
/** The participation region rides a signed field, so this is the widest index it accepts. */
constexpr std::uint32_t kMaximumRegion = 0x7FFFFFFF;
[[nodiscard]] bool valid_sub_blocks(std::span<const BubbleSubBlock> subBlocks) noexcept {
    if (subBlocks.size() > kBubbleSubBlockCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < subBlocks.size(); ++index) {
        const BubbleSubBlock& block = subBlocks[index];
        if (block.bubble > kMaximumSubBlockBubble || block.keys.empty()
            || block.keys.size() > kBubbleKeyCapacity
            || (!block.presence.empty() && block.presence.size() != block.keys.size())) {
            return false;
        }
        for (const auto present : block.presence) {
            if (present > 1) { return false; }
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (subBlocks[earlier].bubble == block.bubble) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Checks the scalars whose out-of-range values would encode with no complaint.
 * @param snapshot Message input.
 * @return True when every scalar fits its field.
 */
[[nodiscard]] bool valid(const Snapshot& snapshot) noexcept {
    if (!lifetime_wire::valid(snapshot.roster)) return false;
    if((snapshot.activityClock && !native::activity_clock::valid(*snapshot.activityClock))
        || (!snapshot.activityClock && snapshot.activityElapsedTicks))return false;
    if(snapshot.dialogues.count && (snapshot.archiveOmega || !native::dialogue::valid(snapshot.dialogues,snapshot.roster))) return false;
    if(snapshot.sequences.count && (snapshot.archiveOmega || !native::world_sequence::valid(snapshot.sequences,snapshot.roster,snapshot.region))) return false;
    if(snapshot.eventParticipants.count && (snapshot.archiveOmega || !native::event_participant::valid(snapshot.eventParticipants,snapshot.roster,snapshot.region))) return false;
    if(snapshot.music.count && (snapshot.archiveOmega || !native::music::valid(snapshot.music,snapshot.roster,snapshot.region))) return false;
    if(!native::player_predicates::compose(snapshot.playerPredicates,snapshot.omegaPortalPlayerHash,
        state::activity::omega_portal_entry::kRequiredPlayerHash))return false;
    if(!native::placement::valid(snapshot.placements,snapshot.roster,snapshot.region)) return false;
    if(!native::lost_sector_shield::valid(snapshot.lostSectorShields,snapshot.roster,snapshot.region)) return false;
    if(!native::engagement::valid(snapshot.engagements,snapshot.roster,snapshot.region)) return false;
    if(!native::forest_generator::valid(snapshot.generators,snapshot.roster,snapshot.region)) return false;
    if(!native::world_device::valid(snapshot.devices,snapshot.roster,snapshot.region)) return false;
    if(!native::npc_animation::valid(snapshot.animations,snapshot.roster,snapshot.region)) return false;
    // Existing archive presentation retains its own authority contract.
    if(snapshot.cues.count) return false;
    if(!native::population::valid(snapshot.populations,snapshot.roster,snapshot.region)) return false;
    if (publishes_music(snapshot) && !music::valid(snapshot.omegaMusic)) { return false; }
    if (std::find(kLifetimeStates.begin(), kLifetimeStates.end(), snapshot.lifetime)
        == kLifetimeStates.end()) {
        return false;
    }
    if (snapshot.hasRegion && snapshot.region > kMaximumRegion) {
        return false;
    }
    if (snapshot.hasSpawnOverride
        && (snapshot.spawnSliceSet > kMaximumSpawnSliceSet || snapshot.spawnSetHash == 0
            || (snapshot.spawnSetHash == kAbsentSpawnSetHash && !snapshot.preferSpawnHistory))) {
        return false;
    }
    if(snapshot.preferSpawnHistory && (!snapshot.hasSpawnOverride
        || snapshot.spawnSetHash!=kAbsentSpawnSetHash))return false;
    if (snapshot.missionDirectorVariant > kMaximumMissionDirectorVariant) {
        return false;
    }
    const bool publishesRuntime = snapshot.omegaOpeningStage == kOmegaOpeningStageTriggered
                                  || snapshot.omegaOpeningStage
                                         == kOmegaOpeningStageCompleted
                                  || snapshot.omegaOpeningStage
                                         == kOmegaForestStageTransition;
    if (snapshot.omegaOpeningStage > kOmegaForestStageSettled
        || snapshot.omegaOpeningStage == kOmegaOpeningStageBaseline
        || snapshot.publishOmegaOpeningTransition
               != publishesRuntime) {
        return false;
    }
    const bool sceneStage = snapshot.omegaOpeningStage == kOmegaOpeningStageScene
                            || snapshot.omegaOpeningStage == kOmegaOpeningStageReady;
    const bool portalAuthorityStage =
        snapshot.omegaOpeningStage == kOmegaOpeningStageTriggered
        || snapshot.omegaOpeningStage == kOmegaOpeningStagePortal;
    const bool portalTransitionStage =
        snapshot.omegaOpeningStage == kOmegaOpeningStageCompleted
        || snapshot.omegaOpeningStage == kOmegaForestStageTransition;
    if ((sceneStage
         && (!snapshot.omegaSceneAuthority || !kOmegaSceneAuthorityBodyReady))
        || (portalAuthorityStage && !snapshot.omegaPortalMutation)
        || (portalTransitionStage && !snapshot.omegaPortalMutation)) {
        return false;
    }
    // The grant is a change, not a value: the client compares it against a mirror that starts at
    // zero, so a token of zero grants nothing.
    if (snapshot.hasGrant
        && (snapshot.grant.bubble > kMaximumGrantBubble
            || snapshot.grant.token < kMinimumGrantToken)) {
        return false;
    }
    // This is host storage for global and bubble-local groups together. Group
    // bodies are presence-terminated; Omega's complete loading roster has 16.
    if (snapshot.roster.groupCount > kGroupCapacity
        || snapshot.roster.topLevelGroupCount > snapshot.roster.groupCount) {
        return false;
    }
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.slotTypes.size() != row.slotFlags.size()
            || row.slotTypes.size() != row.slotIndices.size() || row.slotTypes.empty()) {
            return false;
        }
        for (const std::uint16_t index : row.slotIndices) {
            if (index > kMaximumSlotIndex) {
                return false;
            }
        }
    }
    return valid_sub_blocks(snapshot.roster.bubbleSubBlocks);
}

/**
 * Writes every group's object blocks, in publish order. Every registered object must be seeded
 * before any auth state applies, because the client's gate walks the whole sync-record pool. A
 * partial message seeds nothing that applies.
 * @param writer Body writer sitting after the phase-1 delta.
 * @param snapshot Message input.
 * @return True when every block fits.
 */
/**
 * Emits the Infinite Forest generator group's object blocks (empty bodies) so its sync-pool
 * objects can SEED. The client's record processor rewinds records whose object bubble is not
 * the CURRENT bubble, so bubble-11 objects ignore the early full-roster pushes made while the
 * player stands in the Lighthouse; unless the post-opening stages keep publishing this group,
 * its objects stay pending forever, ClientRosterSync_AllRecordsInBubbleSeeded vetoes bubble
 * 11's seed commit, and the sweep that instantiates the network-replicated map-generator
 * worker never runs. A group block is self-contained, so appending one is framing-safe.
 */
[[nodiscard]] bool write_streamed_group(bits::Writer& writer, const Snapshot& snapshot,
                                        std::uint32_t registry, bool forceAuth) noexcept {
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.key != registry) {
            continue;
        }
        if (row.slotTypes.empty()) {
            return true;
        }
        bool encoded = writer.write(1, kPresenceWidth) && writer.write(row.key, kKeyWidth)
                       && writer.write(0, kKeyWidth);
        for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
            // The extracted flags miss most of this group's sync-carrying slots (the client's
            // pool provably holds objects for 70/0 and 30/2 with extracted flag 0), so force the
            // auth flag on EVERY slot: an empty {reset=1, present=0} block seeds a pool object,
            // and the client rewinds blocks for slots it has no pool entry for, so over-emission
            // is harmless.
            const auto flags = static_cast<std::uint8_t>(row.slotFlags[slot]
                                                        | (forceAuth ? kSlotAuthFlag : 0));
            if ((flags & (kSlotAuthFlag | kSlotSenseFlag)) == 0) { continue; }
            encoded = write_object_block(writer,
                                         snapshot,
                                         row.key,
                                         row.slotTypes[slot],
                                         row.slotIndices[slot],
                                         flags,
                                         false);
        }
        return encoded && writer.write(0, kPresenceWidth);
    }
    return true;
}

[[nodiscard]] bool write_forest_generator_group(bits::Writer& writer,
                                               const Snapshot& snapshot) noexcept {
    return write_streamed_group(writer, snapshot, kOmegaForestGeneratorRegistry, true);
}

/**
 * The Ghost dialogue (53/2) and the objective directive (68/0) live in the top-level mission
 * registry group 82FB58B7, and write_phase_two is the only pass that publishes that group. The
 * Crown-arrival branch of write_body replaces phase two for the rest of the fight, so from Crown
 * arrival on no push carried the group at all: run 27456 (2026-09-06) published rows 15/16 while
 * the type-5 body never grew by the active row's 8 bytes, and the native component's last apply
 * was the last pre-arrival push. Publish these objects and any explicit music authority alongside the
 * restriction: they were being re-applied on every push before arrival, they are neither script
 * nor participation state, and their block shape is the general pass's (extracted flags plus a
 * forced auth flag). A roster without the group writes nothing; a header without objects would
 * desync the object list, so the group opens only when a target slot exists.
 */
[[nodiscard]] bool write_dialogue_group(bits::Writer& writer, const Snapshot& snapshot,
                                        bool& musicWritten) noexcept {
    if (!kOmegaDialogueBodyReady || !snapshot.omegaSceneAuthority || !snapshot.omegaDialogueArm) {
        return true;
    }
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.key != kOmegaDialogueRegistry) {
            continue;
        }
        const auto selected = [&row, &snapshot](std::size_t slot) noexcept {
            return (row.slotTypes[slot] == kOmegaDialogueSlotType
                    && row.slotIndices[slot] == kOmegaDialogueSlotIndex)
                   || (row.slotTypes[slot] == kOmegaDirectiveSlotType
                       && row.slotIndices[slot] == kOmegaDirectiveSlotIndex)
                   || (publishes_music(snapshot)
                       && music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot]));
        };
        bool hasTarget = false;
        for (std::size_t slot = 0; !hasTarget && slot < row.slotTypes.size(); ++slot) {
            hasTarget = selected(slot);
        }
        if (!hasTarget) {
            return true;
        }
        bool encoded = writer.write(1, kPresenceWidth) && writer.write(row.key, kKeyWidth)
                       && writer.write(0, kKeyWidth);
        for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
            if (!selected(slot)) {
                continue;
            }
            encoded = write_object_block(writer, snapshot, row.key, row.slotTypes[slot],
                                         row.slotIndices[slot],
                                         static_cast<std::uint8_t>(row.slotFlags[slot] | kSlotAuthFlag),
                                         false);
            musicWritten = musicWritten
                || music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot]);
        }
        return encoded && writer.write(0, kPresenceWidth);
    }
    return true;
}

/** Keep the native music object supplied through filtered opening/streaming paths. */
[[nodiscard]] bool write_music_group(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    if (!publishes_music(snapshot)) { return true; }
    const Group* selected{};
    std::size_t selectedSlot{};
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const auto& row = snapshot.roster.groups[group];
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if (!music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot])) { continue; }
            if (selected != nullptr) { return false; }
            selected = &row;
            selectedSlot = slot;
        }
    }
    if (selected == nullptr) { return true; }
    return writer.write(1, kPresenceWidth) && writer.write(selected->key, kKeyWidth)
        && writer.write(0, kKeyWidth)
        && write_object_block(writer, snapshot, selected->key, 11, 1,
            static_cast<std::uint8_t>(selected->slotFlags[selectedSlot] | kSlotAuthFlag), false)
        && writer.write(0, kPresenceWidth);
}

/** Publish only the carrier when the native-owned path omits other opening objects. */
[[nodiscard]] bool write_portal_entry(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const auto& row = snapshot.roster.groups[group];
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if (!portal::is_carrier(row.key,row.slotTypes[slot],row.slotIndices[slot])) { continue; }
            return writer.write(1,kPresenceWidth) && writer.write(row.key,kKeyWidth)
                && writer.write(0,kKeyWidth)
                && write_object_block(writer,snapshot,row.key,row.slotTypes[slot],row.slotIndices[slot],
                    static_cast<std::uint8_t>(row.slotFlags[slot] | kSlotAuthFlag),false)
                && writer.write(0,kPresenceWidth);
        }
    }
    return true;
}

/** Update one registered Lighthouse object without reopening native runtime authority. */
[[nodiscard]] bool write_ikora_object(bits::Writer& writer, const Snapshot& snapshot,
                                     std::uint8_t type, std::uint16_t index) noexcept {
    const Group* selected{};
    std::size_t selectedSlot{};
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const auto& row = snapshot.roster.groups[group];
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if (row.key != kOmegaOpeningRegistry || row.slotTypes[slot] != type
                || row.slotIndices[slot] != index) continue;
            if (selected != nullptr) return false;
            selected = &row;
            selectedSlot = slot;
        }
    }
    // A direct Forest/Lair roster may omit the Lighthouse group entirely.
    if (selected == nullptr) return true;
    return writer.write(1, kPresenceWidth) && writer.write(selected->key, kKeyWidth)
        && writer.write(0, kKeyWidth)
        && write_object_block(writer, snapshot, selected->key, type, index,
            static_cast<std::uint8_t>(selected->slotFlags[selectedSlot] | kSlotAuthFlag), false)
        && writer.write(0, kPresenceWidth);
}

/** Publish only the two authored restriction records at Crown arrival. Opening
 * the general authority pass again would overwrite native script/participation
 * state. Validate both slots before writing either, and order lifetime first. */
[[nodiscard]] bool write_crown_restriction(bits::Writer& writer,const Snapshot& snapshot) noexcept {
    const Group* selected{};
    std::size_t lifetime{},director{};
    unsigned lifetimeCount{},directorCount{};
    for(std::size_t i=0;i<snapshot.roster.groupCount;++i) {
        const auto& row=snapshot.roster.groups[i];
        if(row.key!=crown::kRegistry) { continue; }
        if(selected!=nullptr) { return false; }
        selected=&row;
        for(std::size_t j=0;j<row.slotTypes.size();++j) {
            if(crown::is_lifetime(row.key,row.slotTypes[j],row.slotIndices[j])) { lifetime=j;++lifetimeCount; }
            if(crown::is_director(row.key,row.slotTypes[j],row.slotIndices[j])) { director=j;++directorCount; }
        }
    }
    if(selected==nullptr || lifetimeCount!=1 || directorCount!=1
        || (selected->slotFlags[lifetime]&kSlotAuthFlag)==0
        || (selected->slotFlags[director]&kSlotAuthFlag)==0) { return false; }
    return writer.write(1,kPresenceWidth) && writer.write(selected->key,kKeyWidth) && writer.write(0,kKeyWidth)
        && write_object_block(writer,snapshot,selected->key,17,3,selected->slotFlags[lifetime],false)
        && write_object_block(writer,snapshot,selected->key,35,1,selected->slotFlags[director],false)
        && writer.write(0,kPresenceWidth);
}

/**
 * The native return resets the pool, including 19 runtime objects omitted by the Crown filter.
 * Original 4D6530 requires every object to receive a reset before any pending body can apply.
 * During the ending offer only, seed those exact objects with schema defaults: no script body,
 * player participation key, or synthetic revision. Keep the two existing restriction writers.
 */
[[nodiscard]] bool write_ending_runtime_seed(bits::Writer& writer,
                                            const Snapshot& snapshot) noexcept {
    constexpr std::array<std::uint8_t,21> types{
        16,35,18,17,41,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13};
    const Group* selected{};
    std::array<std::size_t,21> positions{};
    std::array<bool,21> found{};
    for(std::size_t group=0;group<snapshot.roster.groupCount;++group) {
        const auto& row=snapshot.roster.groups[group];
        if(row.key!=kOmegaMissionRuntimeRegistry) { continue; }
        if(selected!=nullptr || row.slotTypes.size()!=types.size()) { return false; }
        selected=&row;
        for(std::size_t slot=0;slot<row.slotTypes.size();++slot) {
            const auto index=row.slotIndices[slot];
            if(index>=types.size() || found[index] || row.slotTypes[slot]!=types[index]
                || row.slotFlags[slot]!=(index<4?2U:3U)) { return false; }
            positions[index]=slot;found[index]=true;
        }
    }
    if(selected==nullptr) { return false; }
    bool encoded=writer.write(1,kPresenceWidth) && writer.write(selected->key,kKeyWidth)
        && writer.write(0,kKeyWidth)
        && write_object_block(writer,snapshot,selected->key,17,3,selected->slotFlags[positions[3]],false)
        && write_object_block(writer,snapshot,selected->key,35,1,selected->slotFlags[positions[1]],false);
    for(std::size_t index=0;encoded && index<types.size();++index) {
        if(index==1 || index==3) { continue; }
        const bool sense=(selected->slotFlags[positions[index]]&kSlotSenseFlag)!=0;
        // Native per-object envelope: auth reset=1, auth present=0, optional sense absent=0.
        encoded=writer.write(1,kPresenceWidth) && writer.write(selected->key,kKeyWidth)
            && writer.write(std::uint32_t{types[index]}+kSlotTypeBias,kSlotTypeWidth)
            && writer.write(static_cast<std::uint32_t>(index)+kSlotIndexBias,kSlotIndexWidth)
            && writer.write(sense?3U:2U,kKeyWidth)
            && writer.write(1,kPresenceWidth) && writer.write(0,kPresenceWidth)
            && (!sense || writer.write(0,kPresenceWidth));
    }
    return encoded && writer.write(0,kPresenceWidth);
}

[[nodiscard]] bool write_phase_two(bits::Writer& writer, const Snapshot& snapshot,
                                   bool& portalWritten, bool& musicWritten, bool& latticeWritten,
                                   bool& sceneWritten) noexcept {
    if (snapshot.omegaOpeningStage == kOmegaOpeningStageScene
        || snapshot.omegaOpeningStage == kOmegaOpeningStageReady) {
        // The authored source must publish before the Scene that requests it. The Ghost group
        // remains optional; encode() owns the enclosing group-list terminator.
        const bool dialogue = kOmegaDialogueBodyReady && snapshot.omegaSceneAuthority
                              && snapshot.omegaDialogueArm;
        bool encoded = true;
        std::size_t selectedGroups = 0;
        std::size_t sceneObjects = 0;
        std::size_t sourceObjects = 0;
        std::size_t dialogueObjects = 0;
        std::size_t musicObjects = 0;
        for (std::size_t group = 0;
             encoded && group < snapshot.roster.groupCount;
             ++group) {
            const Group& row = snapshot.roster.groups[group];
            const bool sceneGroup = row.key == kOmegaOpeningRegistry;
            const bool dialogueGroup = (dialogue || publishes_music(snapshot))
                                      && row.key == kOmegaDialogueRegistry;
            if (!sceneGroup && !dialogueGroup) {
                continue;
            }
            // Only open a group block if it carries the object we publish; a header with no slot
            // would desync the object list.
            bool hasTarget = false;
            for (std::size_t slot = 0; !hasTarget && slot < row.slotTypes.size(); ++slot) {
                const bool scene = sceneGroup && row.slotTypes[slot] == kOmegaSceneSlotType
                                   && row.slotIndices[slot] == kOmegaSceneSlotIndex
                                   && (row.slotFlags[slot] & (kSlotAuthFlag | kSlotSenseFlag)) != 0;
                const bool line = dialogue && dialogueGroup && row.slotTypes[slot] == kOmegaDialogueSlotType
                                  && row.slotIndices[slot] == kOmegaDialogueSlotIndex;
                const bool musicSlot = publishes_music(snapshot)
                    && music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot]);
                hasTarget = scene || line || musicSlot;
            }
            if (!hasTarget) {
                continue;
            }
            encoded = writer.write(1, kPresenceWidth)
                      && writer.write(row.key, kKeyWidth) && writer.write(0, kKeyWidth);
            std::size_t selectedInGroup = 0;
            if (sceneGroup) {
                // Explicit first pass keeps this dependency ordered even if extracted slots
                // are reordered. A missing or duplicated source rejects the complete message.
                for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
                    if (row.slotTypes[slot] != 1 || row.slotIndices[slot] != 0
                        || (row.slotFlags[slot] & (kSlotAuthFlag | kSlotSenseFlag)) == 0) {
                        continue;
                    }
                    encoded = write_object_block(writer, snapshot, row.key, 1, 0,
                        static_cast<std::uint8_t>(row.slotFlags[slot] | kSlotAuthFlag), false);
                    ++sourceObjects;
                    ++selectedInGroup;
                }
            }
            for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
                const bool selectedScene =
                    sceneGroup && row.slotTypes[slot] == kOmegaSceneSlotType
                    && row.slotIndices[slot] == kOmegaSceneSlotIndex
                    && (row.slotFlags[slot] & (kSlotAuthFlag | kSlotSenseFlag)) != 0;
                const bool selectedDialogue =
                    dialogue && dialogueGroup && row.slotTypes[slot] == kOmegaDialogueSlotType
                    && row.slotIndices[slot] == kOmegaDialogueSlotIndex;
                const bool selectedMusic = publishes_music(snapshot)
                    && music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot]);
                if (!selectedScene && !selectedDialogue && !selectedMusic) {
                    continue;
                }
                // The dialogue slot's authored flags may omit auth; force it so its body emits.
                const std::uint8_t flags =
                    selectedDialogue ? kSlotAuthFlag : selectedMusic
                        ? static_cast<std::uint8_t>(row.slotFlags[slot] | kSlotAuthFlag)
                        : row.slotFlags[slot];
                encoded = write_object_block(writer,
                                             snapshot,
                                             row.key,
                                             row.slotTypes[slot],
                                             row.slotIndices[slot],
                                             flags,
                                             false);
                sceneObjects += selectedScene ? 1U : 0U;
                sceneWritten = sceneWritten || (selectedScene && (flags & kSlotAuthFlag) != 0);
                dialogueObjects += selectedDialogue ? 1U : 0U;
                musicObjects += selectedMusic ? 1U : 0U;
                musicWritten = musicWritten || selectedMusic;
                ++selectedInGroup;
            }
            encoded = encoded && selectedInGroup == (sceneGroup ? 2U : dialogueObjects + musicObjects)
                      && writer.write(0, kPresenceWidth);
            ++selectedGroups;
        }
        const std::size_t expectedGroups = 1U + (dialogueObjects + musicObjects > 0 ? 1U : 0U);
        return encoded && sourceObjects == 1 && sceneObjects == 1 && dialogueObjects <= 1 && musicObjects <= 1
               && selectedGroups == expectedGroups
               && write_forest_generator_group(writer, snapshot);
    }
    if (snapshot.omegaOpeningStage == kOmegaOpeningStageTriggered
        || snapshot.omegaOpeningStage == kOmegaOpeningStagePortal) {
        const bool includesRuntime =
            snapshot.omegaOpeningStage == kOmegaOpeningStageTriggered;
        bool encoded = true;
        std::size_t selectedGroups = 0;
        std::size_t selectedObjects = 0;
        for (std::size_t group = 0;
             encoded && group < snapshot.roster.groupCount;
             ++group) {
            const Group& row = snapshot.roster.groups[group];
            const bool runtimeGroup =
                includesRuntime && row.key == kOmegaMissionRuntimeRegistry;
            const bool teleportGroup = row.key == kOmegaTeleportRegistry;
            const bool visualGroup = row.key == kOmegaOpeningRegistry;
            if (!runtimeGroup && !teleportGroup && !visualGroup) {
                continue;
            }
            encoded = writer.write(1, kPresenceWidth)
                      && writer.write(row.key, kKeyWidth) && writer.write(0, kKeyWidth);
            std::size_t selectedInGroup = 0;
            for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
                const std::uint16_t slotIndex = row.slotIndices[slot];
                const std::uint8_t slotType = row.slotTypes[slot];
                const bool selectedRuntime =
                    runtimeGroup
                    && (slotType == kSlotTypeActivityScript
                        || slotType == kSlotTypeMissionDirector);
                const bool selectedTeleport = teleportGroup
                                              && slotType == kOmegaPortalVisualSlotType
                                              && slotIndex == kOmegaTeleportSlotIndex;
                const bool selectedGate = teleportGroup
                                          && slotType == kOmegaPortalGateSlotType
                                          && slotIndex == kOmegaPortalGateSlotIndex;
                const bool selectedVisual = visualGroup
                                            && slotType == kOmegaPortalVisualSlotType
                                            && slotIndex >= kOmegaPortalVisualFirstIndex
                                            && slotIndex <= kOmegaPortalVisualLastIndex;
                const bool selectedMissionGate =
                    visualGroup && slotType == kOmegaPortalGateSlotType
                    && slotIndex == kOmegaGateControllerSlotIndex;
                const bool selectedEngagement =
                    visualGroup && slotType == kOmegaEngagementSlotType
                    && slotIndex == kOmegaEngagementSlotIndex;
                const bool selectedMonitor =
                    visualGroup && slotType == kOmegaMonitorSlotType
                    && (slotIndex == kOmegaOpeningMonitorSlotIndex
                        || slotIndex == kOmegaEntranceMonitorSlotIndex);
                if ((row.slotFlags[slot] & (kSlotAuthFlag | kSlotSenseFlag)) == 0
                    || (!selectedRuntime && !selectedTeleport && !selectedGate
                        && !selectedVisual && !selectedMissionGate
                        && !selectedEngagement && !selectedMonitor)) {
                    continue;
                }
                encoded = write_object_block(writer,
                                             snapshot,
                                             row.key,
                                             row.slotTypes[slot],
                                             row.slotIndices[slot],
                                             row.slotFlags[slot],
                                             false);
                ++selectedInGroup;
                ++selectedObjects;
                portalWritten = portalWritten || selectedTeleport;
                latticeWritten = latticeWritten || (selectedMissionGate
                    && (row.slotFlags[slot] & kSlotAuthFlag) != 0);
            }
            const std::size_t expectedInGroup = runtimeGroup     ? 2U
                                                : teleportGroup ? 2U
                                                                : 7U;
            encoded = encoded && selectedInGroup == expectedInGroup
                      && writer.write(0, kPresenceWidth);
            ++selectedGroups;
        }
        const std::size_t expectedGroups = includesRuntime ? 3U : 2U;
        const std::size_t expectedObjects = includesRuntime ? 11U : 9U;
        return encoded && selectedGroups == expectedGroups
               && selectedObjects == expectedObjects
               && write_forest_generator_group(writer, snapshot);
    }
    if (snapshot.omegaOpeningStage == kOmegaOpeningStageCompleted
        || snapshot.omegaOpeningStage == kOmegaForestStageTransition) {
        for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
            const Group& row = snapshot.roster.groups[group];
            if (row.key != kOmegaMissionRuntimeRegistry) {
                continue;
            }
            bool encoded = writer.write(1, kPresenceWidth)
                           && writer.write(row.key, kKeyWidth) && writer.write(0, kKeyWidth);
            std::size_t selected = 0;
            for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
                const std::uint8_t slotType = row.slotTypes[slot];
                if ((row.slotFlags[slot] & (kSlotAuthFlag | kSlotSenseFlag)) == 0
                    || (slotType != kSlotTypeActivityScript
                        && slotType != kSlotTypeMissionDirector)) {
                    continue;
                }
                encoded = write_object_block(writer,
                                             snapshot,
                                             row.key,
                                             slotType,
                                             row.slotIndices[slot],
                                             row.slotFlags[slot],
                                             false);
                ++selected;
            }
            return encoded && selected == 2 && writer.write(0, kPresenceWidth)
                   && write_forest_generator_group(writer, snapshot);
        }
        return false;
    }
    if (snapshot.omegaOpeningStage == kOmegaOpeningStageSettled) {
        // Phase 1 still refreshes the roster; only the forest generator group needs to keep
        // publishing so its bubble-11 sync objects can seed once the player stands there.
        return write_forest_generator_group(writer, snapshot);
    }
    bool encoded = true;
    bool keyPlaced = false;
    for (std::size_t group = 0; encoded && group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        // Published once after every opening-stage path, including the preserved runtime path.
        if (row.key == kOmegaLairRegistry
            || row.key == state::activity::omega_presentation::kBossRegistry
            || row.key == state::activity::omega_enemy_crown::kRegistry
            || row.key == state::activity::omega_lair_full_roster::kHive.key
            || row.key == state::activity::omega_lair_full_roster::kVex.key
            || row.key == state::activity::omega_lair_full_roster::kRescue.key
            || row.key == state::activity::omega_lair_full_roster::kCabal.key) { continue; }
        if(row.key==state::activity::omega_ending::kRegistry) { continue; }
        // The generator group's extracted flags miss most of its sync-carrying slots (the
        // client's pool holds objects for 70/0 and 30/2 with extracted flag 0), and a slot
        // that never receives a block never SEEDS, which vetoes bubble 11's replicated-content
        // sweep. Force the auth flag on all of its slots; the client rewinds blocks for slots
        // it has no pool entry for, so over-emission is harmless.
        const bool forestGroup = row.key == kOmegaForestGeneratorRegistry;
        // The filler word after the key is read and discarded.
        encoded = writer.write(1, kPresenceWidth) && writer.write(row.key, kKeyWidth)
                  && writer.write(0, kKeyWidth);
        for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
            const std::uint8_t slotFlags =
                (forestGroup || (publishes_music(snapshot)
                    && music::is_sensor(row.key, row.slotTypes[slot], row.slotIndices[slot])))
                    ? static_cast<std::uint8_t>(row.slotFlags[slot] | kSlotAuthFlag)
                            : row.slotFlags[slot];
            if ((slotFlags & (kSlotAuthFlag | kSlotSenseFlag)) == 0) {
                continue;
            }
            const std::uint8_t slotType = row.slotTypes[slot];
            if (snapshot.preserveMissionAuthorityState
                && (slotType == kSlotTypeActivityScript
                    || slotType == kSlotTypeMissionDirector
                    || slotType == kSlotTypeParticipation)) {
                continue;
            }
            const bool firstOrEvery = !keyPlaced || snapshot.keyOnEveryParticipationSlot;
            const bool carriesPlayerKey = slotType == kSlotTypeParticipation
                                          && row.key == snapshot.roster.playerKeyGroup
                                          && firstOrEvery;
            keyPlaced = keyPlaced || carriesPlayerKey;
            encoded = write_object_block(writer,
                                         snapshot,
                                         row.key,
                                         slotType,
                                         row.slotIndices[slot],
                                         slotFlags,
                                         carriesPlayerKey);
            portalWritten = portalWritten || portal::is_carrier(row.key,slotType,row.slotIndices[slot]);
            latticeWritten = latticeWritten || ((row.slotFlags[slot] & kSlotAuthFlag) != 0
                && lattice::is_gate({row.key, slotType, row.slotIndices[slot]}));
            sceneWritten = sceneWritten || ((row.slotFlags[slot] & kSlotAuthFlag) != 0
                && row.key == kOmegaOpeningRegistry && slotType == kOmegaSceneSlotType
                && row.slotIndices[slot] == kOmegaSceneSlotIndex);
            musicWritten = musicWritten || (publishes_music(snapshot)
                && music::is_sensor(row.key, slotType, row.slotIndices[slot]));
        }
        encoded = encoded && writer.write(0, kPresenceWidth);
    }
    return encoded;
}

/**
 * Writes the whole body through one writer.
 * @param writer Real or measuring writer positioned at the first bit.
 * @param snapshot Message input.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_body(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // The hardwipe token is unchecked unless the client's `use_hardwipe_tokens` config is on.
    bool encoded = writer.write(0, kHardwipeWidth)
                   && writer.write(snapshot.patchEpoch.first, kEpochWidth)
                   && writer.write(snapshot.patchEpoch.second, kEpochWidth)
                   && writer.write(snapshot.hasGrant ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasGrant) {
        encoded = write_bubble_block(writer, snapshot.grant);
    }
    const std::size_t latchBit = kLatchBitWithoutGrant + (snapshot.hasGrant ? kBubbleBlockBits : 0);
    // Native3C9FC0 synchronizes the source context clock from this header.
    // No opt-in retains the exact legacy zero bytes.
    encoded = encoded && native::activity_clock::write_elapsed(writer, snapshot.activityElapsedTicks) && writer.bit_count() == latchBit;
    // The enable latch is not sticky, so it goes on every message.
    encoded = encoded && writer.write(1, kPresenceWidth)
              && write_roster_delta(writer, snapshot.roster, snapshot.stateSequence)
              && writer.bit_count()
                     == latchBit + 1
                            + delta_bits(top_level_key_count(snapshot.roster),
                                         snapshot.roster.bubbleSubBlocks);
    // Once the client has acknowledged type 18, even reapplying unrelated authority objects is
    // destructive: the native publish pass copies their neutral route input over the live script
    // phase. Keep publishing the phase-1 roster delta, but leave the entire phase-2 object list
    // absent so the changed-object collector stays empty and native simulation state survives.
    if (encoded && !snapshot.phaseOneOnly) {
        bool portalWritten = false;
        bool musicWritten = false;
        bool latticeWritten = false;
        bool sceneWritten = false;
        if(snapshot.omegaEndingSeedRuntime && snapshot.omegaSceneAuthority
            && snapshot.omegaEndingState==1) {
            // Return travel rebuilds the sync pool even when the mission's
            // components retain their authority. Native 4D6530 blocks every
            // pending command until all root records have been initialized.
            // This writer sends empty initialization envelopes for script and
            // participation; it never republishes their retained values.
            encoded=write_ending_runtime_seed(writer,snapshot)
                && write_dialogue_group(writer,snapshot,musicWritten);
        } else if(crown::publishes(snapshot.omegaSceneAuthority,
                           crown::intent(snapshot.omegaCrownRestriction,snapshot.omegaCrownRestricted))) {
            encoded=write_forest_generator_group(writer,snapshot) && write_crown_restriction(writer,snapshot)
                && write_dialogue_group(writer,snapshot,musicWritten);
        } else if (!snapshot.preserveMissionAuthorityState
            || snapshot.omegaOpeningStage != kOmegaOpeningStageNone) {
            encoded = write_phase_two(writer, snapshot, portalWritten, musicWritten, latticeWritten, sceneWritten);
        } else {
            // The forest generator group is exempt from the suppression: its blocks are empty
            // ({reset=1, present=0}, no authority applied, nothing clobbered) and they are the
            // only way its bubble-11 sync objects ever SEED — without them
            // ClientRosterSync_AllRecordsInBubbleSeeded vetoes the bubble's seed commit and
            // the replicated map-generator worker never instantiates.
            // Dialogue and objectives are host-owned too. Suppressing them here
            // delayed Ghost's arrival line until the scene bootstrap and left
            // the HUD on its opening objective after the native entrance edge.
            encoded = write_forest_generator_group(writer, snapshot)
                && write_dialogue_group(writer, snapshot, musicWritten);
        }
        // Arrival created the actor and scene. A later approach changes only
        // the scene's retained event list; keep its generation and source fixed.
        if (encoded && snapshot.omegaSceneAuthority && snapshot.omegaIkoraPortalRequested && !sceneWritten) {
            encoded = write_ikora_object(writer, snapshot, kOmegaSceneSlotType, kOmegaSceneSlotIndex);
        }
        // Like the contact carrier, the gate remains host-owned after native
        // mission initialization. Publish its retained release through every
        // filtered path, without resending script, participation or scene bodies.
        if (encoded && snapshot.omegaSceneAuthority && snapshot.omegaIkoraLatticeReleased && !latticeWritten) {
            encoded = write_ikora_object(writer, snapshot, lattice::kGateType, lattice::kGateIndex);
        }
        // A later carrier activation must never reopen participation publication. In stages
        // that already wrote this exact slot, retain that single body instead of duplicating it.
        if (encoded && snapshot.omegaSceneAuthority && snapshot.omegaPortalEntry && !portalWritten) {
            encoded = write_portal_entry(writer,snapshot);
        }
        if (encoded && !musicWritten) { encoded = write_music_group(writer, snapshot); }
        // The Lair is streamed long after the opening stage. Its complete descriptor-backed
        // group must keep seeding at that boundary; a type-6 body writer alone never reaches
        // the client through the opening stage's filtered object list. Only the reveal slot
        // carries authority; the other Lair objects receive registration defaults.
        encoded = encoded && write_streamed_group(writer, snapshot, kOmegaLairRegistry, false)
            && write_streamed_group(writer, snapshot, state::activity::omega_presentation::kBossRegistry, false)
            && write_streamed_group(writer, snapshot, state::activity::omega_enemy_crown::kRegistry, false);
        for(const auto& group:state::activity::omega_lair_full_roster::kCombatGroups) {
            encoded=encoded && write_streamed_group(writer,snapshot,group.key,false);
        }
        encoded=encoded && write_streamed_group(writer,snapshot,state::activity::omega_ending::kRegistry,false);
    }
    // The entity-group loop end, then the trailing pair, which short-circuits to one bit.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth);
}

} // namespace

/** Encodes one `sensor_auth_update` body. */
bool encode_sensor_auth_update(const Snapshot& snapshot,
                               std::span<std::byte> output,
                               std::size_t& written) noexcept {
    written = 0;
    // A zero-width invalid dialogue must reject the entire packet; otherwise
    // the object filter silently omits the required mission speech.
    const auto validDialogue=[](const auto& frame) noexcept {
        return !frame.enabled || state::activity::coo::native_presentation::dialogue_bits(frame.generations,frame.activeRow)!=0;
    };
    if(!validDialogue(snapshot.deadly_trial) || !validDialogue(snapshot.gateway)
        || !validDialogue(snapshot.beyond_infinity) || !validDialogue(snapshot.deep_storage)
        || !validDialogue(snapshot.hijacked) || !validDialogue(snapshot.strike_bond)
        || !validDialogue(snapshot.strike_pact)) { return false; }
    if (!snapshot.archiveOmega) { return legacy_encode_sensor_auth_update(snapshot, output, written); }

    written = 0;
    if (output.empty() || !valid(snapshot)) {
        return false;
    }

    // Measure first. The writer clears and fills the caller's storage as it goes, so a body that
    // does not fit would leave a partial one behind.
    bits::Writer measure = bits::Writer::measuring();
    std::size_t required = 0;
    if (!write_body(measure, snapshot) || !measure.finish(required) || required > output.size()) {
        return false;
    }

    bits::Writer writer(output);
    std::size_t produced = 0;
    if (!write_body(writer, snapshot) || !writer.finish(produced) || produced != required) {
        return false;
    }
    written = produced;
    return true;
}

} // namespace dawn::middleware::bap::activity_message::sensor_auth_update
