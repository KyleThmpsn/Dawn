#pragma once

namespace dawn::client::hooks::probes::selector_watch {

/**
 * Installs a read-only memory watch that never stops a game thread: the exception handler and
 * the thread that polls `Dawn\selector_watch.txt`. The file's format and what each touch logs
 * are described at the top of selector_watch.cpp. Nothing is armed while the file is absent.
 * @return True when the handler and the polling thread are in place, or were already.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Stops the polling thread and disarms every page. The exception handler stays registered, inert,
 * because a thread may still be single-stepping out of a guard fault.
 */
[[nodiscard]] bool uninstall() noexcept;

} // namespace dawn::client::hooks::probes::selector_watch
