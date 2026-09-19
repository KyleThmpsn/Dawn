// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <map>
#include <string>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../controls.h"
#include "../internal.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/** Scope rows, in the order of the PlugScope values. */
constexpr const char* kScopeLabels[]{
    "Compatible", "Socket + gear type", "Socket type", "Gear type", "All"};
/** Warning shown for every scope past the compatible one. */
constexpr const char* kExpandedScopeWarning = "May include perks this item does not support.";
/** Stronger warning for the unrestricted scope. */
constexpr const char* kUnrestrictedScopeWarning =
    "Every discovered perk; some combinations stop it loading.";

constexpr float kPopupWidth = 520.0F;
constexpr float kPopupHeight = 560.0F;
constexpr float kCancelButtonWidth = 100.0F;
/** Divider between two rows of the picker list, as the tooltip rules its own perk rows. */
constexpr ImVec4 kListRule{1.0F, 1.0F, 1.0F, 0.10F};
/** Height the results list leaves for the row of actions under it. */
constexpr float kActionRowLines = 1.6F;
/** The sheet's own frame: the tooltip's ground, a hairline, and the dim it lays on the page. */
constexpr float kPickerPadding = 12.0F;
constexpr float kPickerHairline = 1.0F;
constexpr ImVec4 kPickerGround{0.09F, 0.09F, 0.10F, 0.98F};
constexpr ImVec4 kPickerDim{0.0F, 0.0F, 0.0F, 0.55F};
/** The item's name heads the sheet in its title cut, a little under the tooltip's band size. */
constexpr float kPickerTitleScale = 1.3F;
constexpr float kPickerTitleWeight = 1.0F;
/** Width of the scope selector at the end of the filter row. */
constexpr float kScopeWidth = 190.0F;
/** The second filter row: type, rarity and order selectors, then the internal-plug toggle. */
constexpr float kTypeFilterWidth = 180.0F;
constexpr float kRarityFilterWidth = 110.0F;
constexpr float kSortWidth = 110.0F;
constexpr const char* kSortLabels[]{"By type", "Name A-Z", "Rarity"};
static_assert(std::size(kSortLabels) == static_cast<std::size_t>(Sort::count),
              "Every sort order needs a label.");
constexpr const char* kAnyRarityLabel = "All rarities";
constexpr const char* kDummyItemsLabel = "Dummy items";
/** 64 bytes hold a plug type with its count. */
constexpr std::size_t kTypePreviewCapacity = 64;

/** Title of the picker, used by both the open call and the modal. */
constexpr const char* kPickerTitle = "Choose a perk";

/** Corner radius of the fill a row shows under the pointer. */
constexpr float kRowHoverRounding = 3.0F;

/** The plugs the picker offers after its filters, and what each type would have shown. */
struct Matches {
    std::vector<const edit::CatalogItem*> options;
    /** Plugs each type would show, counted before the type filter so the type picker can say. */
    std::map<std::string, std::size_t> types;
    std::size_t total{};
};

/** @return Every offered plug passing the picker's filters, in its chosen order. */
[[nodiscard]] Matches matching_plugs() noexcept {
    Model& state = model();
    const SocketPicker& picker = state.picker;
    const std::string query = edit::searchable(picker.search);
    // A plug the native registry treats as inert is offered only under the unrestricted scope,
    // where everything discovered is on the table.
    const bool everything = picker.scope == edit::PlugScope::all;
    Matches matches;
    for (const std::uint16_t id : picker.options) {
        const edit::CatalogItem* plug = state.catalog.index(id);
        if (plug == nullptr || (!everything && plug->inert)
            || (!picker.includeInternal && plug->internal)
            || (picker.rarity != 0 && plug->definition.tier != picker.rarity)
            || !edit::matches(*plug, query)) {
            continue;
        }
        ++matches.total;
        if (!plug->type.empty()) {
            ++matches.types[plug->type];
        }
        if (picker.type.empty() || plug->type == picker.type) {
            matches.options.push_back(plug);
        }
    }
    const Sort sort = picker.sort;
    std::sort(matches.options.begin(),
              matches.options.end(),
              [sort](const edit::CatalogItem* a, const edit::CatalogItem* b) {
                  if (sort == Sort::type && a->type != b->type) {
                      if (a->type.empty() != b->type.empty()) {
                          return !a->type.empty();
                      }
                      return a->type < b->type;
                  }
                  if (sort == Sort::rarity && a->definition.tier != b->definition.tier) {
                      return a->definition.tier > b->definition.tier;
                  }
                  return a->name < b->name;
              });
    return matches;
}

