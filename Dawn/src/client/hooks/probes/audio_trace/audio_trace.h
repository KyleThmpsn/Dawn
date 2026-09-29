#pragma once

namespace dawn::client::hooks::probes::audio_trace {
/**
 * Observe native bank and PCM loading when Dawn\audio_trace_watch.txt exists. The file's format
 * is described at the top of audio_trace.cpp.
 */
[[nodiscard]] bool install() noexcept;
/** Detach only after all native forwarding calls have completed. */
[[nodiscard]] bool uninstall() noexcept;
}
