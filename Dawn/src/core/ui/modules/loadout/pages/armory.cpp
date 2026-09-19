// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <map>
#include <string>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../internal.h"
#include "../controls.h"
#include "../art.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/** Category tabs, in the order of the Category values. */
constexpr const char* kCategoryLabels[]{"Weapons", "Armor", "Cosmetics", "Perks", "Materials"};
static_assert(std::size(kCategoryLabels) == static_cast<std::size_t>(Category::count),
              "Every browsed category needs a tab label.");
/** Search hints, one per category. */
constexpr const char* kSearchHints[]{"Search weapons...",
                                     "Search armor...",
                                     "Search cosmetics...",
                                     "Search perks...",
                                     "Search materials..."};
/** Sort labels, in the order of the Sort values. */
constexpr const char* kSortLabels[]{"By type", "Name A-Z", "Rarity"};
static_assert(std::size(kSortLabels) == static_cast<std::size_t>(Sort::count),
              "Every sort order needs a label.");
/** Rarity filter row that clears the filter; the tier rows take their names from art. */
constexpr const char* kAnyRarityLabel = "All rarities";

/** 380 authored pixels of height are needed before the tabs and the grid both fit. */
constexpr float kCompactHeight = 380.0F;
/** Widest one category tab is drawn, before the row is divided evenly. */
constexpr float kCategoryTabMaximumWidth = 110.0F;
constexpr float kCategoryTabRowInset = 36.0F;
constexpr float kCompactCategoryWidth = 120.0F;
/** Widths of the filter row selectors, and the least the search keeps when they crowd it. */
constexpr float kSortWidth = 140.0F;
constexpr float kTypeFilterWidth = 200.0F;
constexpr float kRarityFilterWidth = 124.0F;
constexpr float kSearchMinimumWidth = 160.0F;
/** The two words on the row that are not selectors. */
constexpr const char* kClassOnlyLabel = "This class only";
/** Heading for a run of results whose definitions name no type. */
constexpr const char* kUntypedGroup = "Other";
/** Sundial's name for the internal, placeholder and test definitions the catalog carries. */
constexpr const char* kDummyItemsLabel = "Dummy items";
constexpr const char* kResetLabel = "Reset";
/** 64 bytes hold a type name with its result count. */
constexpr std::size_t kTypePreviewCapacity = 64;

/** @return True when one catalog item belongs to the browsed category. */
[[nodiscard]] bool in_category(const edit::CatalogItem& item, Category category) noexcept {
    switch (category) {
    case Category::weapons:
        return item.kind == edit::GearKind::weapon && !item.plug;
    case Category::armor:
        return item.kind == edit::GearKind::armor && !item.plug;
    case Category::cosmetics:
        return item.kind == edit::GearKind::cosmetic;
    case Category::perks:
        return item.plug;
    case Category::materials:
        return item.kind == edit::GearKind::other && !item.plug;
    case Category::count:
        break;
    }
    return false;
}

/**
 * @return True when one item passes every armory filter except the item type.
 * The type is left out so `rebuild_results` can count how many items each type would show.
 */
[[nodiscard]] bool passes_filters(const edit::CatalogItem& item, const std::string& query) noexcept {
    const Browse& browse = model().browse;
    return in_category(item, browse.category) && (browse.includeInternal || !item.internal)
           && (browse.rarity == 0 || item.definition.tier == browse.rarity)
           && (!browse.classOnly || edit::fits_class(item, character().characterClass))
           && edit::matches(item, query);
}

/** Draws the category tabs, or the picker that replaces them when the panel is short. */
void draw_category_navigation(bool compact) noexcept {
    Browse& browse = model().browse;
    int category = static_cast<int>(browse.category);
    if (compact) {
        if (controls::picker("##collection_category",
                             category,
                             kCategoryLabels,
                             static_cast<int>(std::size(kCategoryLabels)),
                             pixels(kCompactCategoryWidth))) {
            browse.category = static_cast<Category>(category);
            browse.type.clear();
            model().results.key.clear();
        }
        ImGui::SameLine();
        return;
    }
    const float width =
        (std::min)(pixels(kCategoryTabMaximumWidth),
                   (ImGui::GetContentRegionAvail().x - pixels(kCategoryTabRowInset))
                       / std::size(kCategoryLabels));
    for (std::size_t i = 0; i < std::size(kCategoryLabels); ++i) {
        if (i != 0) {
            ImGui::SameLine();
        }
        if (controls::tab(kCategoryLabels[i], category == static_cast<int>(i), width)) {
            browse.category = static_cast<Category>(i);
            browse.type.clear();
            model().results.key.clear();
        }
    }
    controls::space(controls::kRowSpacing);
}

/**
 * Draws the one filter row: the search leads, then the type, rarity and sort selectors, the
 * class toggle, and a Reset that only appears once something is narrowing the list. One row
 * reads as one set of controls over the grid; stacked rows read as two toolbars.
 * @param types Item types present in the current results, with their counts.
 * @param total Number of results across every type.
 */
