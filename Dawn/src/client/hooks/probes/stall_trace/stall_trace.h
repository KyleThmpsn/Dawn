#pragma once

#include <cstdint>

namespace dawn::client::hooks::probes::stall_trace {

/**
 * Research aid gated by `client.stall_trace`. On the first occurrence of a hitch the watchdog
 * reports for any stalled job, logs the world tag loader and every thread's stack so the stall
 * shows what it waits on. A trace starts on the first message of each run of a stalled hitch; a
 * different assert in between starts a new run. At most three per process.
 * @param text Formatted assert message.
 * @param repeats Length of the current run of this message, as counted by the observer.
 */
void observe(const char* text, std::uint32_t repeats) noexcept;

/**
 * @return When the assert log last kept a watchdog hitch message, as a GetTickCount64 value; zero
 * for never. Repeats the log counts without keeping are not seen here.
 */
[[nodiscard]] std::uint64_t last_hitch_tick() noexcept;

} // namespace dawn::client::hooks::probes::stall_trace
