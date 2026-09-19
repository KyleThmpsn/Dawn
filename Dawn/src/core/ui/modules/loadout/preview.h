#pragma once
#include <cstdint>
#include <imgui.h>
struct ID3D11Device;

namespace dawn::core::ui::modules::loadout::preview {
bool unavailable(std::uint32_t tag) noexcept;
// Device lifecycle functions run on the presentation thread, alongside ImGui's renderer.
void attach(ID3D11Device* device) noexcept;
void release() noexcept;
void shutdown() noexcept;
// Requests only visible artwork, then draws background, primary, watermark and foreground.
// `tint` multiplies every layer, which is how a monochrome UI glyph is given its meaning colour.
bool draw(std::uint32_t tag,
          ImVec2 position,
          float size,
          ImU32 tint = IM_COL32_WHITE) noexcept;
}
