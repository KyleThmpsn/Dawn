#pragma once

namespace dawn::client::hooks::probes::effect_trace {

/**
 * Starts the effect trace: every effect the game applies or removes, kind-3 perk spawn requests,
 * every weapon fire source, and every projectile launch with its early flight, written to
 * dawn.log from a thread of its own. Read-only throughout, and no other Dawn hook shares its
 * targets. The effect detour runs in front of every effect handler, including those other probes
 * hook, and forwards with a jump so they still see the game's own caller.
 * @return True when at least one of its hook sets attached.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Removes the hooks, reports projectiles still in flight and counts still held back, and drains
 * the writer.
 */
[[nodiscard]] bool uninstall() noexcept;

} // namespace dawn::client::hooks::probes::effect_trace
