/**
 * Weapon fire sources, every projectile launch, and each projectile's early flight.
 * Fire is seen at the one proven firing call of a float decoder, launches at the native
 * projectile initializer, and flight by re-reading the projectile's own component from the
 * trace's writer thread, so no other Dawn hook is shared. A weapon source or projectile type is
 * dumped in full the first time it appears; after that each shot is counted and each launch is
 * one line.
 */

#include "internal.h"
#include "resource_identity.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <intrin.h>
#include <span>
#include <string_view>

#include "../../../hooking/detour.h"
#include "../detach.h"
#include "../../../patterns/image_scan.h"
#include "../../../patterns/signature_text.h"
#include "../../../targets/game/content.h"

namespace dawn::client::hooks::probes::effect_trace {
namespace {

using patterns::signature;
using patterns::signature_length;

// The executable contains two byte-identical float decoders. Resolve the one actually called
// by this unique firing site instead of treating a generic decoder prologue as unique.
constexpr std::string_view kGetterCallText =
    "49 8D 8F A0 07 00 00 F3 41 0F 11 46 14 E8 ? ? ? ?";
constexpr auto kGetterCall = signature<signature_length(kGetterCallText)>(kGetterCallText);
constexpr std::array<unsigned char, 15> kGetterPrefix{
    0x48, 0x83, 0xEC, 0x28, 0xF3, 0x0F, 0x10, 0x41,
    0x04, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x30};
constexpr std::string_view kShotText =
    "41 0F 28 C5 F3 0F 59 85 80 00 00 00 41 0F 28 CD B8 01 00 00 00";
constexpr auto kShot = signature<signature_length(kShotText)>(kShotText);
constexpr std::string_view kLaunchText =
    "4C 8B DC 55 56 41 56 49 8D AB 48 FF FF FF 48 81 EC A0 01 00 00";
constexpr auto kLaunch = signature<signature_length(kLaunchText)>(kLaunchText);
constexpr std::string_view kLaunchCallText =
    "49 8B CE 41 C6 86 28 01 00 00 01 E8 ? ? ? ? 0F B6 D8 84 C0";
constexpr auto kLaunchCall = signature<signature_length(kLaunchCallText)>(kLaunchCallText);

// Profile recovered from mapped 86657.20.08.23.1800 code. Both the unique signatures and these
// RVAs must agree before any detour is placed.
constexpr std::uintptr_t kGetterRva = 0xB8B7F0;
constexpr std::uintptr_t kShotAnchorRva = 0x181D9D9;
/** The one decoder call that fires a weapon, before the native projectile initializer. */
constexpr std::uintptr_t kFireCallRva = 0x181DDC8;
constexpr std::uintptr_t kInitializerRva = 0xCF2380;
constexpr std::uintptr_t kInitializerCallRva = 0xCF21BC;
/** The decoder's argument sits this far into the weapon barrel. */
constexpr std::uintptr_t kBarrelWrapperOffset = 0x8F0;

constexpr std::uint32_t kBarrelSchema = 0x80803889U;
constexpr std::uint32_t kMagazineSchema = 0x80803A0FU;
constexpr std::uint32_t kWeaponContentSchema = 0x80803ACBU;
constexpr std::uint32_t kProjectileSchema = 0x8080388FU;
constexpr std::uint32_t kProjectileResourceSchema = 0x80803B73U;

/** A fire this recent on the launching thread is named as the launch's likely source. */
constexpr std::uint64_t kAttributionMs = 250;
/** Weapon sources tracked, and how often each one's shot count is written. */
constexpr std::size_t kSourceCapacity = 64;
constexpr std::uint64_t kFireSummaryMs = 5000;
/** Projectile definitions whose first launch gets a raw dump. */
constexpr std::size_t kDefinitionCapacity = 256;
/** Projectiles followed at once, and the ages at which each is sampled. */
constexpr std::size_t kFlightCapacity = 64;
constexpr std::array<std::uint64_t, 4> kSampleAges{50, 150, 400, 1000};

using Getter = float(__fastcall*)(const void*) noexcept;
using Initializer = bool(__fastcall*)(void*, const void*, const void*) noexcept;

SRWLOCK g_lifecycle{SRWLOCK_INIT};
std::array<hooking::detour::Handle, 2> g_handles{};
std::atomic<Getter> g_original{};
std::atomic<Initializer> g_initializer{};
std::atomic_bool g_installing{}, g_enabled{};
std::atomic_uint32_t g_active{};
std::atomic_uint64_t g_launchIds{};
std::uintptr_t g_base{};

struct ActiveCall final {
    ActiveCall() noexcept { g_active.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveCall() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
};

[[nodiscard]] bool idle() noexcept { return g_active.load(std::memory_order_acquire) == 0; }

template <std::size_t Size> Region<Size> read_region(std::uintptr_t address) noexcept {
    Region<Size> region{};
    region.address = address;
    region.readable = address >= 0x10000 && address < 0x0000800000000000ULL - Size
                      && memory::read_current_process(nullptr, address, region.bytes);
    return region;
}

// Outer pointers and array pointers are signed, self-relative. Keep these reads local and bounded.
std::uintptr_t nearby(std::uintptr_t field, std::int64_t offset) noexcept {
    if (offset == 0 || offset < -0x100000 || offset > 0x100000 || field < 0x110000
        || field >= 0x00007FFFFFF00000ULL) { return 0; }
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(field) + offset);
}

[[nodiscard]] content::handles::Source content_source() noexcept {
    return {reinterpret_cast<std::uintptr_t>(targets::game::content::get().contentHandleTablesSlot),
            nullptr, &memory::read_current_process};
}

template <std::size_t Size, std::size_t HeaderSize>
Region<Size> definition(const Region<HeaderSize>& component) noexcept {
    if (!component.readable || !targets::game::content::is_resolved()) { return {}; }
    const auto tag = component.template value<std::uint32_t>(0);
    const auto offset = component.template value<std::uint64_t>(8);
    if ((tag & 0x80000000U) == 0 || offset > 0x100000) { return {}; }
    std::uintptr_t owner = 0;
    if (!content::handles::resolve(content_source(), tag, owner) || owner >= 0x00007FFFFFF00000ULL) {
        return {};
    }
    return read_region<Size>(owner + static_cast<std::uintptr_t>(offset));
}

bool resource_snapshot(std::uint32_t resource, ResourceIdentity& current) noexcept {
    return targets::game::content::is_resolved()
           && resolve_resource(content_source(), resource, current);
}

bool resource_identity(std::uintptr_t root, std::uint32_t resource,
                       std::uint32_t expectedSchema = 0) noexcept {
    ResourceIdentity current{};
    return resource_snapshot(resource, current) && current.address == root
           && (expectedSchema == 0 || current.schema == expectedSchema);
}

template <std::size_t Size>
void report_region(const char* group, std::uint64_t id, const char* name,
                   const Region<Size>& region, std::size_t baseOffset = 0) noexcept {
    line("ev=effect_trace stage=region group=%s id=%llu name=%s address=0x%llX size=0x%zX readable=%u",
         group, static_cast<unsigned long long>(id), name,
         static_cast<unsigned long long>(region.address), Size, region.readable ? 1U : 0U);
    if (!region.readable) { return; }
    for (std::size_t offset = 0; offset < Size; offset += 32) {
        std::array<char, 80> words{};
        std::size_t used = 0;
        for (std::size_t word = offset; word + 4 <= (std::min)(Size, offset + 32); word += 4) {
            const int count = std::snprintf(words.data() + used, words.size() - used,
                "%s%08X", used == 0 ? "" : ",", region.template value<std::uint32_t>(word));
            if (count > 0) { used += static_cast<std::size_t>(count); }
        }
        line("ev=effect_trace stage=raw group=%s id=%llu name=%s offset=0x%zX words=%s",
             group, static_cast<unsigned long long>(id), name, baseOffset + offset, words.data());
    }
}

// ---- Fire -------------------------------------------------------------------------------------

/** What names a weapon source: its runtime resource and the definitions behind it. */
struct FireKey final {
    std::uint32_t resource{kNone}, object{kNone};
    std::uint64_t controllerDefinition{}, contentDefinition{};
    bool valid{};
};

/** Reads only what names the source, so an ordinary shot costs a handful of reads. */
FireKey identify_fire(std::uintptr_t barrel) noexcept {
    FireKey key{};
    const auto barrelHeader = read_region<0x30>(barrel);
    if (!barrelHeader.readable || barrelHeader.value<std::uint32_t>(0x20) != kBarrelSchema) {
        return key;
    }
    const auto controller = nearby(barrel + 0x10, barrelHeader.value<std::int64_t>(0x10));
    const auto controllerHeader = read_region<0x30>(controller);
    key.resource = barrelHeader.value<std::uint32_t>(0x28);
    key.object = controllerHeader.value<std::uint32_t>(0x2C);
    if (!controllerHeader.readable || !resource_identity(controller, key.resource)) {
        return key;
    }
    key.controllerDefinition = controllerHeader.value<std::uint64_t>(0);
    std::uint32_t contentHandle = kNone;
    ResourceIdentity content{};
    if (read(controller + 0x8C0, contentHandle) && resource_snapshot(contentHandle, content)
        && content.schema == kWeaponContentSchema) {
        key.contentDefinition = content.header.value<std::uint64_t>(0);
    }
    key.valid = true;
    return key;
}

/** The full firing context, read once per new source for its raw dump. */
FireContext capture_fire(std::uintptr_t barrel) noexcept {
    FireContext result{};
    result.tick = GetTickCount64();
    result.thread = GetCurrentThreadId();
    result.barrel = barrel;
    const auto barrelHeader = read_region<0x30>(barrel);
    if (!barrelHeader.readable || barrelHeader.value<std::uint32_t>(0x20) != kBarrelSchema) {
        return result;
    }
    result.controller = nearby(barrel + 0x10, barrelHeader.value<std::int64_t>(0x10));
    result.controllerHeader = read_region<0x30>(result.controller);
    result.resource = barrelHeader.value<std::uint32_t>(0x28);
    result.object = result.controllerHeader.value<std::uint32_t>(0x2C);
    // Generated weapon definitions vary by family; authenticate the resource pool and
    // typed embedded components instead of requiring one definition class.
    if (!resource_identity(result.controller, result.resource)) { return result; }
    const auto magazine = read_region<0x240>(result.controller + 0x24D0);
    if (!component_header_matches(magazine, kMagazineSchema, result.resource)
        || nearby(magazine.address + 0x10, magazine.value<std::int64_t>(0x10)) != result.controller) {
        return result;
    }
    result.controllerState = read_region<0x380>(result.controller + 0x8C0);
    result.magazine = magazine;
    result.magazineDefinition = definition<0x250>(magazine);
    ResourceIdentity weaponContent{};
    const auto contentHandle = result.controllerState.value<std::uint32_t>(0);
    if (result.controllerState.readable && resource_snapshot(contentHandle, weaponContent)
        && weaponContent.schema == kWeaponContentSchema) {
        result.weaponContent = read_region<0xA0>(weaponContent.address);
        result.weaponContentDefinition = definition<0x270>(result.weaponContent);
        if (!resource_identity(weaponContent.address, contentHandle, kWeaponContentSchema)) {
            result.weaponContent.readable = false;
            result.weaponContentDefinition.readable = false;
        }
    }
    result.identityValid = resource_identity(result.controller, result.resource);
    return result;
}

struct Source final {
    std::uint64_t id{}, controllerDefinition{}, contentDefinition{}, since{}, shots{}, windowShots{};
    std::uint32_t resource{kNone};
};
SRWLOCK g_sourceLock{SRWLOCK_INIT};
std::array<Source, kSourceCapacity> g_sources{};
std::size_t g_sourceCount{};
bool g_sourceOverflowReported{};

/** The last fire on this thread, which a launch shortly after it is attributed to. */
struct LastFire final {
    std::uint64_t tick{}, source{};
    std::uint32_t resource{kNone};
};
thread_local LastFire t_lastFire{};

void observe_fire(std::uintptr_t barrel) noexcept {
    const FireKey key = identify_fire(barrel);
    if (!key.valid) { return; }
    const std::uint64_t now = GetTickCount64();
    std::uint64_t sourceId = 0;
    bool created = false, overflowed = false;
    AcquireSRWLockExclusive(&g_sourceLock);
    Source* source = nullptr;
    for (std::size_t i = 0; i < g_sourceCount; ++i) {
        auto& candidate = g_sources[i];
        if (candidate.resource == key.resource
            && candidate.controllerDefinition == key.controllerDefinition
            && candidate.contentDefinition == key.contentDefinition) {
            source = &candidate;
            break;
        }
    }
    if (source == nullptr && g_sourceCount < g_sources.size()) {
        source = &g_sources[g_sourceCount];
        *source = {g_sourceCount + 1, key.controllerDefinition, key.contentDefinition, now, 0, 0,
                   key.resource};
        ++g_sourceCount;
        created = true;
    }
    if (source != nullptr) {
        ++source->shots;
        if (source->windowShots++ == 0) { source->since = now; }
        sourceId = source->id;
    } else if (!g_sourceOverflowReported) {
        g_sourceOverflowReported = overflowed = true;
    }
    ReleaseSRWLockExclusive(&g_sourceLock);
    t_lastFire = {now, sourceId, key.resource};
    if (overflowed) {
        line("ev=effect_trace stage=fire_source result=capacity_reached sources=%zu", kSourceCapacity);
    }
    if (!created) { return; }
    const FireContext fire = capture_fire(barrel);
    line("ev=effect_trace stage=fire_source source=%llu tick=%llu thread=%u resource=0x%08X "
         "object=0x%08X controller_definition=0x%llX content_definition=0x%llX identity=%u "
         "combat_attribute=0x%08X item_attribution=unresolved perk_attribution=unresolved",
         static_cast<unsigned long long>(sourceId), static_cast<unsigned long long>(fire.tick),
         fire.thread, key.resource, key.object,
         static_cast<unsigned long long>(key.controllerDefinition),
         static_cast<unsigned long long>(key.contentDefinition), fire.identityValid ? 1U : 0U,
         fire.controllerState.value<std::uint32_t>(0x374));
    report_region("fire_source", sourceId, "controller_header", fire.controllerHeader);
    report_region("fire_source", sourceId, "controller_state", fire.controllerState, 0x8C0);
    report_region("fire_source", sourceId, "weapon_content", fire.weaponContent);
    report_region("fire_source", sourceId, "weapon_content_definition", fire.weaponContentDefinition);
    report_region("fire_source", sourceId, "magazine", fire.magazine);
    report_region("fire_source", sourceId, "magazine_definition", fire.magazineDefinition);
}

/** Writes each source's shot count once its interval closes, or all of them when `all`. */
void summarize_sources(std::uint64_t now, bool all) noexcept {
    struct Summary final {
        std::uint64_t id{}, shots{}, total{}, span{};
        std::uint32_t resource{};
    };
    std::array<Summary, kSourceCapacity> summaries{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_sourceLock);
    for (std::size_t i = 0; i < g_sourceCount; ++i) {
        auto& source = g_sources[i];
        if (source.windowShots != 0 && (all || now - source.since >= kFireSummaryMs)) {
            summaries[count++] = {source.id, source.windowShots, source.shots, now - source.since,
                                  source.resource};
            source.windowShots = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_sourceLock);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& summary = summaries[i];
        line("ev=effect_trace stage=fire_summary source=%llu resource=0x%08X shots=%llu span_ms=%llu "
             "total=%llu calls_are_not_confirmed_shots=1",
             static_cast<unsigned long long>(summary.id), summary.resource,
             static_cast<unsigned long long>(summary.shots),
             static_cast<unsigned long long>(summary.span),
             static_cast<unsigned long long>(summary.total));
    }
}

// ---- Flight -----------------------------------------------------------------------------------

struct FlightSample final {
    std::uint64_t age{};
    std::array<float, 3> position{}, velocity{};
    std::uint8_t trajectories{};
};

struct Flight final {
    std::uint64_t id{}, launched{};
    std::uintptr_t component{};
    std::uint32_t resource{kNone}, object{kNone};
    std::array<FlightSample, kSampleAges.size()> samples{};
    std::size_t count{};
    bool active{};
};

SRWLOCK g_flightLock{SRWLOCK_INIT};
std::array<Flight, kFlightCapacity> g_flights{};

void report_flight(const Flight& flight, const char* end) noexcept {
    std::array<char, kLineCapacity> buffer{};
    int used = std::snprintf(buffer.data(), buffer.size(),
                             "ev=effect_trace stage=flight id=%llu object=0x%08X resource=0x%08X "
                             "samples=%zu end=%s",
                             static_cast<unsigned long long>(flight.id), flight.object,
                             flight.resource, flight.count, end);
    for (std::size_t i = 0; i < flight.count && used > 0
                            && static_cast<std::size_t>(used) < buffer.size(); ++i) {
        const auto& sample = flight.samples[i];
        const bool valid = finite(sample.position) && finite(sample.velocity);
        const int more = std::snprintf(
            buffer.data() + used, buffer.size() - static_cast<std::size_t>(used),
            " t%zu=%llu pos=%.5g,%.5g,%.5g vel=%.5g,%.5g,%.5g speed=%.5g rows=%u", i,
            static_cast<unsigned long long>(sample.age), static_cast<double>(sample.position[0]),
            static_cast<double>(sample.position[1]), static_cast<double>(sample.position[2]),
            static_cast<double>(sample.velocity[0]), static_cast<double>(sample.velocity[1]),
            static_cast<double>(sample.velocity[2]), valid ? length(sample.velocity) : 0.0,
            static_cast<unsigned>(sample.trajectories));
        if (more <= 0) { break; }
        used += more;
    }
    if (used > 0) {
        emit({buffer.data(), (std::min)(static_cast<std::size_t>(used), buffer.size() - 1)});
    }
}

/** Starts following one launched projectile, reporting whatever slot it displaces. */
void follow(std::uint64_t id, const NativeLaunch& launch) noexcept {
    Flight displaced{};
    AcquireSRWLockExclusive(&g_flightLock);
    Flight* slot = nullptr;
    for (auto& flight : g_flights) {
        if (!flight.active) { slot = &flight; break; }
    }
    if (slot == nullptr) {
        slot = &g_flights[0];
        for (auto& flight : g_flights) {
            if (flight.launched < slot->launched) { slot = &flight; }
        }
        displaced = *slot;
    }
    *slot = {id, launch.tick, launch.after.address, launch.resource, launch.object, {}, 0, true};
    ReleaseSRWLockExclusive(&g_flightLock);
    if (displaced.active) { report_flight(displaced, "displaced"); }
}

/**
 * Reads one followed projectile's first trajectory row. The component must still carry the same
 * resource before and after, so a freed and reused component is never read as the projectile.
 * @return False once the projectile is gone.
 */
bool sample_flight(std::uintptr_t address, std::uint32_t resource, FlightSample& out) noexcept {
    if (!resource_identity(address, resource, kProjectileResourceSchema)) { return false; }
    const auto component = read_region<0x1E0>(address);
    if (!component.readable || component.value<std::uint32_t>(4) != kProjectileSchema
        || component.value<std::uint32_t>(0x24) != resource) {
        return false;
    }
    const auto count = component.value<std::uint8_t>(0x1C0);
    const auto capacity = component.value<std::uint64_t>(0x1A8);
    const auto rows = nearby(component.address + 0x1B0, component.value<std::int64_t>(0x1B0));
    out.trajectories = count;
    if (rows != 0 && count != 0 && capacity <= 256 && count <= capacity) {
        const auto row = read_region<0x20>(rows + 0x40);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            out.position[axis] = row.value<float>(axis * 4);
            out.velocity[axis] = row.value<float>(0x10 + axis * 4);
        }
    }
    return resource_identity(address, resource, kProjectileResourceSchema);
}

// ---- Launch -----------------------------------------------------------------------------------

SRWLOCK g_definitionLock{SRWLOCK_INIT};
std::array<std::uint64_t, kDefinitionCapacity> g_definitions{};
std::size_t g_definitionCount{};

/** @return True the first time a projectile definition is seen, while there is room to remember it. */
bool first_definition(std::uint64_t handle) noexcept {
    AcquireSRWLockExclusive(&g_definitionLock);
    bool first = false;
    if (std::find(g_definitions.begin(), g_definitions.begin() + g_definitionCount, handle)
        == g_definitions.begin() + g_definitionCount && g_definitionCount < g_definitions.size()) {
        g_definitions[g_definitionCount++] = handle;
        first = true;
    }
    ReleaseSRWLockExclusive(&g_definitionLock);
    return first;
}

void report_launch(const NativeLaunch& launch) noexcept {
    const auto id = g_launchIds.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto definitionHandle = launch.before.value<std::uint64_t>(0);
    const LastFire fire = t_lastFire;
    const bool attributed = fire.resource != kNone && launch.tick >= fire.tick
                            && launch.tick - fire.tick <= kAttributionMs;
    std::array<float, 3> velocity{};
    bool moving = false;
    if (launch.trajectoryCount != 0 && launch.identityValid && launch.succeeded) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            velocity[axis] = launch.trajectories[0].value<float>(0x10 + axis * 4);
        }
        moving = finite(velocity);
    }
    const bool typed = launch.before.value<std::uint32_t>(4) == kProjectileSchema;
    line("ev=effect_trace stage=launch id=%llu tick=%llu thread=%u object=0x%08X resource=0x%08X "
         "definition=0x%llX caller=+0x%llX succeeded=%u identity=%u speed_multiplier=%.6g "
         "damage=%.6g,%.6g authored_speed=%.6g speed_field=%.6g,%.6g factor=%.6g "
         "trajectories=%zu velocity=%.6g,%.6g,%.6g speed=%.6g fire_source=%llu fire_ms=%lld",
         static_cast<unsigned long long>(id), static_cast<unsigned long long>(launch.tick),
         launch.thread, launch.object, launch.resource,
         static_cast<unsigned long long>(definitionHandle),
         static_cast<unsigned long long>(launch.callerRva), launch.succeeded ? 1U : 0U,
         launch.identityValid ? 1U : 0U,
         static_cast<double>(launch.parameters.value<float>(0x28)),
         static_cast<double>(launch.parameters.value<float>(0x34)),
         static_cast<double>(launch.parameters.value<float>(0x38)),
         typed ? static_cast<double>(launch.definition.value<float>(0x88)) : 0.0,
         static_cast<double>(launch.before.value<float>(0x144)),
         launch.identityValid ? static_cast<double>(launch.after.value<float>(0x144)) : 0.0,
         launch.identityValid ? static_cast<double>(launch.after.value<float>(0x140)) : 0.0,
         launch.trajectoryCount, static_cast<double>(velocity[0]),
         static_cast<double>(velocity[1]), static_cast<double>(velocity[2]),
         moving ? length(velocity) : 0.0,
         static_cast<unsigned long long>(attributed ? fire.source : 0),
         attributed ? static_cast<long long>(launch.tick - fire.tick) : -1LL);
    if (definitionHandle != 0 && first_definition(definitionHandle)) {
        report_region("launch", id, "parameters", launch.parameters);
        report_region("launch", id, "component_before", launch.before);
        report_region("launch", id, "component_after", launch.after);
        report_region("launch", id, "projectile_definition", launch.definition);
        for (std::size_t i = 0; i < launch.trajectoryCount; ++i) {
            report_region("launch", id, i == 0 ? "trajectory_0" : i == 1 ? "trajectory_1"
                          : i == 2 ? "trajectory_2" : "trajectory_3", launch.trajectories[i], 0x40);
        }
    }
    if (launch.succeeded && launch.identityValid) {
        follow(id, launch);
    }
}

