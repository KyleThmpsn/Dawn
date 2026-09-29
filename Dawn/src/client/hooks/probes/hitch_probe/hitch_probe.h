#pragma once

#include <cstdint>

namespace dawn::client::hooks::probes::hitch_probe {

/** Attaches the read-only hitch snapshot dump. @return True when the detour attaches. */
[[nodiscard]] bool install() noexcept;

/** Detaches the hitch snapshot dump. */
[[nodiscard]] bool uninstall() noexcept;

/**
 * @return When the watchdog last reported a hitch, as a GetTickCount64 value; zero until the first
 * report after install. Every report is seen, not only those the assert log keeps.
 */
[[nodiscard]] std::uint64_t last_report_tick() noexcept;

} // namespace dawn::client::hooks::probes::hitch_probe
