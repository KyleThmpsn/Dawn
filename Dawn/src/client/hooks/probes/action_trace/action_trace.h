#pragma once

namespace dawn::client::hooks::probes::action_trace {
/**
 * Observe input, ability, animation and health paths when Dawn\action_trace_watch.txt exists. The
 * file's format is described at the top of action_trace.cpp.
 */
[[nodiscard]] bool install() noexcept;
/** Remove observation only after forwarding calls have drained. */
[[nodiscard]] bool uninstall() noexcept;
}
