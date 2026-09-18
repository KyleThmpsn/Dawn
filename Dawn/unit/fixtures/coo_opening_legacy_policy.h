#pragma once
#include "core/logging/log.h"
#include <cstdio>
namespace frozen_opening {
namespace state = dawn::state;
namespace core = dawn::core;
namespace middleware = dawn::middleware;
inline void discard_log(core::log::Channel, core::log::Level, std::string_view) {}
// Frozen production policy. Only logging and the external quiescence read are substituted.
// Original route SHA256: 16af433860b8bd51bbc9de0936a477dae87e9168c3299ce8325ae3e0cfbcd58d
template<class Session>
void apply(Session& session, const dawn::middleware::bap::activity_message::sense_update::SenseUpdate& update,
           const dawn::server::bap::encrypted::activity_message::omega_opening_intake::Context& c, bool quiesced=false) {
    namespace admission = dawn::server::bap::encrypted::activity_message;
    const bool rosterReady = c.destinationBound && admission::omega_roster_readiness::exact_omega_initial_report(update);
    const bool opening = c.parsed && admission::omega_monitor_edges::entered(update, 20);
    // This accepted capture used portal_mutation=true in the frozen policy.
    const bool forestEntrance = c.parsed && admission::omega_monitor_edges::entered(update, 24);
    const bool destinationBound = c.destinationBound;
    const bool observerReset = c.parsed && c.handleBound && c.epochBound
        && (!destinationBound || (update.hasRosterAcknowledgement && !rosterReady));
    if (observerReset) {
        // A bound teardown/replacement report invalidates both ordering and dedupe state. Preserve
        // only the packet ordinal so the diagnostic stream remains monotonic on this connection.
        session.activity.sensorObservation = {};
        session.activity.omegaOpeningStage =
            middleware::bap::activity_message::sensor_auth_update::kOmegaOpeningStageNone;
    }
    const bool becameReady = rosterReady && !session.activity.sensorObservation.omegaRosterReady;
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
    const char* transition = observerReset ? "reset" : "none";
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
                    discard_log(core::log::Channel::server, core::log::Level::info,
                        {line.data(), static_cast<std::size_t>(length)});
                }
            }
        }
    }
    const char* forestResult = "none";
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
                session.activity.key.generation.value, !quiesced);
            const bool carrierArmed = portalPlan.active && portalPlan.position.revision == 2;
            std::array<char, 224> portalLine{};
            const int portalLength = std::snprintf(portalLine.data(),portalLine.size(),
                "ev=omega_portal stage=entry path=native_only result=%s "
                "carrier=BA5F26EF/4/0 carrier_armed=%u fallback=none arrival_verified=0",
                carrierArmed ? "await_native_arrival" : "carrier_not_armed",carrierArmed?1U:0U);
            if (portalLength > 0 && static_cast<std::size_t>(portalLength) < portalLine.size()) {
                discard_log(core::log::Channel::server,core::log::Level::info,
                    {portalLine.data(),static_cast<std::size_t>(portalLength)});
            }
        }
    }
    (void)transition;
    (void)forestResult;
}
}
