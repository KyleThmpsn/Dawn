#pragma once

#include <Windows.h>

#include <span>

#include "../../hooking/detour.h"

namespace dawn::client::hooks::probes {

/** Removal attempts, and the pause between them, while a game thread is inside a probe's hook. */
inline constexpr unsigned kDetachAttempts = 50;
inline constexpr DWORD kDetachRetryMs = 10;

/**
 * Removes a probe's detours, trying again while a game thread is inside one of them. Probes hook
 * functions the game calls every frame, so a single attempt at shutdown usually lands mid-call.
 * Until an attempt finds the hooks idle they stay attached and keep forwarding every call. A
 * failed attempt is not retried: its cause does not pass, and each attempt suspends every
 * thread, so the probe's own lock-free idle test gates it first.
 * @return The last attempt's result: removed, still active after every attempt, or failed.
 */
[[nodiscard]] inline hooking::detour::UninstallResult
detach(std::span<hooking::detour::Handle> handles,
       std::span<const hooking::detour::ProtectedCodeEntry> entries,
       hooking::detour::IdleCheck idle) noexcept {
    auto result = hooking::detour::UninstallResult::protectedCodeActive;
    for (unsigned attempt = 0; attempt < kDetachAttempts; ++attempt) {
        if (idle == nullptr || idle()) {
            result = hooking::detour::uninstall(handles, entries, idle);
            if (result != hooking::detour::UninstallResult::protectedCodeActive) {
                break;
            }
        }
        Sleep(kDetachRetryMs);
    }
    return result;
}

} // namespace dawn::client::hooks::probes
