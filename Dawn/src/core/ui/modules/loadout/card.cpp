// SPDX-License-Identifier: GPL-3.0-only
#include "card.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui.h>

#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "art.h"
#include "internal.h"
#include "tooltip.h"
#include "state/account/inventory/placement.h"

namespace dawn::core::ui::modules::loadout::card {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Card geometry: a rarity band over the fields that edit the item. */
constexpr float kPadding = 6.0F;
/** The game's cards are square-cornered, as its tooltip is. */
constexpr float kRounding = 0.0F;
/** The name on the band is set in the title cut, in capitals, a little over the body. */
constexpr float kBandNameScale = 1.35F;
constexpr float kBandNameWeight = 0.8F;
/** The control row is set shorter than a page control: one text line with a sliver over it. */
constexpr float kControlPaddingY = 1.0F;
constexpr float kControlTextScale = 0.88F;
/** Id of the card's context menu. */
constexpr const char* kMenuId = "card_menu";
/** Set between the item's type and the word after it on the band's second line. */
constexpr const char* kDetailSeparator = "  |  ";
/**
 * The band is as tall as the icon it carries, because the icon sits flush inside it.
 * 36 authored pixels is the shortest band that still clears two lines of text, and it keeps the
 * card icon plainly smaller than the one the tooltip sets.
 */
constexpr float kBandHeight = 38.0F;
/** Gap between the name on a band and the label under it, matching the tooltip's own header. */
/** Negative: the title face keeps descender room no capital uses, so the type line rides up into it. */
constexpr float kBandLineGap = -2.0F;
/** Gap between the icon and the name column beside it. */
constexpr float kBandTextGap = 7.0F;
/** 52 authored pixels hold a four-figure power without the field reading as an empty box. */
constexpr float kPowerFieldWidth = 52.0F;
/** Socket icons and the gap between them. */
constexpr float kPlugExtent = 24.0F;
constexpr float kPlugGap = 3.0F;
/** Outline of the card the inspector is bound to, the same weight the armory gives its row. */
constexpr float kSelectedThickness = 2.0F;
/** The two lock labels. The button offers the action, so its label is what pressing it does. */
constexpr const char* kLockLabel = "Lock";
constexpr const char* kUnlockLabel = "Unlock";

/** @return How many sockets one owned item shows on its card, or zero when it has none. */
[[nodiscard]] std::size_t plug_count(const edit::Item* item) noexcept {
    if (item == nullptr) {
        return 0;
    }
    const edit::Catalog& catalog = internal::model().catalog;
    edit::Item resolved = *item;
    if (!edit::materialize(resolved, catalog)) {
        return 0;
    }
    // The count has to agree with the row, which leaves out the unnamed stat plugs.
    std::size_t shown = 0;
    for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) {
        const auto& current = resolved.sockets.plugs[lane];
        const edit::CatalogItem* fitted = current ? catalog.find(*current) : nullptr;
        shown += fitted == nullptr || !fitted->unnamed ? 1 : 0;
    }
    return shown;
}

/**
 * @return The content width a card of this outer width offers.
 * The height and the socket wrap are both taken from this, so the height a page reserves and the
 * rows the card actually draws can never disagree.
 * @param width Outer card width in framebuffer pixels.
 */
[[nodiscard]] float inner_width(float width) noexcept {
    return (std::max)(
        0.0F, width - (2.0F * ImGui::GetStyle().ChildBorderSize) - (2.0F * pixels(kPadding)));
}

/** @return How many sockets fit across one card, at least one. */
[[nodiscard]] std::size_t plugs_per_row(float inner) noexcept {
    const float pitch = pixels(kPlugExtent) + pixels(kPlugGap);
    if (pitch <= 0.0F) {
        return 1;
    }
    const auto fit = static_cast<std::size_t>((inner + pixels(kPlugGap)) / pitch);
    return (std::max)(std::size_t{1}, fit);
}

/** @return How many rows the sockets of one item wrap onto. */
[[nodiscard]] std::size_t plug_rows(std::size_t count, float inner) noexcept {
    if (count == 0) {
        return 0;
    }
    const std::size_t perRow = plugs_per_row(inner);
    return (count + perRow - 1) / perRow;
}

/** Sends the armory to the category that fills one equipment slot. */
void browse_for_slot(std::size_t slot) noexcept {
    internal::Model& state = internal::model();
    state.view = internal::View::armory;
    state.browse.category = slot <= internal::kLastWeaponSlot  ? internal::Category::weapons
                            : slot <= internal::kLastArmorSlot ? internal::Category::armor
                                                               : internal::Category::cosmetics;
    state.browse.type.clear();
}

