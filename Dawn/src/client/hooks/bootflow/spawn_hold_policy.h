#pragma once

#include <cstdint>
#include <string_view>

namespace dawn::client::hooks::bootflow::spawn_hold_policy {

/** World-transition state needed by the spawn/fade decision. */
enum class Phase : std::uint8_t {
    idle,
    transitioning,
    arrived,
};

/** Immutable inputs captured by one admitted spawn-gate call. */
struct Input final {
    bool nativeAllowed{};
    Phase phase{Phase::idle};
    bool holdEnabled{};
    bool timedOut{};
    bool alreadyReleased{};
    bool loaderBusy{};
    /** A fast-travel player replacement must finish on the frame's readiness witnesses. */
    bool playerReplacement{};
};

inline constexpr std::uint32_t kNoControlledEntity = UINT32_MAX;

/** Implemented patrols and the Tower replace players without a new boot transition. */
[[nodiscard]] constexpr bool fast_travel_destination(std::string_view name) noexcept {
    return name == "mercury_freeroam" || name == "polaris_freeroam"
        || name == "fleet_freeroam" || name == "planet_x_freeroam"
        || name == "eden_freeroam" || name == "tangled_shore_freeroam"
        || name == "dreaming_city_freeroam" || name == "city_tower_social_d2";
}

/**
 * Fast travel can replace the player without leaving boot step 38. Compare the
 * complete salted handle, not its reusable pool index. An initial observation
 * is not a replacement; a disappearance or a direct replacement is one.
 */
[[nodiscard]] constexpr bool player_replaced(Phase phase, bool patrol,
    std::uint32_t previous, std::uint32_t current) noexcept {
    return patrol && phase == Phase::arrived && previous != kNoControlledEntity
        && current != previous;
}

/** Pure spawn suppression and fade-release decision for one admitted native answer. */
struct Decision final {
    bool loaderLoading{};
    bool loading{};
    bool releaseFade{};
    bool result{};
};

/** Native witnesses required before Towerfall may replace the unreachable migration latch. */
struct TowerfallReadiness final {
    bool nativeAllowed{};
    Phase phase{Phase::idle};
    bool initialSliceComplete{};
    bool worldReadable{};
    std::int32_t worldState{-1};
    bool localReady{};
    bool scriptRuntime{};
    bool directorRuntime{};
};

/**
 * Accepts only Towerfall's fully arrived native spawn boundary. World state 3 is the in-world
 * state observed by the retail spawn gate; type 18 and type 35 prove the script and director
 * runtimes were constructed rather than merely listed in the roster.
 */
[[nodiscard]] constexpr bool towerfall_ready(const TowerfallReadiness& input) noexcept {
    return input.nativeAllowed && input.phase == Phase::arrived
           && input.initialSliceComplete && input.worldReadable && input.worldState == 3
           && input.localReady && input.scriptRuntime && input.directorRuntime;
}

/** Native storage witnesses for Omega; this does not assert that its scene has started. */
struct OmegaRuntimeReadiness final {
    bool selected{};
    Phase phase{Phase::idle};
    bool playerReady{}, worldReadable{};
    std::int32_t worldState{-1};
    bool localReady{}, scriptFound{}, directorFound{};
    std::uint32_t scriptDatum{UINT32_MAX}, directorDatum{UINT32_MAX};
    std::uint32_t scriptComponent{}, directorComponent{};
    std::uint64_t scriptOffset{}, directorOffset{};
};

/** The active spawn/frame owner observes constructed slots after native arrival. */
[[nodiscard]] constexpr bool omega_runtime_ready(const OmegaRuntimeReadiness& input) noexcept {
    return input.selected && input.phase == Phase::arrived && input.playerReady
        && input.worldReadable && input.worldState == 3 && input.localReady
        && input.scriptFound && input.directorFound
        && input.scriptDatum != UINT32_MAX && input.directorDatum != UINT32_MAX
        && input.scriptComponent == 0x80809917U && input.directorComponent == 0x808099BDU
        && input.scriptOffset == 0 && input.directorOffset == 0;
}

/** Evidence for completing a pending arrival after the native player already spawned. */
struct FrameArrival final {
    Phase phase{Phase::idle};
    bool alreadyReleased{};
    bool controlledEntity{};
    bool worldReadable{};
    std::int32_t worldState{-1};
    bool localReady{};
    bool loaderReadable{};
    bool loaderBusy{};
};

/** A frame may finish the fade only with current player ownership and a fully loaded world. */
[[nodiscard]] constexpr bool frame_arrival_ready(const FrameArrival& input) noexcept {
    return input.phase == Phase::arrived && !input.alreadyReleased && input.controlledEntity
           && input.worldReadable && input.worldState == 3 && input.localReady
           && input.loaderReadable && !input.loaderBusy;
}

/**
 * Keeps spawn suppression and fade release on opposite sides of the arrival boundary.
 * Fade release is legal only after arrival, once no configured hold remains, and only once.
 */
[[nodiscard]] constexpr Decision decide(const Input& input) noexcept {
    const bool loaderLoading =
        input.phase == Phase::arrived && !input.alreadyReleased && input.loaderBusy;
    const bool loading = input.holdEnabled && !input.timedOut && !input.alreadyReleased
                         && (input.phase == Phase::transitioning || loaderLoading);
    return Decision{
        loaderLoading,
        loading,
        input.phase == Phase::arrived && !loading && !input.alreadyReleased
            && !input.playerReplacement,
        input.nativeAllowed && !loading,
    };
}

} // namespace dawn::client::hooks::bootflow::spawn_hold_policy
