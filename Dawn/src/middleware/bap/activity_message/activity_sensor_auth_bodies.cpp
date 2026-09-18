#include <array>
#include <bit>

#include "sensor_auth_update.h"
#include "../../../state/activity/coo/native_presentation_authority.h"
#include "../../../state/activity/coo/native_mission_forest_authority.h"
#include "../../../state/activity/beyond_infinity/forest_selection.h"
#include "../../../state/activity/omega_intro_rules.h"
#include "../../../state/activity/omega_ending_rules.h"
#include "../../../state/activity/omega_combatant_authority.h"
#include "../../../state/activity/omega_enemy_lair_catalog.h"
#include "../../../state/activity/omega_enemy_lair_tactics.h"
#include "../../../state/activity/omega_enemy_crown_catalog.h"
#include "../../../state/activity/omega_enemy_crown_tactics.h"
#include "../../../state/activity/omega_first_mancannon_authority.h"
#include "../../../state/activity/omega_crown_transit_authority.h"
#include "../../../state/activity/omega_ikora_lattice.h"
#include "../../../state/activity/omega_portal_entry.h"
#include "../../../state/activity/omega_crown_respawn_authority.h"
#include "../../../state/activity/omega_rescue_scene_authority.h"
#include "../../../state/activity/omega_rescue_marker_authority.h"
#include "../../../state/activity/omega_arc_charge_authority.h"
#include "../../../state/activity/omega_crown_eye_status.h"

namespace dawn::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;
namespace presentation = state::activity::omega_presentation;
namespace ending = state::activity::omega_ending;
namespace lairEnemies = state::activity::omega_enemy_lair;
namespace crownEnemies = state::activity::omega_enemy_crown;
namespace cannon = state::activity::omega_first_mancannon;
namespace transit = state::activity::omega_crown_transit;
namespace lattice = state::activity::omega_ikora_lattice;
namespace crown = state::activity::omega_crown_respawn;
namespace rescue = state::activity::omega_rescue_npc;
namespace rescueMarkers = state::activity::omega_rescue_markers;
namespace charge = state::activity::omega_arc_charge;
namespace eyeStatus = state::activity::omega_crown_eye_status;
namespace music = state::activity::omega_music;

[[nodiscard]] const eyeStatus::Source* omega_eye_status_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    if(!snapshot.omegaSceneAuthority || !snapshot.omegaDialogueArm
        || snapshot.omegaFirstLairGeneration==0 || snapshot.omegaFirstLairGeneration>=0x7FFFFFFFU
        || snapshot.omegaCrownCycle>3) { return nullptr; }
    return eyeStatus::source(key,type,slot);
}

[[nodiscard]] bool omega_transit_scope(const Snapshot& snapshot) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm
        && snapshot.omegaFirstLairGeneration!=0 && snapshot.omegaFirstLairGeneration<0x7FFFFFFDU
        && snapshot.omegaCrownCycle<=3;
}
[[nodiscard]] transit::TransitAuthority omega_transit_state(const Snapshot& snapshot) noexcept {
    return {snapshot.omegaCrownCycle,snapshot.omegaCrownTransitLaunches,
        snapshot.omegaCrownTransitBridge,snapshot.omegaCrownTransitTarget,snapshot.omegaCrownFinalTraversal,
        snapshot.omegaCrownTransitCreated,
        snapshot.omegaCrownChargeDunked,
        snapshot.omegaCrownReturnLaunch};
}
[[nodiscard]] const transit::Source* omega_transit_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_transit_scope(snapshot) && type==4?transit::source(key,slot):nullptr;
}
[[nodiscard]] const transit::Source* omega_transit_gate(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_transit_scope(snapshot) && type==23?transit::gate(key,slot):nullptr;
}
[[nodiscard]] const transit::DunkGate* omega_dunk_gate(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_transit_scope(snapshot) && type==23?transit::dunk_gate(key,slot):nullptr;
}

[[nodiscard]] charge::Source omega_charge_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    if(!snapshot.omegaSceneAuthority || !snapshot.omegaDialogueArm
        || snapshot.omegaFirstLairGeneration==0 || snapshot.omegaFirstLairGeneration>=0x7FFFFFFFU) { return {}; }
    return charge::source(key,type,slot);
}

[[nodiscard]] bool omega_rescue_scope(const Snapshot& snapshot,std::uint32_t key) noexcept {
    // Crown requests these Scenes long after opening seeding ends. Keep the
    // encounter's cast, marker and Scene updates alive for their run generation.
    return snapshot.omegaSceneAuthority
        && (snapshot.seedAuthoredSensors || snapshot.omegaRescueSourcesGeneration!=0)
        && key==rescue::kRegistry;
}
[[nodiscard]] const rescue::Scene* omega_rescue_scene(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_rescue_scope(snapshot,key) && type==43?rescue::find_scene(slot):nullptr;
}
[[nodiscard]] bool omega_rescue_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_rescue_scope(snapshot,key) && type==1 && rescue::find_source(slot)!=nullptr;
}
[[nodiscard]] const rescueMarkers::Marker* omega_rescue_marker(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t slot) noexcept {
    return omega_rescue_scope(snapshot,key) && type==4?rescueMarkers::find(slot):nullptr;
}
[[nodiscard]] rescue::SceneCommand omega_rescue_command(const Snapshot& snapshot,
    std::uint16_t slot) noexcept {
    const auto* desired=rescue::command(snapshot.omegaRescueScenes,slot);
    return desired!=nullptr?rescueMarkers::project(slot,*desired,snapshot.omegaRescueMarkerReadyMask)
        :rescue::SceneCommand{};
}

[[nodiscard]] crown::Restriction omega_crown_intent(const Snapshot& snapshot) noexcept {
    return crown::intent(snapshot.omegaCrownRestriction, snapshot.omegaCrownRestricted);
}
[[nodiscard]] bool omega_crown_publishes(const Snapshot& snapshot) noexcept {
    return crown::publishes(snapshot.omegaSceneAuthority, omega_crown_intent(snapshot));
}
[[nodiscard]] bool omega_crown_restricted(const Snapshot& snapshot) noexcept {
    return omega_crown_publishes(snapshot) && crown::restricted(omega_crown_intent(snapshot));
}

[[nodiscard]] bool omega_ikora_lattice(const Snapshot& snapshot, std::uint32_t key,
                                      std::uint8_t type, std::uint16_t index) noexcept {
    // The native release arrives after the one-shot bootstrap seed. Its exact
    // gate update must survive with global authored seeding disabled.
    return snapshot.omegaSceneAuthority
        && (snapshot.seedAuthoredSensors || snapshot.omegaIkoraLatticeReleased)
        && lattice::is_gate({key, type, index});
}

[[nodiscard]] bool write_ikora_lattice(bits::Writer& writer, bool released) noexcept {
    // 80F47BA0 binds this gate to GUID A4AF647453C511AB (80F5009E row26). Its own
    // device_position graph selects phase=locked at 1 and phase=unlocked at 0.
    // Reconstruct the missing host edge; preserve native power=1 and lock=0 by
    // leaving those channel revisions at -1. The separate interior gate is untouched.
    const lattice::Channel position = released ? lattice::Channel{0.0F, 2, false}
                                               : lattice::Channel{1.0F, 1, true};
    const auto channel = [&](lattice::Channel value) {
        return writer.write(std::bit_cast<std::uint32_t>(value.value), 32)
            && writer.write(static_cast<std::uint32_t>(value.revision + 0x8000), 16)
            && writer.write(value.snap ? 1U : 0U, 1);
    };
    return channel(position) && channel({1.0F, -1, false}) && channel({0.0F, -1, false});
}

