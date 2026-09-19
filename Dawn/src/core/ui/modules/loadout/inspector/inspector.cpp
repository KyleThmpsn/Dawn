// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cfloat>
#include <imgui.h>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../internal.h"
#include "../controls.h"
#include "../tooltip.h"
#include "state/account/inventory/placement.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Height of the actions the inspector offers, and the width of a grant field beside its label. */
constexpr float kSecondaryActionHeight = 28.0F;
constexpr float kGrantFieldWidth = 90.0F;
/** Title of the item removal, used for both the action and its modal. */
constexpr const char* kRemoveItemTitle = "Remove item?";
/** The pane's own controls, which say what they do rather than showing a glyph. */
constexpr const char* kCloseLabel = "Close";
constexpr const char* kUnequipLabel = "Unequip";

/** Where the selected item sits on the character, if it is equipped at all. */
struct Placement {
    bool equipped{};
    std::size_t slot{};
};

/** @return Where one owned instance sits in the character's equipment. */
[[nodiscard]] Placement placement_of(std::uint64_t instance) noexcept {
    const auto& slots = character().equipment.slots;
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        if (slots[slot] && slots[slot]->instanceSoid == instance) {
            return {true, slot};
        }
    }
    return {};
}

/** @return True when the catalog item is a stack rather than a single instance. */
[[nodiscard]] bool stackable(const edit::CatalogItem& definition) noexcept {
    using state::build_data::items::details::InstancedDefinitionState;
    return definition.detail.instancedDefinitionState == InstancedDefinitionState::stackable;
}

/**
 * Draws the pane's own header row, above the item: where the item is on the left, with the
 * action that moves it, and the pane's close on the right.
 * These are the editor's controls, not the game's, so they sit outside the item frame.
 */