__declspec(noinline) float __fastcall get_float(const void* wrapper) noexcept {
    ActiveCall active;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    Getter original = g_original.load(std::memory_order_acquire);
    while (original == nullptr && g_installing.load(std::memory_order_acquire)) {
        YieldProcessor();
        original = g_original.load(std::memory_order_acquire);
    }
    if (original == nullptr) {
        original = g_original.load(std::memory_order_acquire);
    }
    // Never invoke the decoder speculatively: observe the result of the caller's own invocation.
    const float value = original != nullptr ? original(wrapper) : 0.0F;
    if (caller == g_base + kFireCallRva && g_enabled.load(std::memory_order_acquire)) {
        observe_fire(reinterpret_cast<std::uintptr_t>(wrapper) - kBarrelWrapperOffset);
    }
    return value;
}

__declspec(noinline) bool __fastcall initialize_projectile(
    void* component, const void* entry, const void* parameters) noexcept {
    ActiveCall active;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool observe = g_enabled.load(std::memory_order_acquire);
    NativeLaunch record{};
    if (observe) {
        record.tick = GetTickCount64();
        record.thread = GetCurrentThreadId();
        record.callerRva = caller >= g_base ? caller - g_base : 0;
        record.parameters = read_region<0xB0>(reinterpret_cast<std::uintptr_t>(parameters));
        record.before = read_region<0x1E0>(reinterpret_cast<std::uintptr_t>(component));
        record.definition = definition<0xE0>(record.before);
    }
    auto original = g_initializer.load(std::memory_order_acquire);
    while (original == nullptr && g_installing.load(std::memory_order_acquire)) {
        YieldProcessor();
        original = g_initializer.load(std::memory_order_acquire);
    }
    if (original == nullptr) { original = g_initializer.load(std::memory_order_acquire); }
    const bool result = original != nullptr ? original(component, entry, parameters) : false;
    if (observe) {
        record.succeeded = result;
        record.after = read_region<0x1E0>(reinterpret_cast<std::uintptr_t>(component));
        record.object = record.after.value<std::uint32_t>(0x2C);
        record.resource = record.after.value<std::uint32_t>(0x24);
        record.identityValid = record.before.readable && record.after.readable
            && record.before.value<std::uint32_t>(4) == kProjectileSchema
            && record.after.value<std::uint32_t>(4) == kProjectileSchema
            && record.before.value<std::uint32_t>(0x24) == record.resource
            && resource_identity(record.after.address, record.resource, kProjectileResourceSchema);
        const auto count = record.after.value<std::uint8_t>(0x1C0);
        const auto capacity = record.after.value<std::uint64_t>(0x1A8);
        const auto rows = nearby(record.after.address + 0x1B0, record.after.value<std::int64_t>(0x1B0));
        if (result && record.identityValid && rows != 0 && capacity <= 256 && count <= capacity) {
            record.trajectoryCount = (std::min)(static_cast<std::size_t>(count), record.trajectories.size());
            for (std::size_t i = 0; i < record.trajectoryCount; ++i) {
                record.trajectories[i] = read_region<0x20>(rows + i * 0x210 + 0x40);
            }
            record.identityValid = resource_identity(record.after.address, record.resource,
                                                     kProjectileResourceSchema);
        }
        report_launch(record);
    }
    return result;
}

} // namespace

