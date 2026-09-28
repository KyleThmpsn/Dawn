/**
 * Input, ability and animation paths for the IDs you list: weapon and melee input, ability input
 * and attack selection, clip resolution, animation selector choices, held pose choices, private
 * conditions, Create and Remove Entity, player messages, and health.
 * Every hook forwards the native call unchanged and queues a sample; a thread of its own writes
 * them, so gameplay never waits on the disk.
 *
 * Turn on: `"action_trace": true` in the `client` section of Dawn\settings.json, and
 * Dawn\action_trace_watch.txt in place before the game starts. The file is read once, at
 * startup.
 * Watch file: one letter and eight uppercase hex digits per line. Blank lines are skipped. Up to
 * 4096 IDs per letter, 256 for M, and 176 KiB.
 *   C  animation clip
 *   P  animation selector profile
 *   I  input owner; also brings in the melee and conditions it owns
 *   A  Create or Remove Entity graph tag
 *   D  melee or ability definition
 *   H  health component tag, on any player, enemy, object or shield; each write to one of its
 *      region fractions
 *   M  player message type, the header's low byte, from any player
 * For example:
 *   I1A2B3C4D
 *   M00000075
 * Anything else, including an unknown letter, refuses the whole file: no hook attaches, and
 * Dawn\logs\dawn.log gets an `ev=action_trace stage=watch result=fail` line with the reason,
 * and the line number when one line is at fault.
 * Output: Dawn\action_trace.log, with one `kind=` line per sample. It is recreated once the
 * watch file and the game code checks pass, so after a refusal the file from an earlier launch
 * remains.
 * Its first line names every kind number. Otherwise Dawn\logs\dawn.log gets only
 * `ev=action_trace stage=install`: an info line when it installs, which needs
 * `core.logging.levels.client` at `info` or `debug`, and a warn line with the reason when it
 * cannot.
 * Limits: 32768 samples. An unchanged sample from the same call site is skipped for 2 seconds,
 * a held pose choice is written at most once a second, and a health region that is refilling
 * at most four times a second; every drop is written.
 */

#include "action_trace.h"

#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../diagnostics/module_range.h"
#include "../../../hooking/call_gate.h"
#include "../../../hooking/detour.h"
#include "../detach.h"
#include "../../bootflow/gateway_native_read.h"

