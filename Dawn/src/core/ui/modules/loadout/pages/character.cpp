// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <imgui.h>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../controls.h"
#include "../preview.h"
#include "../internal.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/**
 * Field groups sit side by side, as Sundial's character page lays them out.
 * Its widths, label columns and gaps are kept.
 */
/** Sundial: identity, subclass and abilities, at these widths with these label columns. */
constexpr float kGroupWidths[]{196.0F, 268.0F, 300.0F};
constexpr float kGroupLabelWidths[]{58.0F, 66.0F, 88.0F};
/** Gap between two field groups sharing a row. The gap inside a field is controls' own. */
constexpr float kGroupColumnGap = 18.0F;
/** A field control is one line of text plus this, which is as short as a combo reads. */
constexpr float kFieldFramePaddingY = 1.0F;
/** Vertical gap between two stacked fields in a group. */
constexpr float kFieldRowGap = 2.0F;
/** Sundial clamps a selector between these however wide its group is. */
constexpr float kSelectorMinimumWidth = 110.0F;
constexpr float kSelectorMaximumWidth = 190.0F;
/** Armor totals row: Sundial draws 15px icons, 4px between parts and 10px between stats. */
constexpr float kStatIconExtent = 15.0F;
constexpr float kStatItemSpacing = 4.0F;
constexpr float kStatGap = 10.0F;
/** Armor occupies slots 3 through 7, which are the slots that carry stats. */
constexpr std::size_t kFirstArmorSlot = 3;
/** Highest level the game awards, which is where the level field clamps. */
constexpr int kMaximumCharacterLevel = 50;
/** Null-separated combo items, which is the form Dear ImGui takes them in. */
constexpr const char* kClassItems = "Titan\0Hunter\0Warlock\0";
constexpr const char* kRaceItems = "Human\0Awoken\0Exo\0";
constexpr const char* kGenderItems = "Male\0Female\0";
/** Shown on the class field, which cannot apply until the gear matches it. */
constexpr const char* kClassNote =
    "Changing class needs matching armor and a subclass before it can apply.";

/** @return The control width one group gives its fields. */
[[nodiscard]] float selector_width(float groupWidth, float labelWidth) noexcept {
    // The label column and this gap are exactly what `controls::field_label` advances by, so the
    // control takes what is left and the group ends where its authored width says it does.
    return std::clamp(groupWidth - labelWidth - pixels(controls::kFieldColumnGap),
                      pixels(kSelectorMinimumWidth),
                      pixels(kSelectorMaximumWidth));
}

/**
 * Draws one enum picker as a labelled field.
 * @param publish False when the change needs follow-up edits before it can be applied.
 */
template <typename Enum>
void enum_field(const char* label,
                const char* items,
                Enum& value,
                float labelWidth,
                float controlWidth,
                bool publish,
                const char* note = nullptr) noexcept {
    ImGui::PushID(label);
    controls::field_label(label, labelWidth);
    int index = static_cast<int>(value);
    if (controls::picker("##field", index, items, controlWidth)) {
        value = static_cast<Enum>(index);
        mark_changed(publish);
    }
    if (note != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", note);
    }
    ImGui::PopID();
}

/** Draws the level field, which sits under the identity pickers. */
void draw_level_fields(state::CharacterState& value, float label, float control) noexcept {
    controls::field_label("Level", label);
    ImGui::SetNextItemWidth(control);
    int level = value.level;
    // A drag field, not a slider: Sundial edits this with a DragValue, and a slider track beside
    // three combos is the one control on the page that reads as a settings screen. AlwaysClamp
    // keeps a typed value inside the range the uint8_t narrowing below relies on.
    const bool changed = ImGui::DragInt("##level",
                                        &level,
                                        1.0F,
                                        1,
                                        kMaximumCharacterLevel,
                                        "%d",
                                        ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
        value.level = static_cast<std::uint8_t>(level);
    }
    record_scalar_edit(changed);
}

/** Draws the class, race, gender and level group. */
void draw_identity_group(state::CharacterState& value, float groupWidth, float label) noexcept {
    const float control = selector_width(groupWidth, label);
    ImGui::BeginGroup();
    // Class alone leaves the armor and subclass mismatched, so it stays in the draft until the
    // player has re-equipped; applying it on its own would only report the mismatch back.
    enum_field("Class", kClassItems, value.characterClass, label, control, false, kClassNote);
    enum_field("Race", kRaceItems, value.race, label, control, true);
    enum_field("Gender", kGenderItems, value.gender, label, control, true);
    draw_level_fields(value, label, control);
    ImGui::EndGroup();
}