void draw_filter_row(const std::map<std::string, std::size_t>& types, std::size_t total) noexcept {
    Browse& browse = model().browse;
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool narrowed = browse.narrowing() != 0 || !browse.type.empty() || browse.search[0] != '\0';
    // Everything after the search is measured, so the search takes exactly what is left.
    const auto toggle_width = [&](const char* label) {
        return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    };
    const float resetWidth = narrowed ? ImGui::CalcTextSize(kResetLabel).x
                                            + (style.FramePadding.x * 2.0F) + style.ItemSpacing.x
                                      : 0.0F;
    const float trailing = pixels(kTypeFilterWidth) + pixels(kRarityFilterWidth)
                           + pixels(kSortWidth) + toggle_width(kClassOnlyLabel)
                           + toggle_width(kDummyItemsLabel) + resetWidth
                           + (style.ItemSpacing.x * 5.0F);
    // The side workspace lies over the page's right edge, so the row keeps to what it leaves.
    const float visible = ImGui::GetContentRegionAvail().x - overlay_width();
    ImGui::SetNextItemWidth((std::max)(pixels(kSearchMinimumWidth), visible - trailing));
    (void)controls::search("##item_search",
                           kSearchHints[static_cast<std::size_t>(browse.category)],
                           browse.search,
                           sizeof browse.search);
    ImGui::SameLine();

    char preview[kTypePreviewCapacity]{};
    if (browse.type.empty()) {
        (void)std::snprintf(preview, sizeof preview, "All types (%zu)", total);
    } else {
        const auto found = types.find(browse.type);
        (void)std::snprintf(preview,
                            sizeof preview,
                            "%s (%zu)",
                            browse.type.c_str(),
                            found != types.end() ? found->second : 0U);
    }

    if (controls::begin_picker("##type", preview, pixels(kTypeFilterWidth))) {
        char row[kTypePreviewCapacity]{};
        (void)std::snprintf(row, sizeof row, "All types (%zu)", total);
        if (controls::picker_row(row, browse.type.empty())) {
            browse.type.clear();
            model().results.key.clear();
        }
        for (const auto& [type, count] : types) {
            (void)std::snprintf(row, sizeof row, "%s (%zu)", type.c_str(), count);
            if (controls::picker_row(row, browse.type == type)) {
                browse.type = type;
                model().results.key.clear();
            }
        }
        controls::end_picker();
    }

    ImGui::SameLine();
    // The names come from art, so the filter and every band in the module agree on a tier's name.
    if (controls::begin_picker("##rarity",
                               browse.rarity == 0
                                   ? kAnyRarityLabel
                                   : art::tier_name(static_cast<std::uint8_t>(browse.rarity)),
                               pixels(kRarityFilterWidth))) {
        if (controls::picker_row(kAnyRarityLabel, browse.rarity == 0)) {
            browse.rarity = 0;
            model().results.key.clear();
        }
        for (int tier = 1; tier <= static_cast<int>(art::kExoticTier); ++tier) {
            if (controls::picker_row(art::tier_name(static_cast<std::uint8_t>(tier)),
                                     browse.rarity == tier)) {
                browse.rarity = tier;
                model().results.key.clear();
            }
        }
        controls::end_picker();
    }

    ImGui::SameLine();
    int sort = static_cast<int>(browse.sort);
    if (controls::picker("##sort",
                         sort,
                         kSortLabels,
                         static_cast<int>(std::size(kSortLabels)),
                         pixels(kSortWidth))) {
        browse.sort = static_cast<Sort>(sort);
        model().results.key.clear();
    }

    ImGui::SameLine();
    if (ImGui::Checkbox(kClassOnlyLabel, &browse.classOnly)) {
        browse.type.clear();
        model().results.key.clear();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(kDummyItemsLabel, &browse.includeInternal)) {
        model().results.key.clear();
    }
    if (!narrowed) {
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(kResetLabel)) {
        browse.rarity = 0;
        browse.classOnly = true;
        browse.includeInternal = false;
        browse.type.clear();
        browse.search[0] = '\0';
        model().results.key.clear();
    }
}

/** @return One key naming the exact filter set, so results rebuild only when it changes. */
[[nodiscard]] std::string results_key(const std::string& query) noexcept {
    const Browse& browse = model().browse;
    return query + "|" + browse.type + "|" + std::to_string(static_cast<int>(browse.category)) + ":"
           + std::to_string(browse.rarity) + ":" + std::to_string(static_cast<int>(browse.sort)) + ":"
           + std::to_string(browse.classOnly) + ":" + std::to_string(browse.includeInternal) + ":"
           + std::to_string(static_cast<unsigned>(character().characterClass));
}

