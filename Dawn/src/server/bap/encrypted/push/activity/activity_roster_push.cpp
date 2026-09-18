#include "../../../../../state/activity/Newlight/launchpad/runtime.h"
#include "activity_roster_push.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/bap/activity_message/activity_clock_state_encoder.h"
#include "../../../../../state/activity/beyond_infinity/runtime.h"
#include "../../../../../state/activity/deep_storage/runtime.h"
#include "../../../../../state/activity/hijacked/runtime.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../../state/activity/forced/activity_forced_destination.h"
#include "../../diagnostics/omega_trace.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace dawn::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;

// Optional protocol prerequisite for source-owned shared clocks. It is staged
// immediately before its matching type5 synchronization; caller rollback covers
// both frames and both nonces. Delivery is never a native readiness receipt.
[[nodiscard]] bool append_clock_configuration(Scratch& scratch,std::uint64_t sessionId,
    const message::Snapshot& snapshot,std::span<const std::byte,state::kAesKeySize> key,
    std::array<std::byte,state::kBapNonceSize>& nonce,std::span<std::byte> response,std::size_t& written) noexcept {
    if(!snapshot.activityClock)return true;
    namespace clock=middleware::bap::activity_message::native::activity_clock;
    std::array<std::byte,5> body{};middleware::encoding::bits::Writer writer(body);std::size_t bytes{};
    if(!clock::write(writer,*snapshot.activityClock) || !writer.finish(bytes) || bytes!=body.size()
        || !append_notification_frame(scratch,sessionId,clock::kMessageType,body,key,nonce,response,written))return false;
    middleware::secure_channel::advance_nonce(nonce);return true;
}

std::atomic_uint32_t g_towerfallDeliveryReports{};

[[nodiscard]] bool towerfall_forced() noexcept {
    state::activity::forced::ForcedDestination forced{};
    state::activity::forced::snapshot(forced);
    return forced.enabled
           && std::string_view(forced.packageName.data(), forced.packageNameLength)
                  == "mission_towerfall";
}

[[nodiscard]] bool take_towerfall_delivery_report() noexcept {
    return g_towerfallDeliveryReports.fetch_add(1U, std::memory_order_relaxed) < 32U;
}

// The clock state and its matching authority timestamp share the roster's
// existing rollback transaction. Other missions retain their previous traffic.
[[nodiscard]] bool append_beyond_clock(Session& session,Scratch& scratch,
    const message::Snapshot& snapshot,std::string_view name,
    std::span<const std::byte,state::kAesKeySize> key,
    std::array<std::byte,state::kBapNonceSize>& nonce,
    std::span<std::byte> response,std::size_t& written) noexcept {
    const bool vendors=snapshot.vendorPresentation.enabled;
    const bool launchpad=name=="mission_launchpad" && snapshot.launchpad.enabled;
    const bool hijacked=name=="adventure_rumba" && snapshot.hijacked.enabled;
    const bool deep=name=="adventure_whisk" && snapshot.deep_storage.enabled;
    const bool strike=(name=="strike_pact" || name=="mission_pact") && snapshot.strike_pact.enabled;
    const bool garden=(name=="strike_bond" || name=="mission_bond") && snapshot.strike_bond.enabled;
    if(!vendors && !launchpad && !garden && !hijacked && !deep && !strike && (name!="adventure_vod" || !snapshot.beyond_infinity.enabled)) { return true; }
    namespace beyond=state::activity::beyond_infinity;
    namespace clock=middleware::bap::activity_message::clock_state;
    const auto launchpadCurrent=state::activity::newlight::launchpad::request();
    const auto current=beyond::request();
    const auto deepCurrent=state::activity::deep_storage::request();
    const auto hijackedCurrent=state::activity::hijacked::request();
    const auto owner=launchpad?launchpadCurrent.owner:garden?snapshot.strike_bond.completion.owner:hijacked?hijackedCurrent.owner:strike?snapshot.strike_pact.completion.owner:deep?deepCurrent.owner:current.owner;
    const bool enabled=launchpad?launchpadCurrent.frame.enabled:garden?snapshot.strike_bond.enabled:hijacked?hijackedCurrent.frame.enabled:strike?snapshot.strike_pact.enabled:deep?deepCurrent.frame.enabled:current.frame.enabled;
    const auto generation=launchpad?launchpadCurrent.frame.spawnGeneration:garden?snapshot.strike_bond.spawnGeneration:hijacked?hijackedCurrent.frame.spawnGeneration:strike?owner.value:deep?deepCurrent.frame.spawnGeneration:current.frame.spawnGeneration;
    const auto expected=launchpad?snapshot.launchpad.spawnGeneration:garden?snapshot.strike_bond.spawnGeneration:hijacked?snapshot.hijacked.spawnGeneration:strike?snapshot.strike_pact.spawnGeneration:deep?snapshot.deep_storage.spawnGeneration:snapshot.beyond_infinity.spawnGeneration;
    if(session.activity.joinedForeignSession || !lifecycle::activity_binding_is_current(session)
        || !session.activity.lineage.owns(session.activity.instance)
        || (!vendors && (!owner.valid() || owner.run!=state::activity::mission_run_generation()
            || !enabled || generation!=expected))) { return false; }
    // Launchpad owns its opening before arrival. Its initial roster must load
    // the world before a running gameplay clock is required or published.
    if(state::activity::world_phase()!=state::activity::WorldPhase::arrived) { return launchpad || vendors; }
    std::array<std::byte,clock::kEncodedSize> body{};std::size_t size{};
    if(!clock::encode(clock::kRunning,body,size)
        || !append_notification_frame(scratch,session.activity.instance.sessionId,
            clock::kMessageType,body,key,nonce,response,written)) { return false; }
    middleware::secure_channel::advance_nonce(nonce);return true;
}

} // namespace