void service_flights(std::uint64_t now) noexcept {
    summarize_sources(now, false);
    struct Due final {
        std::size_t slot{};
        std::uint64_t id{}, launched{};
        std::uintptr_t component{};
        std::uint32_t resource{};
    };
    std::array<Due, kFlightCapacity> due{};
    std::size_t dueCount = 0;
    AcquireSRWLockShared(&g_flightLock);
    for (std::size_t i = 0; i < g_flights.size(); ++i) {
        const auto& flight = g_flights[i];
        if (flight.active && flight.count < kSampleAges.size()
            && now - flight.launched >= kSampleAges[flight.count]) {
            due[dueCount++] = {i, flight.id, flight.launched, flight.component, flight.resource};
        }
    }
    ReleaseSRWLockShared(&g_flightLock);
    if (dueCount == 0) { return; }
    // Memory is read with the lock released, so a launch on a game thread never waits on it.
    std::array<FlightSample, kFlightCapacity> taken{};
    std::array<bool, kFlightCapacity> alive{};
    for (std::size_t i = 0; i < dueCount; ++i) {
        taken[i].age = now - due[i].launched;
        alive[i] = sample_flight(due[i].component, due[i].resource, taken[i]);
    }
    std::array<Flight, kFlightCapacity> finished{};
    std::array<const char*, kFlightCapacity> ends{};
    std::size_t finishedCount = 0;
    AcquireSRWLockExclusive(&g_flightLock);
    for (std::size_t i = 0; i < dueCount; ++i) {
        auto& flight = g_flights[due[i].slot];
        if (!flight.active || flight.id != due[i].id) { continue; }
        if (alive[i]) {
            flight.samples[flight.count++] = taken[i];
            if (flight.count < kSampleAges.size()) { continue; }
        }
        finished[finishedCount] = flight;
        ends[finishedCount++] = alive[i] ? "complete" : "released";
        flight = {};
    }
    ReleaseSRWLockExclusive(&g_flightLock);
    for (std::size_t i = 0; i < finishedCount; ++i) {
        report_flight(finished[i], ends[i]);
    }
}