/**
 * Draws the sockets of one item as icons, each opening its picker.
 * Armor carries more lanes than a card is wide, so the row wraps at the same count the height was
 * reserved for rather than running off the edge of the card and being clipped.
 */
void draw_plug_row(const edit::CatalogItem& definition,
                   const edit::Item& item,
                   float inner) noexcept {
    const edit::Catalog& catalog = internal::model().catalog;
    edit::Item resolved = item;
    if (!edit::materialize(resolved, catalog)) {
        return;
    }
    const float plug = pixels(kPlugExtent);
    const std::size_t perRow = plugs_per_row(inner);
    std::size_t shown = 0;
    for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) {
        const auto& current = resolved.sockets.plugs[lane];
        const edit::CatalogItem* fitted = current ? catalog.find(*current) : nullptr;
        // Armor's rolled stat plugs have no name and no artwork; the stat bars edit those.
        if (fitted != nullptr && fitted->unnamed) {
            continue;
        }
        ImGui::PushID(static_cast<int>(lane));
        if (shown++ % perRow != 0) {
            ImGui::SameLine(0.0F, pixels(kPlugGap));
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("plug", {plug, plug});
        // Sockets are badged as the tooltip badges them, so a trait reads as a trait on the card.
        tooltip::draw_plug_badge(fitted, at, plug);
        if (ImGui::IsItemHovered()) {
            if (fitted != nullptr) {
                tooltip::draw_plug(*fitted);
            } else {
                ImGui::SetTooltip("Empty socket");
            }
        }
        ImGui::PopID();
        if (clicked) {
            // The picker edits whichever item the inspector holds, so the card selects it first.
            internal::select(definition, item.instanceSoid);
            internal::open_perk_picker(definition, lane);
        }
    }
}

/**
 * Draws the rarity band and leaves the cursor directly under it.
 * The icon is struck against the card's own frame, so it has no padding above, below or to its
 * left, and the band reads as one solid header rather than a picture floating inside one.
 * @return The band's bottom-right corner in screen space, which is also the hover target.
 */
ImVec2 draw_band(const edit::CatalogItem* definition, const char* label) noexcept {
    const float padding = pixels(kPadding);
    const float band = pixels(kBandHeight);
    const float border = ImGui::GetStyle().ChildBorderSize;
    const ImVec2 card = ImGui::GetWindowPos();
    const ImVec2 origin{card.x + border, card.y + border};
    const float right = card.x + ImGui::GetWindowSize().x - border;
    const ImVec4 tint = definition != nullptr ? art::rarity_color(definition->definition.tier)
                                              : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);

    ImGui::GetWindowDrawList()->AddRectFilled(origin,
                                              {right, origin.y + band},
                                              ImGui::GetColorU32(tint),
                                              pixels(kRounding),
                                              ImDrawFlags_RoundCornersTop);
    if (definition != nullptr) {
        art::icon(*definition, origin, band);
    }

    // The name and its label are centred as a pair against the icon beside them. The name is set
    // as the tooltip sets it, in capitals in the title cut, so the card and the tooltip agree.
    const float textLeft = origin.x + band + pixels(kBandTextGap);
    const float textWidth = right - padding - textLeft;
    const float line = ImGui::GetTextLineHeight();
    const float nameSize = ImGui::GetStyle().FontSizeBase * kBandNameScale;
    const float nameWeight = art::push_title(nameSize, pixels(kBandNameWeight));
    const float nameLine = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const std::uint8_t tier = definition != nullptr ? definition->definition.tier : 0;
    // The second line is the item's type, then the word the page adds after it: where the item is
    // equipped, or the bucket it fills. An empty slot has only the bucket to say.
    std::string detail = definition != nullptr ? definition->type : std::string();
    // A bucket named the same as the type, as "Ghost" or "Ship" is, would only say it twice.
    if (label != nullptr && detail != label) {
        detail += detail.empty() ? std::string(label) : kDetailSeparator + std::string(label);
    }
    const float gap = definition != nullptr && !detail.empty() ? pixels(kBandLineGap) : 0.0F;
    const float stack = (definition != nullptr ? nameLine : 0.0F) + (detail.empty() ? 0.0F : line);
    float top = origin.y + ((band - stack - gap) * 0.5F);
    if (definition != nullptr) {
        (void)art::push_title(nameSize, 0.0F);
        art::clipped_text(
            art::shout(definition->name), {textLeft, top}, textWidth, art::band_text(tier), nameWeight);
        ImGui::PopFont();
        top += nameLine + gap;
    }
    if (!detail.empty()) {
        art::clipped_text(detail, {textLeft, top}, textWidth, art::band_text(tier, true));
    }

    // The content cursor sits one padding below the frame, so the spacer covers what is left of
    // the band and the next item starts against its bottom edge.
    ImGui::Dummy({right - origin.x, (std::max)(0.0F, border + band - padding)});
    return {right, origin.y + band};
}

