#pragma once

#include <cstddef>

namespace dawn::core::ui::memory {

/**
 * 64 MiB caps all Dear ImGui context, font, widget, and draw storage.
 * The font atlas is most of it: at its 2048 x 2048 cap it takes 16 MiB, and while it grows or
 * repacks, the textures it replaces stay alive until the renderer lets them go a frame later.
 * Dear ImGui does not check an allocation, so an arena too small for that faults the frame.
 */
inline constexpr std::size_t kArenaCapacityBytes = 67'108'864;

/** Copied allocator counters. The arena storage itself is not exposed. */
struct Stats {
    bool installed{};
    std::size_t capacityBytes{kArenaCapacityBytes};
    std::size_t outstandingAllocations{};
    std::size_t outstandingBytes{};
    std::size_t highWaterBytes{};
    std::size_t largestFreeBytes{};
};

/**
 * Installs the fixed allocator before any Dear ImGui context exists.
 * @return True when the allocator is installed or was already installed.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Restores the earlier Dear ImGui allocator once every owned allocation is freed.
 * @return False while an allocation is still live; the allocator stays active.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return One copy of the allocator counters, read under the lock. */
[[nodiscard]] Stats snapshot() noexcept;

} // namespace dawn::core::ui::memory