void flush_flights() noexcept {
    summarize_sources(GetTickCount64(), true);
    std::array<Flight, kFlightCapacity> remaining{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_flightLock);
    for (auto& flight : g_flights) {
        if (flight.active) {
            remaining[count++] = flight;
            flight = {};
        }
    }
    ReleaseSRWLockExclusive(&g_flightLock);
    for (std::size_t i = 0; i < count; ++i) {
        report_flight(remaining[i], "shutdown");
    }
}

bool install_projectiles() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_handles[0].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* getterCall = patterns::scan_main_image_unique(kGetterCall, "effect_trace_float_callsite");
    auto* getter = getterCall != nullptr
                       ? patterns::resolve_relative(getterCall + 14, getterCall + 18) : nullptr;
    auto* shot = patterns::scan_main_image_unique(kShot, "effect_trace_shot_anchor");
    auto* launch = patterns::scan_main_image_unique(kLaunch, "effect_trace_initializer");
    auto* launchCall = patterns::scan_main_image_unique(kLaunchCall, "effect_trace_initializer_call");
    std::array<unsigned char, kGetterPrefix.size()> prefix{};
    if (getter == nullptr || shot == nullptr || launch == nullptr || launchCall == nullptr
        || reinterpret_cast<std::uintptr_t>(launch) != g_base + kInitializerRva
        || reinterpret_cast<std::uintptr_t>(launchCall) != g_base + kInitializerCallRva
        || patterns::resolve_relative(launchCall + 12, launchCall + 16) != launch
        || reinterpret_cast<std::uintptr_t>(getter) != g_base + kGetterRva
        || reinterpret_cast<std::uintptr_t>(shot) != g_base + kShotAnchorRva
        || !memory::read_current_process(nullptr, reinterpret_cast<std::uintptr_t>(getter),
                                         std::as_writable_bytes(std::span(prefix)))
        || prefix != kGetterPrefix) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        line("ev=effect_trace stage=install part=projectiles result=unavailable reason=code_profile");
        return false;
    }
    g_installing.store(true, std::memory_order_release);
    const std::array specs{hooking::detour::Spec{getter, reinterpret_cast<void*>(&get_float)},
        hooking::detour::Spec{launch, reinterpret_cast<void*>(&initialize_projectile)}};
    if (!hooking::detour::install(specs, g_handles)) {
        g_installing.store(false, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lifecycle);
        line("ev=effect_trace stage=install part=projectiles result=unavailable reason=detour");
        return false;
    }
    g_original.store(reinterpret_cast<Getter>(g_handles[0].original), std::memory_order_release);
    g_initializer.store(reinterpret_cast<Initializer>(g_handles[1].original), std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);
    g_installing.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    return true;
}

bool uninstall_projectiles() noexcept {
    g_enabled.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lifecycle);
    if (!g_handles[0].attached && !g_handles[1].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    const std::array entries{hooking::detour::ProtectedCodeEntry{
        reinterpret_cast<void*>(&get_float)}, hooking::detour::ProtectedCodeEntry{
        reinterpret_cast<void*>(&initialize_projectile)}};
    const auto result = probes::detach(g_handles, entries, &idle);
    if (result == hooking::detour::UninstallResult::removed) {
        g_original.store(nullptr, std::memory_order_release);
        g_initializer.store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    return result == hooking::detour::UninstallResult::removed;
}

} // namespace dawn::client::hooks::probes::effect_trace
