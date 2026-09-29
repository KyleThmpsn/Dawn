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
// Names the class of a bare image record, so a tag of that class is drawn as the one image it is
// rather than read as an icon container. The interface packages keep their art that way.
void set_image_class(std::uint32_t classId) noexcept;
// Requests only visible artwork, then draws background, primary, watermark and foreground.
// `tint` multiplies every layer, which is how a monochrome UI glyph is given its meaning colour.
bool draw(std::uint32_t tag,
          ImVec2 position,
          float size,
          ImU32 tint = IM_COL32_WHITE) noexcept;
// Draws an icon inside a box at its own proportions, centred, never stretched to the box's shape.
// `texel` is the framebuffer size of one texel at the icon's natural size, and `largest` caps the
// scale over that natural size: 1 only ever shrinks an icon, which is what a grid of tiles wants.
// `layer` draws only that layer, counted as `layer_details` counts them, fitted at its own size.
bool draw_fitted(std::uint32_t tag,
                 ImVec2 origin,
                 ImVec2 box,
                 float texel,
                 float largest,
                 ImU32 tint = IM_COL32_WHITE,
                 int layer = -1) noexcept;
// What a drawn icon is made of: its primary layer's size in texels and how many layers it stacks.
struct Details {
    unsigned width{};
    unsigned height{};
    unsigned layers{};
};
// Fills the details once the icon has been drawn and its artwork is in; false until then.
bool details(std::uint32_t tag, Details& output) noexcept;
// One layer of a drawn icon: its place in the stack, the image record it is, and its size in texels.
struct LayerDetails {
    const char* role{};
    std::uint32_t tag{};
    unsigned width{};
    unsigned height{};
};
// Fills one layer's details, in the order the layers are drawn; false until the artwork is in.
bool layer_details(std::uint32_t tag, unsigned index, LayerDetails& output) noexcept;
// Asks the worker to save one icon, its layers stacked as they are drawn, as a PNG in the Dawn
// folder's exports, or only its layer whose image record is `layer`. The page collects the outcome
// with `take_export`.
bool request_export(std::uint32_t tag, std::uint32_t layer = 0) noexcept;
// Takes the outcome of the last export once it has finished; false while none is waiting.
bool take_export(std::uint32_t& tag, bool& saved, std::string& message) noexcept;
}
