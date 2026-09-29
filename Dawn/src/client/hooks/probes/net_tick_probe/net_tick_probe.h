#pragma once

namespace dawn::client::hooks::probes::net_tick_probe {

/**
 * Enables sampling and resolves the latch now, inside the activation sweep, so the first sample
 * never scans the image on the game's thread. Called only when `client.stall_trace` is on.
 */
void prepare() noexcept;

/**
 * Checks the game's networking-tick latch and the pump call count, and writes a line only on the
 * first check and when the latch starts or stops being held set, or the pump stops or restarts.
 * Read-only, self-throttled, and safe on any thread. The stall probe's watcher calls it, so it
 * keeps checking after the pump stops. Returns at once until `prepare` has run.
 */
void sample() noexcept;

} // namespace dawn::client::hooks::probes::net_tick_probe
