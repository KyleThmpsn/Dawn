#include "hijacked_placements.h"
#include "forest_candy_drops.h"
#include "bootflow_hook_lifecycle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "internal.h"
#include "legacy_owner_quarantine_lifecycle.h"
#include "legacy_owner_sentinel.h"
#include "prologue_filler_ready_lifecycle.h"
#include "omega_vex_lattice_probe.h"
#include "omega_arc_charge_receipts.h"
#include "public_event_participant_observer.h"
#include "native_replication_observer.h"
#include "ambient_population_named_observer.h"
#include "native_capture_observer.h"
#include "vance_contact_observer.h"
#include "omega_rescue_scene_receipts.h"
#include "deadly_trial_revival.h"
#include "deadly_trial_lifetime.h"
#include "gateway_patrol.h"

namespace dawn::client::hooks::bootflow {
namespace {

std::atomic_bool g_installed{false};
std::atomic_bool g_acceptLateInstalls{false};
SRWLOCK g_lateInstallLock{SRWLOCK_INIT};
thread_local bool g_freshInstallOwner{};

/**
 * The embedded gameplay host now publishes a real citizen advertisement. The synthetic
 * message-54 bridge assigns the joining client as its own ambassador, which prevents citizen
 * join, so keep the retired bridge detached.
 */
constexpr bool kEnableSyntheticActivityHostAssignment = false;

/** Retired: this experiment skipped native Omega scene retirement and mutated presentation state. */
constexpr bool kEnableActivitySpawnerChainProbe = false;

/** Quarantined legacy groups can alter native feature, selection, or script authority state. */
constexpr bool kEnableUnsafeActivitySelectionProbe = false;
constexpr bool kEnableUnsafeActivityScriptUpstreamProbe = false;
constexpr bool kEnableUnsafeActivityFeatureFlagProbe = false;
constexpr bool kEnableLegacyUnsafeObserverBundle = false;

/** Optional contact-query diagnostics. Mercury gameplay never consumes these samples. */
constexpr bool kEnableVanceContactDiagnostics = false;

static_assert(!kEnableActivitySpawnerChainProbe,
              "The narrow Ikora origin classifier and retired broad probe share hook targets");
static_assert(!kEnableUnsafeActivitySelectionProbe,
              "unsafe_diagnostics must not publish activity selection state");
static_assert(!kEnableUnsafeActivityScriptUpstreamProbe,
              "unsafe_diagnostics must not alter script authority or manager stage");
static_assert(!kEnableUnsafeActivityFeatureFlagProbe,
              "unsafe_diagnostics must not override native feature decisions");
static_assert(!kEnableLegacyUnsafeObserverBundle,
              "unsafe_diagnostics may install only protected, proven observe-only probes");

[[nodiscard]] bool quarantined_legacy_groups_have_ownership() noexcept {
    return legacy_owner_sentinel::any_quarantined_legacy_ownership({
        activity_selection_probe_has_ownership(),
        player_broadcast_create_probe_has_ownership(),
        activity_feature_flag_probe_has_ownership(),
        activity_script_event_probe_has_ownership(),
        activity_script_upstream_probe_has_ownership(),
        activity_notification_type1_apply_probe_has_ownership(),
        activity_behavior_condition_probe_has_ownership(),
        activity_schema_decode_legacy_bundle_has_ownership(),
        group_initial_update_decode_probe_has_ownership(),
    });
}

class LegacyOwnerQuarantineLifecycleOperations final {
public:
    void lock_install() noexcept {
        AcquireSRWLockExclusive(&g_lateInstallLock);
    }

    void unlock_install() noexcept {
        ReleaseSRWLockExclusive(&g_lateInstallLock);
    }