[[nodiscard]] bool omega_first_lair(const Snapshot& snapshot) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm
        && snapshot.omegaFirstLairGeneration!=0 && snapshot.omegaFirstLairGeneration<0x7FFFFFFFU;
}
[[nodiscard]] const lairEnemies::Spawner* omega_lair_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t index) noexcept {
    if(!omega_first_lair(snapshot) || key!=lairEnemies::kRegistry || type!=1) { return nullptr; }
    const auto* source=lairEnemies::find_spawner(index);
    return source!=nullptr && lairEnemies::supported_by_encounter(*source)?source:nullptr;
}
[[nodiscard]] bool omega_lair_anchor(const Snapshot& snapshot,std::uint32_t key,
    std::uint8_t type,std::uint16_t index) noexcept {
    return omega_first_lair(snapshot) && key==lairEnemies::kRegistry && type==2 && index==2;
}
[[nodiscard]] bool omega_crown_enemies(const Snapshot& snapshot) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm
        && snapshot.omegaCrownGeneration!=0 && snapshot.omegaCrownGeneration<0x7FFFFFFFU;
}
[[nodiscard]] const crownEnemies::Spawner* omega_crown_source(const Snapshot& snapshot,
    std::uint32_t key,std::uint8_t type,std::uint16_t index) noexcept {
    if(!omega_crown_enemies(snapshot) || type!=1) { return nullptr; }
    return crownEnemies::find_spawner(index,key);
}
[[nodiscard]] bool omega_crown_anchor(const Snapshot& snapshot,std::uint32_t key,
    std::uint8_t type,std::uint16_t index) noexcept {
    if(!omega_crown_enemies(snapshot) || type!=2) {return false;}
    const auto* registry=crownEnemies::find_registry(key);if(registry==nullptr) {return false;}
    for(const auto& source:registry->spawners) {if(source.memberSlot!=0 && source.memberSlot==index) {return true;}}
    return false;
}
[[nodiscard]] const std::array<std::uint8_t,2>& omega_crown_counts(
    const Snapshot& snapshot,std::uint32_t key,std::uint16_t index) noexcept {
    // Callers first validate the exact catalog source and its bounded native slot.
    if(key==crownEnemies::kHiveRegistry) {return snapshot.omegaHiveLoose[index];}
    if(key==crownEnemies::kVexRegistry) {return snapshot.omegaVexLoose[index];}
    if(key==crownEnemies::kCabalRegistry) {return snapshot.omegaCabalLoose[index];}
    return snapshot.omegaCrownLoose[index];
}
[[nodiscard]] bool omega_crown_anchor_enabled(const Snapshot& snapshot,std::uint32_t key) noexcept {
    if(key==crownEnemies::kHiveRegistry) {return snapshot.omegaHiveAnchor;}
    if(key==crownEnemies::kVexRegistry) {return snapshot.omegaVexAnchor;}
    if(key==crownEnemies::kCabalRegistry) {return snapshot.omegaCabalAnchor;}
    return snapshot.omegaCrownAnchor;
}
[[nodiscard]] bool omega_first_cannon(const Snapshot& snapshot,std::uint32_t key,
    std::uint8_t type,std::uint16_t index) noexcept {
    return omega_first_lair(snapshot) && key==cannon::kRegistry && type==4
        && cannon::source(index)!=nullptr;
}
[[nodiscard]] bool omega_first_cannon_gate(const Snapshot& snapshot,std::uint32_t key,
    std::uint8_t type,std::uint16_t index) noexcept {
    return omega_first_lair(snapshot) && key==cannon::kRegistry && type==23
        && cannon::gate(index)!=nullptr;
}

[[nodiscard]] bool write_first_cannon_gate(bits::Writer& writer,bool active) noexcept {
    // Only device_position receives a positive revision. Keep this FX device's
    // native power=1 and lock=0. Its own script drives model, particles and audio.
    return writer.write(active?0x3F800000U:0U,32) && writer.write(active?0x8001U:0x8000U,16)
        && writer.write(0,1)
        && writer.write(0x3F800000U,32) && writer.write(0x8000U,16) && writer.write(0,1)
        && writer.write(0,32) && writer.write(0x8000U,16) && writer.write(0,1);
}

constexpr std::size_t kOmegaIntroBits = 263;
constexpr std::size_t kOmegaBossBits = 641;
constexpr std::size_t kOmegaBossMemberBits = 42;
[[nodiscard]] bool omega_boss(const Snapshot& snapshot, std::uint32_t key,
                             std::uint8_t type, std::uint16_t index) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm && snapshot.omegaBossGeneration != 0
        && key == presentation::kBossRegistry && type == 1 && index == 0;
}
[[nodiscard]] bool omega_boss_member(const Snapshot& snapshot, std::uint32_t key,
                                    std::uint8_t type, std::uint16_t index) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm && snapshot.omegaBossGeneration != 0
        && key == presentation::kBossRegistry && type == 2 && index == 1;
}
[[nodiscard]] bool omega_intro(const Snapshot& snapshot, std::uint32_t key,
                              std::uint8_t type, std::uint16_t index) noexcept {
    return snapshot.omegaSceneAuthority && snapshot.omegaDialogueArm
        && key == presentation::kIntroRegistry && type == 6 && index == presentation::kIntroSlot;
}
[[nodiscard]] bool omega_ending(const Snapshot& snapshot,std::uint32_t key,
                               std::uint8_t type,std::uint16_t index) noexcept {
    // The ending is requested after opening seeding has finished. Its own
    // retirement/state authority must carry both play and stop revisions.
    return snapshot.omegaSceneAuthority
        && (snapshot.omegaEndingRetire || snapshot.omegaEndingState==ending::kState)
        && key==ending::kRegistry && type==6 && index==ending::kSlot;
}

/** Slot types whose auth body this module fills. Every other block is seed-only. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
constexpr std::uint8_t kSlotTypeLifetime = 17;
constexpr std::uint8_t kSlotTypeActivityScript = 18;
constexpr std::uint8_t kSlotTypeNavigation = 19;
constexpr std::uint8_t kSlotTypeMissionDirector = 35;
constexpr std::uint8_t kSlotTypeConfiguration = 8;
constexpr std::uint8_t kSlotTypePackage = 16;
constexpr std::uint8_t kSlotTypeQueues = 41;
constexpr std::uint8_t kSlotTypeSpawnKeys = 67;
/** scene_ikora_opens_portal's authored Scene controller. */
constexpr std::uint32_t kOmegaOpeningRegistry = 0xD00142CFU;
/** Authored portal transport placed in the Lighthouse mission group. */
constexpr std::uint32_t kOmegaTeleportRegistry = 0xBA5F26EFU;
constexpr std::uint8_t kOmegaSceneSlotType = 43;
constexpr std::uint16_t kOmegaSceneSlotIndex = 1;
// Preserve the established wire generation. The selector comes from the Scene definition.
constexpr std::uint32_t kOmegaSceneGenerationWire = 0x80EC0F96U;
constexpr std::size_t kOmegaSceneBits = 129;
constexpr std::uint32_t kOmegaIkoraPortalEvent = 0xC7ECAA77U;
constexpr std::size_t kOmegaIkoraSourceBits = 641;
constexpr std::uint8_t kOmegaPortalVisualSlotType = 4;
constexpr std::uint16_t kOmegaTeleportSlotIndex = 0;
constexpr std::uint16_t kOmegaPortalVisualFirstIndex = 2;
constexpr std::uint16_t kOmegaPortalVisualLastIndex = 4;
constexpr std::uint16_t kOmegaGateControllerSlotIndex = 16;
/** Legacy disabled _o_gateway_extended_push[0..8] experiment (no placement candidates). */
constexpr std::uint16_t kOmegaGatewayPushFirstIndex = 7;
constexpr std::uint16_t kOmegaGatewayPushLastIndex = 15;
constexpr std::uint8_t kOmegaEngagementSlotType = 70;
constexpr std::uint16_t kOmegaEngagementSlotIndex = 17;
constexpr std::uint8_t kOmegaMonitorSlotType = 30;
constexpr std::uint16_t kOmegaOpeningMonitorSlotIndex = 20;
constexpr std::uint16_t kOmegaEntranceMonitorSlotIndex = 24;
/** Both type-4 definitions use auth schema 0x8080992F. */
constexpr std::size_t kOmegaPortalComponentBits = 253;
/** Schema 0x80804F48: three {float32, biased-int16, bool} link-state records. */
constexpr std::size_t kOmegaPortalGateBits = 147;
/** Ghost pre-roll dialogue (type 53). Schema 0x80804F77 tag-reflection body: a 55-bit root
 * reference then 128 records; record index N selects bank row N, so record 0 = Ghost row 0
 * (AD60F465). Body = 55 + 128*154 = 19,767 bits. */