/** Appends one `sensor_auth_update` svc9 notification carrying the destination's roster. */
bool append_roster_notification(Session& session,
                                Scratch& scratch,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool burst) noexcept {
    if (written > response.size() || !lifecycle::activity_binding_is_current(session)
        || !state::activity::contains(session.activity.instance)
        || session.activity.rosterStaged.staged) {
        return false;
    }
    state::activity::PublicationGeneration publication{};
    if (!lifecycle::stage_roster_publication_generation(session.activity, publication)) {
        return false;
    }
    const auto initialVendorPresence=session.activity.towerVendorPresence;
    const auto initialVendorClockOrigin=session.activity.vendorClockOrigin;
    const std::uint32_t initialRosterGroups = session.activity.rosterGroups;
    const std::uint8_t initialRosterSends = session.activity.rosterSends;
    const std::uint8_t initialRosterState = session.activity.rosterState;
    const auto initialLifetimes = session.activity.rosterLifetimes;
    const std::uint8_t initialOmegaOpeningStage = session.activity.omegaOpeningStage;
    const std::uint16_t initialDirectorSends = session.activity.directorSends;
    const bool initialMissionDirectorActive = session.activity.missionDirectorActive;
    message::Snapshot snapshot{};
    std::array<char, kDestinationCapacity> destination{};
    std::size_t destinationLength = 0;
    RosterOutcome outcome = RosterOutcome::noEpoch;
    if (session.activityPatchEpochSeen) {
        outcome = build_roster_snapshot(
            session, scratch, snapshot, destination, destinationLength, burst);
    }
    const std::string_view name(destination.data(), destinationLength);
    if (outcome != RosterOutcome::published) {
        session.activity.towerVendorPresence=initialVendorPresence;
        session.activity.vendorClockOrigin=initialVendorClockOrigin;
        session.activity.rosterGroups = initialRosterGroups;
        session.activity.rosterSends = initialRosterSends;
        session.activity.rosterState = initialRosterState;
        session.activity.rosterLifetimes = initialLifetimes;
        session.activity.omegaOpeningStage = initialOmegaOpeningStage;
        session.activity.directorSends = initialDirectorSends;
        session.activity.missionDirectorActive = initialMissionDirectorActive;
        report_roster_push(session, snapshot, name, 0, kNoGrant, outcome);
        return false;
    }

    // The grant is picked here and committed only once the frame reaches the caller, so a
    // discarded body leaves the bubble ungranted and the next push retries it.
    state::activity::bubble_authority::Grant grant{};
    // The grant follows the player, not the destination. The client names the region it is in
    // and that moves as it walks between bubbles, so granting the arrival bubble again would leave
    // the bubble the player actually entered without authority.
    // The wire field is unsigned. A value past the signed range turns negative and the selector
    // rejects it, the same answer as its own upper bound.
    if (state::activity::bubble_authority::select_grant(
            session.activity.instance, static_cast<std::int32_t>(snapshot.region), grant)) {
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    bool encoded = append_beyond_clock(session,scratch,snapshot,name,key,nonce,response,written)
        && append_clock_configuration(scratch,session.activity.instance.sessionId,snapshot,key,nonce,response,written)
                   && message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize)
                   && append_notification_frame(scratch,
                                                session.activity.instance.sessionId,
                                                message::kMessageType,
                                                std::span(scratch.responseBody).first(messageSize),
                                                key,
                                                nonce,
                                                response,
                                                written);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // Staged, not published. The grant and the counters are one-way and this body may still be
        // discarded, so they are held here and settled by `commit_staged_roster` or
        // `discard_staged_roster`.
        session.activity.rosterStaged.binding = session.activity.key;
        session.activity.rosterStaged.activity = session.activity.instance;
        session.activity.rosterStaged.publication = publication;
        session.activity.rosterStaged.grant = grant;
        session.activity.rosterStaged.priorVendorPresence=initialVendorPresence;
        session.activity.rosterStaged.priorVendorClockOrigin=initialVendorClockOrigin;
        session.activity.rosterStaged.priorGroups = initialRosterGroups;
        session.activity.rosterStaged.priorSends = initialRosterSends;
        session.activity.rosterStaged.priorState = initialRosterState;
        session.activity.rosterStaged.priorLifetimes = initialLifetimes;
        session.activity.rosterStaged.priorOmegaOpeningStage = initialOmegaOpeningStage;
        session.activity.rosterStaged.priorDirectorSends = initialDirectorSends;
        session.activity.rosterStaged.priorMissionDirectorActive =
            initialMissionDirectorActive;
        session.activity.rosterStaged.omegaOpeningStage =
            session.activity.omegaOpeningStage != initialOmegaOpeningStage
                ? session.activity.omegaOpeningStage
                : message::kOmegaOpeningStageNone;
        session.activity.rosterStaged.omegaOpeningScriptState =
            static_cast<std::uint8_t>(snapshot.activityScriptState);
        session.activity.rosterStaged.towerWatchCueStage = snapshot.authoredCueStage;
        session.activity.rosterStaged.hasGrant = snapshot.hasGrant;
        if (name == "mission_scot") {
            session.activity.rosterStaged.omegaTracePublicationId =
                diagnostics::omega_trace::record_auth_staged(
                    session.activity.instance.sessionId,
                    snapshot,
                    std::span(scratch.responseBody).first(messageSize));
        }
        session.activity.rosterStaged.staged = true;
        session.activity.rosterPublicationClock = publication;
    }
    report_roster_push(session,
                       snapshot,
                       name,
                       encoded ? messageSize : 0,
                       encoded && snapshot.hasGrant ? snapshot.grant.bubble : kNoGrant,
                       encoded ? RosterOutcome::published : RosterOutcome::encodeFailed);
    if (name == "mission_towerfall" && take_towerfall_delivery_report()) {
        std::array<char, 256> line{};
        const int reportWritten = std::snprintf(
            line.data(),
            line.size(),
            "ev=towerfall_roster stage=direct_encode result=%s message_bytes=%zu frame_bytes=%zu staged=%u",
            encoded ? "ok" : "failed",
            messageSize,
            written - initialWritten,
            session.activity.rosterStaged.staged ? 1U : 0U);
        if (reportWritten > 0) {
            core::log::write(core::log::Channel::server,
                             encoded ? core::log::Level::info : core::log::Level::warn,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(reportWritten),
                                         line.size() - 1U)});
        }
    }
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (!encoded) {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activity.towerVendorPresence=initialVendorPresence;
        session.activity.vendorClockOrigin=initialVendorClockOrigin;
        session.activity.rosterGroups = initialRosterGroups;
        session.activity.rosterSends = initialRosterSends;
        session.activity.rosterState = initialRosterState;
        session.activity.rosterLifetimes = initialLifetimes;
        session.activity.omegaOpeningStage = initialOmegaOpeningStage;
        session.activity.directorSends = initialDirectorSends;
        session.activity.missionDirectorActive = initialMissionDirectorActive;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Appends one immutable coordinator-owned roster snapshot without advancing live delivery. */
bool append_roster_notification(
    Session& session,
    Scratch& scratch,
    const RegionTransitionSnapshot& transition,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written) noexcept {
    const std::string_view destination(
        reinterpret_cast<const char*>(transition.destination.packageName.data()),
        transition.destination.packageNameLength);
    const bool towerfall = destination == "mission_towerfall";
    const bool boundsOk = written <= response.size();
    const bool bindingCurrent = lifecycle::activity_binding_is_current(session);
    const bool bindingMatch = session.activity.key == transition.binding;
    const bool activityMatch = session.activity.instance == transition.activity;
    const bool publicationValid = static_cast<bool>(transition.rosterPublication);
    const bool stagingFree = !session.activity.rosterStaged.staged;
    if (!boundsOk || !bindingCurrent || !bindingMatch || !activityMatch || !publicationValid
        || !stagingFree) {
        if (towerfall && take_towerfall_delivery_report()) {
            std::array<char, 320> line{};
            const int reportWritten = std::snprintf(
                line.data(),
                line.size(),
                "ev=towerfall_roster stage=coordinator_guard result=blocked bounds=%u current=%u binding=%u activity=%u publication=%u staging_free=%u",
                boundsOk ? 1U : 0U,
                bindingCurrent ? 1U : 0U,
                bindingMatch ? 1U : 0U,
                activityMatch ? 1U : 0U,
                publicationValid ? 1U : 0U,
                stagingFree ? 1U : 0U);
            if (reportWritten > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 {line.data(),
                                  (std::min)(static_cast<std::size_t>(reportWritten),
                                             line.size() - 1U)});
            }
        }
        return false;
    }
    const std::size_t initialWritten = written;
    const auto initialNonce = nonce;
    std::size_t messageSize = 0;
    const bool clockEncoded = append_beyond_clock(session,scratch,transition.rosterWire,destination,key,nonce,response,written)
        && append_clock_configuration(scratch,transition.activity.sessionId,transition.rosterWire,key,nonce,response,written);
    const bool sensorEncoded = clockEncoded && message::encode_sensor_auth_update(
        transition.rosterWire,scratch.responseBody,messageSize);
    const bool encoded = sensorEncoded && append_notification_frame(
                             scratch,
                             transition.activity.sessionId,
                             message::kMessageType,
                             std::span(scratch.responseBody).first(messageSize),
                             key,
                             nonce,
                             response,
                             written);
    if (destination == "mission_scot" && transition.rosterWire.omegaEndingRetire) {
        const auto& ending = transition.rosterWire;
        const bool seedRuntime = ending.archiveOmega && !ending.phaseOneOnly
            && ending.omegaSceneAuthority && ending.omegaEndingState == 1
            && ending.omegaEndingSeedRuntime;
        static std::atomic_uint64_t lastEndingPublication{UINT64_MAX};
        const auto signature = (state::activity::mission_run_generation() << 32U)
            ^ (std::uint64_t{ending.omegaEndingRevision} << 5U)
            ^ (encoded ? 1U : 0U) ^ (ending.omegaEndingPlay ? 2U : 0U)
            ^ (seedRuntime ? 4U : 0U) ^ (ending.phaseOneOnly ? 8U : 0U)
            ^ (ending.preserveMissionAuthorityState ? 16U : 0U);
        if (lastEndingPublication.exchange(signature, std::memory_order_relaxed) != signature) {
            core::log::writef(core::log::Channel::server,
                encoded ? core::log::Level::info : core::log::Level::warn,
                "ev=omega_ending stage=publication result=%s run=%llu revision=%u play=%u "
                "seed_runtime=%u authority_preserve=%u registration_only=%u bytes=%zu",
                encoded ? "staged" : "failed",
                static_cast<unsigned long long>(state::activity::mission_run_generation()),
                ending.omegaEndingRevision, ending.omegaEndingPlay ? 1U : 0U,
                seedRuntime ? 1U : 0U, ending.preserveMissionAuthorityState ? 1U : 0U,
                ending.phaseOneOnly ? 1U : 0U, messageSize);
        }
    }
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        RosterPublication staged{};
        staged.binding = transition.binding;
        staged.activity = transition.activity;
        staged.sourceHostRegion = transition.sourceHostRegion;
        staged.regionIndex = transition.regionIndex;
        staged.publication = transition.rosterPublication;
        staged.grant = transition.grantCandidate;
        staged.priorVendorPresence=transition.before.vendorPresence;
        staged.priorVendorClockOrigin=transition.before.vendorClockOrigin;
        staged.afterVendorPresence=transition.after.vendorPresence;
        staged.afterVendorClockOrigin=transition.after.vendorClockOrigin;
        staged.priorGroups = transition.before.groups;
        staged.priorSends = transition.before.sends;
        staged.priorState = transition.before.state;
        staged.priorLifetimes = transition.before.lifetimes;
        staged.priorOmegaOpeningStage = transition.before.omegaOpeningStage;
        staged.priorDirectorSends = transition.before.directorSends;
        staged.priorMissionDirectorActive = transition.before.missionDirectorActive;
        staged.afterGroups = transition.after.groups;
        staged.afterSends = transition.after.sends;
        staged.afterState = transition.after.state;
        staged.afterLifetimes = transition.after.lifetimes;
        staged.afterOmegaOpeningStage = transition.after.omegaOpeningStage;
        staged.afterDirectorSends = transition.after.directorSends;
        staged.afterMissionDirectorActive = transition.after.missionDirectorActive;
        staged.omegaOpeningStage =
            transition.after.omegaOpeningStage != transition.before.omegaOpeningStage
                ? transition.after.omegaOpeningStage
                : message::kOmegaOpeningStageNone;
        staged.omegaOpeningScriptState =
            static_cast<std::uint8_t>(transition.rosterWire.activityScriptState);
        staged.towerWatchCueStage = transition.rosterWire.authoredCueStage;
        staged.hasGrant = transition.rosterWire.hasGrant;
        staged.hasAfter = true;
        staged.staged = true;
        session.activity.rosterStaged = staged;
    } else {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
    }
    const bool launchpad = destination == "mission_launchpad" && transition.rosterWire.launchpad.enabled;
    if (launchpad || (destination == "adventure_rumba" && transition.rosterWire.hijacked.enabled)) {
        static std::atomic_uint64_t lastReport{};
        const auto generation = launchpad ? transition.rosterWire.launchpad.spawnGeneration : transition.rosterWire.hijacked.spawnGeneration;
        const auto status = encoded ? 1U : !clockEncoded ? 2U : !sensorEncoded ? 3U : 4U;
        const auto signature = (static_cast<std::uint64_t>(generation) << 8U) | status | (launchpad ? 0x80U : 0U);
        if (lastReport.exchange(signature) != signature) {
            std::array<char, 320> line{};
            std::snprintf(line.data(),line.size(),
                "ev=%s stage=publication result=%s clock=%u sensor=%u groups=%zu bytes=%zu activity=%016llX generation=%u",
                launchpad ? "launchpad" : "hijacked",encoded ? "staged" : "blocked",clockEncoded?1U:0U,sensorEncoded?1U:0U,
                transition.rosterWire.roster.groupCount,messageSize,
                static_cast<unsigned long long>(session.activity.instance.sessionId),generation);
            core::log::write(core::log::Channel::server,encoded?core::log::Level::info:core::log::Level::warn,line.data());
        }
    }
    if (towerfall && take_towerfall_delivery_report()) {
        std::array<char, 320> line{};
        const int reportWritten = std::snprintf(
            line.data(),
            line.size(),
            "ev=towerfall_roster stage=coordinator_encode result=%s message_bytes=%zu frame_bytes=%zu staged=%u has_after=%u state_before=%u state_after=%u",
            encoded ? "ok" : "failed",
            messageSize,
            written - initialWritten,
            session.activity.rosterStaged.staged ? 1U : 0U,
            session.activity.rosterStaged.hasAfter ? 1U : 0U,
            static_cast<unsigned>(transition.before.state),
            static_cast<unsigned>(transition.after.state));
        if (reportWritten > 0) {
            core::log::write(core::log::Channel::server,
                             encoded ? core::log::Level::info : core::log::Level::warn,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(reportWritten),
                                         line.size() - 1U)});
        }
    }
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    return encoded;
}

