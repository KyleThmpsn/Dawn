// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>

#include "state/editor/edit.h"

namespace dawn::core::ui::modules::loadout::card {

namespace edit = state::editor;

/** What the card offers as its own action, which is the only thing the two pages differ on. */
enum class Action {
    /** An equipped slot swaps its item. */
    swap,
    /** A stored item equips into its slot. */
    equip,
    /** A slot with nothing in it. */
    fill,
};

/**
 * Draws one item as a self-contained editor card.
 * This is the single card both the loadout and the inventory use, so an item reads and edits the
 * same way wherever it appears. A rarity band carries the icon and the name, then one row holds
 * the power, the lock and the card's own action, then the item's sockets.
 * @param item Owned instance the card edits, or null for an empty slot.
 * @param label Row label under the name, such as the equipment slot.
 * @param slot Equipment slot the card belongs to, used by the fill and swap actions.
 * @param action Action this card offers.
 * @param width Card width in framebuffer pixels.
 * @param cardHeight Height to draw at, which the page sets to the tallest card in the row.
 */
void draw(edit::Item* item,
          const char* label,
          std::size_t slot,
          Action action,
          float width,
          float cardHeight) noexcept;

/**
 * @return The height this card needs for its own content, in framebuffer pixels.
 * An item with no sockets needs no socket row, so a page gives each row the height of its tallest
 * card rather than giving every card one fixed height with empty space below the short ones.
 * @param item Owned instance the card would draw, or null for an empty slot.
 * @param width Width the card will be drawn at, which decides how its sockets wrap.
 */
[[nodiscard]] float natural_height(const edit::Item* item, float width) noexcept;

} // namespace dawn::core::ui::modules::loadout::card