constexpr std::uint32_t kOmegaDialogueRegistry = 0x82FB58B7U;
constexpr std::uint8_t kOmegaDialogueSlotType = 53;
constexpr std::uint16_t kOmegaDialogueSlotIndex = 2;
constexpr std::size_t kOmegaDialogueRecords = 128;
constexpr std::size_t kOmegaDialogueRecordBits = 65 + 55 + 32 + 2;
/** An eligible row adds one optional u64 time; an omitted time is rejected every tick. */
constexpr std::size_t kOmegaDialogueBits = 55 + kOmegaDialogueRecords * kOmegaDialogueRecordBits;
/** Opening objective tracker (type 68). Schema 0x80804F67: two 55-bit references, three
 * rotating 1,563-bit directive records (stride 0xF8), then a 3-bit ring selector. Record 0
 * active: event C252E306 ("Hunt down and destroy Panoptes"), discriminator 0 (native FNV ->
 * HUD identity 32678D66), lifecycle 0 (active/installable); records 1-2 lifecycle -1 (absent);
 * ring entry 0. Total exactly 4,802 bits. */
constexpr std::uint8_t kOmegaDirectiveSlotType = 68;
constexpr std::uint16_t kOmegaDirectiveSlotIndex = 0;
// Native type37 variable-count authority; independently verified against the original worker.
constexpr std::uint32_t kOmegaForestGeneratorRegistry = 0x2763EC97U;
constexpr std::uint8_t kOmegaForestGeneratorSlotType = 37U;
constexpr std::uint16_t kOmegaForestGeneratorSlotIndex = 1U;
constexpr std::size_t kOmegaForestGeneratorBits = state::activity::coo::native_generator::kActivationBits;
constexpr std::size_t kOmegaDirectiveTargetBits = 55 + 55 + 128 + 1;
constexpr std::size_t kOmegaDirectiveProgressBits = 1 + 5 * 64 + 32;
constexpr std::size_t kOmegaDirectiveRecordBits =
    32 + 32 + 2 + kOmegaDirectiveProgressBits + 128 + 2 + 55 + 3
    + 4 * kOmegaDirectiveTargetBits;
constexpr std::size_t kOmegaDirectiveBits = 55 + 55 + 3 * kOmegaDirectiveRecordBits + 3;
/** Body widths, each checked against the writer after the body is written. */
constexpr std::size_t kParticipationBits = 192;
constexpr std::size_t kParticipationRegionBits = 32;
constexpr std::size_t kLifetimeBits = 520;
constexpr std::size_t kGameplayHashSwitchBits = 32 + 1 + 32 + 32;
constexpr std::uint32_t kForestVexActiveKey = 0x67AF9045U;
constexpr std::uint32_t kGameplayHashClass = 0x80800070U;
constexpr std::uint32_t kForestRaceActiveValue = 0x050C5D2EU;
/** Schema 0x808099C4: one bool, five unsigned 64-bit fields, then one 32-bit scalar. */
constexpr std::size_t kSharedMissionStateBits = 1 + 5 * 64 + 32;
/** Schema 0x808099BF: two bools, two bias-1 two-bit enums, then shared state. */
constexpr std::size_t kMissionDirectorBits = 2 + 2 * 2 + kSharedMissionStateBits;
/** Schema 0x80809919: shared state, one bool, then one biased signed 32-bit scalar. */
constexpr std::size_t kActivityScriptBits = kSharedMissionStateBits + 1 + 32;
/**
 * The mission director was recovered independently before enabling the activity script. The
 * runtime descriptor proves their shared tail ends in 32 bits, not the provisional 64; that prior
 * extra word shifted the proven lifetime and participation objects out of alignment.
 */
constexpr bool kInitializeActivityScript = true;
constexpr bool kInitializeMissionDirector = true;
constexpr std::size_t kConfigurationBits = 35;
constexpr std::size_t kPackageBits = 7;
constexpr std::size_t kQueueBits = 12;
constexpr std::size_t kSpawnKeyBits = 32 * 32 + 1 + 32;

/** Signed fields in these bodies carry a -2^31 bias, so this wire value stores zero. */
constexpr std::uint32_t kSignedZero = 0x80000000;
/** The same bias wraps at the top of the field, so this wire value stores -1. */
constexpr std::uint32_t kSignedMinusOne = 0x7FFFFFFF;
/** The region index rides the same bias, so its wire value is the bias plus the index. */
constexpr std::uint32_t kRegionBias = 0x80000000;
/** Message 52's team-state byte 1, where bit 1 is `awaiting_client_sync`. */
constexpr std::uint32_t kAwaitingClientSync = 2;
/** Neutral runtime-i32 override that forces the type-17 waiting selector to zero. */
constexpr std::uint32_t kWaitingSwitchKey = 0xB3C1251B;
constexpr std::uint32_t kWaitingSwitchClass = 0x80800007;
/** Type 17 carries 3 spawn overrides. Wire zero stores index -1 and disables one. */
constexpr std::size_t kSpawnOverrideCount = 3;
constexpr std::uint8_t kSpawnOverrideIndexWidth = 10;
constexpr std::uint32_t kSpawnOverrideIndexBias = 1;
/** Type 67 maps the 32 spawn-key ordinals to themselves, matching its constructor. */
constexpr std::size_t kSpawnKeyCount = 32;

/**
 * Writes the participation body, which binds the player and latches the region. Zero-fill is not
 * safe here. Every biased field must carry its bias, or a stored zero decodes to the smallest
 * signed value.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_participation(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // An optional field's value follows its presence bit, so sending +0 shifts everything below.
    bool encoded = writer.write(snapshot.hasRegion ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasRegion) {
        encoded = writer.write(kRegionBias + snapshot.region, kParticipationRegionBits);
    }
    // The participation record is this body's head, so struct +8 and +10 are record +8 and +10.
    // Record +8 is step 36 task 9's own term and +10 is the spawn gate's.
    encoded = encoded && writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(1, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(0, kPresenceWidth) && writer.write(1, 3) && writer.write(1, 2)
           && writer.write(0, 3) && writer.write(0, 32) && writer.write(1, 5)
           && writer.write(0, kPresenceWidth) && writer.write(0, 3)
           && writer.write(1, kPresenceWidth) && writer.write(snapshot.playerKey, 64)
           && writer.write(0, 5) && writer.write(3, 6);
    // 80804F30+48 ->808094DD+0C ->808094E1: authored predicate names.
    const auto names=native::player_predicates::compose(snapshot.playerPredicates,snapshot.omegaPortalPlayerHash,
        state::activity::omega_portal_entry::kRequiredPlayerHash);
    encoded=encoded && names && native::player_predicates::write(writer,*names);
    encoded = encoded && writer.write(0, 6)
           && writer.write(1, kPresenceWidth)
           && writer.write(snapshot.awaitClientSync ? kAwaitingClientSync : 0U, 4)
           && writer.write(0, 1) && writer.write(snapshot.nativeRespawnRestricted ? 1U : 0U, 1);
    // Same optional half-float revive delay used by 1AU-UnEx. The three-second wipe
    // precedes this thirty-second delay; ordinary participation stays byte-identical.
    if(snapshot.nativeRespawnRestricted) encoded=encoded && writer.write(0x4F80U,16);
    return encoded && writer.write(0,1) && writer.write(0,kPresenceWidth) && writer.write(128,8)
           && writer.write(kSignedZero,32);
}

/**
 * Writes the lifetime body, which is the activity state the roster reports.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_lifetime(bits::Writer& writer, const Snapshot& snapshot,
                                  bool crownRestricted) noexcept {
    const bool completed=!snapshot.nightfallFailed && snapshot.missionCompletion.valid();
    bool encoded = writer.write((snapshot.nightfallFailed?8U:completed?6U:std::uint32_t{snapshot.lifetime}) + 1, 4) && writer.write(completed?2U:1U, 3)
                   && writer.write(0, kPresenceWidth) && writer.write(kSignedZero, 32)
                   && writer.write(0, 32)
                   && writer.write(kSignedZero + (crownRestricted ? crown::kLairScenarioOrdinal : 0U), 32)
                   && writer.write(state::activity::beyond_infinity::forest::selected(snapshot.beyond_infinity) ? 3U : snapshot.omegaForestVexEncounters ? 2U : 1U, 6)
                   && writer.write(kWaitingSwitchKey, 32) && writer.write(1, kPresenceWidth)
                   && writer.write(kWaitingSwitchClass, 32) && writer.write(kSignedZero, 32);
    if (state::activity::beyond_infinity::forest::selected(snapshot.beyond_infinity)) {
        encoded = encoded && state::activity::beyond_infinity::forest::write(writer,snapshot.beyond_infinity.forestPass);
    } else if (encoded && snapshot.omegaForestVexEncounters) {
        // DBC710 merges this type-17 gameplay switch into the native global
        // store. 80F44B2B then selects the authored Vex population; native
        // encounter creation, AI, remaining counts and gate unlock own the rest.
        // 80800070 is a raw hash, not a signed-i32 value with INT_MIN bias.
        encoded = writer.write(kForestVexActiveKey,32) && writer.write(1,1)
            && writer.write(kGameplayHashClass,32) && writer.write(kForestRaceActiveValue,32);
    }
    encoded = encoded && writer.write(kSignedZero,32);
    for (std::size_t index = 0; encoded && index < kSpawnOverrideCount; ++index) {
        const std::uint32_t slice =
            snapshot.hasSpawnOverride ? snapshot.spawnSliceSet + kSpawnOverrideIndexBias : 0U;
        const std::uint32_t hash =
            snapshot.hasSpawnOverride ? snapshot.spawnSetHash : kAbsentSpawnSetHash;
        encoded = writer.write(slice, kSpawnOverrideIndexWidth) && writer.write(hash, 32);
    }
    // Struct `+1256` is the out-of-bounds `activity_quarantine` selector. The reader arms the
    // quarantine at or below 0x3F unsigned, so minus one leaves it clear and teleports nobody.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, 32)
           && writer.write(kSignedMinusOne, 32) && writer.write(0, 32)
           && writer.write(kSlotTypeBias, kSlotTypeWidth)
           && writer.write(kSlotIndexBias, kSlotIndexWidth) && writer.write(0, 32)
           && writer.write(0, 3);
}

/**
 * Writes the spawn-key body, which maps the 32 ordinals to themselves.
 * @param writer Body writer.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_spawn_keys(bits::Writer& writer) noexcept {
    bool encoded = true;
    for (std::size_t index = 0; encoded && index < kSpawnKeyCount; ++index) {
        encoded = writer.write(kSignedZero + index, 32);
    }
    return encoded && writer.write(0, kPresenceWidth) && writer.write(kSignedMinusOne, 32);
}

/** Writes schema 0x808099C4's constructed state shared by slots 18 and 35. */
[[nodiscard]] bool
write_shared_mission_state(bits::Writer& writer, bool active) noexcept {
    return writer.write(active ? 1U : 0U, kPresenceWidth)
           && pad_bits(writer, 5 * 64 + 32);
}