    [[nodiscard]] bool installed() const noexcept {
        return g_installed.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool accepting() const noexcept {
        return g_acceptLateInstalls.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool quarantined_legacy_ownership() const noexcept {
        return quarantined_legacy_groups_have_ownership();
    }

    void close_late_admission() noexcept {
        g_acceptLateInstalls.store(false, std::memory_order_release);
    }

    void quiesce() noexcept {
        ::dawn::client::hooks::bootflow::quiesce();
    }
};

void report_omega_experiment_manifest(const core::settings::experiments::Omega& omega) noexcept {
    const bool effectiveScene = omega.syntheticStageMachine && omega.sceneAuthority;
    const bool effectiveGate = omega.syntheticStageMachine && omega.gateAuthority;
    const bool effectivePortal = omega.syntheticStageMachine && omega.portalMutation;
    const bool dependencyMissing = !omega.syntheticStageMachine
                                   && (omega.sceneAuthority || omega.gateAuthority
                                       || omega.portalMutation);
    const bool anyRequested = omega.directiveUi || omega.ikoraCarrierModelSuppression
                              || omega.ikoraVfxRebind || omega.sceneAuthority
                              || omega.gateAuthority || omega.portalMutation
                              || omega.syntheticStageMachine || omega.unsafeDiagnostics;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=omega_experiments result=%s directive_ui=%u carrier_suppression=%u "
        "vfx_rebind=%u requested_scene_authority=%u requested_gate_authority=%u "
        "requested_portal_mutation=%u synthetic_stage_machine=%u unsafe_diagnostics=%u "
        "effective_scene_authority=%u effective_gate_authority=%u "
        "effective_portal_mutation=%u dependency_missing=%u forest_candy_drops=%u",
        anyRequested ? "enabled" : "all_off",
        omega.directiveUi ? 1U : 0U,
        omega.ikoraCarrierModelSuppression ? 1U : 0U,
        omega.ikoraVfxRebind ? 1U : 0U,
        omega.sceneAuthority ? 1U : 0U,
        omega.gateAuthority ? 1U : 0U,
        omega.portalMutation ? 1U : 0U,
        omega.syntheticStageMachine ? 1U : 0U,
        omega.unsafeDiagnostics ? 1U : 0U,
        effectiveScene ? 1U : 0U,
        effectiveGate ? 1U : 0U,
        effectivePortal ? 1U : 0U,
        dependencyMissing ? 1U : 0U,
        omega.forestCandyDrops ? 1U : 0U);
    if (written <= 0) {
        return;
    }
    const std::size_t length = (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
    core::log::write(core::log::Channel::client,
                     anyRequested || dependencyMissing ? core::log::Level::warn
                                                       : core::log::Level::info,
                     {line.data(), length});
}

} // namespace

LateInstallGuard::LateInstallGuard() noexcept {
    if (g_freshInstallOwner) {
        accepted_ = true;
        return;
    }

    AcquireSRWLockShared(&g_lateInstallLock);
    if (g_acceptLateInstalls.load(std::memory_order_acquire)) {
        ownsLock_ = true;
        accepted_ = true;
        return;
    }
    ReleaseSRWLockShared(&g_lateInstallLock);
}

LateInstallGuard::~LateInstallGuard() noexcept {
    if (ownsLock_) {
        ReleaseSRWLockShared(&g_lateInstallLock);
    }
}

bool LateInstallGuard::accepted() const noexcept {
    return accepted_;
}

/**
 * Attaches the boot-step fixes that carry sign-in through to orbit.
 * Each fix stands alone at one site, so a miss on one is reported and the others still attach.
 * @return True when every fix attached.
 */
bool install() noexcept {
    LegacyOwnerQuarantineLifecycleOperations quarantineOperations;
    const legacy_owner_quarantine::InstallDisposition quarantineDisposition =
        legacy_owner_quarantine::begin_install(quarantineOperations);
    if (quarantineDisposition
        == legacy_owner_quarantine::InstallDisposition::rejected_closed) {
        return false;
    }
    const bool quarantineClean =
        quarantineDisposition
        != legacy_owner_quarantine::InstallDisposition::rejected_legacy_ownership;
    if (!quarantineClean) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow stage=install result=fail reason=quarantined_owner_state");
        return false;
    }
    // An accepting partial generation retries missed idempotent owners. The policy above rejects
    // a quiesced generation and retains this exclusive lock for the complete accepted retry.
    g_freshInstallOwner = true;

    const auto& omega = core::settings::get().omegaExperiments;
    const bool sceneRetirementProbe = omega.unsafeDiagnostics;
    report_omega_experiment_manifest(omega);