/** Settles a staged roster body that reached the caller. */
void commit_staged_roster(Session& session) noexcept {
    if (!session.activity.rosterStaged.staged) {
        return;
    }
    if (!lifecycle::staged_roster_is_current(session.activity)) {
        if (towerfall_forced() && take_towerfall_delivery_report()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=towerfall_roster stage=commit result=stale");
        }
        session.activity.rosterStaged = {};
        return;
    }
    if (towerfall_forced() && take_towerfall_delivery_report()) {
        std::array<char, 256> line{};
        const int reportWritten = std::snprintf(
            line.data(),
            line.size(),
            "ev=towerfall_roster stage=commit result=ok has_after=%u state_before=%u state_after=%u sends_before=%u sends_after=%u",
            session.activity.rosterStaged.hasAfter ? 1U : 0U,
            static_cast<unsigned>(session.activity.rosterStaged.priorState),
            static_cast<unsigned>(session.activity.rosterStaged.afterState),
            static_cast<unsigned>(session.activity.rosterStaged.priorSends),
            static_cast<unsigned>(session.activity.rosterStaged.afterSends));
        if (reportWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(reportWritten),
                                         line.size() - 1U)});
        }
    }
    if (session.activity.rosterStaged.hasGrant) {
        state::activity::bubble_authority::record_grant(session.activity.rosterStaged.activity,
                                                        session.activity.rosterStaged.grant);
    }
    const std::uint8_t omegaScriptState =
        session.activity.rosterStaged.omegaOpeningScriptState;
    const std::uint8_t omegaStage =
        session.activity.rosterStaged.omegaOpeningStage;
    const std::uint8_t towerWatchStage =
        session.activity.rosterStaged.towerWatchCueStage;
    diagnostics::omega_trace::record_auth_delivery(
        session.activity.rosterStaged.activity.sessionId,
        session.activity.rosterStaged.omegaTracePublicationId,
        omegaScriptState,
        true);
    if (omegaStage == message::kOmegaOpeningStageReady) {
        session.activity.sensorObservation.omegaOpeningReadyAuthorityPublished = true;
        // The Scene handoff can arrive before this prerequisite frame settles. If it already did,
        // wake state 2 now that the delivered order is guaranteed.
        if (session.activity.sensorObservation.omegaSceneHandoffArmed) {
            session.activity.keepaliveDueTick = 0;
        }
    } else if (omegaStage == message::kOmegaOpeningStageTriggered) {
        session.activity.sensorObservation.omegaOpeningAuthorityPublished = true;
    } else if (omegaStage == message::kOmegaForestStageTransition) {
        session.activity.sensorObservation.omegaForestEntranceAuthorityPublished = true;
    }
    if (omegaStage >= message::kOmegaOpeningStageBaseline
        && omegaStage <= message::kOmegaForestStageSettled) {
        std::array<char, 256> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=activity stage=omega_opening_authority result=published opening_stage=%u "
            "script_state=%u scene_selector=0x80EC0F96 direct_spawner=0 "
            "portal_components=%s",
            static_cast<unsigned>(omegaStage),
            static_cast<unsigned>(omegaScriptState),
            (omegaStage == message::kOmegaOpeningStageTriggered
             || omegaStage == message::kOmegaOpeningStagePortal)
                ? "BA5F26EF/4/0,23/1;D00142CF/4/2-4,23/16,70/17,30/20,24"
                : "none");
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              static_cast<std::size_t>(transitionWritten)});
        }
    }
    if (towerWatchStage != 0U) {
        session.activity.sensorObservation.towerWatchPublishedStage = towerWatchStage;
        if (towerWatchStage == 1U
            && session.activity.sensorObservation.towerWatchOpeningCommittedTick == 0U) {
            session.activity.sensorObservation.towerWatchOpeningCommittedTick = GetTickCount64();
        }
        // A startup boundary may already be waiting behind the just-delivered opening beat.
        // Wake the next ordered beat without waiting for the ordinary keepalive interval.
        session.activity.keepaliveDueTick = 0U;
        std::array<char, 256> transitionLine{};
        const int transitionWritten = std::snprintf(
            transitionLine.data(),
            transitionLine.size(),
            "ev=tower_watch_executor stage=commit result=published beat=%u "
            "script_state=%u breach=%u encounter_changed=%u",
            static_cast<unsigned>(towerWatchStage),
            static_cast<unsigned>(omegaScriptState),
            session.activity.sensorObservation.towerWatchBreachSeen ? 1U : 0U,
            session.activity.sensorObservation.towerWatchEncounterChanged ? 1U : 0U);
        if (transitionWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {transitionLine.data(),
                              (std::min)(static_cast<std::size_t>(transitionWritten),
                                         transitionLine.size() - 1U)});
        }
    }
    if (session.activity.rosterStaged.hasAfter) {
        session.activity.towerVendorPresence=session.activity.rosterStaged.afterVendorPresence;
        session.activity.vendorClockOrigin=session.activity.rosterStaged.afterVendorClockOrigin;
        session.activity.rosterGroups = session.activity.rosterStaged.afterGroups;
        session.activity.rosterSends = session.activity.rosterStaged.afterSends;
        session.activity.rosterState = session.activity.rosterStaged.afterState;
        session.activity.rosterLifetimes = session.activity.rosterStaged.afterLifetimes;
        session.activity.omegaOpeningStage =
            session.activity.rosterStaged.afterOmegaOpeningStage;
        session.activity.directorSends = session.activity.rosterStaged.afterDirectorSends;
        session.activity.missionDirectorActive =
            session.activity.rosterStaged.afterMissionDirectorActive;
        session.activity.rosterPublicationClock =
            session.activity.rosterStaged.publication;
    }
    session.activity.rosterStaged = {};
}