/** Writes the neutral mission-director state that makes its authority datum persistent. */
[[nodiscard]] bool write_mission_director(bits::Writer& writer,
                                          const Snapshot& snapshot) noexcept {
    // The two enum fields carry bias 1. Wire zero would decode below their declared range.
    return writer.write(snapshot.missionDirectorVariant & 1U, kPresenceWidth)
           && writer.write((snapshot.missionDirectorVariant >> 1U) & 1U, kPresenceWidth)
           && writer.write(1, 2) && writer.write(1, 2)
           && write_shared_mission_state(writer, snapshot.missionDirectorActive);
}

/** Writes the neutral activity-script state that creates its authority datum. */
[[nodiscard]] bool write_activity_script(bits::Writer& writer,
                                         const Snapshot& snapshot) noexcept {
    // The shared-state head is set in the client storage object. That materializes the datum but
    // does not supply the activity-host executor that advances the authored mission graph.
    return writer.write(1, kPresenceWidth) && pad_bits(writer, 5 * 64 + 32)
           && writer.write(snapshot.activityScriptFlag ? 1U : 0U, kPresenceWidth)
           && writer.write(kSignedZero
                               + static_cast<std::uint32_t>(snapshot.activityScriptState),
                           32);
}

[[nodiscard]] bool omega_opening_scene(std::uint32_t key,
                                       std::uint8_t slotType,
                                       std::uint16_t slotIndex) noexcept {
    return key == kOmegaOpeningRegistry && slotType == kOmegaSceneSlotType
           && slotIndex == kOmegaSceneSlotIndex;
}

[[nodiscard]] bool omega_ikora_source(std::uint32_t key, std::uint8_t slotType,
                                    std::uint16_t slotIndex) noexcept {
    return key == kOmegaOpeningRegistry && slotType == 1 && slotIndex == 0;
}

[[nodiscard]] bool active_omega_portal_component(const Snapshot& snapshot,
                                                 std::uint32_t key,
                                                 std::uint8_t slotType,
                                                 std::uint16_t slotIndex) noexcept {
    if (snapshot.omegaSceneAuthority && snapshot.omegaPortalEntry
        && state::activity::omega_portal_entry::is_carrier(key,slotType,slotIndex)) {
        return true;
    }
    if (!snapshot.omegaPortalMutation
        || (snapshot.omegaOpeningStage != kOmegaOpeningStageTriggered
         && snapshot.omegaOpeningStage != kOmegaOpeningStagePortal)
        || slotType != kOmegaPortalVisualSlotType) {
        return false;
    }
    const bool teleport = key == kOmegaTeleportRegistry
                          && slotIndex == kOmegaTeleportSlotIndex;
    const bool visual = key == kOmegaOpeningRegistry
                        && slotIndex >= kOmegaPortalVisualFirstIndex
                        && slotIndex <= kOmegaPortalVisualLastIndex;
    return teleport || visual;
}

/**
 * Retired hypothesis, kept behind kOmegaGateAuthorityBodyReady=false: these nine type-4
 * entries have no authored placement candidates. They do not provide the near-Ikora lattice;
 * the exact dg_if_gate position policy above owns that model's locked presentation.
 */
[[nodiscard]] bool active_omega_gateway_push(const Snapshot& snapshot,
                                             std::uint32_t key,
                                             std::uint8_t slotType,
                                             std::uint16_t slotIndex) noexcept {
    return snapshot.omegaGateAuthority && kOmegaGateAuthorityBodyReady
           && snapshot.omegaDialogueArm && key == kOmegaOpeningRegistry
           && slotType == kOmegaPortalVisualSlotType
           && slotIndex >= kOmegaGatewayPushFirstIndex
           && slotIndex <= kOmegaGatewayPushLastIndex;
}

[[nodiscard]] bool active_omega_portal_gate(const Snapshot& snapshot,
                                            std::uint32_t key,
                                            std::uint8_t slotType,
                                            std::uint16_t slotIndex) noexcept {
    // Armed on the in-world latch (like the dialogue/directive) rather than the synthetic
    // Triggered/Portal stages, which normal play parks before. The gate_authority experiment
    // remains the operator's switch.
    if (!snapshot.omegaGateAuthority || !kOmegaGateAuthorityBodyReady
        || !snapshot.omegaDialogueArm) {
        return false;
    }
    const bool interior = key == kOmegaTeleportRegistry && slotType == 23 && slotIndex == 1;
    const bool missionController = key == kOmegaOpeningRegistry && slotType == 23
                                   && slotIndex == kOmegaGateControllerSlotIndex;
    return interior || missionController;
}

/**
 * Complete 8080626B: stable generation, non-stop state, one authorized Ikora source,
 * source-list revision 1. Native B3FE80 requires this exact source reference before the
 * authored selector can request and bind its actor. Retaining C7 after the approach policy
 * fires advances the orb animation; B41330 only delivers a newly present event. Neither
 * generation changes, so repeated publication and backtracking cannot restart the Scene.
 */
[[nodiscard]] bool write_omega_scene(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    const bool requested = snapshot.omegaIkoraPortalRequested;
    return writer.write(kOmegaSceneGenerationWire, 32) && writer.write(0, kPresenceWidth)
           && writer.write(1, 4) && writer.write(kOmegaOpeningRegistry, 32)
           && writer.write(2, 7) && writer.write(32768, 16)
           && writer.write(1, 31) && writer.write(requested ? 1U : 0U, 6)
           && (!requested || writer.write(kOmegaIkoraPortalEvent, 32));
}

/**
 * Activates one authored type-4 portal component without inventing an actor reference. This body
 * is shared by the Lighthouse teleporter and the three portal-beam components.
 * The canonical absent reference deliberately selects the component's authored local transform.
 */
[[nodiscard]] bool write_active_omega_portal_component(bits::Writer& writer) noexcept {
    return native::placement::write_active(writer);
}

/**
 * Legacy disabled 80804F48 experiment. Its tuple/power interpretation was an unverified
 * hypothesis. For the actual near-Ikora gate position1 is locked, position0 is unlocked,
 * and power/lock retain native defaults; write_ikora_lattice is the scoped implementation.
 */