    const bool hold = install_character_select_hold();
    const bool sliceSet = install_orbit_slice_set();
    const bool skip = install_profile_setup_skip();
    const bool composition = install_composition_check();
    const bool handoff = install_orbit_handoff();
    const bool joinReady = install_join_request_ready();
    const bool ownerSlot = install_owner_activity_slot();
    const bool activityHostInstalled = kEnableSyntheticActivityHostAssignment
                                       && install_activity_host_assignment();
    const bool activityHost = !kEnableSyntheticActivityHostAssignment || activityHostInstalled;
    const bool activitySelection = !kEnableUnsafeActivitySelectionProbe;
    const bool activityTypeOneApply = !kEnableLegacyUnsafeObserverBundle;
    const bool activityScriptEvents = !kEnableLegacyUnsafeObserverBundle;
    const bool activityScriptUpstream = !kEnableUnsafeActivityScriptUpstreamProbe;
    const bool activityProviderStaleMapping = install_activity_provider_stale_mapping_guard();
    const bool cleanupOwnerGuard = install_native_cleanup_owner_guard();
    const bool propertyListGuard = install_native_property_list_guard();
    const bool localReconnect = install_local_reconnect();
    const bool activityBehaviorConditions = !kEnableLegacyUnsafeObserverBundle;
    const bool activitySpawnerChainInstalled = kEnableActivitySpawnerChainProbe
                                               && install_activity_spawner_chain_probe();
    const bool activitySpawnerChain = !kEnableActivitySpawnerChainProbe
                                      || activitySpawnerChainInstalled;
    // The shared entity factory also discovers New Light's physical shutters.
    // Its production observer is required even when every Omega experiment is off.
    // The owner itself keeps the additional Omega hooks behind their settings.
    const bool omegaIkoraOriginInstalled = install_omega_ikora_origin_probe();
    const bool omegaIkoraOrigin = omegaIkoraOriginInstalled;
    // The shared directive owner always observes Towerfall's class-specific content consumer.
    // Omega uses server-published native objectives; the client only verifies consumption.
    const bool omegaDirectivePresentationInstalled = install_omega_directive_presentation();
    const bool omegaDirectivePresentation = omegaDirectivePresentationInstalled;
    const bool omegaSceneRetirementInstalled =
        sceneRetirementProbe && install_omega_scene_retirement_probe();
    const bool omegaSceneRetirement = !sceneRetirementProbe
                                      || omegaSceneRetirementInstalled;
    const bool type31CaptureInstalled =
        omega.unsafeDiagnostics && install_type31_objective_capture();
    const bool type31Capture = !omega.unsafeDiagnostics || type31CaptureInstalled;
    const bool activitySchemaDecode = !kEnableLegacyUnsafeObserverBundle;
    const bool groupInitialUpdateDecode = !kEnableLegacyUnsafeObserverBundle;
    const bool featureFlagInstalled = kEnableUnsafeActivityFeatureFlagProbe
                                      && install_activity_feature_flag_probe();
    const bool featureFlag = !kEnableUnsafeActivityFeatureFlagProbe || featureFlagInstalled;
    const bool dialogueDispatchProbe = install_omega_dialogue_dispatch_probe();
    const bool omegaNavigation = install_omega_navigation();
    const bool omegaLairCinematic = install_omega_lair_cinematic();
    const bool omegaLairReceipts = install_omega_enemy_lair_receipts();
    const bool vanceContactInstalled = kEnableVanceContactDiagnostics
                                       && install_vance_contact_observer();
    const bool vanceContact = !kEnableVanceContactDiagnostics || vanceContactInstalled;
    const bool ambientNamedPoints = install_ambient_population_named_observer();
    const bool nativeCapture = install_native_capture_observer();
    const bool replicationObserver = native_replication_observer::install();
    const bool omegaCannonReceipt = install_omega_first_cannon_receipt();
    const bool omegaArcCharge = install_omega_arc_charge_receipts();
    const bool publicEventParticipant = public_event_participant_observer::install();
    const bool omegaRescueScenes = install_omega_rescue_scene_receipts();
    const bool trialRevival = deadly_trial_revival::install();
    const bool trialLifetime = deadly_trial_lifetime::install();
    const bool gatewayPatrol = gateway_patrol::install();
    const bool hijackedPlacements = hijacked_placements::install();
    const bool forestCandyInstalled = omega.forestCandyDrops && forest_candy_drops::install();
    const bool forestCandy = !omega.forestCandyDrops || forestCandyInstalled;
    const bool omegaLatticeProbe = omega_vex_lattice_probe::install();
    const bool prologueFiller = install_prologue_filler_ready();
    const bool regionPrivate = install_region_private();
    const bool worldStep = install_world_step();
    const bool spawn = install_spawn_hold();
    const bool towerfallExecutor = install_towerfall_executor_bootstrap();
    const bool fade = install_fade_release();
    const bool anyFix = hold || sliceSet || skip || composition || handoff || joinReady || ownerSlot
                        || activityHostInstalled || activityProviderStaleMapping
                        || activitySpawnerChainInstalled
                        || omegaIkoraOriginInstalled
                        || omegaDirectivePresentationInstalled
                        || omegaSceneRetirementInstalled
                        || type31CaptureInstalled || dialogueDispatchProbe || omegaNavigation || omegaLairCinematic
                        || omegaLairReceipts || vanceContactInstalled || ambientNamedPoints || nativeCapture || replicationObserver || omegaCannonReceipt || omegaArcCharge || publicEventParticipant || omegaRescueScenes || trialRevival || trialLifetime || hijackedPlacements
                        || forestCandyInstalled || omegaLatticeProbe || gatewayPatrol
                        || featureFlagInstalled
                        || prologueFiller || regionPrivate || cleanupOwnerGuard || propertyListGuard || localReconnect
                        || worldStep || spawn || towerfallExecutor || fade;
    const bool allInstalled = hold && sliceSet && skip && composition && handoff && joinReady
                              && ownerSlot && activityHost && activitySelection
                              && activityTypeOneApply && activityScriptEvents
                              && activityScriptUpstream && activityProviderStaleMapping && cleanupOwnerGuard && propertyListGuard && localReconnect
                              && activityBehaviorConditions && activitySpawnerChain
                              && activitySchemaDecode
                              && groupInitialUpdateDecode
                              && omegaIkoraOrigin && omegaDirectivePresentation
                              && omegaSceneRetirement && type31Capture && dialogueDispatchProbe
                              && omegaNavigation && omegaLairCinematic
                              && omegaLairReceipts && vanceContact && ambientNamedPoints && nativeCapture && omegaCannonReceipt && omegaArcCharge && publicEventParticipant && omegaRescueScenes && trialRevival && trialLifetime && hijackedPlacements
                              && forestCandy && featureFlag && prologueFiller && regionPrivate
                              && worldStep && spawn && towerfallExecutor && fade && gatewayPatrol;
    // Admission opens only for this fresh lifecycle and before its installed publication. The
    // exclusive lock keeps retail callbacks from observing either half of that publication.
    g_acceptLateInstalls.store(anyFix, std::memory_order_release);
    g_installed.store(anyFix, std::memory_order_release);
    g_freshInstallOwner = false;
    ReleaseSRWLockExclusive(&g_lateInstallLock);
    return allInstalled;
}

/** Closes protected bootflow publications before their producers begin detaching. */
void quiesce() noexcept {
    // Close late admission first and wait out any operation that was admitted by this lifecycle.
    AcquireSRWLockExclusive(&g_lateInstallLock);
    g_acceptLateInstalls.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lateInstallLock);

