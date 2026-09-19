// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <imgui.h>
#include <string>

#include "state/editor/edit.h"

namespace dawn::core::ui::modules::loadout::art {

namespace edit = state::editor;

/** @return The authored rarity tint for one item tier. */
[[nodiscard]] ImVec4 rarity_color(std::uint8_t tier) noexcept;

/**
 * @return The text color that reads on one tier's band: dark on the light bands, white otherwise.
 * @param tier Rarity tier of the band.
 * @param muted True for the secondary line, which sits a little into the band.
 */
[[nodiscard]] ImU32 band_text(std::uint8_t tier, bool muted = false) noexcept;

/** @return The name of one item tier. Unknown tiers read as unclassified. */
[[nodiscard]] const char* tier_name(std::uint8_t tier) noexcept;

/** Highest rarity tier the catalog carries, which is Exotic. */
inline constexpr std::uint8_t kExoticTier = 5;

/** @return The text in ASCII capitals, as the game sets an item title on its band. */
[[nodiscard]] std::string shout(const std::string& value) noexcept;

/**
 * @return The height one armory result row occupies, on whole framebuffer pixels.
 * The grid pitches its clipper off this, so the row a clipper skips is the row that is drawn.
 */
[[nodiscard]] float collection_card_height() noexcept;

/** @return The name of one character class. */
[[nodiscard]] const char* class_name(state::CharacterClass value) noexcept;

/**
 * Draws one item icon, and a placeholder while the package image loads.
 * @param item Catalog item whose icon tag is requested.
 * @param origin Top-left corner in framebuffer pixels.
 * @param extent Square edge length in framebuffer pixels.
 * @param framed True to outline the icon in its rarity tint. Sockets sit in a dense row where an
 *        outline on every one of them reads as a grid of boxes, so they ask for no frame.
 */
void icon(const edit::CatalogItem& item, ImVec2 origin, float extent, bool framed = true) noexcept;

/**
 * Draws text clipped to a width, appending an ellipsis when it does not fit.
 * Truncation steps back over whole UTF-8 sequences, so a cut never splits a character.
 * @param text Text to draw.
 * @param at Baseline-left position in framebuffer pixels.
 * @param width Space available in framebuffer pixels.
 * @param color Packed text color.
 * @param weight Extra stroke width in framebuffer pixels. Zero draws the plain face.
 */
void clipped_text(const std::string& text,
                  ImVec2 at,
                  float width,
                  ImU32 color,
                  float weight = 0.0F) noexcept;

/**
 * Pushes the cut an item title is set in: the medium weight of the interface family.
 * @param size Font height in framebuffer pixels.
 * @param fallbackWeight Stroke weight to use when the build ships no medium cut.
 * @return The weight to pass on to `bold_text` or `clipped_text`, which is zero once a real cut
 * is pushed and there is nothing left to fake. The caller pops the font either way.
 */
[[nodiscard]] float push_title(float size, float fallbackWeight) noexcept;

/**
 * Pushes the cut a figure set large is set in: the display bold the install ships.
 * A display cut is drawn for sizes like the power figure's, where a text cut's looser fitting and
 * lighter stroke read as thin.
 * @param size Font height in framebuffer pixels.
 * @param fallbackWeight Stroke weight to use when the build ships no display cut.
 * @return The weight to pass on to `bold_text`, zero once a real cut is pushed. The caller pops
 * the font either way.
 */
[[nodiscard]] float push_figure(float size, float fallbackWeight) noexcept;

/**
 * Draws text with extra stroke weight.
 * The interface face ships no bold cut, so weight is made by striking the same text again a
 * fraction of a pixel to the right, as many times as the requested width needs.
 * @param text Null-terminated text to draw.
 * @param at Top-left position in framebuffer pixels.
 * @param color Packed text color.
 * @param weight Extra stroke width in framebuffer pixels. Zero draws the plain face.
 */
void bold_text(const char* text, ImVec2 at, ImU32 color, float weight) noexcept;

/**
 * Draws one armory result row.
 * @param item Catalog definition behind the row.
 * @param width Row width in framebuffer pixels.
 */
void collection_card(const edit::CatalogItem& item, float width) noexcept;

} // namespace dawn::core::ui::modules::loadout::art
