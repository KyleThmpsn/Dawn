#pragma once

#include <cstdint>

namespace dawn::state::activity::newlight::launchpad::shutter {

// Both overlapping physical shutters use this entity resource. Each must
// consume the mission's position channel through its animated 80F3D672 device.
inline constexpr std::uint32_t kEntity = 0x80C44F2BU;

[[nodiscard]] constexpr bool matches(bool selected, std::uint32_t definition,
                                      float x, float y, float z) noexcept {
    // Scope to the Breach doorway before the rifle pickup. The same resource is
    // also placed behind the player at 390.375,-793.076,17.405; keep that door.
    // Ordered comparisons also reject NaN and infinities without a broad match.
    return selected && definition == kEntity
        && x >= 410.053F && x <= 410.153F
        && y >= -788.792F && y <= -788.692F
        && z >= 17.544F && z <= 17.644F;
}

} // namespace dawn::state::activity::newlight::launchpad::shutter
