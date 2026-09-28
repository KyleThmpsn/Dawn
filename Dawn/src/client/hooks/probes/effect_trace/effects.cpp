/**
 * Every perk effect the game applies or removes, whatever its kind.
 * Each effect node starts with a kind byte, and the game applies or removes it through one of two
 * short thunks that jump into a table of per-kind handlers. Detouring the two thunks sees every
 * effect once, before its handler runs, with the definition tag that owns it and the owner it
 * acts for, so a perk can be followed without knowing its effect kinds in advance.
 */

#include "budget.h"
#include "internal.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>

#include "../../../hooking/detour.h"
#include "../detach.h"
#include "../../../patterns/image_scan.h"
#include "../../../patterns/signature_text.h"

namespace dawn::client::hooks::probes::effect_trace {
namespace {

/** Applies or removes one effect node: a tail jump into the handler table for the node's kind. */
using Dispatch = std::uint64_t(__fastcall*)(const void* node, void* context, std::uint64_t retained,
                                            const void* event);

using patterns::signature;
using patterns::signature_length;
// movsx rax,[rcx]; swap the first two arguments; jmp [table + kind * 40 + slot] in
// 86657.20.08.23.1800. The same shape serves the table's other slots, so the table
// displacement is part of each text, which keeps both unique.
constexpr std::string_view kApplyText =
    "48 0F BE 01 4C 8B DA 48 8B D1 49 8B CB 4C 8D 14 80 48 8D 05 F8 A5 91 01 4A FF 24 D0";
constexpr std::string_view kRemoveText =
    "48 0F BE 01 4C 8B D2 48 8B D1 49 8B CA 4C 8D 0C 80 48 8D 05 C8 A1 90 01 4A FF 24 C8";
constexpr auto kApply = signature<signature_length(kApplyText)>(kApplyText);
constexpr auto kRemove = signature<signature_length(kRemoveText)>(kRemoveText);
constexpr std::uintptr_t kApplyRva = 0xEF5C90;
constexpr std::uintptr_t kRemoveRva = 0xF060D0;
/** The handler table, 55 kinds of five handlers; apply runs the first, remove the third. */
constexpr std::uintptr_t kTableRva = 0x28102A0;
constexpr std::ptrdiff_t kRemoveSlot = 0x10;
/** The table lea inside each thunk: its operand, and the instruction after it. */
constexpr std::size_t kTableOperand = 0x14;
constexpr std::size_t kTableNext = 0x18;

/** Lines written per definition and action per interval; the rest are counted. */
constexpr std::uint32_t kLinesPerInterval = 16;
constexpr std::uint64_t kIntervalMs = 10'000;
constexpr std::size_t kBudgetCapacity = 1024;
/** Definitions and nodes described in full the first time they appear. */
constexpr std::size_t kDefinitionCapacity = 512;
constexpr std::size_t kNodeCapacity = 1024;
constexpr std::size_t kNodeBytes = 48;

using Budgets = RollingBudget<kBudgetCapacity, kLinesPerInterval, kIntervalMs>;

SRWLOCK g_lifecycle{SRWLOCK_INIT};
/** Guards the budgets and the first-sight sets below. */
SRWLOCK g_lock{SRWLOCK_INIT};
std::array<hooking::detour::Handle, 2> g_handles{};
std::atomic<Dispatch> g_apply{}, g_remove{};
std::atomic_bool g_installing{}, g_enabled{};
std::atomic_uint32_t g_active{};
std::uintptr_t g_imageBase{};
Budgets g_budgets{};
std::array<std::uint32_t, kDefinitionCapacity> g_definitions{};
std::size_t g_definitionCount{};
std::array<std::uintptr_t, kNodeCapacity> g_nodes{};
std::size_t g_nodeCount{};

struct Active final {
    Active() noexcept { g_active.fetch_add(1, std::memory_order_acq_rel); }
    ~Active() { g_active.fetch_sub(1, std::memory_order_acq_rel); }
};

bool idle() noexcept { return g_active.load(std::memory_order_acquire) == 0; }

/** @return True the first time a value is seen, while the set has room. Caller holds `g_lock`. */
template <typename Value, std::size_t Capacity>
[[nodiscard]] bool first_sight(std::array<Value, Capacity>& seen, std::size_t& count,
                               Value value) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (seen[i] == value) {
            return false;
        }
    }
    if (count == seen.size()) {
        return false;
    }
    seen[count++] = value;
    return true;
}

