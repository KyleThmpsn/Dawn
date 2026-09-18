#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "client/hooking/call_gate.h"
#include "client/hooks/bootflow/spawn_hold_policy.h"
#include "state/activity/tower_spawn_recovery.h"
#include "state/activity/runtime.h"

namespace {

namespace policy = dawn::client::hooks::bootflow::spawn_hold_policy;
using dawn::client::hooking::CallGate;

using NativeCall = bool (*)(std::int32_t) noexcept;

[[nodiscard]] constexpr policy::Input input(bool nativeAllowed,
                                            policy::Phase phase,
                                            bool holdEnabled,
                                            bool timedOut,
                                            bool alreadyReleased,
                                            bool loaderBusy,
                                            bool playerReplacement = false) noexcept {
    return policy::Input{
        nativeAllowed, phase, holdEnabled, timedOut, alreadyReleased, loaderBusy, playerReplacement};
}

constexpr policy::Decision kTransitioningHold =
    policy::decide(input(true, policy::Phase::transitioning, true, false, false, false));
static_assert(kTransitioningHold.loading);
static_assert(!kTransitioningHold.releaseFade);
static_assert(!kTransitioningHold.result);

constexpr policy::Decision kArrivedLoaderBusy =
    policy::decide(input(true, policy::Phase::arrived, true, false, false, true));
static_assert(kArrivedLoaderBusy.loaderLoading);
static_assert(kArrivedLoaderBusy.loading);
static_assert(!kArrivedLoaderBusy.releaseFade);
static_assert(!kArrivedLoaderBusy.result);

constexpr policy::Decision kArrivedReady =
    policy::decide(input(true, policy::Phase::arrived, true, false, false, false));
static_assert(!kArrivedReady.loading);
static_assert(kArrivedReady.releaseFade);
static_assert(kArrivedReady.result);

constexpr policy::Decision kArrivedTimedOut =
    policy::decide(input(true, policy::Phase::arrived, true, true, false, true));
static_assert(kArrivedTimedOut.loaderLoading);
static_assert(!kArrivedTimedOut.loading);
static_assert(kArrivedTimedOut.releaseFade);
static_assert(kArrivedTimedOut.result);

constexpr policy::Decision kTransitionTimedOut =
    policy::decide(input(true, policy::Phase::transitioning, true, true, false, false));
static_assert(!kTransitionTimedOut.loading);
static_assert(!kTransitionTimedOut.releaseFade);
static_assert(kTransitionTimedOut.result);

constexpr policy::Decision kAlreadyReleased =
    policy::decide(input(true, policy::Phase::arrived, true, false, true, true));
static_assert(!kAlreadyReleased.loaderLoading);
static_assert(!kAlreadyReleased.loading);
static_assert(!kAlreadyReleased.releaseFade);
static_assert(kAlreadyReleased.result);

[[nodiscard]] constexpr policy::TowerfallReadiness towerfall_ready_input() noexcept {
    return policy::TowerfallReadiness{
        true, policy::Phase::arrived, true, true, 3, true, true, true};
}

static_assert(policy::towerfall_ready(towerfall_ready_input()));
static_assert(!policy::towerfall_ready([] {
    auto value = towerfall_ready_input();
    value.initialSliceComplete = false;
    return value;
}()));
static_assert(!policy::towerfall_ready([] {
    auto value = towerfall_ready_input();
    value.scriptRuntime = false;
    return value;
}()));
static_assert(!policy::towerfall_ready([] {
    auto value = towerfall_ready_input();
    value.directorRuntime = false;
    return value;
}()));
static_assert(!policy::towerfall_ready([] {
    auto value = towerfall_ready_input();
    value.worldState = 2;
    return value;
}()));

/** Exhaustively checks the source-linked pure policy over every Boolean input combination. */
[[nodiscard]] constexpr bool policy_truth_table_holds() noexcept {
    constexpr std::array phases{
        policy::Phase::idle,
        policy::Phase::transitioning,
        policy::Phase::arrived,
    };
    constexpr std::array values{false, true};
    for (const policy::Phase phase : phases) {
        for (const bool nativeAllowed : values) {
            for (const bool holdEnabled : values) {
                for (const bool timedOut : values) {
                    for (const bool alreadyReleased : values) {
                        for (const bool loaderBusy : values) {
                          for (const bool playerReplacement : values) {
                            const policy::Decision decision = policy::decide(input(nativeAllowed,
                                                                                   phase,
                                                                                   holdEnabled,
                                                                                   timedOut,
                                                                                   alreadyReleased,
                                                                                   loaderBusy,
                                                                                   playerReplacement));
                            const bool expectedLoaderLoading =
                                phase == policy::Phase::arrived && !alreadyReleased && loaderBusy;
                            const bool expectedLoading =
                                holdEnabled && !timedOut && !alreadyReleased
                                && (phase == policy::Phase::transitioning || expectedLoaderLoading);
                            const bool expectedRelease = phase == policy::Phase::arrived
                                                         && !expectedLoading && !alreadyReleased
                                                         && !playerReplacement;
                            if (decision.loaderLoading != expectedLoaderLoading
                                || decision.loading != expectedLoading
                                || decision.releaseFade != expectedRelease
                                || decision.result != (nativeAllowed && !expectedLoading)) {
                                return false;
                            }
                          }
                        }
                    }
                }
            }
        }
    }
    return true;
}

static_assert(policy_truth_table_holds());

/**
 * Generic CallGate model only. This exercises publication and admission primitives; it does not
 * simulate production detour removal or claim coverage of spawn-owner state retention.
 */
class GenericCallGateForwarder final {
public:
    void accept() noexcept {
        gate_.accept();
    }