[[nodiscard]] bool write_active_omega_portal_gate(bits::Writer& writer) noexcept {
    // Historical experiment: position 0.0, revision 1, snap.
    if (!writer.write(0U, 32U) || !writer.write(0x8001U, 16U) || !writer.write(1U, 1U)) {
        return false;
    }
    // Historical experiment: power 1.0, revision 1, snap.
    if (!writer.write(0x3F800000U, 32U) || !writer.write(0x8001U, 16U)
        || !writer.write(1U, 1U)) {
        return false;
    }
    // Channel 2: 0.0, revision 1, snap.
    return writer.write(0U, 32U) && writer.write(0x8001U, 16U) && writer.write(1U, 1U);
}

[[nodiscard]] bool omega_dialogue(std::uint32_t key, std::uint8_t slotType,
                                  std::uint16_t slotIndex) noexcept {
    return key == kOmegaDialogueRegistry && slotType == kOmegaDialogueSlotType
           && slotIndex == kOmegaDialogueSlotIndex;
}

/**
 * One 0x80809C42 reference in its authored-absent form {0x811C9DC5, type -1, slot -1}. The
 * scan's reference gate (+0x4E2990) treats type/slot of -1 as "no target" and dispatches
 * directly; an all-zero reference reads as a SET target, fails to resolve, and skips the
 * dispatch. Wire: u32 bias 0; i8 bias 1 (stored 0 = -1); i16 bias 0x8000 (stored 0x7FFF = -1).
 */
[[nodiscard]] bool write_dialogue_ref_absent(bits::Writer& writer) noexcept {
    return writer.write(kAbsentSpawnSetHash, 32) && writer.write(0, 7)
           && writer.write(0x7FFF, 16);
}

/** 80807EC9 defaults from 80F47B70, with one Scene-requested Ikora. Mode 1 suppresses
 * autonomous population; the Scene's native source node owns the request. Absent rule refs
 * select the registered inline authored placement, and all five template overrides remain 0. */
[[nodiscard]] bool write_omega_ikora_source(bits::Writer& writer) noexcept {
    const auto absent = [&writer]() noexcept {
        return writer.write(1, 1) && write_dialogue_ref_absent(writer);
    };
    return absent() && absent()
        && writer.write(1, 1) && writer.write(0, 3)
        && writer.write(1, 1) && writer.write(1, 4) && writer.write(kSignedZero + 1U, 32)
        && writer.write(1, 1) && writer.write(0, 4)
        && writer.write(1, 1) && writer.write(1, 3) && writer.write(1, 2)
        && writer.write(1, 3) && writer.write(1, 2) && writer.write(1, 3)
        && writer.write(1, 1) && writer.write(1, 31)
        && writer.write(1, 1) && writer.write(0, 32)
        && writer.write(1, 1) && writer.write(0x811C9DC5U, 32)
        && absent() && absent() && absent() && absent()
        && writer.write(1, 1) && writer.write(0, 31)
        && writer.write(1, 1) && writer.write(0, 31)
        && writer.write(1, 1) && writer.write(1, 6)
        && writer.write(1, 1) && writer.write(1, 5)
        && writer.write(1, 1) && writer.write(0, 31)
        && writer.write(1, 2) && writer.write(2, 3)
        && writer.write(1, 1) && writer.write(0x811C9DC5U, 32);
}

/** Complete 80807EC9 authority for sq_boss. Its member controller creates the boss;
 * the counted 80809491 arrays serialize only their declared elements, not all eight slots.
 * The authored 95FB2E01/66/57 rule supplies placement; no world coordinate is synthesized. */
[[nodiscard]] bool write_omega_boss(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // The member owns Panoptes. A loose request would create a duplicate actor.
    return state::activity::omega_combatant_authority::write_source(writer,
        {presentation::kBossRegistry,snapshot.omegaBossGeneration,
         presentation::kBossSpawnRule,0});
}

/** 80807DA1: stable generation, default retirement/location modes, enabled.
 * Optional nested fields retain their native registration defaults. AB6340
 * creates from the configured parent; native reconciliation binds the resulting
 * type-2 origin back to this member. Never convert a live loose-spawn run. */
[[nodiscard]] bool write_omega_boss_member(bits::Writer& writer,const Snapshot& snapshot) noexcept {
    return state::activity::omega::boss_authority::write_member(writer,true,
        snapshot.omegaBossGeneration,snapshot.omegaArchiveArm,true,snapshot.omegaArchiveIntro,true);
}

/** 80804F08, empty audience and dependency collections. Both nested arrays have
 * param_18=1 (counted), so count zero emits no type-35 elements. No fake actor IDs.
 * The -1 player selector carries bias 3, while the absent activity ref uses its
 * own type/slot biases. Native +10697B0 consumes revision/play and owns playback. */
[[nodiscard]] bool write_omega_intro(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    return writer.write(0,64) && writer.write(0,64)
        && writer.write(snapshot.omegaIntroRevision,32)
        && writer.write(snapshot.omegaIntroPlay?1U:0U,1) && writer.write(0,1)
        && write_dialogue_ref_absent(writer) && writer.write(2,6)
        && writer.write(0,5) && writer.write(0,3) && writer.write(0,32);
}

/** The bookend uses the same authored generic-cinematic schema as the reveal.
 * Its separate controller revision owns the complete native cast and movie. */
[[nodiscard]] bool write_omega_ending(bits::Writer& writer,const Snapshot& snapshot) noexcept {
    const bool bookend=snapshot.omegaEndingState==ending::kState;
    return writer.write(0,64) && writer.write(0,64)
        && writer.write(bookend?snapshot.omegaEndingRevision:0U,32)
        && writer.write(bookend && snapshot.omegaEndingPlay?1U:0U,1) && writer.write(0,1)
        && write_dialogue_ref_absent(writer) && writer.write(2,6)
        && writer.write(0,5) && writer.write(0,3) && writer.write(0,32);
}

/**
 * Ghost pre-roll dialogue body. Emits the tag-reflection 0x80804F77 struct: a zero root reference
 * then 128 records. Record 0 is armed (generation 1 vs the memset-0 processed array, mode 2) so the
 * client scan (+0x100A180) dispatches bank row 0 = AD60F465; the rest carry generation 0 and are
 * skipped. Non-selecting fields are written as their bias so they decode to zero; the per-record
 * optional u64 time is omitted. */
[[nodiscard]] bool omega_forest_generator(const Snapshot& snapshot, std::uint32_t key,
                                          std::uint8_t slotType, std::uint16_t slotIndex) noexcept {
    return snapshot.omegaForestGenerator && snapshot.omegaForestSeed!=0
           && key == kOmegaForestGeneratorRegistry
           && slotType == kOmegaForestGeneratorSlotType
           && slotIndex == kOmegaForestGeneratorSlotIndex;
}

[[nodiscard]] bool write_active_omega_dialogue(bits::Writer& writer,
                                               const Snapshot& snapshot) noexcept {
    return state::activity::coo::native_presentation::dialogue(writer,
        snapshot.omegaDialogueGenerations,snapshot.omegaActiveDialogueRow);
}

[[nodiscard]] bool omega_directive(std::uint32_t key, std::uint8_t slotType,
                                   std::uint16_t slotIndex) noexcept {
    return key == kOmegaDialogueRegistry && slotType == kOmegaDirectiveSlotType
           && slotIndex == kOmegaDirectiveSlotIndex;
}

