#pragma once
#include <cstdint>
#include <imgui.h>
#include <string>
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
// Draws an icon inside a box at its own proportions, centred, never stretched to the box's shape.
// `texel` is the framebuffer size of one texel at the icon's natural size, and `largest` caps the
// scale over that natural size: 1 only ever shrinks an icon, which is what a grid of tiles wants.
bool draw_fitted(std::uint32_t tag,
                 ImVec2 origin,
                 ImVec2 box,
                 float texel,
                 float largest,
                 ImU32 tint = IM_COL32_WHITE) noexcept;
// What a drawn icon is made of: its primary layer's size in texels and how many layers it stacks.
struct Details {
    unsigned width{};
    unsigned height{};
    unsigned layers{};
};
// Fills the details once the icon has been drawn and its artwork is in; false until then.
bool details(std::uint32_t tag, Details& output) noexcept;
// Asks the worker to save one icon, its layers stacked as they are drawn, as a PNG in the Dawn
// folder's exports. The page collects the outcome with `take_export`.
bool request_export(std::uint32_t tag) noexcept;
// Takes the outcome of the last export once it has finished; false while none is waiting.
bool take_export(std::uint32_t& tag, bool& saved, std::string& message) noexcept;
}
