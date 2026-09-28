// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <imgui.h>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../internal.h"
#include "../controls.h"
#include "../tooltip.h"
#include "state/account/inventory/placement.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Width of a grant field beside its label. The actions the pane offers take the shared action height. */
constexpr float kGrantFieldWidth = 90.0F;
/** Title of the item removal, used for both the action and its modal. */
constexpr const char* kRemoveItemTitle = "Remove Item?";
/** Width of each of the removal confirmation's two buttons, so the pair reads as one row. */
constexpr float kConfirmButtonWidth = 92.0F;
/** The pane's own controls, which say what they do rather than showing a glyph. */
constexpr const char* kCloseLabel = "Close";
constexpr const char* kUnequipLabel = "Unequip";
constexpr const char* kPullLabel = "Pull";
constexpr const char* kSendLabel = "Send To";
/** Where the item the pane shows is kept, in the header's words. */
constexpr const char* kEquippedLabel = "Equipped";
constexpr const char* kPostmasterLabel = "Postmaster";
constexpr const char* kInInventoryLabel = "In Inventory";
constexpr const char* kNotInInventoryLabel = "Not in Inventory";

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

/**
 * @return Where the character already keeps a copy of one catalog item, in the header's words, or
 * "Not in Inventory" when it keeps none.
 * A pane bound to a catalog entry still offers to add another copy, so it stays bound to the entry;
 * this only lets its header say truthfully whether there is one already. Without it the header read
 * that it was not there beside an item just added to the inventory, and beside any already there.
 * @param hash Definition hash of the catalog item.
 */
[[nodiscard]] const char* holding_of(std::uint32_t hash) noexcept {
    const state::CharacterState& owner = character();
    for (const auto& slot : owner.equipment.slots) {
        if (slot && slot->definitionHash == hash) {
            return kEquippedLabel;
        }
    }
    bool postmaster = false;
    for (std::size_t i = 0; i < owner.inventory.count; ++i) {
        const edit::Item& stowed = owner.inventory.values[i];
        if (stowed.definitionHash != hash) {
            continue;
        }
        if (!stowed.postmaster) {
            return kInInventoryLabel;
        }
        postmaster = true;
    }
    if (postmaster) {
        return kPostmasterLabel;
    }
    // An account stack, such as a material or a shader, is kept by the profile rather than the
    // character, and the page that lists them is its inventory as much as the character's is.
    const state::AccountState& account = model().draft->after;
    for (std::size_t i = 0; i < account.profileItemCount; ++i) {
        if (account.profileItems[i].definitionHash == hash) {
            return kInInventoryLabel;
        }
    }
    return kNotInInventoryLabel;
}

/**
 * @return True when the field just drawn was left with Enter, which submits the grant.
 * The number fields apply what is typed as it is typed. Dear ImGui does not support asking a number
 * field to report Enter instead: it asserts in a debug build, and in a release build the field
 * applied a typed value only when Enter was the way out of it, so a click on an action granted the
 * value from before the edit.
 */