/** Draws the second filter row: plug type, rarity, order, and the internal-plug toggle. */
void draw_filter_row(const Matches& matches) noexcept {
    SocketPicker& picker = model().picker;
    char preview[kTypePreviewCapacity]{};
    if (picker.type.empty()) {
        (void)std::snprintf(preview, sizeof preview, "All types (%zu)", matches.total);
    } else {
        const auto found = matches.types.find(picker.type);
        (void)std::snprintf(preview, sizeof preview, "%s (%zu)", picker.type.c_str(),
                            found != matches.types.end() ? found->second : 0U);
    }
    if (controls::begin_picker("##perk_type", preview, pixels(kTypeFilterWidth))) {
        char row[kTypePreviewCapacity]{};
        (void)std::snprintf(row, sizeof row, "All types (%zu)", matches.total);
        if (controls::picker_row(row, picker.type.empty())) {
            picker.type.clear();
        }
        for (const auto& [type, count] : matches.types) {
            (void)std::snprintf(row, sizeof row, "%s (%zu)", type.c_str(), count);
            if (controls::picker_row(row, picker.type == type)) {
                picker.type = type;
            }
        }
        controls::end_picker();
    }
    ImGui::SameLine();
    if (controls::begin_picker("##perk_rarity",
                               picker.rarity == 0
                                   ? kAnyRarityLabel
                                   : art::tier_name(static_cast<std::uint8_t>(picker.rarity)),
                               pixels(kRarityFilterWidth))) {
        if (controls::picker_row(kAnyRarityLabel, picker.rarity == 0)) {
            picker.rarity = 0;
        }
        for (int tier = 1; tier <= static_cast<int>(art::kExoticTier); ++tier) {
            if (controls::picker_row(art::tier_name(static_cast<std::uint8_t>(tier)),
                                     picker.rarity == tier)) {
                picker.rarity = tier;
            }
        }
        controls::end_picker();
    }
    ImGui::SameLine();
    int sort = static_cast<int>(picker.sort);
    if (controls::picker("##perk_sort", sort, kSortLabels, static_cast<int>(std::size(kSortLabels)),
                         pixels(kSortWidth))) {
        picker.sort = static_cast<Sort>(sort);
    }
    ImGui::SameLine();
    (void)ImGui::Checkbox(kDummyItemsLabel, &picker.includeInternal);
}

/** Draws the scope picker, at the width the caller set, and the warning the chosen scope earns. */
void draw_scope_row(const edit::CatalogItem& definition) noexcept {
    Model& state = model();
    int scope = static_cast<int>(state.picker.scope);
    if (controls::picker("##scope",
                         scope,
                         kScopeLabels,
                         static_cast<int>(std::size(kScopeLabels)),
                         pixels(kScopeWidth))) {
        state.picker.scope = static_cast<edit::PlugScope>(scope);
        state.picker.options =
            state.catalog.candidates(definition, state.picker.lane, state.picker.scope);
    }
    if (state.picker.scope == edit::PlugScope::compatible) {
        return;
    }
    ImGui::TextColored(tooltip::pending(),
                       "%s",
                       state.picker.scope == edit::PlugScope::all ? kUnrestrictedScopeWarning
                                                                  : kExpandedScopeWarning);
}

/**
 * Draws one perk row and applies it when it is picked.
 * @param plug Offered plug.
 * @param item Owned item the plug would go into.
 * @return True when the plug was applied and the picker should close.
 */