/**
 * @return The armor stats as the game displays them.
 * Stored values are summed first and then put through the equipped armor's stat group curve,
 * which is the order the game shows: the curve maps a total investment value to a shown value.
 */
[[nodiscard]] edit::Stats armor_totals(const state::CharacterState& value) noexcept {
    const edit::Catalog& catalog = model().catalog;
    edit::Stats totals{};
    std::uint16_t group = edit::kNoStatGroup;
    for (std::size_t slot = kFirstArmorSlot; slot <= kLastArmorSlot; ++slot) {
        if (!value.equipment.slots[slot]) {
            continue;
        }
        const edit::Stats stats = edit::item_stats(*value.equipment.slots[slot], catalog);
        for (std::size_t i = 0; i < totals.size(); ++i) {
            totals[i] += stats[i];
        }
        // Equipped armor shares one stat group; the first piece that names one speaks for the set.
        if (group == edit::kNoStatGroup) {
            if (const auto* definition = catalog.find(value.equipment.slots[slot]->definitionHash)) {
                group = definition->statGroupIndex;
            }
        }
    }
    if (group == edit::kNoStatGroup) {
        return totals;
    }
    for (std::size_t i = 0; i < totals.size(); ++i) {
        totals[i] = edit::display_stat(catalog, group, catalog.statRows[i], totals[i]);
    }
    return totals;
}

/**
 * Draws the armor totals as one wrapped row, the way Sundial's equipped stat row reads.
 * Six labelled bars filled a column and most of a page; an icon, a name and a value per stat fit
 * on a single line with room to spare.
 */
void draw_armor_totals_row(const state::CharacterState& value) noexcept {
    const edit::Stats totals = armor_totals(value);
    const Model& state = model();
    const float icon = pixels(kStatIconExtent);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2{pixels(kStatItemSpacing), ImGui::GetStyle().ItemSpacing.y});
    for (std::size_t shown = 0; shown < totals.size(); ++shown) {
        const std::size_t i = state.catalog.statOrder[shown];
        if (shown != 0) {
            ImGui::SameLine(0.0F, pixels(kStatGap));
        }
        const std::uint32_t tag = state.catalog.statIconTags[i];
        if (tag != 0) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float lift = (ImGui::GetTextLineHeight() - icon) * 0.5F;
            if (preview::draw(tag, {at.x, at.y + lift}, icon)) {
                ImGui::Dummy({icon, ImGui::GetTextLineHeight()});
                ImGui::SameLine(0.0F, pixels(kStatItemSpacing));
            }
        }
        const std::string_view label = edit::stat_label(state.catalog, i);
        ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
        ImGui::SameLine(0.0F, pixels(kStatItemSpacing));
        ImGui::Text("%d", totals[i]);
    }
    ImGui::PopStyleVar();
}

} // namespace

void draw_armor_totals() noexcept {
    draw_armor_totals_row(character());
}

void draw_character_fields() noexcept {
    state::CharacterState& value = character();
    const float available = ImGui::GetContentRegionAvail().x;
    const float gap = pixels(kGroupColumnGap);
    // The field rows run shorter than the rest of the page; a combo here is a label, not a target.
    // The fields stack with only a sliver between them, so a group reads as one block.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2{ImGui::GetStyle().FramePadding.x, pixels(kFieldFramePaddingY)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2{ImGui::GetStyle().ItemSpacing.x, pixels(kFieldRowGap)});

    // Groups flow onto the row while they fit and wrap when they do not, so a page sharing its
    // width with the workspace still gets two columns instead of dropping to one tall stack.
    float used = 0.0F;
    for (std::size_t group = 0; group < std::size(kGroupWidths); ++group) {
        const float width = (std::min)(pixels(kGroupWidths[group]), available);
        const float label = pixels(kGroupLabelWidths[group]);
        const bool first = used == 0.0F;
        if (!first && used + gap + width <= available) {
            ImGui::SameLine(0.0F, gap);
            used += gap + width;
        } else {
            if (!first) {
                controls::space(controls::kRowSpacing);
            }
            used = width;
        }
        ImGui::PushID(static_cast<int>(group));
        switch (group) {
        case 0:
            draw_identity_group(value, width, label);
            break;
        case 1:
            ImGui::BeginGroup();
            draw_subclass_group(label, selector_width(width, label));
            ImGui::EndGroup();
            break;
        default:
            ImGui::BeginGroup();
            draw_ability_group(label, selector_width(width, label));
            ImGui::EndGroup();
            break;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
}

} // namespace dawn::core::ui::modules::loadout::internal
