#pragma once

namespace dawn::client::hooks::bootflow {

/**
 * Research aid: logs the world tag loader's current and queued requests, with the first bytes of
 * each request record, at warn level. Reads only; safe before the spawn hold is installed.
 * @param reason Short tag naming the caller, written into the line.
 */
void report_loader_diagnostics(const char* reason) noexcept;

} // namespace dawn::client::hooks::bootflow