/** @return The width the lock toggle takes, which is the wider of its two labels. */
[[nodiscard]] float lock_toggle_width() noexcept {
    // Both labels are measured, so the row does not shift when the state flips under the pointer.
    return (std::max)(ImGui::CalcTextSize(kLockLabel).x, ImGui::CalcTextSize(kUnlockLabel).x)
           + (ImGui::GetStyle().FramePadding.x * 2.0F);
}

/** Draws the lock toggle. */
void draw_lock_toggle(edit::Item& item) noexcept {
    const bool locked = (item.flags & inv::kLockedItemFlag) != 0;
    const float width = lock_toggle_width();
    if (ImGui::Button(locked ? kUnlockLabel : kLockLabel, {width, ImGui::GetFrameHeight()})) {
        item.flags = locked ? item.flags & ~inv::kLockedItemFlag
                            : item.flags | inv::kLockedItemFlag;
        internal::mark_changed();
    }
}

/** @return True when the game gives this item a Power, which only gear carries. */
[[nodiscard]] bool carries_power(const edit::CatalogItem& definition) noexcept {
    return definition.kind == edit::GearKind::weapon || definition.kind == edit::GearKind::armor;
}

/** @return The height of the card's control row, which is set shorter than a page control. */
[[nodiscard]] float control_height() noexcept {
    return (ImGui::GetStyle().FontSizeBase * kControlTextScale) + (pixels(kControlPaddingY) * 2.0F);
}

/** Draws the one row of controls: power at the left, the lock and the card's action at the right. */
void draw_controls(const edit::CatalogItem& definition,
                   edit::Item& item,
                   std::size_t slot,
                   Action action) noexcept {
    internal::Model& state = internal::model();
    const float padding = pixels(kPadding);
    const float row = ImGui::GetCursorScreenPos().y;
    // Every control on the row takes the row's own height: its type is set a little under the
    // body and the frame padding is set for it.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kControlTextScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2{ImGui::GetStyle().FramePadding.x, pixels(kControlPaddingY)});
    struct PopRow {
        ~PopRow() {
            ImGui::PopStyleVar();
            ImGui::PopFont();
        }
    } popRow;

    // An emblem or a ship has no Power, so it is offered no field to set one in.
    if (carries_power(definition)) {
        // The field speaks in Power; the account stores the level behind it.
        ImGui::SetNextItemWidth(pixels(kPowerFieldWidth));
        int power = internal::power_of(item.level);
        const bool powerChanged = ImGui::InputInt("##power", &power, 0, 0);
        if (powerChanged) {
            item.level = std::clamp(internal::level_of(power), 0, internal::kMaximumItemLevel);
        }
        internal::record_scalar_edit(powerChanged);
        ImGui::SameLine(0.0F, 0.0F);
    }

    const char* title = action == Action::equip ? "Equip" : "Swap";
    const float button = ImGui::CalcTextSize(title).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    // The action is pinned to the right edge of the card, with the lock beside it, which keeps
    // every row reading alike however wide the grid drew them. Screen space, so nothing is
    // assumed about where a child window puts its local origin relative to its border and its
    // padding.
    const float edge = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x
                       - ImGui::GetStyle().ChildBorderSize - padding - button;
    const float lockLeft = edge - pixels(kPlugGap) - lock_toggle_width();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({(std::max)(at.x, lockLeft), row});
    draw_lock_toggle(item);
    ImGui::SameLine(0.0F, pixels(kPlugGap));
    ImGui::SetCursorScreenPos({(std::max)(ImGui::GetCursorScreenPos().x, edge), row});
    if (!ImGui::Button(title, {button, ImGui::GetFrameHeight()})) {
        return;
    }
    if (action == Action::equip) {
        internal::record_edit(edit::equip(
            *state.draft, state.catalog, state.character, item.instanceSoid, state.status));
    } else {
        browse_for_slot(slot);
    }
}