namespace dawn::client::hooks::probes::action_trace {
namespace {
using Resolve = std::uintptr_t(__fastcall*)(void*, const void*, std::uint32_t);
using Select = std::int32_t(__fastcall*)(const void*, const void*, const void*, void*);
using Input = void(__fastcall*)(void*, float, std::uint32_t, const void*);
using Message = void(__fastcall*)(void*, const void*, std::uint32_t);
using AbilityInput = void(__fastcall*)(void*, void*, const void*);
using AttackSelection = const void*(__fastcall*)(void*, bool, bool, void*, void*, void*);
using EntityEffect = void(__fastcall*)(void*, const void*, std::uint32_t, const void*);
using Condition = bool(__fastcall*)(const void*, void*, const void*, const void*);
using PoseChoice = void(__fastcall*)(void*, std::uint32_t, float, void*, const void*, std::uint16_t, const void*, float);
using HealthFraction = void(__fastcall*)(void*, std::int32_t, float);
std::atomic<Resolve> g_resolve{};
std::atomic<Select> g_select{};
std::atomic<Input> g_input{};
std::atomic<Message> g_message{};
std::atomic<AbilityInput> g_abilityInput{};
std::atomic<AbilityInput> g_baseAbilityInput{};
std::atomic<AttackSelection> g_attackSelection{};
std::atomic<EntityEffect> g_createEntity{}, g_removeEntity{};
std::atomic<Condition> g_condition{};
std::atomic<PoseChoice> g_poseChoice{};
std::atomic<HealthFraction> g_healthFraction{};
hooking::CallGate g_calls;
/** The health hook is last and attaches only when the watch file lists health components. */
std::array<hooking::detour::Handle, 12> g_handles{};
std::size_t g_handleCount{};
SRWLOCK g_lifecycle{SRWLOCK_INIT};
SRWLOCK g_queueLock{SRWLOCK_INIT};
HANDLE g_file{INVALID_HANDLE_VALUE}, g_stop{}, g_worker{};
diagnostics::ModuleRange g_image{};

struct Sample {
    std::uint64_t tick{}, caller{}, object{}, resource{};
    std::uint32_t thread{}, kind{}, context{}, name{}, valid{};
    std::int32_t result{};
    std::array<std::uint32_t, 24> values{};
};
std::array<Sample, 4096> g_queue{};
std::size_t g_head{}, g_count{};
std::atomic<std::uint64_t> g_dropped{}, g_seen{}, g_written{};
std::array<std::uint32_t, 4096> g_clips{}, g_profiles{};
std::array<std::uint32_t, 4096> g_inputs{};
std::array<std::uint32_t, 4096> g_assets{};
/** Melee and ability definitions, read at +4 of the component. */
std::array<std::uint32_t, 4096> g_definitions{};
/** Player message types: the low byte of the message header. */
std::array<std::uint32_t, 256> g_messages{};
/** Health component tags, read at +0 of the component. */
std::array<std::uint32_t, 4096> g_health{};
std::size_t g_clipCount{}, g_profileCount{}, g_inputCount{}, g_assetCount{};
std::size_t g_definitionCount{}, g_messageCount{}, g_healthCount{};

bool copy(const void* from, void* to, std::size_t size) noexcept {
    __try {
        if (from == nullptr) return false;
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool read(const void* from, std::size_t offset, T& value) noexcept {
    if (!from) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(from);
    if (offset > UINTPTR_MAX - address) return false;
    return copy(reinterpret_cast<const void*>(address + offset), &value, sizeof(value));
}
bool watched(const auto& values, std::size_t count, std::uint32_t value) noexcept {
    return std::binary_search(values.begin(), values.begin() + count, value);
}

/** Nonblocking capture. Each producer reports saturation rather than delaying gameplay. */
void enqueue(Sample sample) noexcept {
    // Pose state lives in rotating frame allocations. Key these samples by the
    // immutable choice descriptor and action, otherwise its moving address and
    // changing blend weight exhaust the bounded log before a long held pose ends.
    if (sample.kind == 12) {
        struct PoseLast { std::uint64_t resource{}, tick{}; std::uint32_t profile{}, phase{}; };
        thread_local std::array<PoseLast, 128> poses{};
        const auto now = GetTickCount64();
        auto* oldest = &poses[0];
        for (auto& prior : poses) {
            if (prior.resource == sample.resource && prior.profile == sample.name && prior.phase == sample.context) {
                if (now - prior.tick < 1000) return;
                oldest = &prior;
                break;
            }
            if (prior.tick < oldest->tick) oldest = &prior;
        }
        *oldest = {sample.resource, now, sample.name, sample.context};
    }
    // Emit changes and periodic repeats per call site, not every frame of an idle pose.
    struct Last { std::uint64_t object{}, caller{}, signature{}, tick{}; std::uint32_t kind{}; };
    thread_local std::array<Last, 64> recent{};
    std::uint64_t signature = 14695981039346656037ULL;
    const auto mix = [&signature](std::uint64_t word) { signature = (signature ^ word) * 1099511628211ULL; };
    mix(sample.name); mix(sample.context); mix(sample.valid); mix(static_cast<std::uint32_t>(sample.result));
    for (auto value : sample.values) mix(value);
    sample.tick = GetTickCount64();
    auto* last = &recent[0];
    for (auto& item : recent) {
        if (item.object == sample.object && item.caller == sample.caller && item.kind == sample.kind) {
            if (item.signature == signature && sample.tick - item.tick < 2000) return;
            last = &item;
            break;
        }
        if (item.tick < last->tick) last = &item;
    }
    *last = {sample.object, sample.caller, signature, sample.tick, sample.kind};
    sample.thread = GetCurrentThreadId();
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (!TryAcquireSRWLockExclusive(&g_queueLock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_count == g_queue.size()) g_dropped.fetch_add(1, std::memory_order_relaxed);
    else { g_queue[(g_head + g_count) % g_queue.size()] = sample; ++g_count; }
    ReleaseSRWLockExclusive(&g_queueLock);
}

std::uintptr_t __fastcall resolve_body(void* output, const void* request, std::uint32_t context) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_resolve)(output, request, context);
    if (call.accepts_side_effects()) {
        const void* clip{};
        Sample sample{};
        sample.kind = 1;
        sample.object = reinterpret_cast<std::uintptr_t>(request);
        sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
        sample.context = context;
        if (read(output, 0, clip) && read(clip, 0x120, sample.name)
            && watched(g_clips, g_clipCount, sample.name)) {
            sample.resource = reinterpret_cast<std::uintptr_t>(clip);
            for (std::size_t i = 0; i < 3; ++i)
                if (read(request, i * 4, sample.values[i])) sample.valid |= 1U << i;
            if (read(output, 16, sample.result)) sample.valid |= 1U << 3;
            for (std::size_t i = 0; i < 3; ++i)
                if (read(clip, 0x13C + i * 4, sample.values[3 + i])) sample.valid |= 1U << (4 + i);
            enqueue(sample);
        }
    }
    return result;
}

std::int32_t __fastcall select_body(const void* selector, const void* operation,
                                  const void* inputs, void* diagnostics) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_select)(selector, operation, inputs, diagnostics);
    if (call.accepts_side_effects()) {
        std::uint32_t profile{};
        if (read(inputs, 24 + 3 * 20, profile) && watched(g_profiles, g_profileCount, profile)) {
            Sample sample{};
            sample.kind = 2;
            sample.object = reinterpret_cast<std::uintptr_t>(selector);
            sample.resource = reinterpret_cast<std::uintptr_t>(operation);
            sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
            sample.name = profile;
            sample.result = result;
            (void)read(operation, 0, sample.context);
            for (std::size_t i = 0; i < sample.values.size(); ++i)
                if (read(inputs, 24 + i * 20, sample.values[i])) sample.valid |= 1U << i;
            enqueue(sample);
        }
    }
    return result;
}

/** Read-only observation. Forward every argument exactly as received. */
void __fastcall input_body(void* input, float trigger, std::uint32_t flags, const void* labels) {
    hooking::CallGate::Scope call(g_calls);
    hooking::await_original(g_input)(input, trigger, flags, labels);
    if (!call.accepts_side_effects()) return;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
    // BCDCC0 updates the ordinary, equipped-weapon and melee input instances.
    // Preserve raw references. Definition references can retain package IDs,
    // whereas resolved instance references can contain runtime datum handles.
    if (g_inputCount == 0
        || (caller != 0xBCE158 && caller != 0xBCE1BA && caller != 0xBCE21C)) return;
    std::uint32_t owner{};
    if (!read(input, 0, owner) || !watched(g_inputs, g_inputCount, owner)) return;
    Sample sample{};
    sample.kind = 3;
    sample.name = owner;
    sample.object = reinterpret_cast<std::uintptr_t>(input);
    sample.caller = caller;
    sample.context = flags;
    std::memcpy(&sample.values[0], &trigger, sizeof(trigger));
    sample.valid = 1;
    for (std::size_t i = 0; i < 16; ++i)
        if (read(input, 0x20 + i * 4, sample.values[1 + i])) sample.valid |= 1U << (1 + i);
    for (std::size_t i = 0; i < 4; ++i)
        if (read(input, i * 4, sample.values[17 + i])) sample.valid |= 1U << (17 + i);
    (void)read(input, 8, sample.resource);
    enqueue(sample);

    // This call site passes controller +110. Do not interpret unresolved
    // runtime handles as package offsets or require serialized self pairs.
    if (caller != 0xBCE21C || sample.object < 0x110) return;
    const auto* controller = reinterpret_cast<const void*>(sample.object - 0x110);
    std::uint64_t resource{};
    if (!read(controller, 8, resource)) return;
    sample.kind = 4;
    sample.object = reinterpret_cast<std::uintptr_t>(controller);
    sample.resource = resource;
    sample.values = {};
    sample.valid = 0;
    // The full typed magazine slot exposes its actual resolved owner and offset.
    for (std::size_t i = 0; i < 20; ++i)
        if (read(controller, 0x30 + i * 4, sample.values[i])) sample.valid |= 1U << i;
    for (std::size_t i = 0; i < 4; ++i)
        if (read(controller, 0x230 + i * 4, sample.values[20 + i])) sample.valid |= 1U << (20 + i);
    enqueue(sample);
}

/** Observe the player message boundary without changing its arguments or result. */
void __fastcall message_body(void* player, const void* message, std::uint32_t type) {
    hooking::CallGate::Scope call(g_calls);
    Sample sample{};
    bool capture = false;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
    if (call.accepts_side_effects()
        && (g_inputCount != 0 || g_definitionCount != 0 || g_messageCount != 0)) {
        bootflow::gateway_native::Read memory{g_image.base};
        std::uint32_t handle{}, self{}, owner{}, definition{};
        std::uintptr_t melee{};
        const auto address = reinterpret_cast<std::uintptr_t>(player);
        // The player's live melee component is in scope when its owner or its definition is
        // watched.
        const bool live = memory.value(address + 0x8C0, handle)
            && memory.resolve(handle, melee)
            && memory.value(melee + 0x24, self) && self == handle;
        const bool scoped = live
            && ((memory.value(melee, owner) && watched(g_inputs, g_inputCount, owner))
                || (memory.value(melee + 4, definition)
                    && watched(g_definitions, g_definitionCount, definition)));
        std::uint16_t header{};
        if (read(message, 0, header)
            && (scoped || watched(g_messages, g_messageCount, header & 0xFFU))) {
            capture = true;
            sample.kind = 5;
            sample.object = address;
            sample.resource = reinterpret_cast<std::uintptr_t>(message);
            sample.caller = caller;
            sample.name = type;
            sample.context = header & 0xFF;
            sample.result = scoped ? 1 : 0;
            sample.values[0] = header;
            sample.valid = 1;
            for (std::size_t i = 0; i < 7; ++i)
                if (read(message, i * 4, sample.values[1 + i])) sample.valid |= 1U << (1 + i);
            for (std::size_t i = 0; i < 6; ++i)
                if (read(player, 0x650 + i * 4, sample.values[8 + i])) sample.valid |= 1U << (8 + i);
            for (const auto [offset, index] : std::array<std::pair<std::size_t, std::size_t>, 4>{{
                     {0xB04,14}, {0x8BC,15}, {0x8C0,16}, {4,19}}})
                if (read(player, offset, sample.values[index])) sample.valid |= 1U << index;
            // B31960 dispatches through this loaded wrapper's first method.
            std::uintptr_t wrapper{};
            std::int64_t displacement{};
            std::uintptr_t function{};
            if ((sample.valid & (1U << 8)) && memory.resolve(sample.values[8], wrapper)
                && memory.value(wrapper + 0x18, displacement)
                && displacement >= 0 && displacement <= 65536
                && memory.value(wrapper + 0x30 + displacement, function)
                && function >= g_image.base && function < g_image.end) {
                const std::uint64_t rva = function - g_image.base;
                std::memcpy(&sample.values[17], &rva, sizeof(rva));
                sample.valid |= (1U << 17) | (1U << 18);
            }
            std::array<void*, 12> frames{};
            const auto count = CaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
            std::size_t saved = 0;
            for (USHORT i = 0; i < count && saved < 4; ++i) {
                const auto frame = reinterpret_cast<std::uintptr_t>(frames[i]);
                if (frame >= g_image.base && frame < g_image.end) {
                    sample.values[20 + saved] = static_cast<std::uint32_t>(frame - g_image.base);
                    sample.valid |= 1U << (20 + saved++);
                }
            }
        }
    }
    hooking::await_original(g_message)(player, message, type);
    if (capture && call.accepts_side_effects()) enqueue(sample);
}

/** The ability input callback reads raw fire independently of the weapon input filter. */
void observe_ability_input(void* ability, void* current, const void* previous,
                           std::atomic<AbilityInput>& original, std::uintptr_t caller, std::uint32_t kind) {
    hooking::CallGate::Scope call(g_calls);
    Sample sample{};
    std::uint32_t definition{};
    const bool capture = call.accepts_side_effects() && g_definitionCount != 0
        && read(ability, 4, definition) && watched(g_definitions, g_definitionCount, definition);
    if (capture) {
        sample.kind = kind;
        sample.object = reinterpret_cast<std::uintptr_t>(ability);
        sample.resource = reinterpret_cast<std::uintptr_t>(current);
        sample.caller = caller;
        (void)read(ability, 0, sample.name);
        for (std::size_t i = 0; i < 2; ++i) {
            const auto* command = i == 0 ? current : previous;
            if (read(command, 4, sample.values[i * 2])) sample.valid |= 1U << (i * 2);
            if (read(command, 0x34, sample.values[i * 2 + 1])) sample.valid |= 1U << (i * 2 + 1);
        }
        for (const auto [offset, index] : std::array<std::pair<std::size_t, std::size_t>, 7>{{
                 {0x260,4}, {0x264,5}, {0x1240,6}, {0x1244,7}, {0xD18,8}, {0xD1C,9}, {0x65C,10}}})
            if (read(ability, offset, sample.values[index])) sample.valid |= 1U << index;
    }
    hooking::await_original(original)(ability, current, previous);
    if (capture && call.accepts_side_effects()) {
        for (const auto [offset, index] : std::array<std::pair<std::size_t, std::size_t>, 5>{{
                 {0x260,11}, {0x264,12}, {0x1240,13}, {0x1244,14}, {0xD34,15}}})
            if (read(ability, offset, sample.values[index])) sample.valid |= 1U << index;
        if (read(current, 0x34, sample.values[16])) sample.valid |= 1U << 16;
        enqueue(sample);
    }
}

void __fastcall ability_input_body(void* ability, void* current, const void* previous) {
    observe_ability_input(ability, current, previous, g_abilityInput,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base, 6);
}

void __fastcall base_ability_input_body(void* ability, void* current, const void* previous) {
    observe_ability_input(ability, current, previous, g_baseAbilityInput,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base, 7);
}

const void* __fastcall attack_selection_body(void* ability, bool alternate, bool initial,
                                            void* owner, void* actor, void* context) {
    hooking::CallGate::Scope call(g_calls);
    const auto* result = hooking::await_original(g_attackSelection)(ability, alternate, initial, owner, actor, context);
    std::uint32_t definition{};
    if (call.accepts_side_effects() && g_definitionCount != 0 && read(ability, 4, definition)
        && watched(g_definitions, g_definitionCount, definition)) {
        Sample sample{};
        sample.kind = 8;
        sample.object = reinterpret_cast<std::uintptr_t>(ability);
        sample.resource = reinterpret_cast<std::uintptr_t>(result);
        sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
        sample.context = static_cast<std::uint32_t>(alternate) | (static_cast<std::uint32_t>(initial) << 1);
        (void)read(ability, 0, sample.name);
        for (std::size_t i = 0; i < 2; ++i)
            if (read(result, i * 4, sample.values[i])) sample.valid |= 1U << i;
        if (read(owner, 0, sample.values[2])) sample.valid |= 1U << 2;
        if (read(actor, 0, sample.values[3])) sample.valid |= 1U << 3;
        std::int64_t relative{};
        if (read(result, 8, relative) && relative > 0 && relative <= 0x100000) {
            const auto* shape = reinterpret_cast<const void*>(sample.resource + 8 + relative);
            for (std::size_t i = 0; i < 20; ++i)
                if (read(shape, i * 4, sample.values[4 + i])) sample.valid |= 1U << (4 + i);
        }
        enqueue(sample);
    }
    return result;
}

/** Observe a selected package action without changing its target or retained state. */
void observe_entity_effect(void* context, const void* node, std::uint32_t retained,
                           const void* event, std::atomic<EntityEffect>& original,
                           std::uintptr_t caller, std::uint32_t kind) {
    hooking::CallGate::Scope call(g_calls);
    Sample sample{};
    const bool capture = call.accepts_side_effects()
        && read(node, 0x10, sample.name)
        && watched(g_assets, g_assetCount, sample.name);
    if (capture) {
        sample.kind = kind;
        sample.object = reinterpret_cast<std::uintptr_t>(context);
        sample.resource = reinterpret_cast<std::uintptr_t>(node);
        sample.caller = caller;
        sample.context = retained;
        for (std::size_t i = 0; i < 12; ++i)
            if (read(node, i * 4, sample.values[i])) sample.valid |= 1U << i;
        for (std::size_t i = 0; i < 8; ++i)
            if (read(context, i * 4, sample.values[12 + i])) sample.valid |= 1U << (12 + i);
        for (std::size_t i = 0; i < 4; ++i)
            if (read(event, i * 4, sample.values[20 + i])) sample.valid |= 1U << (20 + i);
    }
    hooking::await_original(original)(context, node, retained, event);
    if (capture) enqueue(sample);
}

void __fastcall create_entity_body(void* context, const void* node, std::uint32_t retained,
                                  const void* event) {
    observe_entity_effect(context, node, retained, event, g_createEntity,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base, 9);
}

void __fastcall remove_entity_body(void* context, const void* node, std::uint32_t retained,
                                  const void* event) {
    observe_entity_effect(context, node, retained, event, g_removeEntity,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base, 10);
}

/** Observe the actual condition result for a watched private action. */
bool __fastcall condition_body(const void* node, void* context, const void* retained,
                              const void* event) {
    hooking::CallGate::Scope call(g_calls);
    const auto result = hooking::await_original(g_condition)(node, context, retained, event);
    std::uint32_t owner{};
    if (call.accepts_side_effects() && read(context, 0, owner)
        && watched(g_inputs, g_inputCount, owner)) {
        Sample sample{};
        sample.kind = 11;
        sample.name = owner;
        sample.object = reinterpret_cast<std::uintptr_t>(context);
        sample.resource = reinterpret_cast<std::uintptr_t>(node);
        sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
        sample.result = result ? 1 : 0;
        std::uint8_t kind{};
        if (read(node, 5, kind)) sample.context = kind;
        const auto nodeSize = kind == 15 || kind == 17 ? 112U : kind == 23 ? 12U : 8U;
        for (std::size_t i = 0; i < (std::min)(nodeSize / 4U, 8U); ++i)
            if (read(node, i * 4, sample.values[i])) sample.valid |= 1U << i;
        if (kind == 20) {
            // The supported general predicate holds the weapon-state mask and
            // final inversion separately from its header.
            if (read(node, 0x80, sample.values[3])) sample.valid |= 1U << 3;
            if (read(node, 0xF8, sample.values[4])) sample.valid |= 1U << 4;
        }
        for (std::size_t i = 0; i < 8; ++i)
            if (read(context, i * 4, sample.values[8 + i])) sample.valid |= 1U << (8 + i);
        // ECB950 forwards its FOURTH argument as the event to the native
        // checker. Its third argument is optional retained condition state.
        // ADS events contain an owning-weapon handle and a state byte.
        // The other observed weapon events also have this minimum header size.
        for (std::size_t i = 0; i < 2; ++i)
            if (read(event, i * 4, sample.values[16 + i])) sample.valid |= 1U << (16 + i);
        enqueue(sample);
    }
    return result;
}

/** Observe selected holding overlays, including their native blend weights. */
void __fastcall pose_choice_body(void* state, std::uint32_t phase, float weight,
                                void* manager, const void* inputs, std::uint16_t choice,
                                const void* choices, float elapsed) {
    hooking::CallGate::Scope call(g_calls);
    hooking::await_original(g_poseChoice)(state, phase, weight, manager, inputs, choice, choices, elapsed);
    std::uint32_t profile{};
    if (call.accepts_side_effects() && read(inputs, 24 + 3 * 20, profile)
        && watched(g_profiles, g_profileCount, profile)) {
        Sample sample{};
        sample.kind = 12;
        sample.name = profile;
        sample.object = reinterpret_cast<std::uintptr_t>(state);
        sample.resource = reinterpret_cast<std::uintptr_t>(choices);
        sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
        sample.context = phase;
        std::memcpy(&sample.values[0], &weight, sizeof(weight));
        sample.values[1] = choice;
        sample.valid = 3;
        if (read(state, 0x70, sample.values[2])) sample.valid |= 4;
        for (std::size_t row = 0; row < 4; ++row) {
            constexpr std::array<std::size_t, 4> offsets{0, 4, 12, 16};
            for (std::size_t field = 0; field < offsets.size(); ++field) {
                const auto index = 3 + row * 4 + field;
                if (read(state, row * 28 + offsets[field], sample.values[index])) sample.valid |= 1U << index;
            }
        }
        const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
        const auto inputAddress = reinterpret_cast<std::uintptr_t>(inputs);
        std::memcpy(&sample.values[19], &managerAddress, sizeof(managerAddress));
        std::memcpy(&sample.values[21], &inputAddress, sizeof(inputAddress));
        sample.valid |= 0x00780000;
        enqueue(sample);
    }
}

/**
 * Observes a watched health component's region fraction write. The native setter only stores the
 * value, and it receives the same component, region and fraction.
 * context: the region. name: the component tag. result: stack frames captured.
 * values: 0 the fraction before and 1 the fraction written (float bits), 2 the self handle
 * (+0x24), 3 the owning entity (+0x2C), 4-6 the per-region bit masks (+0x300, +0x304, +0x308),
 * 7 the word at +0x338, then up to eight return addresses in the game image as 64-bit RVAs.
 */
void __fastcall health_fraction_body(void* component, std::int32_t region, float fraction) {
    hooking::CallGate::Scope call(g_calls);
    Sample sample{};
    std::uint32_t kind{};
    bool observe = call.accepts_side_effects() && read(component, 0, sample.name)
        && watched(g_health, g_healthCount, sample.name) && read(component, 4, kind)
        && kind == 0x80804B8AU;
    if (observe) {
        sample.kind = 13;
        sample.object = reinterpret_cast<std::uintptr_t>(component);
        sample.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_image.base;
        sample.context = static_cast<std::uint32_t>(region);
        std::memcpy(&sample.values[1], &fraction, sizeof(fraction));
        sample.valid = 2;
        // The setter's own addressing: the region array at +0x340 (count, then a relative offset
        // at +0x348), 0x70 bytes per region.
        std::uint64_t regions{};
        std::int64_t table{};
        if (region >= 0 && read(component, 0x340, regions) && regions <= 256
            && static_cast<std::uint64_t>(region) < regions && read(component, 0x348, table)
            && table >= 0 && table < 0x100000
            && read(component,
                    0x348 + static_cast<std::size_t>(table)
                        + (static_cast<std::size_t>(region) + 1) * 0x70,
                    sample.values[0])) {
            sample.valid |= 1;
        }
        if (read(component, 0x24, sample.values[2])) sample.valid |= 4;
        // Every drop is written; a region refilling from one call site at most four times a second.
        float before{};
        std::memcpy(&before, &sample.values[0], sizeof(before));
        struct HealthLast { std::uint64_t object{}, caller{}, tick{}; std::uint32_t region{}, self{}; };
        thread_local std::array<HealthLast, 32> recent{};
        const auto now = GetTickCount64();
        auto* oldest = &recent[0];
        for (auto& prior : recent) {
            if (prior.object == sample.object && prior.caller == sample.caller
                && prior.region == sample.context && prior.self == sample.values[2]) {
                if ((sample.valid & 1) && fraction >= before && now - prior.tick < 250) observe = false;
                oldest = &prior;
                break;
            }
            if (prior.tick < oldest->tick) oldest = &prior;
        }
        if (observe) {
            *oldest = {sample.object, sample.caller, now, sample.context, sample.values[2]};
            constexpr std::array<std::size_t, 5> offsets{0x2C, 0x300, 0x304, 0x308, 0x338};
            for (std::size_t i = 0; i < offsets.size(); ++i)
                if (read(component, offsets[i], sample.values[3 + i])) sample.valid |= 1U << (3 + i);
            std::array<void*, 8> stack{};
            const auto frames = CaptureStackBackTrace(0, static_cast<DWORD>(stack.size()), stack.data(), nullptr);
            sample.result = frames;
            for (std::size_t i = 0; i < frames; ++i) {
                const auto address = reinterpret_cast<std::uintptr_t>(stack[i]);
                if (address < g_image.base || address >= g_image.end) continue;
                const auto rva = static_cast<std::uint64_t>(address - g_image.base);
                std::memcpy(&sample.values[8 + i * 2], &rva, sizeof(rva));
                sample.valid |= 3U << (8 + i * 2);
            }
        }
    }
    hooking::await_original(g_healthFraction)(component, region, fraction);
    if (observe) enqueue(sample);
}

/** Only the writer thread performs file I/O. Trace size is capped at 32768 records. */
void write_line(const char* line) noexcept {
    DWORD written{};
    if (g_file != INVALID_HANDLE_VALUE)
        (void)WriteFile(g_file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
}
void drain() noexcept {
    std::array<Sample, 64> batch{};
    for (;;) {
        AcquireSRWLockExclusive(&g_queueLock);
        const auto count = (std::min)(batch.size(), g_count);
        for (std::size_t i = 0; i < count; ++i) batch[i] = g_queue[(g_head + i) % g_queue.size()];
        g_head = (g_head + count) % g_queue.size();
        g_count -= count;
        ReleaseSRWLockExclusive(&g_queueLock);
        if (count == 0) return;
        for (std::size_t i = 0; i < count; ++i) {
            if (g_written.fetch_add(1, std::memory_order_relaxed) >= 32768) {
                g_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto& s = batch[i];
            char line[768]{};
            auto used = std::snprintf(line, sizeof(line),
                "tick=%llu tid=%u kind=%u caller=%llX object=%llX resource=%llX context=%08X name=%08X result=%d valid=%08X values=",
                s.tick, s.thread, s.kind, s.caller, s.object, s.resource, s.context, s.name, s.result, s.valid);
            for (std::size_t j = 0; j < s.values.size() && used > 0 && used < 720; ++j)
                used += std::snprintf(line + used, sizeof(line) - used, "%s%08X", j == 0 ? "" : ",", s.values[j]);
            if (used > 0 && used < 766) { line[used] = '\n'; line[used + 1] = 0; write_line(line); }
        }
    }
}
DWORD WINAPI writer(void*) noexcept {
    while (WaitForSingleObject(g_stop, 250) == WAIT_TIMEOUT) drain();
    drain();
    char summary[192]{};
    std::snprintf(summary, sizeof(summary), "stage=stop seen=%llu written=%llu dropped=%llu\n",
                  g_seen.load(), (std::min)(g_written.load(), 32768ULL), g_dropped.load());
    write_line(summary);
    return 0;
}

/** Appends one parsed ID to the list its letter names. @return False when the list is full. */
template<std::size_t N>
bool add(std::array<std::uint32_t, N>& values, std::size_t& count, std::uint32_t value) noexcept {
    if (count == values.size()) return false;
    values[count++] = value;
    return true;
}

/** Logs why the watch file was refused; the trace then stays detached. */
void refuse(const char* reason, std::size_t lineNumber) noexcept {
    core::log::writef(core::log::Channel::client, core::log::Level::warn,
                      "ev=action_trace stage=watch result=fail reason=%s line=%zu", reason,
                      lineNumber);
}

/**
 * One letter and eight uppercase hex digits per line; the letters are listed at the top.
 * @return True when the file holds at least one ID and nothing else. A missing file is silent;
 * any other refusal is logged with its reason and line.
 */
bool watch_file() noexcept {
    core::path::Buffer path;
    if (!core::path::artifact_file(L"action_trace_watch.txt", path)
        || GetFileAttributesW(path.chars.data()) == INVALID_FILE_ATTRIBUTES) return false;
    // Static: the file can be large, and this runs on the game's thread during activation.
    static std::array<char, 180225> text{};
    if (!core::path::read_artifact_text(L"action_trace_watch.txt", text)) {
        refuse("unreadable_or_empty_or_too_large", 0);
        return false;
    }
    g_clipCount = g_profileCount = g_inputCount = g_assetCount = 0;
    g_definitionCount = g_messageCount = g_healthCount = 0;
    std::size_t at = 0;
    std::size_t lineNumber = 1;
    while (at < text.size() && text[at]) {
        if (text[at] == '\n') { ++lineNumber; ++at; continue; }
        if (text[at] == '\r') { ++at; continue; }
        const char kind = text[at++];
        if (kind != 'C' && kind != 'P' && kind != 'I' && kind != 'A' && kind != 'D' && kind != 'H'
            && kind != 'M') {
            refuse("letter", lineNumber);
            return false;
        }
        std::uint32_t value{};
        for (std::size_t i = 0; i < 8; ++i) {
            const char c = at < text.size() ? text[at++] : '\0';
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) {
                refuse("digits", lineNumber);
                return false;
            }
            value = value * 16U + static_cast<std::uint32_t>(digit);
        }
        if (at == text.size() || (text[at] && text[at] != '\n' && text[at] != '\r')) {
            refuse("digits", lineNumber);
            return false;
        }
        // A message type is one header byte; a wider value could never match.
        if (kind == 'M' && value > 0xFF) {
            refuse("message_range", lineNumber);
            return false;
        }
        bool added = false;
        switch (kind) {
        case 'C': added = add(g_clips, g_clipCount, value); break;
        case 'P': added = add(g_profiles, g_profileCount, value); break;
        case 'I': added = add(g_inputs, g_inputCount, value); break;
        case 'A': added = add(g_assets, g_assetCount, value); break;
        case 'D': added = add(g_definitions, g_definitionCount, value); break;
        case 'H': added = add(g_health, g_healthCount, value); break;
        case 'M': added = add(g_messages, g_messageCount, value); break;
        default: break;
        }
        if (!added) {
            refuse("too_many", lineNumber);
            return false;
        }
    }
    if (g_clipCount + g_profileCount + g_inputCount + g_assetCount + g_definitionCount
        + g_healthCount + g_messageCount == 0) {
        refuse("no_ids", 0);
        return false;
    }
    std::sort(g_clips.begin(), g_clips.begin() + g_clipCount);
    std::sort(g_profiles.begin(), g_profiles.begin() + g_profileCount);
    std::sort(g_inputs.begin(), g_inputs.begin() + g_inputCount);
    std::sort(g_assets.begin(), g_assets.begin() + g_assetCount);
    std::sort(g_definitions.begin(), g_definitions.begin() + g_definitionCount);
    std::sort(g_health.begin(), g_health.begin() + g_healthCount);
    std::sort(g_messages.begin(), g_messages.begin() + g_messageCount);
    return true;
}
bool idle() noexcept { return g_calls.idle(); }
void close_output() noexcept {
    if (g_worker) { SetEvent(g_stop); WaitForSingleObject(g_worker, INFINITE); CloseHandle(g_worker); g_worker = nullptr; }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}
} // namespace

bool install() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    const auto finish = [](bool result) { ReleaseSRWLockExclusive(&g_lifecycle); return result; };
    const auto fail = [&finish](const char* reason) {
        core::log::writef(core::log::Channel::client, core::log::Level::warn,
            "ev=action_trace stage=install result=fail reason=%s", reason);
        return finish(false);
    };
    if (g_handles[0].attached) return finish(g_calls.accepting());
    if (!watch_file()) return finish(true);
    if (!diagnostics::module_range(GetModuleHandleW(nullptr), g_image)) return fail("module_range");
    if (g_inputCount != 0) {
        constexpr std::array<unsigned char, 12> expected{
            0x48,0x81,0xC1,0x10,0x01,0x00,0x00,0xE8,0x54,0xCF,0x1D,0x00};
        std::array<unsigned char, 12> actual{};
        if (g_image.base + 0xBCE210 + actual.size() > g_image.end
            || !copy(reinterpret_cast<void*>(g_image.base + 0xBCE210), actual.data(), actual.size())
            || actual != expected) return fail("input_site");
    }
    constexpr std::array<std::uintptr_t, 12> addresses{0xC886C0, 0xF67330, 0xDAB170, 0xBBC760, 0xD2C330, 0xBB0F50, 0xD16640, 0x1089400, 0x108B550, 0xECB950, 0x1047590, 0xCDE4A0};
    constexpr std::array<std::array<unsigned char, 16>, 12> prefixes{{
        {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x4C,0x8B,0xD2,0x8B,0x0A,0x83,0xF9},
        {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x4C},
        {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x41},
        {0x44,0x89,0x44,0x24,0x18,0x53,0x48,0x83,0xEC,0x50,0x83,0xB9,0x58,0x06,0x00,0x00},
        {0x4C,0x8B,0xDC,0x49,0x89,0x6B,0x18,0x49,0x89,0x73,0x20,0x57,0x48,0x81,0xEC,0x80},
        {0x48,0x89,0x5C,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57},
        {0x4C,0x8B,0xDC,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x49,0x8D,0xAB,0x88,0xFE},
        {0x40,0x55,0x53,0x57,0x41,0x55,0x41,0x57,0x48,0x8D,0xAC,0x24,0x60,0xF8,0xFF,0xFF},
        {0x41,0x83,0xF8,0xFF,0x0F,0x84,0x24,0x05,0x00,0x00,0x55,0x41,0x56,0x41,0x57,0x48},
        {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48},
        {0x48,0x89,0x5C,0x24,0x18,0x56,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83},
        {0x4C,0x8B,0xC1,0x48,0x63,0xCA,0x49,0x8B,0x80,0x48,0x03,0x00,0x00,0x49,0x03,0xC0},
    }};
    const std::array replacements{reinterpret_cast<void*>(&resolve_body), reinterpret_cast<void*>(&select_body), reinterpret_cast<void*>(&input_body), reinterpret_cast<void*>(&message_body), reinterpret_cast<void*>(&ability_input_body), reinterpret_cast<void*>(&base_ability_input_body), reinterpret_cast<void*>(&attack_selection_body), reinterpret_cast<void*>(&create_entity_body), reinterpret_cast<void*>(&remove_entity_body), reinterpret_cast<void*>(&condition_body), reinterpret_cast<void*>(&pose_choice_body), reinterpret_cast<void*>(&health_fraction_body)};
    std::array<hooking::detour::Spec, 12> specs{};
    g_handleCount = g_healthCount != 0 ? addresses.size() : addresses.size() - 1;
    for (std::size_t i = 0; i < g_handleCount; ++i) {
        std::array<unsigned char, 16> actual{};
        const auto target = g_image.base + addresses[i];
        if (target < g_image.base || target + actual.size() > g_image.end
            || !copy(reinterpret_cast<void*>(target), actual.data(), actual.size()) || actual != prefixes[i]) {
            core::log::writef(core::log::Channel::client, core::log::Level::warn,
                "ev=action_trace stage=install result=fail reason=prefix rva=%llX", addresses[i]);
            return finish(false);
        }
        specs[i] = {reinterpret_cast<void*>(target), replacements[i]};
    }
    core::path::Buffer path;
    if (!core::path::artifact_file(L"action_trace.log", path)) return fail("log_file");
    g_file = CreateFileW(path.chars.data(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return fail("log_file");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) { close_output(); return fail("writer_thread"); }
    g_head = g_count = 0;
    g_seen = 0; g_written = 0; g_dropped = 0;
    g_worker = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    if (!g_worker) { close_output(); return fail("writer_thread"); }
    g_calls.quiesce();
    if (!hooking::detour::install(std::span(specs).first(g_handleCount),
                                  std::span(g_handles).first(g_handleCount))) {
        close_output();
        return fail("detour");
    }
    hooking::publish_original(g_resolve, reinterpret_cast<Resolve>(g_handles[0].original));
    hooking::publish_original(g_select, reinterpret_cast<Select>(g_handles[1].original));
    hooking::publish_original(g_input, reinterpret_cast<Input>(g_handles[2].original));
    hooking::publish_original(g_message, reinterpret_cast<Message>(g_handles[3].original));
    hooking::publish_original(g_abilityInput, reinterpret_cast<AbilityInput>(g_handles[4].original));
    hooking::publish_original(g_baseAbilityInput, reinterpret_cast<AbilityInput>(g_handles[5].original));
    hooking::publish_original(g_attackSelection, reinterpret_cast<AttackSelection>(g_handles[6].original));
    hooking::publish_original(g_createEntity, reinterpret_cast<EntityEffect>(g_handles[7].original));
    hooking::publish_original(g_removeEntity, reinterpret_cast<EntityEffect>(g_handles[8].original));
    hooking::publish_original(g_condition, reinterpret_cast<Condition>(g_handles[9].original));
    hooking::publish_original(g_poseChoice, reinterpret_cast<PoseChoice>(g_handles[10].original));
    if (g_healthCount != 0)
        hooking::publish_original(g_healthFraction, reinterpret_cast<HealthFraction>(g_handles[11].original));
    write_line("stage=install version=11 result=ok native_forwarding=unchanged input_scope=watched_owners message_scope=watched_melee_or_type ability_scope=watched_definitions health_scope=watched_components kinds=1:clip_resolve,2:selector,3:input,4:melee_slot,5:player_message,6:ability_input,7:base_ability_input,8:attack_selection,9:entity_create,10:entity_remove,11:private_condition,12:pose_choice,13:health_fraction\n");
    g_calls.accept();
    core::log::writef(core::log::Channel::client, core::log::Level::info,
        "ev=action_trace stage=install result=ok clips=%zu profiles=%zu inputs=%zu assets=%zu "
        "definitions=%zu health=%zu messages=%zu", g_clipCount, g_profileCount, g_inputCount,
        g_assetCount, g_definitionCount, g_healthCount, g_messageCount);
    return finish(true);
}

bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lifecycle);
    g_calls.quiesce();
    if (!g_handles[0].attached) { ReleaseSRWLockExclusive(&g_lifecycle); return true; }
    const std::array protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&resolve_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&select_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&input_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&message_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&ability_input_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&base_ability_input_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&attack_selection_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe_ability_input)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&create_entity_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&remove_entity_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe_entity_effect)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&condition_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&pose_choice_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&health_fraction_body)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&hooking::call_gate_detail::enter)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&hooking::call_gate_detail::leave)},
    };
    const bool removed = probes::detach(std::span(g_handles).first(g_handleCount), protectedEntries, &idle)
        == hooking::detour::UninstallResult::removed;
    if (removed) { g_resolve = nullptr; g_select = nullptr; g_input = nullptr; g_message = nullptr; g_abilityInput = nullptr; g_baseAbilityInput = nullptr; g_attackSelection = nullptr; g_createEntity = nullptr; g_removeEntity = nullptr; g_condition = nullptr; g_poseChoice = nullptr; g_healthFraction = nullptr; close_output(); }
    ReleaseSRWLockExclusive(&g_lifecycle);
    return removed;
}
} // namespace dawn::client::hooks::probes::action_trace
