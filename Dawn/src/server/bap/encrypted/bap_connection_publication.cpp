#include "bap_connection_publication.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/events/activity_event_selection.h"
#include "../../../state/activity/forced/activity_forced_destination.h"
#include "push/activity/internal.h"

namespace dawn::server::bap::encrypted {
namespace {

/** Measured delay before the second Family-4 snapshot. */
constexpr std::uint64_t kFamily4RepushDelayMs = 400;
/** The banner pair lands the same unsolicited way and hits the same record-state race. */
constexpr std::uint64_t kBannerRepushDelayMs = 400;
/**
 * How long the roster keeps its faster cadence after a load starts.
 * The slice-set load step costs 9.2 to 14.1 s, so this covers it.
 */
constexpr std::uint64_t kTransitionWindowMs = 15'000;
/** The unpatched archived client reports this epoch when its first type-52 message arrives. */
constexpr std::uint64_t kInitialPatchEpoch = (std::numeric_limits<std::uint64_t>::max)();

} // namespace

/** Captures the connection fields one service outcome carries. */
ConnectionFields connection_fields(const ServiceOutcome& outcome) noexcept {
    ConnectionFields fields{};
    const auto* plan = transaction_if<activity_message::ActivityPlan>(outcome);
    if (plan == nullptr) {
        return fields;
    }
    if (plan->delivery == activity_message::Delivery::joinNotifications) {
        fields.joinMemberKey = plan->entitySlotMutation.memberKey;
        fields.joinCharacterSoid = plan->joinCharacterSoid;
        fields.joinsActivity = true;
    }
    // The initial load is a transition too, and its token does not arrive for several seconds.
    fields.opensTransitionWindow =
        plan->delivery == activity_message::Delivery::joinNotifications || plan->transitionStarted;
    if (plan->mutationDomain == activity_message::MutationDomain::patchEpoch) {
        fields.patchEpoch = plan->patchEpoch;
        fields.retainsPatchEpoch = true;
    }
    fields.clientAuthoritative =
        plan->membershipMutation.kind
        == state::activity::membership::MutationKind::authoritative;
    if (fields.clientAuthoritative) {
        fields.teleportPresent = plan->membershipMutation.authoritativeInput.hasTeleport;
        fields.regionMoved = plan->regionMoved;
    }
    return fields;
}

/** Checks every fallible binding precondition before State is allowed to commit. */
bool can_publish_connection_fields(
    const Session& session,
    state::activity::BindingKey stagedBinding,
    const transactions::Publication& publication) noexcept {
    return !publication.hasActivityBinding
           || (static_cast<bool>(publication.activity)
               && lifecycle::is_staged_activity_binding_key(session, stagedBinding)
               && publication.hasRegionLineage
               && static_cast<bool>(publication.regionLineage)
               && publication.regionLineage.bound == publication.activity);
}

/** Publishes the captured connection fields after a successful commit. */
bool publish_connection_fields(Session& session,
                               state::activity::BindingKey stagedBinding,
                               const transactions::Publication& publication,
                               const ConnectionFields& fields) noexcept {
    const ActivityBindingState previous = session.activity;
    if (publication.hasActivityBinding) {
        if (!can_publish_connection_fields(session, stagedBinding, publication)) {
            return false;
        }
        const lifecycle::ActivityBindingOrigin origin =
            publication.activityBindingCreatedByBap
                ? lifecycle::ActivityBindingOrigin::allocation
                : lifecycle::ActivityBindingOrigin::join;
        const state::activity::ActivityInstanceKey retireAfterAssignment =
            lifecycle::lease_to_retire_after_replacement(previous,
                                                         publication.activity,
                                                         origin,
                                                         publication.atomicReplacement);
        ActivityBindingState next{};
        next.key = stagedBinding;
        next.instance = publication.activity;
        next.ownerLease = lifecycle::lease_after_replacement(
            previous, publication.activity, origin);
        next.lineage = publication.regionLineage;
        next.joinedForeignSession = lifecycle::joined_foreign_session_after_replacement(
            previous, publication.activity, publication.activityBindingFromJoin);
        if (fields.joinMemberKey != 0) {
            next.memberKey = fields.joinMemberKey;
        }
        if (fields.joinCharacterSoid != 0) {
            next.characterSoid = fields.joinCharacterSoid;
        }
        if (fields.opensTransitionWindow) {
            next.transitionUntilTick = GetTickCount64() + kTransitionWindowMs;
        }
        session.activityBindingClock = stagedBinding.generation;
        session.activity = next;
        update_hud_anchor_after_binding_replacement_locked(previous, session.activity);
        if (static_cast<bool>(retireAfterAssignment)) {
            retire_bap_activity_lease_locked(retireAfterAssignment);
        }
    }
    if (!publication.hasActivityBinding && fields.joinMemberKey != 0) {
        session.activity.memberKey = fields.joinMemberKey;
    }
    if (!publication.hasActivityBinding && fields.joinCharacterSoid != 0) {
        session.activity.characterSoid = fields.joinCharacterSoid;
    }
    if (fields.retainsPatchEpoch) {
        const bool epochChanged =
            !session.activityPatchEpochSeen
            || session.activityPatchEpoch.first != fields.patchEpoch.first
            || session.activityPatchEpoch.second != fields.patchEpoch.second;
        if (epochChanged) {
            session.activity.sensorObservation = {};
            if (session.activity.rosterLifetimes.identity.owner) {
                session.activity.rosterLifetimes = {};
                session.activity.rosterSends = 0;
                session.activity.rosterGroups = 0;
            }
        }
        session.activityPatchEpoch = fields.patchEpoch;
        session.activityPatchEpochSeen = true;
    }
    if (!publication.hasActivityBinding && fields.opensTransitionWindow) {
        session.activity.transitionUntilTick = GetTickCount64() + kTransitionWindowMs;
    }
    namespace omega_message =
        middleware::bap::activity_message::sensor_auth_update;
    const bool omegaPortalArmed =
        session.activity.omegaOpeningStage == omega_message::kOmegaOpeningStageTriggered
        || session.activity.omegaOpeningStage == omega_message::kOmegaOpeningStagePortal
        || session.activity.omegaOpeningStage == omega_message::kOmegaForestStageTransition;
    const bool omegaPortalMutation =
        core::settings::get().omegaExperiments.syntheticStageMachine
        && core::settings::get().omegaExperiments.portalMutation;
    if (omegaPortalMutation && fields.clientAuthoritative && omegaPortalArmed
        && (fields.teleportPresent || fields.regionMoved)
        && !session.activity.sensorObservation.omegaPortalTransportConfirmed) {
        session.activity.sensorObservation.omegaPortalTransportConfirmed = true;
        session.activity.keepaliveDueTick = 0;
        std::array<char, 256> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=omega_portal_transport result=confirmed teleport_present=%u "
            "region_moved=%u opening_stage=%u host_action=confirm_after_authority",
            fields.teleportPresent ? 1U : 0U,
            fields.regionMoved ? 1U : 0U,
            static_cast<unsigned>(session.activity.omegaOpeningStage));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    // A join resets the client's roster container, so the warm-up is re-armed. Its unconditional
    // state-byte moves make the client deactivate and rebuild every roster-owned object, and the
    // player object binds to the published membership only on that rebuild.
    if (fields.joinsActivity) {
        session.activity.sensorObservation = {};
        session.activity.rosterSends = 0;
        session.activity.rosterGroups = 0;
        session.activity.directorSends = 0;
        session.activity.missionDirectorActive = false;

        // Homecoming reaches prologue setup roughly one second after the accepted join, while the
        // ordinary keepalive is five seconds away. Seed the join identity now and make the first
        // post-join keepalive immediately due so message 12 creates the activity player before the
        // world-controller validates its lifecycle hierarchy. The archived unpatched client later
        // reports the all-one epoch itself; publishing that observed initial value lets the roster
        // registration accompany this first membership snapshot instead of waiting until cleanup.
        if (!session.activityPatchEpochSeen) {
            session.activityPatchEpoch.first = kInitialPatchEpoch;
            session.activityPatchEpoch.second = kInitialPatchEpoch;
            session.activityPatchEpochSeen = true;
        }
        const bool identitySeeded = push::activity::seed_identity(
            session.activity.instance, session.activity.memberKey, session.activity.characterSoid);
        const bool tokenSeeded = push::activity::seed_transition_token(session.activity.instance);
        const std::uint64_t now = GetTickCount64();
        session.activity.keepaliveDueTick = now;
        session.activity.rosterDueTick = now;
        state::activity::events::reload();

        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity stage=join_membership_seed result=%s session=0x%llX member=0x%llX character=0x%llX token=%u epoch=initial",
            identitySeeded ? "ok" : "fail",
            static_cast<unsigned long long>(session.activity.instance.sessionId),
            static_cast<unsigned long long>(session.activity.memberKey),
            static_cast<unsigned long long>(session.activity.characterSoid),
            tokenSeeded ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             identitySeeded ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return true;
}

/** Arms the owed Family-4 and banner re-pushes when the queuez publication asks for them. */
void arm_repushes(Session& session, const queuez::StagedPublication& queuezPublication) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (queuezPublication.hasState) {
        if (queuezPublication.cancelFamily4RepushRoot != 0
            && queuezPublication.cancelFamily4RepushRoot == session.family4RepushRoot) {
            session.family4RepushArmed = false;
            session.family4RepushRoot = 0;
            session.family4RepushDueTick = 0;
        }
        if (queuezPublication.cancelBannerRepushRoot != 0
            && queuezPublication.cancelBannerRepushRoot == session.bannerRepushRoot) {
            session.bannerRepushArmed = false;
            session.bannerRepushRoot = 0;
            session.bannerRepushDueTick = 0;
        }
        if (queuezPublication.releasedFamily4) {
            session.accountResyncArmed = false;
            session.accountResyncGeneration = 0;
        }
    }
    if (queuezPublication.armsFamily4Repush && queuezPublication.family4RepushRoot != 0) {
        session.family4RepushDueTick = now + kFamily4RepushDelayMs;
        session.family4RepushRoot = queuezPublication.family4RepushRoot;
        session.family4RepushArmed = true;
    }
    // Armed on its own signal, not on family four's. Family zero re-subscribes on every record
    // cycle, and each subscribe needs a delayed copy because the immediate answer arrives too soon.
    if (queuezPublication.armsBannerRepush && queuezPublication.bannerRepushRoot != 0) {
        session.bannerRepushDueTick = now + kBannerRepushDelayMs;
        session.bannerRepushRoot = queuezPublication.bannerRepushRoot;
        session.bannerRepushArmed = true;
    }
}

} // namespace dawn::server::bap::encrypted