/** The budget key: one budget per definition for applies and one for removals. */
constexpr std::uint64_t budget_key(std::uint32_t definition, bool removal) noexcept {
    return (static_cast<std::uint64_t>(definition) << 1) | (removal ? 1U : 0U);
}

void report_suppressed(const Budgets::Ended& ended) noexcept {
    line("ev=effect_trace stage=effect_suppressed action=%s definition=0x%08X lines=%u "
         "interval_ms=%llu", (ended.key & 1U) != 0 ? "remove" : "apply",
         static_cast<std::uint32_t>(ended.key >> 1), ended.suppressed,
         static_cast<unsigned long long>(ended.spanMs));
}

/** Writes the effect line, and the first-sight dumps for a new definition or node. */
void observe(bool removal, const void* node, const void* context, std::uint64_t retained,
             const void* event, std::uintptr_t caller) noexcept {
    const auto nodeAddress = reinterpret_cast<std::uintptr_t>(node);
    std::uint8_t kind = 0;
    // The context opens with the definition tag that owns the effect list, then the owner.
    std::array<std::uint32_t, 8> words{};
    if (!read(nodeAddress, kind) || !read(reinterpret_cast<std::uintptr_t>(context), words)) {
        return;
    }
    const std::uint32_t definition = words[0];
    const std::uint64_t now = GetTickCount64();
    Budgets::Ended ended{};
    AcquireSRWLockExclusive(&g_lock);
    const bool admitted = g_budgets.admit(budget_key(definition, removal), now, ended);
    const bool newDefinition =
        admitted && first_sight(g_definitions, g_definitionCount, definition);
    const bool newNode = admitted && first_sight(g_nodes, g_nodeCount, nodeAddress);
    ReleaseSRWLockExclusive(&g_lock);
    if (ended.suppressed != 0) {
        report_suppressed(ended);
    }
    if (!admitted) {
        return;
    }
    line("ev=effect_trace stage=effect action=%s kind=%u definition=0x%08X owner=0x%08X "
         "node=0x%llX retained=%d caller=+0x%llX", removal ? "remove" : "apply",
         static_cast<unsigned>(kind), definition, words[1],
         static_cast<unsigned long long>(nodeAddress), static_cast<int>(retained & 0xFFFFFFFFU),
         static_cast<unsigned long long>(caller));
    if (newDefinition) {
        // The remove thunk reuses the fourth argument register before its jump, so a removal
        // carries no event to read.
        std::array<std::uint32_t, 8> eventWords{};
        const bool eventRead =
            !removal && read(reinterpret_cast<std::uintptr_t>(event), eventWords);
        line("ev=effect_trace stage=effect_definition definition=0x%08X "
             "context=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X "
             "event_read=%u event=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X", definition,
             words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7],
             eventRead ? 1U : 0U, eventWords[0], eventWords[1], eventWords[2], eventWords[3],
             eventWords[4], eventWords[5], eventWords[6], eventWords[7]);
    }
    if (newNode) {
        std::array<std::uint8_t, kNodeBytes> bytes{};
        if (read(nodeAddress, bytes)) {
            constexpr char kDigits[] = "0123456789ABCDEF";
            std::array<char, kNodeBytes * 2 + 1> hex{};
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                hex[i * 2] = kDigits[bytes[i] >> 4U];
                hex[i * 2 + 1] = kDigits[bytes[i] & 0xFU];
            }
            line("ev=effect_trace stage=effect_node node=0x%llX kind=%u definition=0x%08X bytes=%s",
                 static_cast<unsigned long long>(nodeAddress), static_cast<unsigned>(kind),
                 definition, hex.data());
        }
    }
}

[[nodiscard]] Dispatch await_original(std::atomic<Dispatch>& slot) noexcept {
    // A call can reach a replacement in the few instructions before install publishes it.
    while (g_installing.load(std::memory_order_acquire)) {
        YieldProcessor();
    }
    return slot.load(std::memory_order_acquire);
}

/**
 * Everything a replacement does before it forwards. The in-flight count covers only this, so a
 * replacement can end in a tail jump and hold nothing once it has jumped.
 * @return The trampoline to forward to, or null while none is published.
 */