/** One 1,563-bit directive record; biased numerics are written to decode as zero. */
[[nodiscard]] bool write_directive_record(bits::Writer& writer,
                                          bool active,
                                          std::uint32_t activeEvent) noexcept {
    // Event key, then the biased discriminator (decoded 0 -> HUD identity 32678D66).
    if (!writer.write(active ? activeEvent : kAbsentSpawnSetHash, 32)
        || !writer.write(0x80000000ULL, 32)) {
        return false;
    }
    // Lifecycle (2 bits, bias 1): 0 = active/installable, -1 = absent entry.
    if (!writer.write(active ? 1U : 0U, 2)) {
        return false;
    }
    // 0x808099C4 time/progress: the u64 time values use the engine's -1 "no value" form; a
    // zero end-time renders as a 0:00 HUD countdown that retail's opening does not show.
    if (!writer.write(0, kPresenceWidth)) {
        return false;
    }
    for (std::size_t value = 0; value < 5; ++value) {
        if (!writer.write(0xFFFFFFFFFFFFFFFFULL, 64)) {
            return false;
        }
    }
    if (!writer.write(0, 32)) {
        return false;
    }
    // Four biased i32 values, decoded -1 (the engine's no-value form).
    for (std::size_t value = 0; value < 4; ++value) {
        if (!writer.write(0x7FFFFFFFULL, 32)) {
            return false;
        }
    }
    // enum (2 bits, bias 1) decoded 0; absent reference; enum (3 bits, bias 1) decoded 0.
    if (!writer.write(1, 2) || !write_dialogue_ref_absent(writer) || !writer.write(1, 3)) {
        return false;
    }
    for (std::size_t target = 0; target < 4; ++target) {
        if (!write_dialogue_ref_absent(writer) || !write_dialogue_ref_absent(writer)) {
            return false;
        }
        for (std::size_t value = 0; value < 4; ++value) {
            if (!writer.write(0, 32)) {
                return false;
            }
        }
        if (!writer.write(0, kPresenceWidth)) {
            return false;
        }
    }
    return true;
}

/** The opening objective body: record 0 active on C252E306, ring entry 0. */
[[nodiscard]] bool write_active_omega_directive(bits::Writer& writer,
                                                const Snapshot& snapshot) noexcept {
    const std::uint32_t activeEvent = snapshot.omegaObjectiveEvent;
    if (!write_dialogue_ref_absent(writer) || !write_dialogue_ref_absent(writer)) {
        return false;
    }
    for (std::size_t record = 0; record < 3; ++record) {
        if (!write_directive_record(writer, record == 0, activeEvent)) {
            return false;
        }
    }
    // Ring selector (3 bits, bias 1): decoded 0 selects the active entry.
    return writer.write(1, 3);
}

/** One mission component's exact neutral auth-body shape. */
struct MissionBody final {
    std::uint32_t registryKey;
    std::uint16_t slotIndex;
    std::uint8_t slotType;
    std::uint32_t authClass;
    std::uint16_t width;
    const std::uint16_t* ones;
    std::uint16_t oneCount;
};

// Earlier schema tooling started the codec descriptor array 0x18 bytes too early. Every body that
// was generated from that view was structurally short or semantically biased (including Omega's
// type-30 monitors and type-70 engagement state). Keep the registry empty until each body is
// recovered from the corrected reflection walker; registration/sense delivery does not require an
// invented auth body.
constexpr std::array<MissionBody, 0> kMissionBodies{};

[[nodiscard]] const MissionBody* find_mission_body(std::uint32_t key,
                                                   std::uint8_t slotType,
                                                   std::uint16_t slotIndex) noexcept {
    for (const MissionBody& body : kMissionBodies) {
        if (body.registryKey == key && body.slotType == slotType && body.slotIndex == slotIndex) {
            return &body;
        }
    }
    return nullptr;
}

[[nodiscard]] bool active_omega_boundary_body(const Snapshot& snapshot,
                                              std::uint32_t key,
                                              std::uint8_t slotType,
                                              std::uint16_t slotIndex) noexcept {
    return (snapshot.omegaOpeningStage == kOmegaOpeningStageTriggered
            || snapshot.omegaOpeningStage == kOmegaOpeningStagePortal)
           && find_mission_body(key, slotType, slotIndex) != nullptr;
}

