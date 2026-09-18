#include <cstdio>
#include <cstdlib>
#include <limits>
#include "state/activity/omega_boss_teleport_action.h"

namespace teleport = dawn::state::activity::omega_boss_teleport;
namespace {
unsigned checks{};
void check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
constexpr std::uint64_t request = 7, motion = 0x123400000001ULL;
// The final move's selector adjusts the authored destination before placement.
constexpr std::array<float, 3> requested{-1490.12109375F, -491.90216064453125F, -32.849102F};
constexpr std::array<float, 3> placed{-1490.12109375F, -491.90216064453125F, -33.20964813232422F};
teleport::Owner owner(std::uint8_t island = 4) {
    teleport::Owner value{};
    value.run = 1; value.actor = 2; value.character = 3; value.entity = 4;
    value.generation = 1; value.revision = 2; value.biped = 5;
    value.selector = 6; value.interfaceHandle = 7; value.movementBody = 8;
    value.island = island; value.actionEpoch = 13;
    return value;
}
teleport::CycleTracker departing(const teleport::Owner& value) {
    teleport::CycleTracker tracker;
    tracker.begin_run(value.run);
    check(tracker.claim(value, request, true, requested, 0.25F), "claim exact native request");
    check(tracker.bind_destination(value, request, motion, requested, placed, placed),
        "bind independently observed native placement");
    check(tracker.observe(value, request, 1, motion, {true, teleport::Stage::departure, true})
        == teleport::CycleEvent::started, "observe actual departure");
    return tracker;
}
void final_move() {
    const auto value = owner();
    auto tracker = departing(value);
    check(tracker.observe(value, request, 2, motion, {true, teleport::Stage::relocation, false})
        == teleport::CycleEvent::finished, "final callback ends after native placement");
    check(tracker.phase() == teleport::CyclePhase::finished,
        "terminal update alone does not complete traversal");
    check(tracker.cleanup(value, request, 3, motion, true, placed)
        == teleport::CycleEvent::completed, "owned inactive selector at placed destination completes");
    check(tracker.cleanup(value, request, 4, motion, true, placed)
        == teleport::CycleEvent::none, "completion cannot repeat");
    for (unsigned failure = 0; failure < 5; ++failure) {
        tracker = departing(value);
        check(tracker.observe(value, request, 2, motion, {true, teleport::Stage::relocation, false})
            == teleport::CycleEvent::finished, "terminal receipt before cleanup rejection");
        auto actual = placed;
        if (failure == 1) actual = requested;
        if (failure == 2) actual[0] += 0.26F;
        if (failure == 3) actual[1] = std::numeric_limits<float>::quiet_NaN();
        if (failure == 4) actual[2] = std::numeric_limits<float>::infinity();
        check(tracker.cleanup(value, request, 3, motion, failure != 0, actual)
            == teleport::CycleEvent::interrupted,
            "active selector, wrong destination and nonfinite positions cannot release cannon");
    }
    tracker = departing(value);
    check(tracker.cleanup(value, request, 2, motion, true, placed)
        == teleport::CycleEvent::interrupted, "placement alone without terminal callback is insufficient");
}
void exact_ownership() {
    const auto value = owner();
    for (unsigned failure = 0; failure < 7; ++failure) {
        auto tracker = departing(value);
        auto other = value;
        if (failure == 0) ++other.actionEpoch;
        if (failure == 1) ++other.selector;
        if (failure == 2) ++other.run;
        check(tracker.observe(other, request + (failure == 3), failure == 4 ? 1 : 2,
            motion + (failure == 5), {failure != 6, teleport::Stage::relocation, false})
            == teleport::CycleEvent::none, "stale or unowned terminal receipt is ignored");
        check(tracker.phase() == teleport::CyclePhase::departing, "rejection cannot finish the move");
        check(tracker.observe(value, request, 3, motion, {true, teleport::Stage::relocation, false})
            == teleport::CycleEvent::finished, "valid later terminal receipt is accepted");
        check(tracker.cleanup(other, request + (failure == 3), failure == 4 ? 3 : 4,
            motion + (failure == 5), true, placed)
            == (failure == 6 ? teleport::CycleEvent::completed : teleport::CycleEvent::none),
            "cleanup must match owner, request, motion and fresh serial");
    }
    teleport::CycleTracker unbound;
    unbound.begin_run(value.run);
    check(unbound.claim(value, request, true, requested, 0.25F), "claim without placement binding");
    check(unbound.observe(value, request, 1, motion, {true, teleport::Stage::relocation, false})
        == teleport::CycleEvent::none, "unbound callback cannot finish");
    check(unbound.cleanup(value, request, 2, motion, true, placed)
        == teleport::CycleEvent::none, "unbound cleanup cannot finish");
}
void earlier_moves() {
    for (std::uint8_t island = 0; island < 4; ++island) {
        const auto value = owner(island);
        auto tracker = departing(value);
        check(tracker.observe(value, request, 2, motion, {true, teleport::Stage::relocation, false})
            == teleport::CycleEvent::interrupted, "early callback stop remains cancellation outside final arena");
        check(tracker.cleanup(value, request, 3, motion, true, placed)
            == teleport::CycleEvent::none, "cancelled earlier move cannot complete");
    }
    for (std::uint8_t island = 0; island <= 4; ++island) {
        const auto value = owner(island);
        auto tracker = departing(value);
        check(tracker.observe(value, request, 2, motion, {true, teleport::Stage::relocation, true})
            == teleport::CycleEvent::relocated, "continuing callback follows ordinary arrival path");
        check(tracker.observe(value, request, 3, motion, {true, teleport::Stage::arrival, false})
            == teleport::CycleEvent::finished, "ordinary terminal arrival preserved");
        check(tracker.cleanup(value, request, 4, motion, true, placed)
            == teleport::CycleEvent::completed, "ordinary cleanup still completes");
    }
}
}
int main() {
    final_move(); exact_ownership(); earlier_moves();
    std::printf("PASS Omega teleport completion: %u checks\n", checks);
}