void draw_pane_header(const edit::Item* owned) noexcept {
    Model& state = model();
    const Placement placement = owned != nullptr ? placement_of(owned->instanceSoid) : Placement{};
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(owned == nullptr        ? "Not owned"
                        : placement.equipped    ? "Equipped"
                        : owned->postmaster     ? "Postmaster"
                                                : "In inventory");
    if (placement.equipped) {
        ImGui::SameLine();
        if (ImGui::SmallButton(kUnequipLabel)) {
            record_edit(edit::unequip(
                *state.draft, state.catalog, state.character, placement.slot, state.status));
        }
    }
    const float closeWidth =
        ImGui::CalcTextSize(kCloseLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    if (ImGui::SmallButton(kCloseLabel)) {
        clear_selection();
    }
}

/** @return True when the game gives this item a Power, which only gear carries. */
[[nodiscard]] bool carries_power(const edit::CatalogItem& definition) noexcept {
    return definition.kind == edit::GearKind::weapon || definition.kind == edit::GearKind::armor;
}

/**
 * Draws the grant controls for an item the character does not own yet: a power field for gear,
 * a quantity field for a stack, then the add actions on one row. Enter in either field adds.
 */
void draw_grant(const edit::CatalogItem& definition) noexcept {
    Model& state = model();
    const bool powered = carries_power(definition);
    const bool stacked = stackable(definition);
    const bool equippable = definition.slot < inv::kEquipmentSlotCount;
    bool add = false;
    bool equip = false;
    // Both fields share one label column, and only the fields the item has are offered: an
    // emblem has no Power and a weapon is no stack.
    const float column = ImGui::CalcTextSize(powered ? "Power" : "Quantity").x;
    if (powered) {
        controls::field_label("Power", column);
        ImGui::SetNextItemWidth(pixels(kGrantFieldWidth));
        if (ImGui::InputInt("##power", &state.grant.power, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            add = true;
        }
        state.grant.power = std::clamp(state.grant.power, 0, kPowerSliderMaximum);
    }
    if (stacked) {
        controls::field_label("Quantity", column);
        ImGui::SetNextItemWidth(pixels(kGrantFieldWidth));
        if (ImGui::InputInt("##quantity", &state.grant.quantity, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            add = true;
        }
        state.grant.quantity =
            std::clamp(state.grant.quantity, 1, (std::max)(1, definition.detail.maxStackSize));
    }
    if (powered || stacked) {
        controls::space(controls::kRowSpacing);
    }

    // The two actions share one row, the primary first; an item nothing equips gets one action.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = equippable ? (width - spacing) * 0.5F : width;
    add |= controls::primary_button("Add to inventory", {half, pixels(kSecondaryActionHeight)});
    if (equippable) {
        ImGui::SameLine();
        equip = ImGui::Button("Add and equip", {half, pixels(kSecondaryActionHeight)});
    }
    if (!add && !equip) {
        return;
    }
    const bool granted = edit::give(*state.draft,
                                    state.catalog,
                                    state.character,
                                    definition.definition.definitionHash,
                                    stacked ? state.grant.quantity : 1,
                                    level_of(state.grant.power),
                                    equip,
                                    state.status);
    record_edit(granted);
    // The owned-item editor takes over on the next frame, bound to the instance just equipped.
    if (equip && granted && equippable) {
        const auto& slot = character().equipment.slots[definition.slot];
        if (slot) {
            state.selection.instanceSoid = slot->instanceSoid;
        }
    }
}

/** Draws the removal action, which only an unequipped, unlocked item offers. */
void draw_remove_action() noexcept {
    if (ImGui::Button("Remove from inventory", {-FLT_MIN, pixels(kSecondaryActionHeight)})) {
        ImGui::OpenPopup(kRemoveItemTitle);
    }
    if (!ImGui::BeginPopupModal(kRemoveItemTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("Remove this item from the inventory?");
    if (ImGui::Button("Remove")) {
        (void)erase_owned_item(model().selection.instanceSoid);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/** @return How many sockets one owned item resolves to, or zero when it has none. */
[[nodiscard]] std::size_t socket_count(const edit::Item& item) noexcept {
    edit::Item resolved = item;
    return edit::materialize(resolved, model().catalog) ? resolved.sockets.plugCount : 0;
}

/**
 * Draws the item itself: the tooltip's own summary, with the armor stat block taking targets,
 * then every socket as a row that opens its picker. All of it sits in one tooltip frame, so the
 * item reads in the pane exactly as it does under the pointer, with the editing laid into it.
 * @param definition Catalog definition of the item.
 * @param owned Owned instance, or null for a catalog item nobody owns yet.
 * @param width Outer width of the frame in framebuffer pixels.
 */
void draw_item_frame(const edit::CatalogItem& definition,
                     edit::Item* owned,
                     float width) noexcept {
    // The bars only take a drag when a roll can actually move: armor whose stat plugs have no
    // alternative in their socket is shown as the tooltip shows it, and nothing pretends otherwise.
    const bool armor = owned != nullptr && definition.kind == edit::GearKind::armor
                       && edit::adjustable_stats(*owned, model().catalog);
    const std::size_t sockets = owned != nullptr ? socket_count(*owned) : 0;
    if (!tooltip::begin_frame("item", width)) {
        tooltip::end_frame();
        return;
    }
    const float inner = width - (tooltip::padding() * 2.0F);
    tooltip::StatEdit stats;
    if (armor) {
        stats.targets = &stat_targets_for(*owned);
        stats.limit = kMaximumStatTarget;
    }
    tooltip::draw_summary(definition, owned, inner, armor || sockets != 0, armor ? &stats : nullptr);
    if (armor) {
        settle_stat_targets(*owned, stats.released);
    }
    if (sockets != 0) {
        tooltip::draw_rule();
        draw_perk_editor(definition, *owned, inner);
    }
    tooltip::end_frame();
}

/** Draws what the pane offers under the item frame for an item the character owns. */
void draw_owned_actions(const edit::CatalogItem& definition, edit::Item& item) noexcept {
    if (stackable(definition)) {
        controls::space(controls::kSectionSpacing);
        const float column = ImGui::CalcTextSize("Quantity").x;
        controls::field_label("Quantity", column);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool quantityChanged = ImGui::InputInt("##owned_quantity", &item.quantity);
        if (quantityChanged) {
            item.quantity =
                std::clamp(item.quantity, 1, (std::max)(1, definition.detail.maxStackSize));
        }
        record_scalar_edit(quantityChanged);
    }
    if (!placement_of(item.instanceSoid).equipped && (item.flags & inv::kLockedItemFlag) == 0) {
        controls::space(controls::kSectionSpacing);
        draw_remove_action();
    }
}

} // namespace

void draw_inspector() noexcept {
    Model& state = model();
    const edit::CatalogItem* definition = state.catalog.find(state.selection.definitionHash);
    if (definition == nullptr) {
        ImGui::TextDisabled("No item selected");
        return;
    }
    draw_pane_header(selected_item());
    // Unequipping moves the item between two arrays, so the pointer is taken again after it.
    edit::Item* owned = selected_item();
    controls::space(controls::kRowSpacing);
    draw_item_frame(*definition, owned, ImGui::GetContentRegionAvail().x);
    if (owned != nullptr) {
        draw_owned_actions(*definition, *owned);
    } else {
        controls::space(controls::kSectionSpacing);
        draw_grant(*definition);
    }
}

} // namespace dawn::core::ui::modules::loadout::internal
