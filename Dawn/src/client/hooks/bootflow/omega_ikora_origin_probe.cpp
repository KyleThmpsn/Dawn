#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/activity/forced/activity_forced_destination.h"
#include "../../../state/activity/Newlight/launchpad/runtime.h"
#include "../../../state/activity/Newlight/launchpad/shutter.h"
#include "../../hooking/call_gate.h"
#include "../../hooking/detour.h"
#include "internal.h"
#include "omega_enemy_lair_receipts.h"
#include "../../../state/activity/omega_presentation.h"
#include "../../../state/activity/omega_first_lair_runtime.h"

namespace dawn::client::hooks::bootflow {
namespace {

// Pinned-client targets and prefixes shared with the retired, much broader
// activity_spawner_chain_probe. This treatment hooks only the two source boundaries, the common
// entity factory needed to classify Omega's duplicate Ikora, and the lifecycle tail-dispatch used
// to hide the exact three render models owned by Scene 80EC0FA8.
constexpr std::uintptr_t kSpawnerDeficitRva = 0x4E4580U;
constexpr std::uintptr_t kSceneActorSchedulerRva = 0x5902C0U;
constexpr std::uintptr_t kEntityFactoryRva = 0x56D9B0U;
constexpr std::uintptr_t kComponentStartRva = 0xB31910U;
constexpr std::uintptr_t kEffectTransformComposeRva = 0x11F0510U;
// Actual 808062FD child-scene creator and selector parameter-to-object resolver.
// These receipts preserve every argument/result; they do not repair actor ownership.
constexpr std::uintptr_t kSelectorChildCreateRva = 0x1B69C30U;
constexpr std::uintptr_t kSelectorObjectResolveRva = 0xDB4630U;
constexpr std::uintptr_t kPinnedImageExtent = 0x8A5EA00U;

constexpr std::uint32_t kOmegaSpawnerRegistry = 0xD00142CFU;
constexpr std::uint32_t kIkoraEntityDefinition = 0x80EC0F27U;
constexpr std::uint32_t kOmegaIkoraSceneOne = 0x80EC0F0EU;
constexpr std::uint32_t kOmegaIkoraVfxCarrierScene = 0x80EC0FA8U;
constexpr std::uint32_t kOmegaIkoraSceneThree = 0x80EC0FA6U;
constexpr std::uint32_t kOmegaIkoraSelectorGraph = 0x80EC0F95U;
constexpr std::uint32_t kOmegaIkoraAlternateScene = 0x80EC0F9CU;
constexpr std::uint32_t kIkoraPrimaryModelDefinition = 0x80EC0F17U;
constexpr std::uint32_t kIkoraMeshClothModelDefinition = 0x80EC0F1DU;
constexpr std::uint32_t kIkoraHeadModelDefinition = 0x80F2EC2FU;
constexpr std::uintptr_t kIkoraPrimaryModelBuildRva = 0x1225BB0U;
constexpr std::uintptr_t kIkoraMeshClothModelBuildRva = 0x11761F0U;
constexpr std::uintptr_t kIkoraHeadModelBuildRva = 0x1225BB0U;
constexpr std::array<std::uint32_t, 4> kOmegaPurpleEffectHandles{
    0x80C71D8EU,
    0x80C71D8AU,
    0x80F1FCCAU,
    0x80C71D70U,
};
constexpr std::uintptr_t kObjectTablePointerRva = 0x1F93428U;
constexpr std::uintptr_t kObjectTableStrideRva = 0x1F93430U;
constexpr std::uintptr_t kPositionMaskOneRva = 0x1B9E420U;
constexpr std::uintptr_t kPositionMaskTwoRva = 0x1B9E430U;
constexpr std::uintptr_t kPositionKeyXyRva = 0x6260781U;
constexpr std::uintptr_t kPositionKeyZwRva = 0x584DFF2U;
constexpr std::size_t kObjectPositionOffset = 0xD0U;
constexpr std::uint32_t kAbsentHash = 0x811C9DC5U;
constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFU;

constexpr std::array<std::byte, 16> kSpawnerDeficitPrefix{
    std::byte{0x4C}, std::byte{0x8B}, std::byte{0xDC}, std::byte{0x49},
    std::byte{0x89}, std::byte{0x5B}, std::byte{0x10}, std::byte{0x49},
    std::byte{0x89}, std::byte{0x6B}, std::byte{0x18}, std::byte{0x56},
    std::byte{0x57}, std::byte{0x41}, std::byte{0x56}, std::byte{0x48}};
constexpr std::array<std::byte, 24> kSceneActorSchedulerPrefix{
    std::byte{0x4C}, std::byte{0x8B}, std::byte{0xDC}, std::byte{0x55},
    std::byte{0x56}, std::byte{0x41}, std::byte{0x55}, std::byte{0x41},
    std::byte{0x57}, std::byte{0x49}, std::byte{0x8D}, std::byte{0xAB},
    std::byte{0xE8}, std::byte{0xF7}, std::byte{0xFF}, std::byte{0xFF},
    std::byte{0x48}, std::byte{0x81}, std::byte{0xEC}, std::byte{0xF8},
    std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x48}};
constexpr std::array<std::byte, 16> kEntityFactoryPrefix{
    std::byte{0x48}, std::byte{0x89}, std::byte{0x5C}, std::byte{0x24},
    std::byte{0x18}, std::byte{0x48}, std::byte{0x89}, std::byte{0x6C},
    std::byte{0x24}, std::byte{0x20}, std::byte{0x56}, std::byte{0x57},
    std::byte{0x41}, std::byte{0x56}, std::byte{0x48}, std::byte{0x81}};
constexpr std::array<std::byte, 16> kComponentStartPrefix{
    std::byte{0x48}, std::byte{0x8B}, std::byte{0x11}, std::byte{0x48},
    std::byte{0x8B}, std::byte{0x49}, std::byte{0x08}, std::byte{0x48},
    std::byte{0x8B}, std::byte{0x42}, std::byte{0x18}, std::byte{0x48},
    std::byte{0xFF}, std::byte{0x64}, std::byte{0x02}, std::byte{0x30}};
constexpr std::array<std::byte, 16> kEffectTransformComposePrefix{
    std::byte{0x40}, std::byte{0x53}, std::byte{0x55}, std::byte{0x56},
    std::byte{0x57}, std::byte{0x41}, std::byte{0x56}, std::byte{0x48},
    std::byte{0x83}, std::byte{0xEC}, std::byte{0x50}, std::byte{0x0F},
    std::byte{0x29}, std::byte{0x74}, std::byte{0x24}, std::byte{0x40}};
constexpr std::array<std::byte, 24> kSelectorChildCreatePrefix{
    std::byte{0x40}, std::byte{0x55}, std::byte{0x53}, std::byte{0x41},
    std::byte{0x55}, std::byte{0x41}, std::byte{0x56}, std::byte{0x41},
    std::byte{0x57}, std::byte{0x48}, std::byte{0x8D}, std::byte{0xAC},
    std::byte{0x24}, std::byte{0x30}, std::byte{0xF8}, std::byte{0xFF},
    std::byte{0xFF}, std::byte{0x48}, std::byte{0x81}, std::byte{0xEC},
    std::byte{0xD0}, std::byte{0x08}, std::byte{0x00}, std::byte{0x00}};
constexpr std::array<std::byte, 24> kSelectorObjectResolvePrefix{
    std::byte{0x48}, std::byte{0x89}, std::byte{0x74}, std::byte{0x24},
    std::byte{0x20}, std::byte{0x57}, std::byte{0x48}, std::byte{0x83},
    std::byte{0xEC}, std::byte{0x20}, std::byte{0xC7}, std::byte{0x02},
    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
    std::byte{0x48}, std::byte{0x8B}, std::byte{0xF2}, std::byte{0x48},
    std::byte{0x8B}, std::byte{0xF9}, std::byte{0x48}, std::byte{0x85}};

using SpawnerDeficit = NativePopulationDispatch;
using SceneActorScheduler = void(__fastcall*)(std::uint32_t* scene) noexcept;
using EntityFactory = std::int32_t*(__fastcall*)(std::int32_t* result,
                                                  const std::byte* descriptor,
                                                  std::uint32_t table,
                                                  std::int32_t record) noexcept;
// +B31910 is a tail-dispatch stub. Preserve XMM1/R8/R9 explicitly when forwarding so the selected
// component lifecycle handler receives the same passthrough arguments as the native caller.
using ComponentStart = void(__fastcall*)(const std::uintptr_t* entry,
                                          float elapsed,
                                          std::uintptr_t context,
                                          std::uintptr_t auxiliary) noexcept;
using EffectTransformCompose = void(__fastcall*)(
    std::byte* effect,
    const std::uintptr_t* transformArrayBase,
    const std::uintptr_t* selectorArrayBase,
    std::uintptr_t extraTranslationBase) noexcept;
using PositionKey = std::uint32_t(__fastcall*)() noexcept;
using SelectorChildCreate = bool(__fastcall*)(std::byte* selector, std::uint32_t entity,
    std::int32_t anchorParameter, std::int32_t transformOnly,
    const std::byte* actorBindings, std::uint64_t* result) noexcept;
using SelectorObjectResolve = bool(__fastcall*)(const std::byte* parameter,
    std::int32_t* result) noexcept;

enum class Source : std::uint8_t {
    other,
    spawner,
    scene,
};

struct SourceContext final {
    Source source{Source::other};
    std::uint32_t spawnerKey{kInvalidHandle};
    std::uint32_t sceneHandle{kInvalidHandle};
    std::uint64_t sourceTickMs{};
};

enum class HookSlot : std::size_t {
    spawnerDeficit,
    sceneActorScheduler,
    entityFactory,
    componentStart,
    effectTransformCompose,
    selectorChildCreate,
    selectorObjectResolve,
    count,
};

constexpr std::size_t kHookCount = static_cast<std::size_t>(HookSlot::count);

std::array<hooking::detour::Handle, kHookCount> g_handles{};
std::atomic_bool g_omegaProbeActive{};
std::atomic<SpawnerDeficit> g_spawnerDeficitOriginal{nullptr};
std::atomic<SceneActorScheduler> g_sceneActorSchedulerOriginal{nullptr};
std::atomic<EntityFactory> g_entityFactoryOriginal{nullptr};
std::atomic<ComponentStart> g_componentStartOriginal{nullptr};
std::atomic<EffectTransformCompose> g_effectTransformComposeOriginal{nullptr};
std::atomic<SelectorChildCreate> g_selectorChildCreateOriginal{nullptr};
std::atomic<SelectorObjectResolve> g_selectorObjectResolveOriginal{nullptr};
std::atomic_uint32_t g_selectorCaptureLogs{};
std::atomic<const std::byte*> g_image{nullptr};
std::atomic_uint32_t g_ikoraFactoryCalls{};
std::atomic_uint32_t g_sceneTwoModelSuppressions{};
std::atomic_uint32_t g_sceneOneObject{kInvalidHandle};
std::atomic_uint32_t g_sceneTwoObject{kInvalidHandle};
std::atomic_uint32_t g_sceneThreeObject{kInvalidHandle};
std::atomic_uint32_t g_effectRebindCalls{};
std::atomic_uint32_t g_effectRebindSeenMask{};
hooking::CallGate g_callGate{};

thread_local SourceContext g_sourceContext{};
thread_local bool g_ikoraFactoryActive{};
thread_local std::uint32_t g_ikoraFactoryScene{kInvalidHandle};
thread_local std::uint32_t g_selectorChildEntity{kInvalidHandle};

[[nodiscard]] bool calls_idle() noexcept {
    return g_callGate.idle();
}

template <typename Value>
[[nodiscard]] Value safe_read(const void* address, Value fallback = {}) noexcept {
    if (address == nullptr) {
        return fallback;
    }
    __try {
        return *static_cast<const Value*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

[[nodiscard]] bool safe_copy(void* destination,
                             const void* source,
                             std::size_t bytes) noexcept {
    if (destination == nullptr || source == nullptr || bytes == 0U) {
        return false;
    }
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <std::size_t Size>
[[nodiscard]] bool prefix_matches(const std::byte* target,
                                  const std::array<std::byte, Size>& expected) noexcept {
    if (target == nullptr) {
        return false;
    }
    __try {
        return std::memcmp(target, expected.data(), expected.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void report(const char* format, ...) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return;
    }
    const std::size_t length = std::min(static_cast<std::size_t>(written), line.size() - 1U);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     {line.data(), length});
}

[[nodiscard]] bool omega_forced() noexcept {
    state::activity::forced::ForcedDestination forced{};
    state::activity::forced::snapshot(forced);
    const std::string_view package(forced.packageName.data(), forced.packageNameLength);
    return state::activity::forced::override_active() && package == "mission_scot";
}

[[nodiscard]] std::uintptr_t image_rva(std::uintptr_t address) noexcept {
    const std::uintptr_t image = reinterpret_cast<std::uintptr_t>(
        g_image.load(std::memory_order_acquire));
    return image != 0U && address >= image && address < image + kPinnedImageExtent
               ? address - image
               : 0U;
}

[[nodiscard]] const char* source_name(Source source) noexcept {
    switch (source) {
        case Source::spawner:
            return "spawner";
        case Source::scene:
            return "scene";
        default:
            return "other";
    }
}

[[nodiscard]] std::uint32_t spawner_key(const std::byte* payload) noexcept {
    if (payload == nullptr) {
        return kInvalidHandle;
    }
    const std::uint32_t key90 = safe_read<std::uint32_t>(payload + 0x90U, kInvalidHandle);
    const std::uint32_t key98 = safe_read<std::uint32_t>(payload + 0x98U, kInvalidHandle);
    if (key90 == kOmegaSpawnerRegistry || key98 == kOmegaSpawnerRegistry) {
        return kOmegaSpawnerRegistry;
    }
    if (key90 != 0U && key90 != kAbsentHash && key90 != kInvalidHandle) {
        return key90;
    }
    return key98 != 0U && key98 != kAbsentHash ? key98 : kInvalidHandle;
}

[[nodiscard]] bool selector_capture_budget() noexcept {
    // A failed or looping authored node must not flood the log. No game state is changed.
    return g_selectorCaptureLogs.fetch_add(1U, std::memory_order_relaxed) < 512U;
}

__declspec(noinline) bool __fastcall selector_child_create(std::byte* selector,
    std::uint32_t entity, std::int32_t anchorParameter, std::int32_t transformOnly,
    const std::byte* actorBindings, std::uint64_t* result) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    const auto original = hooking::await_original(g_selectorChildCreateOriginal);
    const bool inspect = call.accepts_side_effects()
        && safe_read<std::uint32_t>(selector, kInvalidHandle) == kOmegaIkoraSelectorGraph
        && omega_forced();
    const auto previous = g_selectorChildEntity;
    if (inspect) { g_selectorChildEntity = entity; }
    const auto run = inspect ? state::activity::omega_presentation::navigation().run : 0;
    const auto before = inspect ? safe_read<std::uint64_t>(result, UINT64_MAX) : UINT64_MAX;
    auto selectedAnchor = anchorParameter;
    // Both rescue Scenes use holding timeline 80EC0E00. Scene 9 anchors it to
    // ps_echo_converge_move_to (registry 99BD2FEB, type 48, slot 133).
    // Scene 27 exposes that same marker as parameter 0, but its hold node uses
    // parameter 2 (slot 134). Match the first hold without moving the entrance,
    // release animation, eye DPS, or the final-arena Scene 33 sharing this graph.
    if (call.accepts_side_effects() && entity == 0x80EC0E00U && anchorParameter == 2
        && safe_read<std::uint32_t>(selector, kInvalidHandle) == 0x80EC0DD4U
        && safe_read<std::uint32_t>(selector + 4U, kInvalidHandle) == 0x80806384U
        && omega_forced()) {
        namespace fight = state::activity::omega_first_lair;
        const auto nav = state::activity::omega_presentation::navigation();
        const auto status = fight::status(nav.run);
        const auto request = fight::scene_request(nav.run, 27);
        if (nav.enabled && nav.run != 0 && status.enabled && !status.failed
            && status.cycle == 2 && status.crownStage == fight::CrownStage::rescue
            && status.token.valid() && status.token.boss.run == nav.run
            && request.enabled && request.token == status.token
            && request.command.generation != 0 && !request.command.stop
            && request.command.eventCount == 0
            && state::activity::omega_rescue_npc::valid(request.command)) {
            selectedAnchor = 0;
        }
    }
    const bool success = original(selector, entity, selectedAnchor, transformOnly,
                                  actorBindings, result);
    if (selectedAnchor != anchorParameter && call.accepts_side_effects()
        && selector_capture_budget()) {
        report("ev=omega_osiris_hold stage=anchor slot=27 child=%08X "
               "anchor_param=%d selected_param=%d marker=133 success=%u",
               entity, anchorParameter, selectedAnchor, success ? 1U : 0U);
    }
    g_selectorChildEntity = previous;
    if (inspect && call.accepts_side_effects() && selector_capture_budget()) {
        report("ev=omega_ikora_selector stage=child_create run=%llu selector=%p graph=%08X "
               "child=%08X anchor_param=%d transform_only=%d bindings=%p binding_count=%lld "
               "success=%u before=%016llX after=%016llX thread=%lu mutation=observe_only",
               static_cast<unsigned long long>(run), selector, kOmegaIkoraSelectorGraph,
               entity, anchorParameter, transformOnly, actorBindings,
               static_cast<long long>(safe_read<std::int64_t>(actorBindings, -1)),
               success ? 1U : 0U, static_cast<unsigned long long>(before),
               static_cast<unsigned long long>(safe_read<std::uint64_t>(result, UINT64_MAX)),
               GetCurrentThreadId());
    }
    return success;
}

__declspec(noinline) bool __fastcall selector_object_resolve(const std::byte* parameter,
    std::int32_t* result) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    const auto original = hooking::await_original(g_selectorObjectResolveOriginal);
    const bool inspect = call.accepts_side_effects()
        && safe_read<std::uint32_t>(parameter, kInvalidHandle) == kOmegaIkoraSelectorGraph
        && omega_forced();
    const auto run = inspect ? state::activity::omega_presentation::navigation().run : 0;
    const bool success = original(parameter, result);
    if (!inspect || !call.accepts_side_effects()) { return success; }

    // Emit changes per exact live parameter, full result handle and run. Repeated invalid
    // resolutions are expected for transform parameters and are not an error diagnosis.
    struct Receipt {
        const std::byte* parameter{};
        std::uint32_t object{kInvalidHandle};
        std::uint32_t child{kInvalidHandle};
        bool success{};
    };
    struct Cache {
        std::uint64_t run{};
        std::array<Receipt, 16> rows{};
        std::size_t used{};
    };
    thread_local Cache cache{};
    if (cache.run != run) { cache = {}; cache.run = run; }
    const auto object = safe_read<std::uint32_t>(result, kInvalidHandle);
    Receipt* row = nullptr;
    for (std::size_t index = 0; index < cache.used; ++index) {
        if (cache.rows[index].parameter == parameter) { row = &cache.rows[index]; break; }
    }
    if (row != nullptr && row->object == object && row->child == g_selectorChildEntity
        && row->success == success) { return success; }
    if (row == nullptr) {
        if (cache.used == cache.rows.size()) { return success; }
        row = &cache.rows[cache.used++];
    }
    *row = {parameter, object, g_selectorChildEntity, success};
    if (selector_capture_budget()) {
        report("ev=omega_ikora_selector stage=object_resolve run=%llu parameter=%p "
               "graph=%08X definition_offset=%llX child=%08X success=%u object=%08X "
               "thread=%lu mutation=observe_only",
               static_cast<unsigned long long>(run), parameter, kOmegaIkoraSelectorGraph,
               static_cast<unsigned long long>(safe_read<std::uint64_t>(parameter + 8U)),
               g_selectorChildEntity, success ? 1U : 0U, object, GetCurrentThreadId());
    }
    return success;
}

[[nodiscard]] std::uintptr_t component_handler_rva(const std::uintptr_t* entry) noexcept {
    const std::uintptr_t descriptor = safe_read<std::uintptr_t>(entry, 0U);
    const std::uintptr_t relative = safe_read<std::uintptr_t>(
        reinterpret_cast<const void*>(descriptor + 0x18U), UINTPTR_MAX);
    if (descriptor < 0x10000U || relative == UINTPTR_MAX || relative > 0x10000U) {
        return 0U;
    }
    const std::uintptr_t handler = safe_read<std::uintptr_t>(
        reinterpret_cast<const void*>(descriptor + 0x30U + relative), 0U);
    return image_rva(handler);
}

struct ObjectPosition final {
    std::array<float, 3> value{};
    bool valid{};
};

[[nodiscard]] float float_from_bits(std::uint32_t bits) noexcept {
    float value = 0.0F;
    static_assert(sizeof value == sizeof bits);
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

/** Reads the live actor root through the same protected object-position path as +1087F70. */
[[nodiscard]] ObjectPosition object_position(std::uint32_t handle) noexcept {
    ObjectPosition result{};
    auto* const image = const_cast<std::byte*>(g_image.load(std::memory_order_acquire));
    if (image == nullptr || handle == kInvalidHandle) {
        return result;
    }
    std::byte* const table = safe_read<std::byte*>(image + kObjectTablePointerRva, nullptr);
    const std::uint32_t stride = safe_read<std::uint32_t>(image + kObjectTableStrideRva, 0U);
    if (table == nullptr || stride == 0U || stride > 0x10000U) {
        return result;
    }
    const std::byte* const record =
        table + static_cast<std::size_t>(handle & 0x1FFFU) * stride;
    if (safe_read<std::uint32_t>(record + 0x0CU, kInvalidHandle) != handle) {
        return result;
    }

    const auto keyXy = reinterpret_cast<PositionKey>(image + kPositionKeyXyRva);
    const auto keyZw = reinterpret_cast<PositionKey>(image + kPositionKeyZwRva);
    std::uint32_t xy = 0U;
    std::uint32_t zw = 0U;
    __try {
        xy = keyXy();
        zw = keyZw();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return result;
    }
    const std::byte* const encoded = record + kObjectPositionOffset;
    for (std::size_t lane = 0U; lane < result.value.size(); ++lane) {
        const std::uint32_t raw = safe_read<std::uint32_t>(
            encoded + lane * sizeof(std::uint32_t), 0U);
        const std::uint32_t first = safe_read<std::uint32_t>(
            image + kPositionMaskOneRva + lane * sizeof(std::uint32_t), 0U);
        const std::uint32_t second = safe_read<std::uint32_t>(
            image + kPositionMaskTwoRva + lane * sizeof(std::uint32_t), 0U);
        const std::uint32_t key = lane < 2U ? xy : zw;
        result.value[lane] = float_from_bits(
            ((raw ^ key) & first & second) | ((~second) & 0x3F800000U));
    }
    result.valid = std::isfinite(result.value[0]) && std::isfinite(result.value[1])
                   && std::isfinite(result.value[2])
                   && std::fabs(result.value[0]) < 10'000'000.0F
                   && std::fabs(result.value[1]) < 10'000'000.0F
                   && std::fabs(result.value[2]) < 10'000'000.0F;
    return result;
}

void spawner_deficit_body(std::uint32_t* instance,
                          std::uint32_t reason,
                          const std::byte* payload,
                          const hooking::CallGate::Scope& call) noexcept {
    const SpawnerDeficit original = hooking::await_original(g_spawnerDeficitOriginal);

    if (!call.accepts_side_effects()) {
        original(instance, reason, payload);
        return;
    }

    const SourceContext previous = g_sourceContext;
    if (omega_forced()) {
        g_sourceContext = {
            Source::spawner,
            spawner_key(payload),
            kInvalidHandle,
            GetTickCount64(),
        };
    }
    bool reportBossReturn = false;
    if (safe_read<std::uint32_t>(instance) == state::activity::omega_presentation::kBossDefinition
        && omega_forced()) {
        const auto generation = safe_read<std::uint32_t>(payload + 0x7C);
        static std::atomic_uint32_t lastGeneration{UINT32_MAX};
        if (lastGeneration.exchange(generation) != generation) {
            reportBossReturn = true;
            report("ev=omega_boss stage=deficit instance=%p generation=%u count=%u requested=%u "
                   "squad=%08X/%u/%u active=%u mode=%u variant=%d thread=%lu",
                instance,generation,safe_read<std::uint32_t>(payload+0x2C),
                safe_read<std::uint32_t>(payload+0x30),safe_read<std::uint32_t>(payload+0x98),
                safe_read<std::uint8_t>(payload+0x9C),safe_read<std::uint16_t>(payload+0x9E),
                safe_read<std::uint8_t>(payload+0xBC),safe_read<std::uint8_t>(payload+0xBD),
                static_cast<int>(safe_read<std::int8_t>(payload+0x74)),GetCurrentThreadId());
        }
    }
    dispatch_native_population_source(instance, reason, payload, original);
    if (reportBossReturn) {
        report("ev=omega_boss stage=deficit_return instance=%p thread=%lu",
               instance,GetCurrentThreadId());
    }
    g_sourceContext = previous;
}

__declspec(noinline) void __fastcall spawner_deficit(std::uint32_t* instance,
                                                      std::uint32_t reason,
                                                      const std::byte* payload) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    spawner_deficit_body(instance, reason, payload, call);
}

void scene_actor_scheduler_body(std::uint32_t* scene,
                                const hooking::CallGate::Scope& call) noexcept {
    const SceneActorScheduler original = hooking::await_original(g_sceneActorSchedulerOriginal);

    if (!call.accepts_side_effects()) {
        original(scene);
        return;
    }

    const SourceContext previous = g_sourceContext;
    if (omega_forced()) {
        g_sourceContext = {
            Source::scene,
            kInvalidHandle,
            safe_read<std::uint32_t>(scene, kInvalidHandle),
            GetTickCount64(),
        };
    }
    original(scene);
    g_sourceContext = previous;
}

__declspec(noinline) void __fastcall scene_actor_scheduler(std::uint32_t* scene) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    scene_actor_scheduler_body(scene, call);
}

std::int32_t* entity_factory_body(
    std::int32_t* result,
    const std::byte* descriptor,
    std::uint32_t table,
    std::int32_t record,
    const hooking::CallGate::Scope& call) noexcept {
    const EntityFactory original = hooking::await_original(g_entityFactoryOriginal);

    const std::uint32_t definition = safe_read<std::uint32_t>(descriptor, kInvalidHandle);
    namespace launchpad = state::activity::newlight::launchpad;
    state::activity::coo::Generation shutterOwner{};
    bool shutterAtDoor{};
    if (call.accepts_side_effects() && result != nullptr
        && definition == launchpad::shutter::kEntity) {
        // Selection exists before initial world streaming. native_run() would
        // be too late here because it requires arrival in the loaded world.
        const auto owner = launchpad::native_owner();
        const float x = safe_read<float>(descriptor + 0x20U);
        const float y = safe_read<float>(descriptor + 0x24U);
        const float z = safe_read<float>(descriptor + 0x28U);
        const bool matches = launchpad::shutter::matches(owner.valid(), definition, x, y, z);
        shutterOwner=owner;shutterAtDoor=matches;
        static std::atomic_uint32_t attempts{};
        const auto attempt = attempts.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (attempt <= 16U || (attempt & (attempt - 1U)) == 0U) {
            report("ev=launchpad stage=breach_shutter_factory attempt=%u run=%llu "
                   "definition=%08X table=%08X record=%d pos=%.3f,%.3f,%.3f action=%s",
                   attempt, static_cast<unsigned long long>(owner.run), definition, table,
                   record, x, y, z, "native");
        }
    }
    // Release defaults must observe shutters without enabling the unrelated
    // Omega diagnostics and presentation experiments that share this factory.
    if (!g_omegaProbeActive.load(std::memory_order_acquire)) {
        std::int32_t* const returned = original(result, descriptor, table, record);
        if (shutterOwner.valid()) {
            launchpad::observe_native_shutter(shutterOwner,
                safe_read<std::uint32_t>(returned,kInvalidHandle),shutterAtDoor);
        }
        return returned;
    }
    // Forest generator lane: log any construction of the six 808099D6 worker containers or
    // their placed platform entities (all class 80809C0F), regardless of the Ikora filter.
    // Whether these ever construct decides the whole platform lane (WORKLOG 2026-08-27).
    {
        constexpr std::array<std::uint32_t, 10> kForestTags{
            0x80F4D0F2U, 0x80F4D840U, 0x80F4E01FU, 0x80F4E6E8U, 0x80F4EE11U, 0x80F4FAC9U,
            0x80F44ACFU, 0x80F44ACBU, 0x80F4A5E3U, 0x80C6BE1FU};
        bool forest = false;
        for (const std::uint32_t candidate : kForestTags) {
            forest = forest || definition == candidate;
        }
        if (forest) {
            // _ReturnAddress here lands in our own detour trampoline; walk a few frames and
            // report the first return address inside the game image instead.
            const auto* const image = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
            std::uint64_t callerRva = 0U;
            std::array<void*, 8> frames{};
            const USHORT captured = RtlCaptureStackBackTrace(1U, 8U, frames.data(), nullptr);
            for (USHORT frame = 0; frame < captured && callerRva == 0U; ++frame) {
                const auto* const address = static_cast<const std::byte*>(frames[frame]);
                if (image != nullptr && address > image && address < image + 0x9000000U) {
                    callerRva = static_cast<std::uint64_t>(address - image);
                }
            }
            std::array<char, 160> line{};
            const int written = std::snprintf(
                line.data(), line.size(),
                "ev=forest stage=factory definition=0x%08X table=0x%08X record=%d caller=+0x%llX",
                definition, table, record, static_cast<unsigned long long>(callerRva));
            if (written > 0) {
                core::log::write(core::log::Channel::client, core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    const bool boss = call.accepts_side_effects()
        && definition == state::activity::omega_presentation::kBossEntity && omega_forced();
    if (boss) {
        static std::atomic_uint32_t attempts{};
        const auto attempt = attempts.fetch_add(1U,std::memory_order_relaxed) + 1U;
        if (attempt <= 4U || (attempt & (attempt - 1U)) == 0U) {
            report("ev=omega_boss stage=factory_enter attempt=%u definition=%08X table=%08X "
                   "record=%d thread=%lu",attempt,definition,table,record,GetCurrentThreadId());
        }
    }
    const auto bossRun = boss ? state::activity::omega_presentation::navigation().run : 0;
    const bool inspect = call.accepts_side_effects() && definition == kIkoraEntityDefinition
                         && omega_forced();
    const SourceContext context = g_sourceContext;
    const void* const callerAddress = inspect ? _ReturnAddress() : nullptr;
    const std::uint64_t tickMs = inspect ? GetTickCount64() : 0U;
    std::array<float, 3> position{};
    std::array<float, 4> orientation{};
    if (inspect && descriptor != nullptr) {
        position = {
            safe_read<float>(descriptor + 0x20U),
            safe_read<float>(descriptor + 0x24U),
            safe_read<float>(descriptor + 0x28U),
        };
        orientation = {
            safe_read<float>(descriptor + 0x10U),
            safe_read<float>(descriptor + 0x14U),
            safe_read<float>(descriptor + 0x18U),
            safe_read<float>(descriptor + 0x1CU),
        };
    }
    const bool previousFactoryActive = g_ikoraFactoryActive;
    const std::uint32_t previousFactoryScene = g_ikoraFactoryScene;
    if (inspect && call.accepts_side_effects()) {
        g_ikoraFactoryActive = true;
        g_ikoraFactoryScene = context.sceneHandle;
    }
    std::int32_t* const returned = original(result, descriptor, table, record);
    if(shutterOwner.valid()) {
        launchpad::observe_native_shutter(shutterOwner,safe_read<std::uint32_t>(returned,kInvalidHandle),shutterAtDoor);
    }
    g_ikoraFactoryActive = previousFactoryActive;
    g_ikoraFactoryScene = previousFactoryScene;
    if (boss && call.accepts_side_effects()) {
        const auto object = safe_read<std::uint32_t>(returned, UINT32_MAX);
        static std::atomic_uint64_t lastResult{UINT64_MAX};
        const auto stamp = (bossRun << 32U) | object;
        if (lastResult.exchange(stamp) != stamp) {
            report("ev=omega_boss stage=factory run=%llu definition=%08X object=%08X "
                   "table=%08X record=%d pos=%.3f,%.3f,%.3f",
                static_cast<unsigned long long>(bossRun),definition,object,table,record,
                safe_read<float>(descriptor+0x20),safe_read<float>(descriptor+0x24),
                safe_read<float>(descriptor+0x28));
        }
    }

    if (inspect && call.accepts_side_effects()) {
        const std::uint32_t sequence =
            g_ikoraFactoryCalls.fetch_add(1U, std::memory_order_relaxed) + 1U;
        const std::int32_t created = safe_read<std::int32_t>(returned, -1);
        const std::uint32_t createdHandle = static_cast<std::uint32_t>(created);
        if (context.source == Source::scene
            && context.sceneHandle == kOmegaIkoraAlternateScene && selector_capture_budget()) {
            report("ev=omega_ikora_selector stage=alternate_actor scene=%08X object=%08X "
                   "run=%llu mutation=observe_only", context.sceneHandle, createdHandle,
                   static_cast<unsigned long long>(state::activity::omega_presentation::navigation().run));
        }
        if (context.source == Source::scene) {
            if (context.sceneHandle == kOmegaIkoraSceneOne) {
                g_sceneOneObject.store(createdHandle, std::memory_order_release);
            } else if (context.sceneHandle == kOmegaIkoraVfxCarrierScene) {
                g_sceneTwoObject.store(createdHandle, std::memory_order_release);
            } else if (context.sceneHandle == kOmegaIkoraSceneThree) {
                g_sceneThreeObject.store(createdHandle, std::memory_order_release);
            }
        }
        report("ev=omega_ikora_origin t=%llu source_t=%llu n=%u source=%s "
               "spawner_key=%08X scene=%08X "
               "caller=+%llX caller_address=%p definition=%08X descriptor=%p "
               "table=%08X record=%d result=%08X result_ptr=%p returned_ptr=%p "
               "pos=%.3f,%.3f,%.3f quat=%.3f,%.3f,%.3f,%.3f "
               "mutation=observe_plus_narrow_model_suppression",
               static_cast<unsigned long long>(tickMs),
               static_cast<unsigned long long>(context.sourceTickMs),
               sequence,
               source_name(context.source),
               context.spawnerKey,
               context.sceneHandle,
               static_cast<unsigned long long>(
                   image_rva(reinterpret_cast<std::uintptr_t>(callerAddress))),
               callerAddress,
               definition,
               descriptor,
               table,
               record,
               static_cast<std::uint32_t>(created),
               result,
               returned,
               position[0],
               position[1],
               position[2],
               orientation[0],
               orientation[1],
               orientation[2],
               orientation[3]);
    }
    return returned;
}

__declspec(noinline) std::int32_t* __fastcall entity_factory(
    std::int32_t* result,
    const std::byte* descriptor,
    std::uint32_t table,
    std::int32_t record) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    return entity_factory_body(result, descriptor, table, record, call);
}

void effect_transform_compose_body(
    std::byte* effect,
    const std::uintptr_t* transformArrayBase,
    const std::uintptr_t* selectorArrayBase,
    std::uintptr_t extraTranslationBase,
    const hooking::CallGate::Scope& call) noexcept {
    const EffectTransformCompose original =
        hooking::await_original(g_effectTransformComposeOriginal);
    original(effect, transformArrayBase, selectorArrayBase, extraTranslationBase);

    if (!call.accepts_side_effects()) {
        return;
    }

    const auto& omega = core::settings::get().omegaExperiments;
    if (effect == nullptr || (!omega.ikoraVfxRebind && !omega.unsafeDiagnostics)) {
        return;
    }
    const std::uint32_t handle = safe_read<std::uint32_t>(effect + 0x3CU, kInvalidHandle);
    std::size_t handleIndex = kOmegaPurpleEffectHandles.size();
    for (std::size_t index = 0U; index < kOmegaPurpleEffectHandles.size(); ++index) {
        if (handle == kOmegaPurpleEffectHandles[index]) {
            handleIndex = index;
            break;
        }
    }
    if (handleIndex == kOmegaPurpleEffectHandles.size() || !omega_forced()) {
        return;
    }

    const ObjectPosition carrier = object_position(
        g_sceneTwoObject.load(std::memory_order_acquire));
    ObjectPosition visible = object_position(
        g_sceneThreeObject.load(std::memory_order_acquire));
    const char* visibleScene = "80EC0FA6";
    if (!visible.valid) {
        visible = object_position(g_sceneOneObject.load(std::memory_order_acquire));
        visibleScene = "80EC0F0E";
    }
    if (!carrier.valid || !visible.valid) {
        return;
    }

    std::array<float, 3> delta{};
    float distanceSquared = 0.0F;
    for (std::size_t lane = 0U; lane < delta.size(); ++lane) {
        delta[lane] = visible.value[lane] - carrier.value[lane];
        distanceSquared += delta[lane] * delta[lane];
    }
    if (!std::isfinite(distanceSquared) || distanceSquared > 2500.0F) {
        return;
    }
    if (!call.accepts_side_effects()) {
        return;
    }

    const std::uint32_t sequence =
        g_effectRebindCalls.fetch_add(1U, std::memory_order_relaxed) + 1U;
    const std::uint32_t bit = 1U << static_cast<std::uint32_t>(handleIndex);
    const bool first =
        (g_effectRebindSeenMask.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0U;
    if (first) {
        report("ev=omega_ikora_vfx_rebind n=%u handle=%08X index=%zu "
               "carrier_scene=80EC0FA8 visible_scene=%s "
               "carrier=%.3f,%.3f,%.3f visible=%.3f,%.3f,%.3f "
               "delta=%.3f,%.3f,%.3f distance=%.3f "
               "mutation=observe_only conclusion=root_delta_is_not_an_attachment_rebind",
               sequence,
               handle,
               handleIndex,
               visibleScene,
               static_cast<double>(carrier.value[0]),
               static_cast<double>(carrier.value[1]),
               static_cast<double>(carrier.value[2]),
               static_cast<double>(visible.value[0]),
               static_cast<double>(visible.value[1]),
               static_cast<double>(visible.value[2]),
               static_cast<double>(delta[0]),
               static_cast<double>(delta[1]),
               static_cast<double>(delta[2]),
               static_cast<double>(std::sqrt(distanceSquared)));
    }
}

__declspec(noinline) void __fastcall effect_transform_compose(
    std::byte* effect,
    const std::uintptr_t* transformArrayBase,
    const std::uintptr_t* selectorArrayBase,
    std::uintptr_t extraTranslationBase) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    effect_transform_compose_body(effect,
                                  transformArrayBase,
                                  selectorArrayBase,
                                  extraTranslationBase,
                                  call);
}

void component_start_body(const std::uintptr_t* entry,
                          float elapsed,
                          std::uintptr_t context,
                          std::uintptr_t auxiliary,
                          const hooking::CallGate::Scope& call) noexcept {
    const ComponentStart original = hooking::await_original(g_componentStartOriginal);
    if (!call.accepts_side_effects() || !g_ikoraFactoryActive) {
        original(entry, elapsed, context, auxiliary);
        return;
    }
    if (!core::settings::get().omegaExperiments.ikoraCarrierModelSuppression) {
        original(entry, elapsed, context, auxiliary);
        return;
    }

    const std::uintptr_t instance = safe_read<std::uintptr_t>(entry + 1, 0U);
    const std::uint32_t definition = safe_read<std::uint32_t>(
        reinterpret_cast<const void*>(instance), kInvalidHandle);
    const std::uintptr_t handlerRva = component_handler_rva(entry);
    const bool sceneTwo = g_ikoraFactoryActive
                          && g_ikoraFactoryScene == kOmegaIkoraVfxCarrierScene;
    const bool primary = sceneTwo && definition == kIkoraPrimaryModelDefinition
                         && handlerRva == kIkoraPrimaryModelBuildRva;
    const bool meshCloth = sceneTwo && definition == kIkoraMeshClothModelDefinition
                           && handlerRva == kIkoraMeshClothModelBuildRva;
    const bool head = sceneTwo && definition == kIkoraHeadModelDefinition
                      && handlerRva == kIkoraHeadModelBuildRva;
    if (!primary && !meshCloth && !head) {
        original(entry, elapsed, context, auxiliary);
        return;
    }
    if (!call.accepts_side_effects()) {
        original(entry, elapsed, context, auxiliary);
        return;
    }

    // 0FA8 owns the wanted void-beam events, but its cast model is not visible in retail. Keep the
    // Scene, animation graph and terminal retirement native; suppress only its three render-model
    // construction callbacks so the concurrent 0F0E/0FA6 actor remains the sole visible Ikora.
    const std::uint32_t sequence =
        g_sceneTwoModelSuppressions.fetch_add(1U, std::memory_order_relaxed) + 1U;
    report("ev=omega_ikora_model_suppression n=%u scene=%08X definition=%08X "
           "handler=+%llX model=%s action=skip_native_render_model_construction "
           "scene_events=native animation_graph=native retirement=native "
           "mutation=suppress_vfx_carrier_model",
           sequence,
           g_ikoraFactoryScene,
           definition,
           static_cast<unsigned long long>(handlerRva),
           primary ? "primary" : meshCloth ? "mesh_cloth" : "head_child");
}

__declspec(noinline) void __fastcall component_start(const std::uintptr_t* entry,
                                                       float elapsed,
                                                       std::uintptr_t context,
                                                       std::uintptr_t auxiliary) noexcept {
    hooking::CallGate::Scope call(g_callGate);
    component_start_body(entry,
                         elapsed,
                         context,
                         auxiliary,
                         call);
}

} // namespace

bool install_omega_ikora_origin_probe() noexcept {
    constexpr auto factorySlot = static_cast<std::size_t>(HookSlot::entityFactory);
    if (g_handles[factorySlot].attached) {
        return g_callGate.accepting();
    }
    g_callGate.quiesce();
    const auto& omega = core::settings::get().omegaExperiments;
    const bool omegaProbe = omega.ikoraCarrierModelSuppression || omega.ikoraVfxRebind
                            || omega.unsafeDiagnostics;

    auto* const image = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (image == nullptr) {
        report("ev=omega_ikora_origin stage=install result=image_unavailable "
               "mutation=observe_only");
        return false;
    }

    const std::array<std::byte*, kHookCount> targets{
        image + kSpawnerDeficitRva,
        image + kSceneActorSchedulerRva,
        image + kEntityFactoryRva,
        image + kComponentStartRva,
        image + kEffectTransformComposeRva,
        image + kSelectorChildCreateRva,
        image + kSelectorObjectResolveRva,
    };
    const bool spawnerPrefix = !omegaProbe || prefix_matches(targets[0], kSpawnerDeficitPrefix);
    const bool scenePrefix = !omegaProbe || prefix_matches(targets[1], kSceneActorSchedulerPrefix);
    const bool factoryPrefix = prefix_matches(targets[2], kEntityFactoryPrefix);
    const bool componentPrefix = !omegaProbe || prefix_matches(targets[3], kComponentStartPrefix);
    const bool effectPrefix = !omegaProbe || prefix_matches(targets[4], kEffectTransformComposePrefix);
    const bool childPrefix = !omegaProbe || prefix_matches(targets[5], kSelectorChildCreatePrefix);
    const bool resolvePrefix = !omegaProbe || prefix_matches(targets[6], kSelectorObjectResolvePrefix);
    if (!spawnerPrefix || !scenePrefix || !factoryPrefix || !componentPrefix || !effectPrefix
        || !childPrefix || !resolvePrefix) {
        report("ev=omega_ikora_origin stage=install result=prefix_mismatch "
               "spawner=%u scene=%u factory=%u component=%u effect=%u child=%u resolve=%u "
               "targets=+%llX,+%llX,+%llX,+%llX,+%llX,+%llX,+%llX "
               "mutation=observe_only",
               spawnerPrefix ? 1U : 0U,
               scenePrefix ? 1U : 0U,
               factoryPrefix ? 1U : 0U,
               componentPrefix ? 1U : 0U,
               effectPrefix ? 1U : 0U,
               childPrefix ? 1U : 0U,
               resolvePrefix ? 1U : 0U,
               static_cast<unsigned long long>(kSpawnerDeficitRva),
               static_cast<unsigned long long>(kSceneActorSchedulerRva),
               static_cast<unsigned long long>(kEntityFactoryRva),
               static_cast<unsigned long long>(kComponentStartRva),
               static_cast<unsigned long long>(kEffectTransformComposeRva),
               static_cast<unsigned long long>(kSelectorChildCreateRva),
               static_cast<unsigned long long>(kSelectorObjectResolveRva));
        return false;
    }

    const std::array<hooking::detour::Spec, kHookCount> specs{{
        {targets[0], reinterpret_cast<void*>(&spawner_deficit)},
        {targets[1], reinterpret_cast<void*>(&scene_actor_scheduler)},
        {targets[2], reinterpret_cast<void*>(&entity_factory)},
        {targets[3], reinterpret_cast<void*>(&component_start)},
        {targets[4], reinterpret_cast<void*>(&effect_transform_compose)},
        {targets[5], reinterpret_cast<void*>(&selector_child_create)},
        {targets[6], reinterpret_cast<void*>(&selector_object_resolve)},
    }};
    const auto firstHook = omegaProbe ? std::size_t{0} : factorySlot;
    const auto hookCount = omegaProbe ? kHookCount : std::size_t{1};
    if (!hooking::detour::install(std::span(specs).subspan(firstHook,hookCount),
                                std::span(g_handles).subspan(firstHook,hookCount))) {
        report("ev=omega_ikora_origin stage=install result=attach_fail "
               "transaction=atomic mutation=observe_plus_narrow_model_suppression");
        return false;
    }

    g_image.store(image, std::memory_order_release);
    hooking::publish_original(
        g_spawnerDeficitOriginal,
        reinterpret_cast<SpawnerDeficit>(g_handles[0].original));
    hooking::publish_original(
        g_sceneActorSchedulerOriginal,
        reinterpret_cast<SceneActorScheduler>(g_handles[1].original));
    hooking::publish_original(
        g_entityFactoryOriginal,
        reinterpret_cast<EntityFactory>(g_handles[2].original));
    hooking::publish_original(
        g_componentStartOriginal,
        reinterpret_cast<ComponentStart>(g_handles[3].original));
    hooking::publish_original(
        g_effectTransformComposeOriginal,
        reinterpret_cast<EffectTransformCompose>(g_handles[4].original));
    hooking::publish_original(
        g_selectorChildCreateOriginal,
        reinterpret_cast<SelectorChildCreate>(g_handles[5].original));
    hooking::publish_original(
        g_selectorObjectResolveOriginal,
        reinterpret_cast<SelectorObjectResolve>(g_handles[6].original));
    g_omegaProbeActive.store(omegaProbe,std::memory_order_release);
    g_callGate.accept();
    report("ev=launchpad stage=breach_shutter_observer result=installed hooks=%zu omega_probe=%u",
           hookCount,omegaProbe ? 1U : 0U);
    report("ev=omega_ikora_origin stage=install result=ok transaction=atomic "
           "targets=+%llX,+%llX,+%llX,+%llX,+%llX,+%llX,+%llX filter=%08X "
           "scene2_model_suppression=%08X vfx_rebind=observe_actor_root_delta "
           "mutation=observe_plus_narrow_model_suppression",
           static_cast<unsigned long long>(kSpawnerDeficitRva),
           static_cast<unsigned long long>(kSceneActorSchedulerRva),
           static_cast<unsigned long long>(kEntityFactoryRva),
           static_cast<unsigned long long>(kComponentStartRva),
           static_cast<unsigned long long>(kEffectTransformComposeRva),
           static_cast<unsigned long long>(kSelectorChildCreateRva),
           static_cast<unsigned long long>(kSelectorObjectResolveRva),
           kIkoraEntityDefinition,
           kOmegaIkoraVfxCarrierScene);
    return true;
}

void quiesce_omega_ikora_origin_probe() noexcept {
    g_callGate.quiesce();
}

bool uninstall_omega_ikora_origin_probe() noexcept {
    quiesce_omega_ikora_origin_probe();
    constexpr auto factorySlot = static_cast<std::size_t>(HookSlot::entityFactory);
    if (!g_handles[factorySlot].attached) {
        return true;
    }

    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&spawner_deficit)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&scene_actor_scheduler)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&entity_factory)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&component_start)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&effect_transform_compose)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&selector_child_create)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&selector_object_resolve)},
        hooking::detour::ProtectedCodeEntry{
            reinterpret_cast<void*>(&hooking::call_gate_detail::enter)},
        hooking::detour::ProtectedCodeEntry{
            reinterpret_cast<void*>(&hooking::call_gate_detail::leave)},
    };
    // Retain the installed selection through quiescing and a deferred removal.
    // Passing unattached optional handles would reject factory-only teardown.
    const bool omegaProbe = g_omegaProbeActive.load(std::memory_order_acquire);
    const auto firstHook = omegaProbe ? std::size_t{0} : factorySlot;
    const auto hookCount = omegaProbe ? kHookCount : std::size_t{1};
    const hooking::detour::UninstallResult result = hooking::detour::uninstall(
        std::span(g_handles).subspan(firstHook,hookCount), protectedEntries, &calls_idle);
    report("ev=omega_ikora_origin stage=uninstall result=%s active_calls=%u retained=%u "
           "transaction=atomic mutation=observe_plus_narrow_model_suppression",
           result == hooking::detour::UninstallResult::removed
               ? "ok"
               : result == hooking::detour::UninstallResult::protectedCodeActive ? "deferred"
                                                                                 : "failed",
           g_callGate.active_calls(),
           result == hooking::detour::UninstallResult::removed ? 0U : 1U);
    if (result != hooking::detour::UninstallResult::removed) {
        return false;
    }

    g_omegaProbeActive.store(false,std::memory_order_release);
    g_effectTransformComposeOriginal.store(nullptr, std::memory_order_release);
    g_selectorChildCreateOriginal.store(nullptr, std::memory_order_release);
    g_selectorObjectResolveOriginal.store(nullptr, std::memory_order_release);
    g_componentStartOriginal.store(nullptr, std::memory_order_release);
    g_entityFactoryOriginal.store(nullptr, std::memory_order_release);
    g_sceneActorSchedulerOriginal.store(nullptr, std::memory_order_release);
    g_spawnerDeficitOriginal.store(nullptr, std::memory_order_release);
    g_image.store(nullptr, std::memory_order_release);
    g_ikoraFactoryCalls.store(0U, std::memory_order_release);
    g_sceneTwoModelSuppressions.store(0U, std::memory_order_release);
    g_sceneOneObject.store(kInvalidHandle, std::memory_order_release);
    g_sceneTwoObject.store(kInvalidHandle, std::memory_order_release);
    g_sceneThreeObject.store(kInvalidHandle, std::memory_order_release);
    g_effectRebindCalls.store(0U, std::memory_order_release);
    g_effectRebindSeenMask.store(0U, std::memory_order_release);
    g_selectorCaptureLogs.store(0U, std::memory_order_release);
    g_sourceContext = {};
    g_ikoraFactoryActive = false;
    g_ikoraFactoryScene = kInvalidHandle;
    return true;
}

} // namespace dawn::client::hooks::bootflow