    void quiesce() noexcept {
        gate_.quiesce();
    }

    void publish(NativeCall original) noexcept {
        dawn::client::hooking::publish_original(original_, original);
    }

    [[nodiscard]] bool invoke(std::int32_t datum) noexcept {
        CallGate::Scope call(gate_);
        const NativeCall original = dawn::client::hooking::await_original(original_);
        const bool result = original(datum);
        if (call.accepts_side_effects()) {
            sideEffectCalls_.fetch_add(1U, std::memory_order_relaxed);
        }
        return result;
    }

    [[nodiscard]] bool idle() const noexcept {
        return gate_.idle();
    }

    [[nodiscard]] std::uint32_t active_calls() const noexcept {
        return gate_.active_calls();
    }

    [[nodiscard]] std::uint32_t side_effect_calls() const noexcept {
        return sideEffectCalls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<NativeCall> original_{};
    std::atomic_uint32_t sideEffectCalls_{};
    CallGate gate_{};
};

int g_failureCount = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::cerr << __FILE__ << ':' << line << ": check failed: " << expression << '\n';
    ++g_failureCount;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

std::atomic_uint32_t g_nativeCalls{};
std::atomic_bool g_nativeEntered{};
std::atomic_bool g_releaseNative{};

bool immediate_native(std::int32_t datum) noexcept {
    g_nativeCalls.fetch_add(1U, std::memory_order_relaxed);
    return datum == 41;
}

bool blocked_native(std::int32_t datum) noexcept {
    g_nativeCalls.fetch_add(1U, std::memory_order_relaxed);
    g_nativeEntered.store(true, std::memory_order_release);
    g_nativeEntered.notify_all();
    g_releaseNative.wait(false, std::memory_order_acquire);
    return datum == 41;
}

void reset_native_barrier() noexcept {
    g_nativeCalls.store(0U, std::memory_order_relaxed);
    g_nativeEntered.store(false, std::memory_order_relaxed);
    g_releaseNative.store(false, std::memory_order_relaxed);
}

template <typename Predicate> [[nodiscard]] bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

void frame_completion_after_last_spawn_call() {
    // Gateway: the timeout lets the native player spawn during loading. There are no
    // more spawn-gate calls after this, so only the camera frame can finish arrival.
    const auto lastSpawn = policy::decide(
        input(true, policy::Phase::transitioning, true, true, false, false));
    CHECK(lastSpawn.result);
    CHECK(!lastSpawn.releaseFade);
    policy::FrameArrival frame{
        policy::Phase::transitioning, false, true, true, 3, true, true, false};
    CHECK(!policy::frame_arrival_ready(frame));
    frame.phase = policy::Phase::arrived;
    frame.loaderBusy = true;
    CHECK(!policy::frame_arrival_ready(frame));
    frame.loaderBusy = false;
    CHECK(policy::frame_arrival_ready(frame));
    frame.alreadyReleased = true;
    CHECK(!policy::frame_arrival_ready(frame));
}

void frame_completion_requires_current_native_evidence() {
    const policy::FrameArrival ready{
        policy::Phase::arrived, false, true, true, 3, true, true, false};
    auto frame = ready;
    frame.controlledEntity = false;
    CHECK(!policy::frame_arrival_ready(frame));
    frame = ready;
    frame.worldReadable = false;
    CHECK(!policy::frame_arrival_ready(frame));
    frame = ready;
    frame.worldState = 2;
    CHECK(!policy::frame_arrival_ready(frame));
    frame = ready;
    frame.localReady = false;
    CHECK(!policy::frame_arrival_ready(frame));
    frame = ready;
    frame.loaderReadable = false;
    CHECK(!policy::frame_arrival_ready(frame));
    frame = ready;
    frame.phase = policy::Phase::idle;
    CHECK(!policy::frame_arrival_ready(frame));
    // A completed prior arrival must not complete another load while it is still transitioning.
    frame = ready;
    frame.alreadyReleased = true;
    frame.phase = policy::Phase::transitioning;
    CHECK(!policy::frame_arrival_ready(frame));
}

void generic_publication_window_waits_then_forwards_exactly_once() {
    reset_native_barrier();
    GenericCallGateForwarder forwarder{};
    forwarder.accept();

    bool result = false;
    std::thread caller([&forwarder, &result] { result = forwarder.invoke(41); });
    const bool entered = wait_until([&forwarder] { return forwarder.active_calls() == 1U; });
    CHECK(entered);
    CHECK(g_nativeCalls.load(std::memory_order_relaxed) == 0U);

    forwarder.publish(&immediate_native);
    caller.join();

    CHECK(result);
    CHECK(g_nativeCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(forwarder.side_effect_calls() == 1U);
    CHECK(forwarder.idle());
}

void patrol_fast_travel_rearms_without_a_boot_transition() {
    constexpr auto oldPlayer = 0x3DFAA417U;
    constexpr auto newPlayerSameSlot = 0x3EFAA417U;
    constexpr auto absent = policy::kNoControlledEntity;
    CHECK(policy::fast_travel_destination("tangled_shore_freeroam"));
    CHECK(policy::fast_travel_destination("planet_x_freeroam"));
    CHECK(policy::fast_travel_destination("city_tower_social_d2"));
    CHECK(!policy::fast_travel_destination("mission_launchpad"));
    CHECK(!policy::fast_travel_destination("mission_scot"));
    CHECK(!policy::fast_travel_destination(""));
    CHECK(!policy::player_replaced(policy::Phase::arrived, true, absent, oldPlayer));
    CHECK(!policy::player_replaced(policy::Phase::arrived, true, oldPlayer, oldPlayer));
    CHECK(policy::player_replaced(policy::Phase::arrived, true, oldPlayer, absent));
    CHECK(!policy::player_replaced(policy::Phase::arrived, true, absent, absent));
    CHECK(policy::player_replaced(policy::Phase::arrived, true, oldPlayer, newPlayerSameSlot));
    CHECK(!policy::player_replaced(policy::Phase::transitioning, true, oldPlayer, absent));
    CHECK(!policy::player_replaced(policy::Phase::arrived, false, oldPlayer, absent));
    // The old launch latch blocks completion even after the new player appears.
    policy::FrameArrival frame{policy::Phase::arrived, true, true, true, 3, true, true, false};
    CHECK(!policy::frame_arrival_ready(frame));
    frame.alreadyReleased = !policy::player_replaced(frame.phase, true, oldPlayer, newPlayerSameSlot);
    CHECK(policy::frame_arrival_ready(frame));
    frame.controlledEntity = false;
    CHECK(!policy::frame_arrival_ready(frame));
    frame.controlledEntity = true;
    frame.loaderBusy = true;
    CHECK(!policy::frame_arrival_ready(frame));
    frame.loaderBusy = false;
    frame.alreadyReleased = true;
    CHECK(!policy::frame_arrival_ready(frame));
    // A timeout or native spawn permission cannot bypass the frame witnesses.
    for (bool allowed : {false, true}) {
        for (bool busy : {false, true}) {
            auto pending = input(allowed, policy::Phase::arrived, true, true, false, busy);
            pending.playerReplacement = true;
            const auto decision = policy::decide(pending);
            CHECK(!decision.releaseFade);
            CHECK(decision.result == allowed);
        }
    }
}

/** Policy sequence model: repeated travel, death/respawn, and leaving the patrol lifetime. */
void patrol_arrival_sequences() {
    constexpr auto absent = policy::kNoControlledEntity;
    std::uint32_t previous = absent;
    bool released = false;
    unsigned releases = 0;
    const auto tick = [&](policy::Phase phase, bool patrol, std::uint32_t player, bool busy) {
        if (phase != policy::Phase::arrived) {
            previous = absent;
            released = false;
            return;
        }
        if (policy::player_replaced(phase, patrol, previous, player)) {
            released = false;
        }
        previous = patrol ? player : absent;
        if (policy::frame_arrival_ready(
                {phase, released, player != absent, true, 3, true, true, busy})) {
            released = true;
            ++releases;
        }
    };
    tick(policy::Phase::arrived, true, 0x01000417U, false);
    CHECK(releases == 1);
    for (unsigned travel = 2; travel <= 4; ++travel) {
        tick(policy::Phase::arrived, true, absent, true);
        tick(policy::Phase::arrived, true, absent, false);
        CHECK(releases == travel - 1);
        const auto player = (travel << 24U) | 0x417U;
        tick(policy::Phase::arrived, true, player, true);
        CHECK(releases == travel - 1);
        tick(policy::Phase::arrived, true, player, false);
        tick(policy::Phase::arrived, true, player, false);
        CHECK(releases == travel);
    }
    // Death/respawn and a replacement between camera polls share this same safe boundary.
    tick(policy::Phase::arrived, true, 0x05000417U, false);
    CHECK(releases == 5);
    tick(policy::Phase::idle, false, absent, false);
    tick(policy::Phase::transitioning, false, 0x06000417U, false);
    CHECK(releases == 5);
    tick(policy::Phase::arrived, false, 0x06000417U, false);
    CHECK(releases == 6);
    tick(policy::Phase::arrived, false, 0x07000417U, false);
    CHECK(releases == 6); // Mission player replacements do not gain this patrol-only behavior.
}

void tower_recovery_requires_rebuilt_uninitialized_world() {
    namespace recovery=dawn::state::activity::tower_spawn_recovery;
    recovery::Watch watch;
    recovery::Observation o{0x3EF90002,false,false,true,true,true};
    CHECK(!watch.observe(o,1000));CHECK(!watch.observe(o,2000)); // No prior arrival.
    o.player=o.worldReady=true;CHECK(!watch.observe(o,2001));
    o.player=false;o.worldReady=false;
    CHECK(!watch.observe(o,3000));CHECK(!watch.observe(o,4000)); // Same salted object.
    o.lifetime=0x67F90024;o.loaderIdle=false;
    CHECK(!watch.observe(o,5000));CHECK(!watch.observe(o,6000)); // Still loading.
    o.loaderIdle=true;CHECK(!watch.observe(o,7000));
    CHECK(!watch.observe(o,7499));CHECK(watch.observe(o,7500));
    CHECK(!watch.observe(o,10000)); // No retry loop that keeps destroying the spawn root.
    o.lifetime=0x68F90024;CHECK(!watch.observe(o,12000));
    o.player=o.worldReady=true;CHECK(!watch.observe(o,12001));
    o.player=false;CHECK(!watch.observe(o,13000)); // Normal death with initialized world.
    o.worldReady=false;o.lifetime=0x69F90024;o.localReady=false;
    CHECK(!watch.observe(o,14000));o.localReady=true;
    CHECK(!watch.observe(o,15000));CHECK(watch.observe(o,15500)); // Second travel.
    watch.reset();CHECK(!watch.observe(o,20000));CHECK(!watch.observe(o,25000));
}

void omega_runtime_arrival_witness() {
    namespace activity = dawn::state::activity;
    // Captured from the failed live run at t=108781: the native runtime existed,
    // but only a detached legacy post-apply probe could acknowledge it.
    const policy::OmegaRuntimeReadiness captured{
        true, policy::Phase::arrived, true, true, 3, true, true, true,
        0x64F90002U, 0x4AF90001U, 0x80809917U, 0x808099BDU, 0, 0};
    CHECK(policy::omega_runtime_ready(captured));
    for (unsigned missing = 0; missing < 6; ++missing) {
        auto value = captured;
        switch (missing) {
        case 0: value.selected = false; break;
        case 1: value.playerReady = false; break;
        case 2: value.worldReadable = false; break;
        case 3: value.localReady = false; break;
        case 4: value.scriptFound = false; break;
        case 5: value.directorFound = false; break;
        }
        CHECK(!policy::omega_runtime_ready(value));
    }
    for (auto phase : {policy::Phase::idle, policy::Phase::transitioning}) {
        auto value = captured; value.phase = phase;
        CHECK(!policy::omega_runtime_ready(value));
    }
    for (int world : {-1, 0, 1, 2, 4}) {
        auto value = captured; value.worldState = world;
        CHECK(!policy::omega_runtime_ready(value));
    }
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        auto value = captured;
        switch (invalid) {
        case 0: value.scriptDatum = UINT32_MAX; break;
        case 1: value.directorDatum = UINT32_MAX; break;
        case 2: value.scriptComponent = 0x80809919U; break; // Schema is not a runtime.
        case 3: value.directorComponent = 0x808099BFU; break;
        case 4: value.scriptOffset = 8; break;
        case 5: value.directorOffset = 8; break;
        }
        CHECK(!policy::omega_runtime_ready(value));
    }
    activity::reset_mission_authority_runtime_initialization();
    CHECK(!activity::mission_authority_runtime_initialized());
    if (policy::omega_runtime_ready(captured)) {
        CHECK(activity::acknowledge_mission_authority_runtime_initialized());
    }
    CHECK(activity::mission_authority_runtime_initialized());
    CHECK(!activity::acknowledge_mission_authority_runtime_initialized());
    activity::reset_mission_authority_runtime_initialization();
    CHECK(!activity::mission_authority_runtime_initialized());
}

void generic_quiesced_call_forwards_without_side_effects() {
    reset_native_barrier();
    GenericCallGateForwarder forwarder{};
    forwarder.publish(&immediate_native);
    forwarder.accept();
    forwarder.quiesce();

    CHECK(forwarder.invoke(41));
    CHECK(g_nativeCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(forwarder.side_effect_calls() == 0U);
    CHECK(forwarder.idle());
}

void generic_active_call_stays_owned_through_native_interval() {
    reset_native_barrier();
    GenericCallGateForwarder forwarder{};
    forwarder.publish(&blocked_native);
    forwarder.accept();

    bool result = false;
    std::thread caller([&forwarder, &result] { result = forwarder.invoke(41); });
    const bool entered = wait_until([] { return g_nativeEntered.load(std::memory_order_acquire); });
    CHECK(entered);
    forwarder.quiesce();
    CHECK(!forwarder.idle());
    CHECK(forwarder.active_calls() == 1U);

    g_releaseNative.store(true, std::memory_order_release);
    g_releaseNative.notify_all();
    caller.join();

    CHECK(result);
    CHECK(g_nativeCalls.load(std::memory_order_relaxed) == 1U);
    CHECK(forwarder.side_effect_calls() == 0U);
    CHECK(forwarder.idle());
}

} // namespace

int main() {
    frame_completion_after_last_spawn_call();
    frame_completion_requires_current_native_evidence();
    patrol_fast_travel_rearms_without_a_boot_transition();
    patrol_arrival_sequences();
    tower_recovery_requires_rebuilt_uninitialized_world();
    omega_runtime_arrival_witness();
    generic_publication_window_waits_then_forwards_exactly_once();
    generic_quiesced_call_forwards_without_side_effects();
    generic_active_call_stays_owned_through_native_interval();

    if (g_failureCount != 0) {
        std::cerr << g_failureCount << " call-gate/spawn-policy check(s) failed\n";
        return 1;
    }

    std::cout << "all call-gate and source-linked spawn-policy checks passed\n";
    return 0;
}