[[nodiscard]] bool draw_perk_row(const edit::CatalogItem& plug,
                                 edit::Item& item,
                                 float width,
                                 float rowHeight) noexcept {
    Model& state = model();
    ImGui::PushID(static_cast<int>(plug.definition.definitionIndex));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const auto& current = item.sockets.plugs[state.picker.lane];
    const bool chosen = current && *current == plug.definition.definitionHash;
    // The row is the tooltip's own perk row with the plug's type under its name, and the plug's
    // own tooltip under the pointer: the same plug reads the same in the list as it does fitted.
    const bool clicked = ImGui::InvisibleButton("perk", {width, rowHeight});
    const bool hovered = ImGui::IsItemHovered();
    if (chosen || hovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            origin,
            {origin.x + width, origin.y + rowHeight},
            ImGui::GetColorU32(chosen ? ImGuiCol_Header : ImGuiCol_FrameBgHovered),
            pixels(kRowHoverRounding));
    }
    ImGui::GetWindowDrawList()->AddLine({origin.x, origin.y + rowHeight},
                                        {origin.x + width, origin.y + rowHeight},
                                        ImGui::GetColorU32(kListRule));
    ImGui::SetCursorScreenPos(origin);
    tooltip::draw_perk_row(&plug, width, tooltip::PerkDetail::typed);
    if (hovered) {
        tooltip::draw_plug(plug);
    }
    bool applied = false;
    if (clicked) {
        applied = edit::set_plug(item,
                                 state.catalog,
                                 state.picker.lane,
                                 plug.definition.definitionIndex,
                                 state.picker.scope);
        if (applied) {
            state.status = "Perk applied.";
            record_edit(true);
        }
    }
    ImGui::PopID();
    return applied;
}

} // namespace

void open_perk_picker(const edit::CatalogItem& definition, std::size_t lane) noexcept {
    Model& state = model();
    state.picker.lane = lane;
    state.picker.search[0] = '\0';
    state.picker.type.clear();
    state.picker.rarity = 0;
    state.picker.options = state.catalog.candidates(definition, lane, state.picker.scope);
    // The popup is opened from the scope that submits it, which is the page window rather
    // than the card's child: a card that scrolls away or is culled must not take the modal with it.
    state.picker.requested = true;
}

void draw_perk_editor(const edit::CatalogItem& definition,
                      const edit::Item& item,
                      float width) noexcept {
    const edit::Catalog& catalog = model().catalog;
    // The instance carries the lanes it rolled; the definition's own defaults fill the rest.
    edit::Item resolved = item;
    if (!edit::materialize(resolved, catalog)) {
        return;
    }
    auto* draw = ImGui::GetWindowDrawList();
    bool first = true;
    for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) {
        const auto& current = resolved.sockets.plugs[lane];
        const edit::CatalogItem* fitted = current ? catalog.find(*current) : nullptr;
        // The bank names nothing for armor's rolled stat plugs; those lanes are what the stat
        // bars above edit, so they are not listed as sockets the player would pick a perk for.
        if (fitted != nullptr && fitted->unnamed) {
            continue;
        }
        // Each socket is the tooltip's own perk row, as it is on the tooltip, with the plug's own
        // tooltip under the pointer, and the whole row is the control that opens the picker for
        // that lane. The hit target goes in first, so the hover fill sits under the row.
        const float rowHeight = tooltip::perk_row_height(fitted, width, tooltip::PerkDetail::name);
        ImGui::PushID(static_cast<int>(lane));
        if (!first) {
            tooltip::draw_rule();
        }
        first = false;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("socket", {width, rowHeight});
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            draw->AddRectFilled(origin,
                                {origin.x + width, origin.y + rowHeight},
                                ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
                                pixels(kRowHoverRounding));
        }
        ImGui::SetCursorScreenPos(origin);
        tooltip::draw_perk_row(fitted, width, tooltip::PerkDetail::name);
        if (hovered && fitted != nullptr) {
            tooltip::draw_plug(*fitted);
        }
        ImGui::PopID();
        if (clicked) {
            select(definition, item.instanceSoid);
            open_perk_picker(definition, lane);
        }
    }
}