    quiesce_native_cleanup_owner_guard();
    quiesce_native_property_list_guard();
    quiesce_local_reconnect();
    hijacked_placements::quiesce();
    forest_candy_drops::quiesce();
    quiesce_spawn_hold();
    quiesce_towerfall_executor_bootstrap();
    quiesce_omega_navigation();
    quiesce_omega_lair_cinematic();
    quiesce_omega_enemy_lair_receipts();
    if constexpr (kEnableVanceContactDiagnostics) {
        quiesce_vance_contact_observer();
    }
    quiesce_ambient_population_named_observer();
    quiesce_native_capture_observer();
    native_replication_observer::quiesce();
    quiesce_omega_first_cannon_receipt();
    quiesce_omega_arc_charge_receipts();
    public_event_participant_observer::quiesce();
    quiesce_omega_rescue_scene_receipts();
    deadly_trial_revival::quiesce();
    deadly_trial_lifetime::quiesce();
    gateway_patrol::quiesce();
    omega_vex_lattice_probe::quiesce();

    quiesce_type31_objective_capture();
    quiesce_prologue_filler_ready();
    quiesce_omega_scene_retirement_probe();
    quiesce_omega_directive_presentation();
    quiesce_omega_ikora_origin_probe();
}

/** Detaches every boot-step fix, respecting producer-before-dependency ownership. */
bool uninstall() noexcept {
    LegacyOwnerQuarantineLifecycleOperations quarantineOperations;
    const bool quarantineClean =
        legacy_owner_quarantine::begin_uninstall(quarantineOperations);
    if (!quarantineClean) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=bootflow stage=uninstall result=retained owner=quarantined_legacy");
        return false;
    }

