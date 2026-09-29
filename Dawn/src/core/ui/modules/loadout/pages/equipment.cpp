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
/** The power field under the slot list: its label column and its own width. */
constexpr float kRandomizerLabelWidth = 110.0F;
constexpr float kRandomizerFieldWidth = 90.0F;
/** Title of the randomizer, used both for the action and for its modal. */
constexpr const char* kRandomizeTitle = "Randomize Loadout";
/**
 * The heading row's actions, as words: no ellipsis, no arrow. Randomize is also the action the
 * randomizer's confirmation ends on, so the word that opened it is the word that rolls.
 */
constexpr const char* kRandomizeLabel = "Randomize";
constexpr const char* kInventoryLabel = "Inventory";
constexpr const char* kLoadoutsLabel = "Loadouts";
constexpr const char* kOptimizeLabel = "Optimize Armor";

/** Draws the randomizer modal and runs one roll when it is confirmed, by button or by Enter. */
void draw_randomizer_modal() noexcept {
    if (!ImGui::BeginPopupModal(kRandomizeTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    Model& state = model();
    for (std::size_t slot = 0; slot < state.randomizer.slots.size(); ++slot) {
        bool enabled = state.randomizer.slots[slot];
        ImGui::PushID(static_cast<int>(slot));
        if (controls::checkbox(edit::kSlots[slot], &enabled)) {
            state.randomizer.slots[slot] = enabled;
        }
        ImGui::PopID();
        if (slot % 2 == 0) {
            ImGui::SameLine(pixels(kRandomizerColumn));
        }
    }
    // A drag field, not a slider track: the same control the Level field on the page uses.
    controls::space(controls::kRowSpacing);
    controls::field_label("New Item Power", pixels(kRandomizerLabelWidth));
    ImGui::SetNextItemWidth(pixels(kRandomizerFieldWidth));
    ImGui::DragInt("##random_power", &state.grant.power, 10.0F, 0, kPowerSliderMaximum, "%d",
                   ImGuiSliderFlags_AlwaysClamp);
    controls::space(controls::kSectionSpacing);
    const controls::Answer answer = controls::confirm_footer(kRandomizeLabel);
    if (answer == controls::Answer::confirm) {
        record_edit(edit::randomize(*state.draft,
                                    state.catalog,
                                    state.character,
                                    state.randomizer.slots,
                                    level_of(state.grant.power),
                                    state.randomizer.engine,
                                    state.status));
    }
    if (answer != controls::Answer::none) {
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
    const float loadoutsWidth =
        ImGui::CalcTextSize(kLoadoutsLabel).x + (style.FramePadding.x * 2.0F);
    const float optimizeWidth =
        ImGui::CalcTextSize(kOptimizeLabel).x + (style.FramePadding.x * 2.0F);
    const float randomizeWidth =
        ImGui::CalcTextSize(kRandomizeLabel).x + (style.FramePadding.x * 2.0F);
    const float inventoryWidth =
        ImGui::CalcTextSize(kInventoryLabel).x + (style.FramePadding.x * 2.0F);
    const float actions = loadoutsWidth + optimizeWidth + randomizeWidth + inventoryWidth
                          + (style.ItemSpacing.x * 3.0F);
    // The actions end where the side workspace begins rather than at the page edge: the workspace
    // lies over the page, and set at the edge they sat under it whenever an item was open.
    const float right =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - overlay_width();
    // The heading sets its capitals at the top of the line, where a small button sets its words, so
    // the row is not aligned to frame padding: that would drop the buttons below the heading.
    controls::heading("Equipped Loadout");
    ImGui::SameLine(right - actions);
    if (ImGui::SmallButton(kLoadoutsLabel)) {
        open_loadouts();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(kOptimizeLabel)) {
        open_optimizer();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(kRandomizeLabel)) {
        ImGui::OpenPopup(kRandomizeTitle);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(kInventoryLabel)) {
        state.view = View::characterInventory;
    }
    draw_randomizer_modal();
    draw_loadouts_modal();
    draw_optimizer_modal();
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