[[nodiscard]] bool write_mission_body(bits::Writer& writer, const MissionBody& body) noexcept {
    std::size_t next = 0;
    for (std::uint16_t bit = 0; bit < body.width; ++bit) {
        std::uint32_t value = 0;
        if (next < body.oneCount && body.ones[next] == bit) {
            value = 1;
            ++next;
        }
        if (!writer.write(value, 1)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Reports how many bits of auth body one slot carries. */
std::size_t
auth_body_bits(const Snapshot& snapshot,
               std::uint32_t key,
               std::uint8_t slotType,
               std::uint16_t slotIndex,
               bool carriesPlayerKey) noexcept {
    if(const auto* request=native::engagement::find(snapshot.engagements,key,slotType,slotIndex)) return native::engagement::body_bits(*request);
    if(const auto* request=native::world_device::find(snapshot.devices,key,slotType,slotIndex)) return native::world_device::valid(request->state)?native::world_device::kPayloadBits:0;
    if(const auto* request=native::forest_generator::find(snapshot.generators,key,slotType,slotIndex)) return native::forest_generator::body_bits(request->state);
    if(native::npc_animation::find(snapshot.animations,key,slotType,slotIndex)) return native::npc_animation::kBodyBits;
    if(const auto* request=native::dialogue::find(snapshot.dialogues,key,slotType,slotIndex)) return native::dialogue::body_bits(*request);
    if(const auto* request=native::world_sequence::find(snapshot.sequences,key,slotType,slotIndex)) return native::world_sequence::valid(*request)?native::world_sequence::kPayloadBits:0;
    if(const auto* request=native::event_participant::find(snapshot.eventParticipants,key,slotType,slotIndex)) return native::event_participant::body_bits(*request);
    if(const auto* request=native::music::find(snapshot.music,key,slotType,slotIndex)) return native::music::valid(*request)?native::music::kBits:0;
    if(const auto* request=native::placement::find(snapshot.placements,key,slotType,slotIndex)) return native::placement::body_bits(*request);
    if(const auto* request=native::population::find(snapshot.populations,key,slotType,slotIndex))
        return native::population::bits(*request);
    if(const auto* request=native::population::find_member(snapshot.populations,key,slotType,slotIndex))
        return native::population::member_bits(request->source.retireOwned);
    if (!snapshot.archiveOmega) { return legacy_auth_body_bits(snapshot, key, slotType, slotIndex, carriesPlayerKey); }

    if(const auto* status=omega_eye_status_source(snapshot,key,slotType,slotIndex)) {
        return eyeStatus::active(*status,snapshot.omegaCrownCycle,snapshot.omegaCrownEyeStatusActive)
            ?eyeStatus::kActiveBits:eyeStatus::kInactiveBits;
    }
    if(const auto* scene=omega_rescue_scene(snapshot,key,slotType,slotIndex)) {
        return rescue::scene_bits(*scene,omega_rescue_command(snapshot,slotIndex));
    }
    if(omega_rescue_source(snapshot,key,slotType,slotIndex)) { return rescue::kSceneSourceBits; }
    if(omega_rescue_marker(snapshot,key,slotType,slotIndex)!=nullptr) { return cannon::kAuthorityBits; }
    if(const auto source=omega_charge_source(snapshot,key,slotType,slotIndex);source.cycle!=nullptr) {
        return charge::authority_bits(source,{snapshot.omegaFirstLairGeneration,snapshot.omegaCrownCycle,
            snapshot.omegaCrownChargeEnabled,snapshot.omegaCrownChargeDunked,snapshot.omegaCrownTransitBridge});
    }
    if(omega_transit_source(snapshot,key,slotType,slotIndex)!=nullptr) { return cannon::kAuthorityBits; }
    if(omega_transit_gate(snapshot,key,slotType,slotIndex)!=nullptr
        || omega_dunk_gate(snapshot,key,slotType,slotIndex)!=nullptr) { return transit::kGateBits; }
    if(omega_lair_source(snapshot,key,slotType,slotIndex)!=nullptr) {
        return state::activity::omega_combatant_authority::kSourceBits;
    }
    if(omega_lair_anchor(snapshot,key,slotType,slotIndex)) { return kOmegaBossMemberBits; }
    if(const auto* source=omega_crown_source(snapshot,key,slotType,slotIndex)) {
        return source->categories==2?state::activity::omega_combatant_authority::kTwoCategorySourceBits:kOmegaBossBits;
    }
    if(omega_crown_anchor(snapshot,key,slotType,slotIndex)) { return kOmegaBossMemberBits; }
    if(omega_first_cannon(snapshot,key,slotType,slotIndex)) { return cannon::kAuthorityBits; }
    if(omega_first_cannon_gate(snapshot,key,slotType,slotIndex)) { return kOmegaPortalGateBits; }
    if (omega_ikora_lattice(snapshot,key,slotType,slotIndex)) { return kOmegaPortalGateBits; }
    if (omega_boss(snapshot,key,slotType,slotIndex)) { return kOmegaBossBits; }
    if (omega_boss_member(snapshot,key,slotType,slotIndex)) {
        return state::activity::omega::boss_authority::member_bits(true,
            snapshot.omegaBossGeneration,snapshot.omegaArchiveArm,true,snapshot.omegaArchiveIntro,true);
    }
    if (omega_intro(snapshot,key,slotType,slotIndex)) { return kOmegaIntroBits; }
    if (omega_ending(snapshot,key,slotType,slotIndex)) { return kOmegaIntroBits; }
    if (active_omega_portal_component(snapshot, key, slotType, slotIndex)) {
        return kOmegaPortalComponentBits;
    }
    if (active_omega_portal_gate(snapshot, key, slotType, slotIndex)) {
        return kOmegaPortalGateBits;
    }
    if (active_omega_gateway_push(snapshot, key, slotType, slotIndex)) {
        return kOmegaPortalComponentBits;
    }
    if (active_omega_boundary_body(snapshot, key, slotType, slotIndex)) {
        return find_mission_body(key, slotType, slotIndex)->width;
    }
    if (snapshot.omegaSceneAuthority && kOmegaSceneAuthorityBodyReady) {
        if (snapshot.seedAuthoredSensors && omega_ikora_source(key, slotType, slotIndex)) {
            return kOmegaIkoraSourceBits;
        }
        if ((snapshot.seedAuthoredSensors || snapshot.omegaIkoraPortalRequested)
            && omega_opening_scene(key, slotType, slotIndex)) {
            return kOmegaSceneBits + (snapshot.omegaIkoraPortalRequested ? 32U : 0U);
        }
    }
    if (omega_forest_generator(snapshot, key, slotType, slotIndex)) {
        return kOmegaForestGeneratorBits;
    }
    if (snapshot.omegaSceneAuthority && snapshot.omegaMusicPresent
        && music::is_sensor(key, slotType, slotIndex)) {
        return music::kAuthorityBits;
    }
    if (snapshot.omegaSceneAuthority && kOmegaDialogueBodyReady
        && snapshot.omegaDialogueArm && omega_dialogue(key, slotType, slotIndex)) {
        return state::activity::coo::native_presentation::dialogue_bits(
            snapshot.omegaDialogueGenerations,snapshot.omegaActiveDialogueRow);
    }
    if (snapshot.omegaSceneAuthority && kOmegaDirectiveBodyReady
        && snapshot.omegaDialogueArm && omega_directive(key, slotType, slotIndex)) {
        return kOmegaDirectiveBits;
    }
    if(slotType==13 && !native::player_predicates::compose(snapshot.playerPredicates,snapshot.omegaPortalPlayerHash,
        state::activity::omega_portal_entry::kRequiredPlayerHash))return 0;
    if (slotType == kSlotTypeParticipation) {
        return carriesPlayerKey
                   ? kParticipationBits + (snapshot.hasRegion ? kParticipationRegionBits : 0)
                         + (snapshot.nativeRespawnRestricted ? 16U : 0U)
                         + 32U*native::player_predicates::compose(snapshot.playerPredicates,snapshot.omegaPortalPlayerHash,
                             state::activity::omega_portal_entry::kRequiredPlayerHash).value().count
                   : 0;
    }
    if (slotType == kSlotTypeLifetime) {
        return kLifetimeBits + (state::activity::beyond_infinity::forest::selected(snapshot.beyond_infinity)
            ? 2*state::activity::beyond_infinity::forest::kSwitchBits
            : snapshot.omegaForestVexEncounters ? kGameplayHashSwitchBits : 0);
    }
    if (slotType == kSlotTypeActivityScript && kInitializeActivityScript
        && (snapshot.initializeMissionAuthorityRuntime
            || snapshot.publishOmegaOpeningTransition)) {
        return kActivityScriptBits;
    }
    if (slotType == kSlotTypeMissionDirector && kInitializeMissionDirector
        && (snapshot.initializeMissionAuthorityRuntime
            || snapshot.publishOmegaOpeningTransition
            || (omega_crown_publishes(snapshot) && crown::is_director(key,slotType,slotIndex)))) {
        return kMissionDirectorBits;
    }
    if (slotType == kSlotTypeConfiguration) {
        return kConfigurationBits;
    }
    if (slotType == kSlotTypePackage) {
        return kPackageBits;
    }
    if (slotType == kSlotTypeQueues) {
        return kQueueBits;
    }
    if (slotType == kSlotTypeSpawnKeys) {
        return kSpawnKeyBits;
    }
    if (snapshot.seedAuthoredSensors && key == 0x30A025E8U) {
        const MissionBody* const body = find_mission_body(key, slotType, slotIndex);
        if (body != nullptr) {
            return body->width;
        }
    }
    return 0;
}

/** Writes one slot's auth body. */
bool write_auth_body(bits::Writer& writer,
                     const Snapshot& snapshot,
                     std::uint32_t key,
                     std::uint8_t slotType,
                     std::uint16_t slotIndex,
                     bool carriesPlayerKey) noexcept {
    if(const auto* request=native::engagement::find(snapshot.engagements,key,slotType,slotIndex)) return native::engagement::write(writer,*request);
    if(const auto* request=native::world_device::find(snapshot.devices,key,slotType,slotIndex)) return native::world_device::write_payload(writer,request->state);
    if(const auto* request=native::forest_generator::find(snapshot.generators,key,slotType,slotIndex)) return native::forest_generator::write_payload(writer,request->state);
    if(const auto* request=native::npc_animation::find(snapshot.animations,key,slotType,slotIndex))
        return native::npc_animation::write(writer,request->control);
    if(const auto* request=native::dialogue::find(snapshot.dialogues,key,slotType,slotIndex)) return native::dialogue::write(writer,*request);
    if(const auto* request=native::world_sequence::find(snapshot.sequences,key,slotType,slotIndex)) return native::world_sequence::write(writer,*request);
    if(const auto* request=native::event_participant::find(snapshot.eventParticipants,key,slotType,slotIndex)) return native::event_participant::write(writer,*request);
    if(const auto* request=native::music::find(snapshot.music,key,slotType,slotIndex)) return native::music::write(writer,*request);
    if(const auto* request=native::placement::find(snapshot.placements,key,slotType,slotIndex)) return native::placement::write(writer,*request);
    if(const auto* request=native::population::find(snapshot.populations,key,slotType,slotIndex))
        return native::combatant_source::write_source(writer,request->source);
    if(const auto* request=native::population::find_member(snapshot.populations,key,slotType,slotIndex))
        return native::population::write_member(writer,request->source.generation,
            request->source.retireOwned);
    if (!snapshot.archiveOmega) { return legacy_write_auth_body(writer, snapshot, key, slotType, slotIndex, carriesPlayerKey); }

    const std::size_t start = writer.bit_count();
    const std::size_t expected =
        auth_body_bits(snapshot, key, slotType, slotIndex, carriesPlayerKey);
    bool encoded = true;
    if(const auto* status=omega_eye_status_source(snapshot,key,slotType,slotIndex)) {
        encoded=eyeStatus::write_authority(writer,
            eyeStatus::active(*status,snapshot.omegaCrownCycle,snapshot.omegaCrownEyeStatusActive));
    } else if(omega_rescue_scene(snapshot,key,slotType,slotIndex)!=nullptr) {
        encoded=rescue::write_scene(writer,slotIndex,omega_rescue_command(snapshot,slotIndex));
    } else if(omega_rescue_source(snapshot,key,slotType,slotIndex)) {
        const auto generation=snapshot.omegaRescueSourcesGeneration;
        encoded=rescue::write_source(writer,generation==0?1U:generation,
            generation!=0 && rescue::source_requested(snapshot.omegaRescueScenes,slotIndex));
    } else if(const auto* marker=omega_rescue_marker(snapshot,key,slotType,slotIndex)) {
        const auto generation=snapshot.omegaRescueSourcesGeneration;
        encoded=rescueMarkers::write_authority(writer,generation,
            generation!=0 && rescueMarkers::requested(snapshot.omegaRescueScenes,*marker));
    } else if(const auto chargeSource=omega_charge_source(snapshot,key,slotType,slotIndex);chargeSource.cycle!=nullptr) {
        encoded=charge::write_authority(writer,chargeSource,
            {snapshot.omegaFirstLairGeneration,snapshot.omegaCrownCycle,
             snapshot.omegaCrownChargeEnabled,snapshot.omegaCrownChargeDunked,
             snapshot.omegaCrownTransitBridge});
    } else if(const auto* transitSource=omega_transit_source(snapshot,key,slotType,slotIndex)) {
        encoded=transit::write_source(writer,*transitSource,
            snapshot.omegaFirstLairGeneration,omega_transit_state(snapshot));
    } else if(const auto* transitGate=omega_transit_gate(snapshot,key,slotType,slotIndex)) {
        encoded=transit::write_gate(writer,*transitGate,omega_transit_state(snapshot));
    } else if(const auto* dunkGate=omega_dunk_gate(snapshot,key,slotType,slotIndex)) {
        encoded=transit::write_dunk_gate(writer,*dunkGate,
            snapshot.omegaCrownCycle,snapshot.omegaCrownChargeDunked,snapshot.omegaCrownTransitBridge);
    } else if(const auto* source=omega_lair_source(snapshot,key,slotType,slotIndex)) {
        encoded=state::activity::omega_combatant_authority::write_source(writer,
            {lairEnemies::kRegistry,snapshot.omegaFirstLairGeneration,source->ruleSlot,
             source->memberSlot!=0?std::uint8_t{0}:snapshot.omegaFirstLairLoose[source->slot],
             state::activity::omega_enemy_lair_tactics::for_source(key,slotType,slotIndex)});
    } else if(const auto* crownSource=omega_crown_source(snapshot,key,slotType,slotIndex)) {
        const auto& counts=omega_crown_counts(snapshot,key,crownSource->slot);
        encoded=state::activity::omega_combatant_authority::write_source(writer,
            {key,snapshot.omegaCrownGeneration,crownSource->ruleSlot,
             crownSource->memberSlot!=0?std::uint8_t{0}:counts[0],
             state::activity::omega_enemy_crown_tactics::for_source(key,slotType,slotIndex),
             counts[1],crownSource->categories==2});
    } else if(omega_crown_anchor(snapshot,key,slotType,slotIndex)) {
        encoded=writer.write(1,1) && writer.write(snapshot.omegaCrownGeneration,31)
            && writer.write(1,2) && writer.write(1,3)
            && writer.write(omega_crown_anchor_enabled(snapshot,key)?1U:0U,1) && writer.write(0,4);
    } else if(omega_lair_anchor(snapshot,key,slotType,slotIndex)) {
        encoded=writer.write(1,1) && writer.write(snapshot.omegaFirstLairGeneration,31)
            && writer.write(1,2) && writer.write(1,3)
            && writer.write(snapshot.omegaFirstLairAnchor?1U:0U,1) && writer.write(0,4);
    } else if(omega_first_cannon(snapshot,key,slotType,slotIndex)) {
        // All FX definitions use deferred creation and need a newer active generation.
        // All cores create immediately and retain their own prepared generation.
        const auto& launcher=*cannon::source(slotIndex);
        const bool active=cannon::active(launcher,snapshot.omegaFirstCannonActive,
                                        snapshot.omegaFinalCannonActive);
        const auto generation=snapshot.omegaFirstLairGeneration
            +(slotIndex==launcher.fxSlot && active?1U:0U);
        encoded=cannon::write_authority(writer,generation,active);
    } else if(omega_first_cannon_gate(snapshot,key,slotType,slotIndex)) {
        encoded=write_first_cannon_gate(writer,cannon::active(*cannon::gate(slotIndex),
            snapshot.omegaFirstCannonActive,snapshot.omegaFinalCannonActive));
    } else if (omega_ikora_lattice(snapshot,key,slotType,slotIndex)) {
        encoded = write_ikora_lattice(writer,snapshot.omegaIkoraLatticeReleased);
    } else if (omega_boss_member(snapshot,key,slotType,slotIndex)) {
        encoded = write_omega_boss_member(writer,snapshot);
    } else if (omega_boss(snapshot,key,slotType,slotIndex)) {
        encoded = write_omega_boss(writer,snapshot);
    } else if (omega_intro(snapshot,key,slotType,slotIndex)) {
        encoded = write_omega_intro(writer,snapshot);
    } else if (omega_ending(snapshot,key,slotType,slotIndex)) {
        encoded = write_omega_ending(writer,snapshot);
    } else if (active_omega_portal_component(snapshot, key, slotType, slotIndex)) {
        encoded = write_active_omega_portal_component(writer);
    } else if (active_omega_portal_gate(snapshot, key, slotType, slotIndex)) {
        encoded = write_active_omega_portal_gate(writer);
    } else if (active_omega_gateway_push(snapshot, key, slotType, slotIndex)) {
        encoded = write_active_omega_portal_component(writer);
    } else if (active_omega_boundary_body(snapshot, key, slotType, slotIndex)) {
        encoded = write_mission_body(writer, *find_mission_body(key, slotType, slotIndex));
    } else if (snapshot.omegaSceneAuthority && kOmegaSceneAuthorityBodyReady
               && snapshot.seedAuthoredSensors && omega_ikora_source(key, slotType, slotIndex)) {
        encoded = write_omega_ikora_source(writer);
    } else if (snapshot.omegaSceneAuthority && kOmegaSceneAuthorityBodyReady
               && (snapshot.seedAuthoredSensors || snapshot.omegaIkoraPortalRequested)
               && omega_opening_scene(key, slotType, slotIndex)) {
        encoded = write_omega_scene(writer, snapshot);
    } else if (omega_forest_generator(snapshot, key, slotType, slotIndex)) {
        encoded = state::activity::coo::native_generator::write_activation(writer,
            state::activity::coo::native_generator::omega_request(snapshot.omegaForestSeed));
    } else if (snapshot.omegaSceneAuthority && snapshot.omegaMusicPresent
               && music::is_sensor(key, slotType, slotIndex)) {
        encoded = music::write(writer, snapshot.omegaMusic);
    } else if (snapshot.omegaSceneAuthority && kOmegaDialogueBodyReady
               && snapshot.omegaDialogueArm
               && omega_dialogue(key, slotType, slotIndex)) {
        encoded = write_active_omega_dialogue(writer, snapshot);
    } else if (snapshot.omegaSceneAuthority && kOmegaDirectiveBodyReady
               && snapshot.omegaDialogueArm
               && omega_directive(key, slotType, slotIndex)) {
        encoded = write_active_omega_directive(writer, snapshot);
    } else if (slotType == kSlotTypeParticipation && carriesPlayerKey) {
        encoded = write_participation(writer, snapshot);
    } else if (slotType == kSlotTypeLifetime) {
        encoded = write_lifetime(writer, snapshot,
            omega_crown_restricted(snapshot) && crown::is_lifetime(key,slotType,slotIndex));
    } else if (slotType == kSlotTypeActivityScript && kInitializeActivityScript
               && (snapshot.initializeMissionAuthorityRuntime
                   || snapshot.publishOmegaOpeningTransition)) {
        encoded = write_activity_script(writer, snapshot);
    } else if (slotType == kSlotTypeMissionDirector && kInitializeMissionDirector
               && (snapshot.initializeMissionAuthorityRuntime
                   || snapshot.publishOmegaOpeningTransition
                   || (omega_crown_publishes(snapshot) && crown::is_director(key,slotType,slotIndex)))) {
        encoded = omega_crown_publishes(snapshot) && crown::is_director(key,slotType,slotIndex)
            ? crown::write_director(writer, omega_crown_intent(snapshot)) : write_mission_director(writer, snapshot);
    } else if (slotType == kSlotTypeConfiguration) {
        // Both optional arrays absent and the terminal tag clear is the constructed state.
        encoded = writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                  && writer.write(0, kPresenceWidth) && writer.write(0, 32);
    } else if (slotType == kSlotTypePackage) {
        // 7 absent top-level fields keep the package-owned configuration.
        encoded = pad_bits(writer, kPackageBits);
    } else if (slotType == kSlotTypeQueues) {
        encoded = writer.write(0, 7) && writer.write(0, 5);
    } else if (slotType == kSlotTypeSpawnKeys) {
        encoded = write_spawn_keys(writer);
    } else if (snapshot.seedAuthoredSensors && key == 0x30A025E8U) {
        const MissionBody* const body = find_mission_body(key, slotType, slotIndex);
        if (body != nullptr) {
            encoded = write_mission_body(writer, *body);
        }
    }
    return encoded && writer.bit_count() == start + expected;
}

} // namespace dawn::middleware::bap::activity_message::sensor_auth_update