/**
 * Draws the card's context menu, which offers every action the card has as words: opening it in
 * the inspector, equipping or unequipping, locking, and removing a stowed item. A locked item
 * cannot be removed until it is unlocked, which is the game's own rule for the lock.
 */
void draw_menu(const edit::CatalogItem& definition,
               edit::Item& item,
               std::size_t slot,
               Action action) noexcept {
    if (!ImGui::BeginPopup(kMenuId)) {
        return;
    }
    internal::Model& state = internal::model();
    const bool equipped = action == Action::swap;
    const bool locked = (item.flags & inv::kLockedItemFlag) != 0;
    if (ImGui::Selectable("Open")) {
        internal::select(definition, item.instanceSoid);
    }
    if (equipped) {
        if (ImGui::Selectable("Unequip")) {
            internal::record_edit(edit::unequip(
                *state.draft, state.catalog, state.character, slot, state.status));
        }
        if (ImGui::Selectable("Swap")) {
            browse_for_slot(slot);
        }
    } else if (ImGui::Selectable("Equip")) {
        internal::record_edit(edit::equip(
            *state.draft, state.catalog, state.character, item.instanceSoid, state.status));
    }
    if (ImGui::Selectable(locked ? kUnlockLabel : kLockLabel)) {
        item.flags = locked ? item.flags & ~inv::kLockedItemFlag : item.flags | inv::kLockedItemFlag;
        internal::mark_changed();
    }
    if (!equipped && !locked && ImGui::Selectable("Remove")) {
        (void)internal::erase_owned_item(item.instanceSoid);
    }
    ImGui::EndPopup();
}

} // namespace

float natural_height(const edit::Item* item, float width) noexcept {
    const ImGuiStyle& style = ImGui::GetStyle();
    // The band is flush with the frame, so it carries the top border rather than a padding.
    float total = style.ChildBorderSize + pixels(kBandHeight);
    total += style.ItemSpacing.y + control_height();
    const std::size_t rows = plug_rows(plug_count(item), inner_width(width));
    total += static_cast<float>(rows) * (style.ItemSpacing.y + pixels(kPlugExtent));
    // The control row's type is scaled, so its height lands on a fraction; the card rounds up
    // rather than clipping the last row by that fraction.
    return std::ceil(total + pixels(kPadding) + style.ChildBorderSize);
}

void draw(edit::Item* item,
          const char* label,
          std::size_t slot,
          Action action,
          float width,
          float cardHeight) noexcept {
    internal::Model& state = internal::model();
    const edit::CatalogItem* definition =
        item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
    const float padding = pixels(kPadding);

    ImGui::PushID(static_cast<int>(slot));
    ImGui::PushID(item != nullptr ? static_cast<int>(item->instanceSoid) : 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{padding, padding});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, pixels(kRounding));
    // A card is sized to its content by the page, so it never scrolls: a stray pixel of overflow
    // must not grow a scrollbar over the action pinned to its right edge.
    if (ImGui::BeginChild("card",
                          {width, cardHeight},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 bandOrigin = ImGui::GetWindowPos();
        const ImVec2 bandCorner = draw_band(definition, label);

        if (definition == nullptr) {
            if (ImGui::Button("Choose item", {-FLT_MIN, control_height()})) {
                browse_for_slot(slot);
            }
        } else {
            // The whole band is the hover target, not only the name text inside it.
            if (ImGui::IsMouseHoveringRect(bandOrigin, bandCorner) && ImGui::IsWindowHovered()) {
                tooltip::draw(*definition, item);
            }
            draw_controls(*definition, *item, slot, action);
            draw_plug_row(*definition, *item, inner_width(width));
            // The card as a whole opens the item in the inspector. This is asked after its own
            // controls have been submitted, so a click that landed on the lock, the swap, the
            // level field or a socket is already accounted for and does not select as well.
            if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                internal::select(*definition, item->instanceSoid);
            }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup(kMenuId);
            }
            draw_menu(*definition, *item, slot, action);
        }
    }
    ImGui::EndChild();
    // The card the inspector holds is outlined in the accent, as the armory outlines its own
    // selected row, so the page says which item the pane beside it is editing.
    if (definition != nullptr && state.selection.holds(definition->definition.definitionHash,
                                                        item->instanceSoid)) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
                                            ImGui::GetItemRectMax(),
                                            ImGui::GetColorU32(ImGuiCol_Text),
                                            pixels(kRounding),
                                            0,
                                            pixels(kSelectedThickness));
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::PopID();
}

} // namespace dawn::core::ui::modules::loadout::card