__declspec(noinline) Dispatch prepare(std::atomic<Dispatch>& slot, bool removal, const void* node,
                                      const void* context, std::uint64_t retained,
                                      const void* event, std::uintptr_t caller) noexcept {
    Active active;
    const Dispatch original = await_original(slot);
    if (original != nullptr && g_enabled.load(std::memory_order_acquire)) {
        observe(removal, node, context, retained, event, caller);
    }
    return original;
}

// Only the native call is forwarded, with every argument as received. It is the last thing each
// replacement does, so it compiles to a jump: the handler behind the thunk then returns straight
// to the game's caller and sees the same return address it would without this hook.
__declspec(noinline) std::uint64_t __fastcall apply_effect(const void* node, void* context,
                                                           std::uint64_t retained,
                                                           const void* event) {
    const Dispatch original = prepare(g_apply, false, node, context, retained, event,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_imageBase);
    return original != nullptr ? original(node, context, retained, event) : 0;
}

__declspec(noinline) std::uint64_t __fastcall remove_effect(const void* node, void* context,
                                                            std::uint64_t retained,
                                                            const void* event) {
    const Dispatch original = prepare(g_remove, true, node, context, retained, event,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_imageBase);
    return original != nullptr ? original(node, context, retained, event) : 0;
}

} // namespace

void service_effect_budgets(std::uint64_t now, bool closing) noexcept {
    std::array<Budgets::Ended, kBudgetCapacity> ended{};
    AcquireSRWLockExclusive(&g_lock);
    const std::size_t count = g_budgets.collect(now, closing, ended);
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t i = 0; i < count; ++i) {
        report_suppressed(ended[i]);
    }
}

bool install_effects() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_handles[0].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::byte* const apply = patterns::scan_main_image_unique(kApply, "effect_trace_effect_apply");
    std::byte* const remove = patterns::scan_main_image_unique(kRemove, "effect_trace_effect_remove");
    const std::byte* const applyTable = apply == nullptr
        ? nullptr : patterns::resolve_relative(apply + kTableOperand, apply + kTableNext);
    const std::byte* const removeTable = remove == nullptr
        ? nullptr : patterns::resolve_relative(remove + kTableOperand, remove + kTableNext);
    // Both thunks must index the one handler table, run and undo slots. Fail closed otherwise.
    if (apply == nullptr || remove == nullptr || applyTable == nullptr || removeTable == nullptr
        || reinterpret_cast<std::uintptr_t>(apply) != base + kApplyRva
        || reinterpret_cast<std::uintptr_t>(remove) != base + kRemoveRva
        || reinterpret_cast<std::uintptr_t>(applyTable) != base + kTableRva
        || removeTable != applyTable + kRemoveSlot) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        line("ev=effect_trace stage=install part=effects result=unavailable reason=code_profile");
        return false;
    }
    g_imageBase = base;
    AcquireSRWLockExclusive(&g_lock);
    g_budgets.clear();
    g_definitionCount = 0;
    g_nodeCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_installing.store(true, std::memory_order_release);
    const std::array specs{
        hooking::detour::Spec{apply, reinterpret_cast<void*>(&apply_effect)},
        hooking::detour::Spec{remove, reinterpret_cast<void*>(&remove_effect)}};
    const bool installed = hooking::detour::install(specs, g_handles);
    if (installed) {
        g_apply.store(reinterpret_cast<Dispatch>(g_handles[0].original),
                      std::memory_order_release);
        g_remove.store(reinterpret_cast<Dispatch>(g_handles[1].original),
                       std::memory_order_release);
        g_enabled.store(true, std::memory_order_release);
    }
    g_installing.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    if (!installed) {
        line("ev=effect_trace stage=install part=effects result=unavailable reason=detour");
    }
    return installed;
}

bool uninstall_effects() noexcept {
    g_enabled.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lifecycle);
    if (!g_handles[0].attached && !g_handles[1].attached) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        return true;
    }
    // `prepare` drops its in-flight count just before it returns the trampoline, so it is
    // protected too: no removal lands between that and a replacement's jump. A thread already
    // inside a trampoline is moved out of it by Detours.
    const std::array entries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&apply_effect)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&remove_effect)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&prepare)}};
    const auto result = probes::detach(g_handles, entries, &idle);
    if (result == hooking::detour::UninstallResult::removed) {
        g_apply.store(nullptr, std::memory_order_release);
        g_remove.store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    return result == hooking::detour::UninstallResult::removed;
}

} // namespace dawn::client::hooks::probes::effect_trace