void draw_perk_picker() noexcept {
    Model& state = model();
    if (state.picker.requested) {
        state.picker.requested = false;
        ImGui::OpenPopup(kPickerTitle);
    }
    // The picker is a sheet on the tooltip's own ground: square, hairlined, and dimming the page
    // behind it, as the game's socket picker lays over the inventory.
    const float padding = pixels(kPickerPadding);
    // The sheet opens centred and can then be dragged anywhere by its head.
    ImGui::SetNextWindowSize({pixels(kPopupWidth), pixels(kPopupHeight)}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{padding, padding});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, pixels(kPickerHairline));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPickerGround);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, kPickerDim);
    const bool open = ImGui::BeginPopupModal(
        kPickerTitle, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!open) {
        return;
    }
    edit::Item* item = selected_item();
    const edit::CatalogItem* definition =
        item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
    if (definition == nullptr) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    // The head names the item in its title cut and, under it, the socket and what is in it now.
    const auto& current = item->sockets.plugs[state.picker.lane];
    const edit::CatalogItem* fitted = current ? state.catalog.find(*current) : nullptr;
    const float titleSize = ImGui::GetStyle().FontSizeBase * kPickerTitleScale;
    const float titleWeight = art::push_title(titleSize, pixels(kPickerTitleWeight));
    const ImVec2 titleAt = ImGui::GetCursorScreenPos();
    art::clipped_text(art::shout(definition->name),
                      titleAt,
                      ImGui::GetContentRegionAvail().x,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      titleWeight);
    ImGui::Dummy({0.0F, ImGui::GetTextLineHeight()});
    ImGui::PopFont();
    ImGui::TextColored(tooltip::muted(),
                       "Socket %zu  /  %s",
                       state.picker.lane + 1,
                       fitted != nullptr ? fitted->name.c_str() : "Empty");
    controls::space(controls::kSectionSpacing);

    // Search leads the filter row; the scope sits at its end, with its warning under both.
    const float scopeWidth = pixels(kScopeWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - scopeWidth
                            - ImGui::GetStyle().ItemSpacing.x);
    (void)controls::search(
        "##perk_search", "Search perks...", state.picker.search, sizeof state.picker.search);
    ImGui::SameLine();
    draw_scope_row(*definition);

    controls::space(controls::kRowSpacing);
    const Matches matches = matching_plugs();
    draw_filter_row(matches);
    const std::vector<const edit::CatalogItem*>& options = matches.options;
    controls::space(controls::kRowSpacing);
    char count[32]{};
    (void)std::snprintf(count, sizeof count, "%zu perks", options.size());
    ImGui::TextColored(tooltip::muted(), "%s", art::shout(count).c_str());
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();
    bool applied = false;
    // A typed row is one height whatever plug it holds, so the clipper can pitch off it.
    const float rowHeight = tooltip::perk_row_height(nullptr, 0.0F, tooltip::PerkDetail::typed);
    const float pitch = rowHeight + ImGui::GetStyle().ItemSpacing.y;
    const float listHeight =
        (std::max)(rowHeight,
                   ImGui::GetContentRegionAvail().y
                       - (ImGui::GetFrameHeightWithSpacing() * kActionRowLines));
    if (ImGui::BeginChild("perk_results", {0.0F, listHeight})) {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(options.size()), pitch);
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                applied |= draw_perk_row(*options[static_cast<std::size_t>(i)], *item, width, rowHeight);
            }
        }
        if (options.empty()) {
            ImGui::TextColored(tooltip::muted(), "No perks match.");
        }
    }
    ImGui::EndChild();
    controls::space(controls::kRowSpacing);
    // The one action sits at the far end, as the game ends its sheets.
    const float cancelWidth = pixels(kCancelButtonWidth);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cancelWidth);
    if (applied || ImGui::Button("Cancel", {cancelWidth, 0.0F})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace dawn::core::ui::modules::loadout::internal
