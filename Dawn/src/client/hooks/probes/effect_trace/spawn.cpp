/**
 * Kind-3 perk spawn requests: the graph asked for, who asked, where it is placed, and whether its
 * asset is resident. Neither the builder nor the object factory is observed, so no outcome is
 * claimed.
 */

#include "budget.h"
#include "internal.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <string_view>

#include "../../../content/handles/handle_resolver.h"
#include "../../../hooking/detour.h"
#include "../detach.h"
#include "../../../patterns/image_scan.h"
#include "../../../patterns/signature_text.h"
#include "../../../targets/game/content.h"

namespace dawn::client::hooks::probes::effect_trace {
namespace {

using Spawn = void(__fastcall*)(void*, const void*, void*, const void*);
using patterns::signature;
using patterns::signature_length;
// Native kind-3 activation in 86657.20.08.23.1800. The direct builder call validates the profile.
constexpr std::string_view kSpawnText =
    "40 55 53 56 57 41 56 48 8D AC 24 10 F8 FF FF 48 81 EC F0 08 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 E0 07 00 00 48 8B F2 4D 8B F1";
constexpr auto kSpawn = signature<signature_length(kSpawnText)>(kSpawnText);
constexpr std::uintptr_t kSpawnRva = 0x1089900;
/** The builder call inside the activation: opcode offset, and where the call lands. */
constexpr std::size_t kBuilderCall = 0xDD;
constexpr std::uintptr_t kBuilderRva = 0x4B25F0;

/** Requests written per graph per interval; the rest are counted and summarized. */
constexpr std::uint32_t kRequestsPerInterval = 8;
constexpr std::uint64_t kIntervalMs = 10'000;
constexpr std::size_t kGraphCapacity = 512;

SRWLOCK g_lifecycle{SRWLOCK_INIT}, g_lock{SRWLOCK_INIT};
hooking::detour::Handle g_hook{};
std::atomic<Spawn> g_spawn{};
std::atomic_bool g_installing{}, g_enabled{};
std::atomic_uint32_t g_active{};
std::atomic_uint64_t g_requestIds{};

/** Requests per graph. Guarded by `g_lock`. */
using Budgets = RollingBudget<kGraphCapacity, kRequestsPerInterval, kIntervalMs>;
Budgets g_budgets{};

struct Active final {
    Active() noexcept { g_active.fetch_add(1, std::memory_order_acq_rel); }
    ~Active() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
};

bool idle() noexcept { return g_active.load(std::memory_order_acquire) == 0; }

void report_suppressed(const Budgets::Ended& ended) noexcept {
    line("ev=effect_trace stage=spawn_suppressed graph=0x%08X requests=%u interval_ms=%llu",
         static_cast<std::uint32_t>(ended.key), ended.suppressed,
         static_cast<unsigned long long>(ended.spanMs));
}

void describe_asset(std::uint64_t id, std::uint32_t tag) noexcept {
    std::uintptr_t address = 0;
    bool loaded = false;
    std::uint8_t type = 0xFF;
    std::uint32_t flags = 0;
    if (targets::game::content::is_resolved()) {
        const content::handles::Source source{
            reinterpret_cast<std::uintptr_t>(targets::game::content::get().contentHandleTablesSlot),
            nullptr, &memory::read_current_process};
        loaded = content::handles::resolve(source, tag, address)
            && read(address + 0x96, type) && read(address + 0x98, flags);
    }
    line("ev=effect_trace stage=spawn_asset request=%llu graph=0x%08X loaded=%u type=%u flags=0x%08X",
         static_cast<unsigned long long>(id), tag, loaded ? 1U : 0U, static_cast<unsigned>(type),
         flags);
}

// Only the native call is forwarded. No game resolver or factory is called for observation.
__declspec(noinline) void __fastcall spawn(void* context, const void* effect, void* state,
                                           const void* event) {
    Active active;
    while (g_installing.load(std::memory_order_acquire)) { YieldProcessor(); }
    const auto original = g_spawn.load(std::memory_order_acquire);
    if (original == nullptr) { return; }
    std::array<std::uint8_t, 24> bytes{};
    std::uint64_t id = 0;
    std::uint32_t graph = kNone;
    if (g_enabled.load(std::memory_order_acquire)
        && read(reinterpret_cast<std::uintptr_t>(effect), bytes) && bytes[0] == 3) {
        std::memcpy(&graph, bytes.data() + 0x10, sizeof(graph));
        const std::uint64_t now = GetTickCount64();
        Budgets::Ended ended{};
        AcquireSRWLockExclusive(&g_lock);
        const bool admitted = g_budgets.admit(graph, now, ended);
        ReleaseSRWLockExclusive(&g_lock);
        if (ended.suppressed != 0) {
            report_suppressed(ended);
        }
        if (admitted) {
            id = g_requestIds.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    }
    if (id != 0) {
        std::uint32_t owner = kNone, target = kNone;
        const bool ownerRead = read(reinterpret_cast<std::uintptr_t>(context) + 4, owner);
        const bool targetRead = read(reinterpret_cast<std::uintptr_t>(event) + 0x14, target);
        line("ev=effect_trace stage=spawn request=%llu graph=0x%08X effect=0x%llX "
             "context=0x%llX owner=0x%08X owner_read=%u event_target=0x%08X target_read=%u "
             "position_mode=%u surface_adjust=%u orientation_mode=%u",
             static_cast<unsigned long long>(id), graph,
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(effect)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)), owner,
             ownerRead ? 1U : 0U, target, targetRead ? 1U : 0U, unsigned(bytes[4]),
             unsigned(bytes[2]), unsigned(bytes[3]));
        describe_asset(id, graph);
    }
    original(context, effect, state, event);
}

} // namespace

