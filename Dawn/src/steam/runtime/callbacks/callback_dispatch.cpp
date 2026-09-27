#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../client/content/investment/worker.h"
#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../server/runtime/server_runtime.h"
#include "../internal.h"
#include "callback_registry.h"

namespace dawn::steam::runtime::callbacks {
namespace {

/** Steam callback vtable slots for its two Run overloads. */
enum class CallbackMethod : std::size_t {
    callResult = 0,
    regular = 1,
};

/** Calls the regular callback overload through the Steam callback ABI. */
void invoke_callback(void* callback, void* payload) noexcept {
    auto** methods = *static_cast<void***>(callback);
    const auto slot = static_cast<std::size_t>(CallbackMethod::regular);
    if (methods == nullptr || methods[slot] == nullptr) {
        return;
    }
    const auto run = reinterpret_cast<void (*)(void*, void*)>(methods[slot]);
    run(callback, payload);
}

/** Calls the call-result overload through the Steam callback ABI. */
void invoke_call_result(void* callback, void* payload, ApiCall call) noexcept {
    auto** methods = *static_cast<void***>(callback);
    const auto slot = static_cast<std::size_t>(CallbackMethod::callResult);
    if (methods == nullptr || methods[slot] == nullptr) {
        return;
    }
    const auto run = reinterpret_cast<void (*)(void*, void*, bool, ApiCall)>(methods[slot]);
    run(callback, payload, false, call);
}

/**
 * Takes the oldest queued callback event. Runs under the callback lock.
 * @param event Receives one copied event.
 * @return True when an event was there.
 */
[[nodiscard]] bool pop_event(CallbackEvent& event) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_eventCount == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    event = g_events[g_eventHead];
    g_events[g_eventHead] = {};
    g_eventHead = (g_eventHead + 1) % kEventCapacity;
    --g_eventCount;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Finds the registrations, then calls them only after the callback lock is released. */
void dispatch_event(CallbackEvent& event) noexcept {
    std::array<void*, kCallbackCapacity> callbacks{};
    std::array<void*, kCallResultCapacity> callResults{};
    std::size_t callbackCount{};
    std::size_t callResultCount{};
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& entry : g_callbacks) {
        if (entry.callback != nullptr && entry.callbackId == event.callbackId) {
            callbacks[callbackCount++] = entry.callback;
        }
    }
    if (event.call != 0) {
        for (auto& entry : g_callResults) {
            if (entry.callback != nullptr && entry.call == event.call
                && callback_id(entry.callback) == event.callbackId) {
                callResults[callResultCount++] = entry.callback;
                entry = {};
            }
        }
    }
    // Callback code can register again from inside the call, so calls happen after the unlock.
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < callbackCount; ++index) {
        invoke_callback(callbacks[index], event.payload.data());
    }
    for (std::size_t index = 0; index < callResultCount; ++index) {
        invoke_call_result(callResults[index], event.payload.data(), event.call);
    }
}

/** Code of a fault that unwound out of the slice, reported by the next call. Zero means none. */
std::atomic<std::uint32_t> g_faultCode{0};

/**
 * Writes the fault line one call after the fault. Logging from the handler could take the log
 * lock the faulting slice still holds, so the report waits until that stack is gone.
 */
void report_fault_once() noexcept {
    const std::uint32_t code = g_faultCode.exchange(0, std::memory_order_relaxed);
    if (code == 0) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=core stage=pump result=fault code=0x%08X", code);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace
} // namespace dawn::steam::runtime::callbacks

namespace dawn::steam {
namespace {

/** Runs one whole slice. Separated so the caller can wrap it in a structured handler. */
void run_slice() noexcept {
    // Presentation goes in first, so the sweep below has an overlay to draw with. It only hooks
    // Present; the game still decides when a frame is drawn.
    runtime::activate_graphics_once();
    // The sweep stalls this thread, and this thread is the one that draws. The overlay is raised
    // on this call, so a frame carrying it reaches the screen before the stall.
    if (runtime::main_activation_pending()
        && core::ui::busy::raise_early(core::ui::busy::Task::initialization)) {
        return;
    }
    // The network group must own SignOn before callback work can send it.
    const bool mainActive = runtime::activate_main_once();
    // Steam async operations started by a callback cannot complete reentrantly in that same
    // callback batch. Snapshot the queue here: events queued while dispatching wait for the next
    // SteamAPI_RunCallbacks call, after the caller has committed its new operation state.
    AcquireSRWLockShared(&runtime::callbacks::g_lock);
    const std::size_t batchCount = runtime::callbacks::g_eventCount;
    ReleaseSRWLockShared(&runtime::callbacks::g_lock);
    runtime::callbacks::CallbackEvent event;
    for (std::size_t count = 0;
         count < batchCount && runtime::callbacks::pop_event(event);
         ++count) {
        runtime::callbacks::dispatch_event(event);
    }
    if (mainActive) {
        const auto now = GetTickCount64();
        server::service(now);
        client::content::investment::worker::service(now);
    }
}

} // namespace

/**
 * Delivers one capped batch of queued callbacks on the caller thread. The game holds an unguarded
 * re-entrancy latch across this call, so a fault unwinding out of here kills every later tick,
 * and with it the server, which is serviced from inside the slice.
 */
void run_callbacks() noexcept {
    runtime::callbacks::report_fault_once();
    __try {
        run_slice();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime::callbacks::g_faultCode.store(static_cast<std::uint32_t>(GetExceptionCode()),
                                              std::memory_order_relaxed);
    }
}

/** Copies one callback payload into the delivery queue. */
bool queue_callback(int callbackId,
                    ApiCall call,
                    const void* payload,
                    std::size_t payloadSize) noexcept {
    using namespace runtime::callbacks;
    if (callbackId <= 0 || payloadSize > kEventPayloadCapacity
        || (payload == nullptr && payloadSize != 0)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=callback_queue result=invalid");
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_eventCount == kEventCapacity) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(
            core::log::Channel::client, core::log::Level::error, "ev=callback_queue result=full");
        return false;
    }
    // Head plus count names the only free ring slot while the lock is held.
    const std::size_t tail = (g_eventHead + g_eventCount) % kEventCapacity;
    auto& event = g_events[tail];
    event = {};
    event.callbackId = callbackId;
    event.call = call;
    event.payloadSize = payloadSize;
    if (payloadSize != 0) {
        std::memcpy(event.payload.data(), payload, payloadSize);
    }
    ++g_eventCount;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

} // namespace dawn::steam
