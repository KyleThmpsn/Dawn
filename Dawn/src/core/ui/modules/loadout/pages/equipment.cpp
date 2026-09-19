// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../controls.h"
#include "../card.h"
#include "../internal.h"
#include "state/account/inventory/placement.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** 190 authored pixels put the second randomizer column clear of the first label. */
constexpr float kRandomizerColumn = 190.0F;
/** The power field under the slot list, and the action that rolls. */
constexpr float kRandomizerLabelWidth = 110.0F;
constexpr float kRandomizerFieldWidth = 90.0F;
constexpr float kRandomizerButtonWidth = 120.0F;
/** Title of the randomizer, used both for the action and for its modal. */
constexpr const char* kRandomizeTitle = "Randomize loadout";
/** The heading row's two actions, as words: no ellipsis, no arrow. */
constexpr const char* kRandomizeLabel = "Randomize";
constexpr const char* kInventoryLabel = "Inventory";

/** Draws the randomizer modal and runs one roll when it is confirmed. */
void draw_randomizer_modal() noexcept {
    if (!ImGui::BeginPopupModal(kRandomizeTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Model& state = model();
    for (std::size_t slot = 0; slot < state.randomizer.slots.size(); ++slot) {
        bool enabled = state.randomizer.slots[slot];
        ImGui::PushID(static_cast<int>(slot));
        if (ImGui::Checkbox(edit::kSlots[slot], &enabled)) {
            state.randomizer.slots[slot] = enabled;
        }
        ImGui::PopID();
        if (slot % 2 == 0) {
            ImGui::SameLine(pixels(kRandomizerColumn));
        }
    }
    // A drag field, not a slider track: the same control the Level field on the page uses.
    controls::space(controls::kRowSpacing);
    controls::field_label("New item power", pixels(kRandomizerLabelWidth));
    ImGui::SetNextItemWidth(pixels(kRandomizerFieldWidth));
    ImGui::DragInt("##random_power", &state.grant.power, 10.0F, 0, kPowerSliderMaximum, "%d",
                   ImGuiSliderFlags_AlwaysClamp);
    controls::space(controls::kSectionSpacing);
    if (controls::primary_button("Roll loadout", {pixels(kRandomizerButtonWidth), 0.0F})) {
        record_edit(edit::randomize(*state.draft,
                                    state.catalog,
                                    state.character,
                                    state.randomizer.slots,
                                    level_of(state.grant.power),
                                    state.randomizer.engine,
                                    state.status));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/**
 * Draws the heading row: the title in the game's spaced capitals, and the two actions set as
 * plain words at the far end of the row.
 */
void draw_heading_row() noexcept {
    Model& state = model();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float randomizeWidth =
        ImGui::CalcTextSize(kRandomizeLabel).x + (style.FramePadding.x * 2.0F);
    const float inventoryWidth =
        ImGui::CalcTextSize(kInventoryLabel).x + (style.FramePadding.x * 2.0F);
    const float actions = randomizeWidth + inventoryWidth + style.ItemSpacing.x;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    controls::heading("Equipped Loadout");
    ImGui::SameLine(right - actions);
    if (ImGui::SmallButton(kRandomizeLabel)) {
        ImGui::OpenPopup(kRandomizeTitle);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(kInventoryLabel)) {
        state.view = View::characterInventory;
    }
    draw_randomizer_modal();
}

} // namespace

void draw_equipment() noexcept {
    controls::space(controls::kSectionSpacing);
    draw_heading_row();
    // The heading already divides the page, so one rule under the totals is enough to close the
    // band off from the card grid. Two rules around a single line of text was a boxed row.
    controls::space(controls::kRuleSpacing);
    draw_armor_totals();
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();
    controls::space(controls::kRuleSpacing);

    auto& slots = character().equipment.slots;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int columns = 1;
    float width = 0.0F;
    card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kCardMinimumWidth), columns, width);
    // The subclass is skipped, so the drawn slots are gathered first: both the column a card lands
    // in and the height its row takes depend on how many cards precede it, not on the slot index.
    std::vector<std::size_t> drawn;
    drawn.reserve(slots.size());
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        if (slot != kSubclassSlot) {
            drawn.push_back(slot);
        }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, pixels(kCardColumnSpacing)});
    const auto perRow = static_cast<std::size_t>(columns);
    float rowHeight = 0.0F;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        if (i % perRow == 0) {
            // Every card in a row takes the row's tallest natural height, so the row stays level
            // while a row of cards with no sockets still gives its unused height back.
            rowHeight = 0.0F;
            for (std::size_t j = i; j < drawn.size() && j < i + perRow; ++j) {
                const auto& sibling = slots[drawn[j]];
                rowHeight = (std::max)(
                    rowHeight, card::natural_height(sibling ? &*sibling : nullptr, width));
            }
        } else {
            ImGui::SameLine(0.0F, spacing);
        }
        auto& equipped = slots[drawn[i]];
        card::draw(equipped ? &*equipped : nullptr,
                   edit::kSlots[drawn[i]],
                   drawn[i],
                   equipped ? card::Action::swap : card::Action::fill,
                   width,
                   rowHeight);
    }
    ImGui::PopStyleVar();
}

} // namespace dawn::core::ui::modules::loadout::internal