/** Rebuilds and sorts the result list for the current filters. */
void rebuild_results(const std::string& query) noexcept {
    Model& state = model();
    const Sort sort = state.browse.sort;
    state.results.items.clear();
    state.results.types.clear();
    state.results.total = 0;
    for (const auto& item : state.catalog.items) {
        if (!passes_filters(item, query)) {
            continue;
        }
        // The histogram counts what each type would show, so it is taken before the type filter.
        // That is exactly why `passes_filters` leaves the type out.
        ++state.results.total;
        if (!item.type.empty()) {
            ++state.results.types[item.type];
        }
        if (state.browse.type.empty() || item.type == state.browse.type) {
            state.results.items.push_back(&item);
        }
    }
    std::sort(state.results.items.begin(),
              state.results.items.end(),
              [sort](const edit::CatalogItem* a, const edit::CatalogItem* b) {
                  if (sort == Sort::type && a->type != b->type) {
                      // Definitions that name no type go last, not first as an empty string would.
                      if (a->type.empty() != b->type.empty()) {
                          return !a->type.empty();
                      }
                      return a->type < b->type;
                  }
                  if (sort == Sort::rarity && a->definition.tier != b->definition.tier) {
                      return a->definition.tier > b->definition.tier;
                  }
                  return a->name == b->name
                             ? a->definition.definitionHash < b->definition.definitionHash
                             : a->name < b->name;
              });
}

/** Draws the line shown when nothing matches. The filter row above carries the reset. */
void draw_empty_results() noexcept {
    ImGui::TextDisabled("No matching items.");
}

/**
 * Draws one clipped block of result cards.
 * @param first Index of the first card in the block.
 * @param end One past the last card in the block.
 * @param columns Cards per row.
 * @param pitch Whole-pixel distance from one row's top to the next, which the clipper seeks by.
 * @param width Card width in framebuffer pixels.
 */
void draw_result_block(std::size_t first,
                       std::size_t end,
                       int columns,
                       float pitch,
                       float width) noexcept {
    const auto& items = model().results.items;
    const int rows = static_cast<int>((end - first + static_cast<std::size_t>(columns) - 1)
                                      / static_cast<std::size_t>(columns));
    ImGuiListClipper clip;
    clip.Begin(rows, pitch);
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            for (int column = 0; column < columns; ++column) {
                const std::size_t index =
                    first + static_cast<std::size_t>((row * columns) + column);
                if (index >= end) {
                    break;
                }
                if (column != 0) {
                    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
                }
                art::collection_card(*items[index], width);
            }
        }
    }
}

/** Draws the result grid, grouping by item type when that is the chosen order. */
void draw_results_grid(bool compact, bool rebuilt) noexcept {
    Model& state = model();
    if (!ImGui::BeginChild("catalog_grid")) {
        ImGui::EndChild();
        return;
    }
    if (rebuilt) {
        ImGui::SetScrollY(0.0F);
    }
    // Sundial's responsive card grid: columns from the narrowest card, width capped at the widest.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    // ImGui advances a row by IM_TRUNC(row + ItemSpacing.y) while the clipper seeks with an
    // untruncated item_n * ItemsHeight, so the row and the gap are put on whole pixels before the
    // clipper is told the pitch. This is the ONLY source of the pitch; a fractional row height or
    // a second source silently reintroduces the drift with no assert.
    const float gap = std::floor(pixels(kCardColumnSpacing));
    const float pitch = art::collection_card_height() + gap;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, gap});
    int columns = 1;
    float width = 0.0F;
    card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kCardMinimumWidth), columns, width);
    if (compact) {
        columns = 1;
        width = ImGui::GetContentRegionAvail().x;
    }
    const auto& items = state.results.items;
    const bool grouped = state.browse.sort == Sort::type && state.browse.type.empty();
    // Every category draws into this one child, so a heading's folded state is keyed by the
    // category as well as its label; otherwise folding "Other" under Perks folds it everywhere.
    ImGui::PushID(static_cast<int>(state.browse.category));
    for (std::size_t first = 0; first < items.size();) {
        std::size_t end = items.size();
        if (grouped) {
            end = first + 1;
            while (end < items.size() && items[end]->type == items[first]->type) {
                ++end;
            }
            if (first != 0) {
                controls::space(controls::kRowSpacing);
            }
            const std::string& type = items[first]->type;
            if (!controls::section_header(type.empty() ? kUntypedGroup : type.c_str(), end - first)) {
                first = end;
                continue;
            }
        }
        draw_result_block(first, end, columns, pitch, width);
        first = end;
    }
    if (items.empty()) {
        draw_empty_results();
    }
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

} // namespace

void draw_armory_page() noexcept {
    Model& state = model();
    const bool compact = ImGui::GetContentRegionAvail().y < pixels(kCompactHeight);
    draw_category_navigation(compact);

    // The histogram is built by the same memoised pass that builds the list. Counting it here
    // every frame walked the whole catalog through `edit::matches`, which allocates per word per
    // item, for a number that only changes when the filters do.
    const std::string query = edit::searchable(state.browse.search);
    const std::string key = results_key(query);
    const bool rebuilt = key != state.results.key;
    if (rebuilt) {
        state.results.key = key;
        rebuild_results(query);
    }

    // The selectors clear `results.key` when they change, so a click still narrows the list in
    // the same frame even though the rebuild runs above the row rather than below it.
    draw_filter_row(state.results.types, state.results.total);
    controls::space(controls::kRowSpacing);
    // The grid owns the one scrolling child. A second child around it could never scroll, and it
    // took the clipper and the scroll reset a layer further from the rows they act on.
    draw_results_grid(compact, rebuilt);
}

} // namespace dawn::core::ui::modules::loadout::internal
