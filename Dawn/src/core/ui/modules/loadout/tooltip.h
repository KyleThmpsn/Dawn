// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <imgui.h>

#include "state/editor/edit.h"

namespace dawn::core::ui::modules::loadout::tooltip {

namespace edit = state::editor;

/**
 * Draws the game's own item tooltip layout for one item.
 * A rarity band carries the name and tier, then the power, then the stats through their display
 * curve, then the perks the item has fitted. Stored stat values are not the ones the game shows,
 * so every number here goes through `display_stat` first.
 * @param definition Catalog item being described.
 * @param owned Instance whose power and fitted plugs are shown, or null for a catalog entry.
 */
void draw(const edit::CatalogItem& definition, const edit::Item* owned) noexcept;

/**
 * Armor stat targets a summary lets its own stat block set.
 * The block draws the same rows it always draws; with this bound, each bar also takes a drag, and
 * a row whose target has moved off the roll shows the target in the pending colour until a roll
 * reaches it. Targets share the index order of `edit::Stats`.
 */
struct StatEdit {
    /** Targets to show and to move. */
    edit::Stats* targets{};
    /** Highest value a bar accepts, which is as much as one piece of armor can roll. */
    std::int32_t limit{};
    /** Set when a drag moved a target this frame. */
    bool changed{};
    /** Set on the frame a bar is let go of, which is when the ask becomes a roll. */
    bool released{};
};

/**
 * Draws the item's own summary down a given width, with no frame of its own: the rarity band, the
 * description, the title figure and the stat block, in the game's own order.
 * The tooltip is this inside a floating frame; the inspector puts its editing controls under the
 * same thing, so one item reads the same wherever the editor shows it.
 * @param definition Catalog item being described.
 * @param owned Instance whose power and fitted plugs are shown, or null for a catalog entry.
 * @param width Content width in framebuffer pixels.
 * @param continues True when the caller draws more under the summary, which decides whether the
 * description is ruled off from what follows it.
 * @param stats Targets the armor stat block edits, or null to draw it read-only.
 */
void draw_summary(const edit::CatalogItem& definition,
                  const edit::Item* owned,
                  float width,
                  bool continues,
                  StatEdit* stats = nullptr) noexcept;

/**
 * Draws the compact tooltip one socket plug gets: its name, its type and what it does.
 * A plug is not an item: it carries no power, no stats of its own worth a block and no sockets, so
 * it gets a narrower band and its description rather than the full item tooltip.
 * @param plug Catalog definition fitted in the socket.
 */
void draw_plug(const edit::CatalogItem& plug) noexcept;

/**
 * Opens a frame drawn as the tooltip's body: its ground, its border and its padding, sized to
 * whatever is drawn inside it. The band and the rules measure themselves against the window they
 * are in, so a surface that extends the tooltip has to give them one; this is that window.
 * Item spacing is zero inside it, as it is inside the tooltip, so every gap is set by hand.
 * @param id Dear ImGui child id.
 * @param width Outer width in framebuffer pixels.
 * @return True while the frame is visible. `end_frame` is called either way.
 */
[[nodiscard]] bool begin_frame(const char* id, float width) noexcept;

/** Closes a frame opened by `begin_frame`. */
void end_frame() noexcept;

/**
 * Opens a sheet: the modal the editor lays over the page for one task, as the game lays its socket
 * picker over the inventory. It is the tooltip's ground in a square hairlined frame with no title
 * bar, dims the page behind it, and opens centred at the given size; it can then be dragged by its
 * head. A press outside it closes it, so a sheet needs no Close of its own; Escape is each sheet's
 * to take, since what it lets go of first differs. Every sheet the editor opens is this one frame
 * at this one size, so no two of them can drift apart.
 * @param id Popup id, which the caller opens with `ImGui::OpenPopup` from the same scope.
 * @return True while the sheet is open. Call `end_sheet` only then.
 */
[[nodiscard]] bool begin_sheet(const char* id) noexcept;

/** Closes a sheet opened by `begin_sheet`. */
void end_sheet() noexcept;

/**
 * Heads a sheet: its title in the title cut, folded to capitals, over one muted line.
 * @param title What the sheet is about, such as an item's name.
 * @param detail The muted line under it.
 */
void draw_sheet_head(const std::string& title, const char* detail) noexcept;

/** @return The padding the tooltip keeps inside its frame, in framebuffer pixels. */
[[nodiscard]] float padding() noexcept;

/** @return The tooltip's secondary text colour. */
[[nodiscard]] ImVec4 muted() noexcept;

/** @return The colour a value waiting to be applied is set in. */
[[nodiscard]] ImVec4 pending() noexcept;

/** @return The colour of the rule between two rows of a list, the tooltip's own divider. */
[[nodiscard]] ImVec4 rule_color() noexcept;

/** @return The ground of a sheet and of every popup on the page: the tooltip's near-black. */
[[nodiscard]] ImVec4 sheet_ground() noexcept;

/** @return The dim a sheet lays over the page behind it. */
[[nodiscard]] ImVec4 sheet_dim() noexcept;

/** Draws a divider across the whole frame at the cursor, and one block gap under it. */
void draw_rule() noexcept;

/**
 * Draws a divider across the whole frame at the cursor and steps past it with no gap, so the row
 * under it, and any fill that row lays down, meets the rule.
 */
void draw_row_rule() noexcept;

/**
 * Draws a section heading inside the frame, in the muted capitals the game heads a section with.
 * @param text Heading, folded to capitals here.
 */
void draw_heading(const char* text) noexcept;

/**
 * Draws one line of muted capitals at the cursor with no gaps of its own, as a sheet labels a group
 * of controls or counts a list. `draw_heading` is the same line spaced for the inside of a frame.
 * @param text Label, folded to capitals here.
 */
void draw_label(const char* text) noexcept;

/**
 * Draws one plug as the tooltip badges it: a round badge behind a trait, and the plug's own
 * framed artwork on its own for a mod, a shader or an intrinsic.
 * @param plug Plug to draw, or null for an empty socket, which is drawn as a recess.
 * @param origin Top-left of the badge in screen space.
 * @param extent Edge of the badge in framebuffer pixels; the icon sits inside it.
 * @param badged False to draw the icon alone, with no round badge behind a trait.
 */
void draw_plug_badge(const edit::CatalogItem* plug,
                     ImVec2 origin,
                     float extent,
                     bool badged = true) noexcept;

/** How much of a perk one row sets out. */
enum class PerkDetail : std::uint8_t {
    /** The badge and the name, as the tooltip lists a fitted perk. */
    name,
    /** The name over the plug's type, for a list that has to tell a mod from a trait. */
    typed,
    /** Name, type and what the perk does, for a surface with the height to spend on it. */
    full,
};

/**
 * @return The height one perk row takes at a width, in framebuffer pixels.
 * A list submits its own hit target over the row, so it asks for the height before drawing.
 * @param plug Plug the row shows, or null for an empty socket.
 * @param width Row width in framebuffer pixels.
 * @param detail How much of the plug the row sets out.
 */
[[nodiscard]] float perk_row_height(const edit::CatalogItem* plug,
                                    float width,
                                    PerkDetail detail) noexcept;

/**
 * Draws one perk row at the cursor and advances past it: the plug's icon, the name beside it, and
 * under that whatever the detail asks for. An intrinsic frame sits on the panel the game gives it.
 * @param plug Plug the row shows, or null for an empty socket.
 * @param width Row width in framebuffer pixels.
 * @param detail How much of the plug the row sets out.
 * @param lit True to lay the hover fill across the whole frame behind the row.
 */
void draw_perk_row(const edit::CatalogItem* plug, float width, PerkDetail detail, bool lit = false) noexcept;

} // namespace dawn::core::ui::modules::loadout::tooltip