void service_spawn_budgets(std::uint64_t now, bool closing) noexcept {
    std::array<Budgets::Ended, kGraphCapacity> ended{};
    AcquireSRWLockExclusive(&g_lock);
    const std::size_t count = g_budgets.collect(now, closing, ended);
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t i = 0; i < count; ++i) {
        report_suppressed(ended[i]);
    }
}

bool install_spawn() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_hook.attached) { ReleaseSRWLockExclusive(&g_lifecycle); return true; }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = patterns::scan_main_image_unique(kSpawn, "effect_trace_spawn");
    auto* builder = target == nullptr
        ? nullptr
        : patterns::resolve_relative(target + kBuilderCall + 1, target + kBuilderCall + 5);
    // Profile and call opcode agree before detouring. Fail closed on another executable. The
    // builder itself is only located, never detoured: Dawn's Omega hooks own that entry.
    std::uint8_t call = 0;
    if (target == nullptr || builder == nullptr
        || reinterpret_cast<std::uintptr_t>(target) != base + kSpawnRva
        || reinterpret_cast<std::uintptr_t>(builder) != base + kBuilderRva
        || !read(reinterpret_cast<std::uintptr_t>(target) + kBuilderCall, call) || call != 0xE8) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        line("ev=effect_trace stage=install part=spawn result=unavailable reason=code_profile");
        return false;
    }
    g_installing.store(true, std::memory_order_release);
    const bool installed = hooking::detour::install(
        hooking::detour::Spec{target, reinterpret_cast<void*>(&spawn)}, g_hook);
    if (installed) {
        g_spawn.store(reinterpret_cast<Spawn>(g_hook.original), std::memory_order_release);
        g_enabled.store(true, std::memory_order_release);
    }
    g_installing.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    if (!installed) {
        line("ev=effect_trace stage=install part=spawn result=unavailable reason=detour");
    }
    return installed;
}

bool uninstall_spawn() noexcept {
    g_enabled.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lifecycle);
    if (!g_hook.attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    const std::array entries{hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&spawn)}};
    const auto result = probes::detach(std::span(&g_hook, 1), entries, &idle);
    if (result == hooking::detour::UninstallResult::removed) {
        g_spawn.store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    return result == hooking::detour::UninstallResult::removed;
}

} // namespace dawn::client::hooks::probes::effect_trace