[[nodiscard]] bool left_with_enter() noexcept {
    return ImGui::IsItemDeactivated()
           && (ImGui::IsKeyPressed(ImGuiKey_Enter, false)
               || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
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
    ImGui::TextDisabled("%s",
                        owned == nullptr     ? holding_of(state.selection.definitionHash)
                        : placement.equipped ? kEquippedLabel
                        : owned->postmaster  ? kPostmasterLabel
                                             : kInInventoryLabel);
    if (placement.equipped) {
        ImGui::SameLine();
        if (ImGui::SmallButton(kUnequipLabel)) {
            record_edit(edit::unequip(
                *state.draft, state.catalog, state.character, placement.slot, state.status));
        }
    } else if (owned != nullptr && owned->postmaster) {
        // A postmaster item cannot be equipped until it is pulled into its own bucket.
        ImGui::SameLine();
        if (ImGui::SmallButton(kPullLabel)) {
            record_edit(edit::pull_from_postmaster(
                *state.draft, state.catalog, state.character, owned->instanceSoid, state.status));
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
        (void)ImGui::InputInt("##power", &state.grant.power, 0, 0);
        add |= left_with_enter();
        state.grant.power = std::clamp(state.grant.power, 0, kPowerSliderMaximum);
    }
    if (stacked) {
        controls::field_label("Quantity", column);
        ImGui::SetNextItemWidth(pixels(kGrantFieldWidth));
        (void)ImGui::InputInt("##quantity", &state.grant.quantity, 0, 0);
        add |= left_with_enter();
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
    add |= controls::primary_button("Add to Inventory", {half, pixels(controls::kActionHeight)});
    if (equippable) {
        ImGui::SameLine();
        equip = ImGui::Button("Add and Equip", {half, pixels(controls::kActionHeight)});
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
    if (ImGui::Button("Remove from Inventory", {-FLT_MIN, pixels(controls::kActionHeight)})) {
        request_removal(model().selection.instanceSoid);
    }
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

/**
 * Draws the row that sends a stowed item to another character, with a button for each. A
 * character whose class cannot hold the item keeps its button, disabled, so the row says why.
 * @return True when the item was sent, which takes it off this character: the caller must not
 * read the item again.
 */
[[nodiscard]] bool draw_send_row(const edit::CatalogItem& definition, const edit::Item& item) noexcept {
    const Model& state = model();
    const state::AccountState& account = state.draft->after;
    if (account.characterCount < 2) {
        return false;
    }
    controls::space(controls::kSectionSpacing);
    controls::field_label(kSendLabel, ImGui::CalcTextSize(kSendLabel).x);
    bool first = true;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (index == state.character) {
            continue;
        }
        if (!first) {
            ImGui::SameLine();
        }
        first = false;
        const state::CharacterState& other = account.characters[index];
        const bool fits = edit::fits_class(definition, other.characterClass);
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginDisabled(!fits);
        const bool pressed = ImGui::SmallButton(character_label(index).c_str());
        ImGui::EndDisabled();
        if (!fits && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("This item belongs to another class.");
        }
        ImGui::PopID();
        if (pressed && send_item(item.instanceSoid, index)) {
            return true;
        }
    }
    return false;
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
    const bool stowed = !placement_of(item.instanceSoid).equipped;
    // A postmaster item is pulled before it goes anywhere, as in game.
    if (stowed && !item.postmaster && draw_send_row(definition, item)) {
        return;
    }
    if (stowed && (item.flags & inv::kLockedItemFlag) == 0) {
        controls::space(controls::kSectionSpacing);
        draw_remove_action();
    }
}

} // namespace

void request_removal(std::uint64_t instance) noexcept {
    Model& state = model();
    state.removal = instance;
    // The confirmation is opened from the page, as the perk picker is, so the card or the pane
    // that asked can scroll away or close without taking the question with it.
    state.removalRequested = true;
}

void draw_removal_confirm() noexcept {
    Model& state = model();
    if (state.removalRequested) {
        state.removalRequested = false;
        ImGui::OpenPopup(kRemoveItemTitle);
    }
    if (!ImGui::BeginPopupModal(kRemoveItemTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const edit::Item* item = find_owned_item(state.removal);
    if (item == nullptr) {
        // The item went while the question was open, to another edit or the account reloading.
        state.removal = 0;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const edit::CatalogItem* definition = state.catalog.find(item->definitionHash);
    ImGui::TextUnformatted(definition != nullptr ? definition->name.c_str() : "This Item");
    ImGui::TextDisabled("This removes it from the character's inventory.");
    controls::space(controls::kRowSpacing);
    if (controls::primary_button("Remove", {pixels(kConfirmButtonWidth), 0.0F})) {
        (void)erase_owned_item(state.removal);
        state.removal = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {pixels(kConfirmButtonWidth), 0.0F})
        || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.removal = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_inspector() noexcept {
    Model& state = model();
    const edit::CatalogItem* definition = state.catalog.find(state.selection.definitionHash);
    if (definition == nullptr) {
        ImGui::TextDisabled("No Item Selected");
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
