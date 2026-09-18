#include <Windows.h>
#include "server/bap/internal.h"
#include "state/activity/coo/omega_opening_projection.h"
#include "server/bap/encrypted/activity_message/omega_opening_intake.h"
#include "middleware/bap/activity_message/sensor_auth_update.h"
#include "middleware/encoding/bit_writer.h"
#include "fixtures/coo_opening_accepted_capture.h"
#include "fixtures/coo_opening_legacy_policy.h"
#include "fixtures/omega_complete_roster_capture.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace coo = dawn::state::activity::coo;
namespace opening = coo::omega::opening;
namespace intake = dawn::server::bap::encrypted::activity_message::omega_opening_intake;
namespace sense = dawn::middleware::bap::activity_message::sense_update;
namespace wire = dawn::middleware::bap::activity_message::sensor_auth_update;
static unsigned checks{}, bodies{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while(false)

struct Observer {
    opening::Run omegaOpeningExecutor;
    opening::lattice::State omegaIkoraLattice;
    bool omegaRosterReady{}, omegaOpeningTriggered{}, omegaForestEntranceTriggered{};
    bool omegaSceneHandoffArmed{}, omegaSceneCompleted{}, omegaPortalTransportConfirmed{};
    bool omegaForestEntranceAuthorityPublished{};
    unsigned unrelated{};
};
struct Session {
    struct Activity {
        struct Key { struct Generation { std::uint64_t value{7}; } generation; } key;
        Observer sensorObservation;
        std::uint64_t keepaliveDueTick{1};
        std::uint8_t omegaOpeningStage{};
    } activity;
};

sense::SenseUpdate decode(std::size_t index) {
    const auto hex = accepted_opening::packets[index].hex;
    std::vector<std::byte> data;
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        data.push_back(static_cast<std::byte>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    sense::SenseUpdate update{};
    std::size_t consumed{};
    CHECK(sense::parse_omega_sense_update(data, update, consumed));
    CHECK(consumed <= data.size() * 8);
    return update;
}
intake::Context context(std::uint64_t packet) { return {7, packet, true, true, true, true}; }

wire::Snapshot snapshot(const Observer& state, bool executor, bool quiesced, bool seed, int region) {
    wire::Snapshot result{};
    result.archiveOmega = true;
    result.omegaSceneAuthority = !quiesced;
    result.omegaIkoraPortalRequested = !quiesced && state.omegaOpeningTriggered;
    const auto plan = state.omegaIkoraLattice.plan(7, !quiesced);
    result.omegaIkoraLatticeReleased = plan.active && plan.position.revision == 2;
    result.omegaPortalEntry = !quiesced && result.omegaIkoraLatticeReleased;
    result.omegaPortalPlayerHash = !quiesced;
    if (executor) { opening::project(state.omegaOpeningExecutor, result); }
    result.omegaDialogueArm = !quiesced && seed;
    result.omegaTunnelDialogue = result.omegaDialogueArm && state.omegaForestEntranceTriggered;
    result.omegaForestBanner = result.omegaDialogueArm
        && (state.omegaForestEntranceTriggered || (region >= 64 && region <= 112));
    result.omegaForestGenerator = state.omegaForestEntranceTriggered || (region >= 64 && region <= 104);
    return result;
}
void same_wire(const Observer& left, const Observer& right, bool seed = true, int region = 120) {
    CHECK(left.omegaRosterReady == right.omegaRosterReady);
    CHECK(left.omegaOpeningTriggered == right.omegaOpeningTriggered);
    CHECK(left.omegaForestEntranceTriggered == right.omegaForestEntranceTriggered);
    CHECK(left.unrelated == right.unrelated);
    for (bool quiesced : {false, true}) {
        const auto a = snapshot(left, false, quiesced, seed, region);
        const auto b = snapshot(right, true, quiesced, seed, region);
        CHECK(a.omegaIkoraPortalRequested == b.omegaIkoraPortalRequested);
        CHECK(a.omegaIkoraLatticeReleased == b.omegaIkoraLatticeReleased);
        CHECK(a.omegaPortalEntry == b.omegaPortalEntry);
        CHECK(a.omegaTunnelDialogue == b.omegaTunnelDialogue);
        CHECK(a.omegaForestBanner == b.omegaForestBanner);
        CHECK(a.omegaForestGenerator == b.omegaForestGenerator);
        for (auto registry : {0xD00142CFU, 0xBA5F26EFU, 0x2763EC97U}) {
            for (std::uint8_t type : std::array<std::uint8_t, 8>{4, 17, 18, 23, 30, 43, 53, 70}) {
                for (std::uint16_t slot = 0; slot < 64; ++slot) {
                    CHECK(wire::auth_body_bits(a, registry, type, slot, false)
                        == wire::auth_body_bits(b, registry, type, slot, false));
                    if (!wire::auth_body_bits(a, registry, type, slot, false)) { continue; }
                    std::array<std::byte, 2048> x{}, y{};
                    dawn::middleware::encoding::bits::Writer wx(x), wy(y);
                    CHECK(wire::write_auth_body(wx, a, registry, type, slot, false));
                    CHECK(wire::write_auth_body(wy, b, registry, type, slot, false));
                    CHECK(wx.bit_count() == wy.bit_count());
                    std::size_t nx{}, ny{};
                    CHECK(wx.finish(nx) && wy.finish(ny) && nx == ny);
                    CHECK(std::equal(x.begin(), x.begin() + nx, y.begin()));
                    ++bodies;
                }
            }
        }
    }
}

void captured_replay(unsigned batch) {
    Session legacy;
    Observer actual;
    legacy.activity.sensorObservation.unrelated = actual.unrelated = 9;
    for (std::size_t i = 0; i < accepted_opening::packets.size(); ++i) {
        const auto update = decode(i);
        const auto c = context(accepted_opening::packets[i].ordinal);
        const auto before = actual.omegaOpeningExecutor.authority();
        const auto accepted = intake::capture(actual.omegaOpeningExecutor, c, update);
        CHECK(!accepted.overflow);
        CHECK(before.requested == actual.omegaOpeningExecutor.authority().requested);
        CHECK(before.released == actual.omegaOpeningExecutor.authority().released);
        frozen_opening::apply(legacy, update, c);
        if ((i + 1) % batch == 0 || i + 1 == accepted_opening::packets.size()) {
            static_cast<void>(opening::update_observer(actual, 7));
            same_wire(legacy.activity.sensorObservation, actual, accepted_opening::packets[i].tick >= 77187);
        }
    }
    CHECK(actual.omegaOpeningExecutor.authority().released);
    CHECK(actual.omegaOpeningExecutor.authority().entrance);
    CHECK(actual.omegaOpeningExecutor.diagnostics().phase == coo::Phase::complete);
}

void complete_loading_roster() {
    sense::SenseUpdate roster{};
    complete_opening_capture::fill(roster);
    const auto approach = decode(9);
    for (unsigned rejectedGate = 0; rejectedGate < 5; ++rejectedGate) {
        Observer actual;
        auto c = context(1);
        if (rejectedGate == 1) c.destinationBound = false;
        if (rejectedGate == 2) c.handleBound = false;
        if (rejectedGate == 3) c.epochBound = false;
        if (rejectedGate == 4) c.parsed = false;
        CHECK(!intake::capture(actual.omegaOpeningExecutor, c, roster).overflow);
        static_cast<void>(opening::update_observer(actual, 7));
        CHECK(actual.omegaRosterReady == (rejectedGate == 0));
        CHECK(!actual.omegaOpeningTriggered);
        CHECK(!intake::capture(actual.omegaOpeningExecutor, context(2), approach).overflow);
        static_cast<void>(opening::update_observer(actual, 7));
        CHECK(actual.omegaOpeningTriggered == (rejectedGate == 0));
        wire::Snapshot projected{};
        projected.omegaSceneAuthority = true;
        opening::project(actual.omegaOpeningExecutor, projected);
        CHECK(projected.omegaIkoraPortalRequested == (rejectedGate == 0));
        CHECK(!projected.omegaIkoraLatticeReleased);
        // The same 30/24 packet was decoded but ignored in the failing installed
        // run (t=134407). Native entrance observation has no experiment switch.
        const auto entrance = decode(35);
        CHECK(!intake::capture(actual.omegaOpeningExecutor, context(3), entrance).overflow);
        static_cast<void>(opening::update_observer(actual, 7));
        CHECK(actual.omegaForestEntranceTriggered == (rejectedGate == 0));
        const auto forest = snapshot(actual, true, false, true, 120);
        CHECK(forest.omegaForestBanner == (rejectedGate == 0));
        CHECK(forest.omegaForestGenerator == (rejectedGate == 0));
    }
}

void adversarial_receipts() {
    const auto roster = decode(0), approach = decode(9), release = decode(13), entrance = decode(35);
    Session legacy;
    Observer actual;
    std::uint64_t packet{};
    const auto apply = [&](const auto& update, intake::Context c) {
        c.packet = ++packet;
        const auto r = intake::capture(actual.omegaOpeningExecutor, c, update);
        CHECK(!r.overflow);
        frozen_opening::apply(legacy, update, c);
        static_cast<void>(opening::update_observer(actual, 7));
        same_wire(legacy.activity.sensorObservation, actual);
    };
    // Early edges and native release must not be replayed after later admission.
    apply(approach, context(0)); apply(release, context(0)); apply(entrance, context(0));
    CHECK(!actual.omegaOpeningExecutor.authority().requested);
    auto rejected = context(0); rejected.parsed = rejected.destinationBound = false;
    apply(roster, rejected);
    rejected = context(0); rejected.handleBound = rejected.destinationBound = false;
    apply(roster, rejected);
    rejected = context(0); rejected.epochBound = rejected.destinationBound = false;
    apply(roster, rejected);
    apply(roster, context(0)); apply(release, context(0));
    CHECK(!actual.omegaOpeningExecutor.authority().released);
    apply(approach, context(0));
    CHECK(actual.omegaOpeningExecutor.diagnostics().active & (1U << 2));
    // Request publication and repeated updates cannot manufacture native readiness.
    for (unsigned i = 0; i < 100; ++i) { static_cast<void>(opening::update_observer(actual, 7)); }
    CHECK(!actual.omegaOpeningExecutor.authority().released);
    CHECK(actual.omegaOpeningExecutor.diagnostics().active & (1U << 2));
    apply(release, context(0)); apply(release, context(0)); apply(approach, context(0));
    apply(roster, context(0)); // Duplicate roster retains the release.
    CHECK(actual.omegaOpeningExecutor.authority().released);
    apply(entrance, context(0)); apply(entrance, context(0));
    auto badRoster = roster; badRoster.topLevelRosterCount = 0;
    actual.unrelated = legacy.activity.sensorObservation.unrelated = 55;
    const auto incarnation = actual.omegaOpeningExecutor.diagnostics().incarnation;
    apply(badRoster, context(0));
    CHECK(actual.unrelated == 0);
    CHECK(!actual.omegaOpeningExecutor.authority().released);
    apply(release, context(0)); apply(roster, context(0));
    CHECK(actual.omegaOpeningExecutor.diagnostics().incarnation > incarnation);
    CHECK(!actual.omegaOpeningExecutor.authority().requested);
    // Direct Forest entry is independent of the skipped scene branch.
    apply(entrance, context(0));
    CHECK(actual.omegaForestEntranceTriggered && !actual.omegaOpeningTriggered);
    same_wire(legacy.activity.sensorObservation, actual, true, 88);
    rejected = context(0); rejected.destinationBound = false;
    apply(roster, rejected);
}

void coalesced_and_retry() {
    const auto roster = decode(0), approach = decode(9), release = decode(13), entrance = decode(35);
    const auto object = [](const auto& update, std::uint8_t type, std::uint16_t slot) {
        for (std::size_t i = 0; i < update.objectCount; ++i) {
            if (update.objects[i].registryKey == 0xD00142CFU
                && update.objects[i].slotType == type && update.objects[i].slotIndex == slot) {
                return update.objects[i];
            }
        }
        CHECK(false); return sense::SenseObject{};
    };
    sense::SenseUpdate merged{};
    merged.objectCount = 3;
    merged.objects[0] = object(release, 43, 1); // Wire order cannot bypass approach admission.
    merged.objects[1] = object(entrance, 30, 24);
    merged.objects[2] = object(approach, 30, 20);
    Session legacy;
    Observer actual;
    auto c = context(1);
    static_cast<void>(intake::capture(actual.omegaOpeningExecutor, c, roster));
    frozen_opening::apply(legacy, roster, c);
    c.packet = 2;
    static_cast<void>(intake::capture(actual.omegaOpeningExecutor, c, merged));
    frozen_opening::apply(legacy, merged, c);
    CHECK(!actual.omegaOpeningExecutor.authority().released);
    static_cast<void>(opening::update_observer(actual, 7));
    same_wire(legacy.activity.sensorObservation, actual);
    CHECK(actual.omegaOpeningExecutor.authority().released);
    // Delivery builds a detached copy after the persistent update. Discarding
    // that packet must leave the retained release/entrance available for retry.
    auto detached = actual;
    detached = {};
    CHECK(!detached.omegaOpeningExecutor.authority().released);
    const auto incarnation = actual.omegaOpeningExecutor.diagnostics().incarnation;
    for (unsigned i = 0; i < 3; ++i) {
        const auto retry = actual;
        same_wire(legacy.activity.sensorObservation, retry);
        CHECK(retry.omegaOpeningExecutor.diagnostics().incarnation == incarnation);
        CHECK(retry.omegaOpeningExecutor.queued() == 0);
    }
    // A later non-entered report for a duplicate monitor wins within one packet.
    actual = {}; legacy = {};
    static_cast<void>(intake::capture(actual.omegaOpeningExecutor, context(1), roster));
    frozen_opening::apply(legacy, roster, context(1));
    merged.objectCount = 4;
    merged.objects[3] = merged.objects[2]; merged.objects[3].hasDelta = false;
    static_cast<void>(intake::capture(actual.omegaOpeningExecutor, context(2), merged));
    frozen_opening::apply(legacy, merged, context(2));
    static_cast<void>(opening::update_observer(actual, 7));
    same_wire(legacy.activity.sensorObservation, actual);
    CHECK(!actual.omegaOpeningTriggered && !actual.omegaOpeningExecutor.authority().released);
    // Malformed native scene metadata must fail extraction at intake.
    merged.objectCount = 3; merged.objects[0].bodyBits = 34;
    static_cast<void>(intake::capture(actual.omegaOpeningExecutor, context(3), merged));
    frozen_opening::apply(legacy, merged, context(3));
    static_cast<void>(opening::update_observer(actual, 7));
    same_wire(legacy.activity.sensorObservation, actual);
    CHECK(actual.omegaOpeningTriggered && !actual.omegaOpeningExecutor.authority().released);
}

void queue_lifetimes() {
    opening::Run run;
    CHECK(run.enqueue({7, 1, opening::Kind::roster}));
    CHECK(run.enqueue({7, 2, opening::Kind::approach}));
    static_cast<void>(run.update(8));
    CHECK(!run.authority().requested && !run.authority().roster);
    CHECK(run.enqueue({8, 3, opening::Kind::roster}));
    CHECK(run.enqueue({8, 4, opening::Kind::approach}));
    CHECK(run.enqueue({8, 5, opening::Kind::reset}));
    CHECK(run.enqueue({8, 6, opening::Kind::scene, 0x80EC0F96, 3, true, true}));
    CHECK(run.enqueue({8, 7, opening::Kind::roster}));
    static_cast<void>(run.update(8));
    CHECK(run.authority().roster && !run.authority().requested && !run.authority().released);
    const auto incarnation = run.diagnostics().incarnation;
    CHECK(run.enqueue({8, 8, opening::Kind::approach}));
    static_cast<void>(run.update(8));
    for (const auto receipt : {
        opening::Receipt{8, 9, opening::Kind::scene, 0x80EC0F95, 3, true, true},
        opening::Receipt{8, 10, opening::Kind::scene, 0x80EC0F96, 0, true, true},
        opening::Receipt{8, 11, opening::Kind::scene, 0x80EC0F96, 3, false, true}}) {
        CHECK(run.enqueue(receipt)); static_cast<void>(run.update(8)); CHECK(!run.authority().released);
    }
    CHECK(run.enqueue({8, 12, opening::Kind::scene, 0x80EC0F96, 4, true, false}));
    static_cast<void>(run.update(8));
    CHECK(run.diagnostics().active & (1U << 3)); // Scene ready, output still missing.
    CHECK(run.enqueue({8, 13, opening::Kind::scene, 0x80EC0F96, 3, true, true}));
    CHECK(run.enqueue({8, 14, opening::Kind::scene, 0x80EC0F96, 4, true, true}));
    static_cast<void>(run.update(8)); CHECK(!run.authority().released);
    CHECK(run.enqueue({8, 15, opening::Kind::scene, 0x80EC0F96, 5, true, true}));
    static_cast<void>(run.update(8)); CHECK(run.authority().released);
    CHECK(run.diagnostics().incarnation == incarnation);
    for (std::size_t i = 0; i < opening::Run::kCapacity; ++i) {
        CHECK(run.enqueue({8, 16, opening::Kind::entrance}));
    }
    CHECK(!run.enqueue({8, 17, opening::Kind::entrance}));
    CHECK(run.authority().released); // Intake does not retire leases inline.
    CHECK(run.update(8).failed);
    CHECK(run.diagnostics().phase == coo::Phase::failed);
    CHECK(run.diagnostics().failure == coo::Failure::queueOverflow);
    CHECK(!run.update(8).failed);
    CHECK(run.authority().failed && !run.authority().released && !run.authority().requested);
    CHECK(!run.enqueue({8, 18, opening::Kind::roster}));
}

// Use the actual production Session, including its secure-wipe/reassignment
// lifecycle. Policy-only Observer fixtures do not exercise this boundary.
static dawn::server::bap::Session resetSession;
__declspec(noinline) void execute_after_session_reset() {
    namespace lifecycle = dawn::server::bap::lifecycle;
    const auto roster = decode(0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        resetSession.id = 17;
        const auto connection = lifecycle::connection_owned_state(resetSession);
        const auto pending = lifecycle::deferred_matchmaking_retirement(resetSession);
        SecureZeroMemory(&resetSession, sizeof resetSession);
        if (mode == 0) { resetSession = dawn::server::bap::Session{}; }
        else if (mode == 1) { lifecycle::restore_connection_owned_state(resetSession, connection); }
        else { lifecycle::restore_deferred_matchmaking_retirement(resetSession, pending); }
        CHECK(resetSession.id == (mode == 0 ? 0U : 17U));
        CHECK(resetSession.activity.advertisedRegion == -1);
        CHECK(resetSession.matchmakingRetirementPending == (mode == 2));
        auto& observer = resetSession.activity.sensorObservation;
        CHECK(observer.omegaOpeningExecutor.queued() == 0);
        CHECK(!observer.omegaOpeningExecutor.authority().roster);
        CHECK(!observer.omegaOpeningExecutor.authority().requested);
        static_cast<void>(intake::capture(observer.omegaOpeningExecutor, context(1), roster));
        static_cast<void>(opening::update_observer(observer, 7));
        CHECK(observer.omegaOpeningExecutor.authority().roster);
        CHECK(observer.omegaOpeningExecutor.enqueue({7, 2, opening::Kind::approach}));
        static_cast<void>(opening::update_observer(observer, 7));
        CHECK(observer.omegaOpeningExecutor.authority().requested);
        CHECK(observer.omegaOpeningExecutor.enqueue({7, 3, opening::Kind::entrance}));
        // The next secure reset must discard both the active request and queued edge.
    }
}
int guarded_session_reset() {
    __try { execute_after_session_reset(); return 0; }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, "Session reset raised native exception 0x%08lX\n", GetExceptionCode());
        return 1;
    }
}
int main(int argc, char**) {
    if (argc > 1) { return guarded_session_reset(); }
    CHECK(guarded_session_reset() == 0);
    captured_replay(1); captured_replay(7); captured_replay(40);
    complete_loading_roster();
    adversarial_receipts(); coalesced_and_retry(); queue_lifetimes();
    std::printf("PASS: %u checks; %u authority bodies; 40 captured opening packets at 3 update cadences, reset/direct-start/stale/overflow and 3 production secure-reset lifecycles\n", checks, bodies);
}
