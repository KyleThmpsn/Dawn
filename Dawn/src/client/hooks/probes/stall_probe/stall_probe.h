#pragma once

#include <cstdint>

namespace dawn::client::hooks::probes::stall_probe {

/**
 * Starts the stall watcher thread.
 * It polls the pump call count; when the game stops calling the pump it captures every thread's
 * rip and in-image return addresses, which names the blocked code. The same thread samples the
 * networking tick's latch, so the latch is read while the pump is frozen too.
 */
bool install() noexcept;

/** Stops the watcher thread. */
bool uninstall() noexcept;

/** Counts one call the game made into the callback pump. Called from the pump's export. */
void note_pump() noexcept;

/** @return Times the game has called the pump. A frozen count means it stopped calling. */
[[nodiscard]] std::uint64_t pump_count() noexcept;

} // namespace dawn::client::hooks::probes::stall_probe