/** Puts back what a staged roster body advanced, now that the body has been discarded. */
void discard_staged_roster(Session& session) noexcept {
    if (!session.activity.rosterStaged.staged) {
        return;
    }
    if (!lifecycle::staged_roster_is_current(session.activity)) {
        if (towerfall_forced() && take_towerfall_delivery_report()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=towerfall_roster stage=discard result=stale");
        }
        session.activity.rosterStaged = {};
        return;
    }
    if (towerfall_forced() && take_towerfall_delivery_report()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=towerfall_roster stage=discard result=rolled_back");
    }
    if (session.activity.rosterStaged.hasAfter) {
        diagnostics::omega_trace::record_auth_delivery(
            session.activity.rosterStaged.activity.sessionId,
            session.activity.rosterStaged.omegaTracePublicationId,
            session.activity.rosterStaged.omegaOpeningScriptState,
            false);
        session.activity.rosterStaged = {};
        return;
    }
    // The client never saw this body, so its state byte must not be spent. The next push has to
    // move the byte again or the client does not rebuild its roster objects.
    session.activity.towerVendorPresence=session.activity.rosterStaged.priorVendorPresence;
    session.activity.vendorClockOrigin=session.activity.rosterStaged.priorVendorClockOrigin;
    session.activity.rosterGroups = session.activity.rosterStaged.priorGroups;
    session.activity.rosterSends = session.activity.rosterStaged.priorSends;
    session.activity.rosterState = session.activity.rosterStaged.priorState;
    session.activity.rosterLifetimes = session.activity.rosterStaged.priorLifetimes;
    session.activity.omegaOpeningStage =
        session.activity.rosterStaged.priorOmegaOpeningStage;
    session.activity.directorSends = session.activity.rosterStaged.priorDirectorSends;
    session.activity.missionDirectorActive =
        session.activity.rosterStaged.priorMissionDirectorActive;
    diagnostics::omega_trace::record_auth_delivery(
        session.activity.rosterStaged.activity.sessionId,
        session.activity.rosterStaged.omegaTracePublicationId,
        session.activity.rosterStaged.omegaOpeningScriptState,
        false);
    session.activity.rosterStaged = {};
}

} // namespace dawn::server::bap::encrypted::push::activity