    if (!uninstall_spawn_hold()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=uninstall result=retained owner=spawn_hold");
        return false;
    }
    if (!uninstall_towerfall_executor_bootstrap()) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::warn,
            "ev=bootflow stage=uninstall result=retained owner=towerfall_executor_bootstrap");
        return false;
    }
    if (!uninstall_type31_objective_capture()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=uninstall result=retained owner=type31_capture");
        return false;
    }
    if (!uninstall_prologue_filler_ready_checked()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=uninstall result=retained owner=prologue_filler");
        return false;
    }
    uninstall_world_step();
    if (!hijacked_placements::uninstall()) { return false; }
    if (!forest_candy_drops::uninstall()) { return false; }
    uninstall_fade_release();

    uninstall_secure_channel_predicate_probe();
    uninstall_region_private();
    const bool omegaSceneRetirement = uninstall_omega_scene_retirement_probe();
    const bool omegaDirectivePresentation = uninstall_omega_directive_presentation();
    const bool omegaIkoraOrigin = uninstall_omega_ikora_origin_probe();
    if (!omegaSceneRetirement || !omegaDirectivePresentation || !omegaIkoraOrigin) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=uninstall result=deferred protected=omega");
        return false;
    }
    if (!uninstall_omega_navigation()) { return false; }
    if (!uninstall_omega_lair_cinematic()) { return false; }
    if (!uninstall_omega_enemy_lair_receipts()) { return false; }
    if constexpr (kEnableVanceContactDiagnostics) {
        if (!uninstall_vance_contact_observer()) { return false; }
    }
    if (!uninstall_ambient_population_named_observer()) { return false; }
    if (!uninstall_native_capture_observer()) { return false; }
    if (!native_replication_observer::uninstall()) { return false; }
    if (!uninstall_omega_first_cannon_receipt()) { return false; }
    if (!uninstall_omega_arc_charge_receipts()) { return false; }
    if (!public_event_participant_observer::uninstall()) { return false; }
    if (!uninstall_omega_rescue_scene_receipts()) { return false; }
    if (!deadly_trial_revival::uninstall()) { return false; }
    if (!deadly_trial_lifetime::uninstall()) { return false; }
    if (!gateway_patrol::uninstall()) { return false; }
    if (!omega_vex_lattice_probe::uninstall()) { return false; }
    uninstall_omega_dialogue_dispatch_probe();
    uninstall_activity_spawner_chain_probe();
    uninstall_activity_provider_stale_mapping_guard();
    if (!uninstall_native_cleanup_owner_guard()) { return false; }
    if (!uninstall_native_property_list_guard()) { return false; }
    if (!uninstall_local_reconnect()) { return false; }
    // The quarantined mutating legacy groups are unreachable in this lifecycle. Their detach
    // implementations discard individual failures, so teardown deliberately remains a no-op for
    // these statically asserted-never-attached owners.
    static_assert(!kEnableUnsafeActivityFeatureFlagProbe);
    static_assert(!kEnableUnsafeActivityScriptUpstreamProbe);
    static_assert(!kEnableUnsafeActivitySelectionProbe);
    static_assert(!kEnableLegacyUnsafeObserverBundle);
    uninstall_activity_host_assignment();
    uninstall_owner_activity_slot();
    uninstall_join_request_ready();
    uninstall_orbit_handoff();
    uninstall_composition_check();
    uninstall_profile_setup_skip();
    uninstall_orbit_slice_set();
    uninstall_character_select_hold();
    g_installed.store(false, std::memory_order_release);
    return true;
}

/** @return True while at least one boot-step fix is attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace dawn::client::hooks::bootflow
