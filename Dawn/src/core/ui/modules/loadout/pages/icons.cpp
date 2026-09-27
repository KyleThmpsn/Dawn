// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../controls.h"
#include "../internal.h"
#include "../preview.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/**
 * Icons drawn per page.
 * `preview::draw` will not evict a texture another tile asked for in the same frame, so a page
 * larger than the texture cache would leave its overflow blank forever. Paging at well under the
 * cache size means every tile on screen resolves.
 */
constexpr std::size_t kPageSize = 200;
/** Tile size the browser offers. */
constexpr float kMinimumTile = 24.0F;
constexpr float kMaximumTile = 96.0F;
/** Gap between two tiles, and how far an icon sits inside its tile's ground. */
constexpr float kTileGap = 6.0F;
constexpr float kTileInset = 3.0F;
/** Room under a tile for its label, and the size that label is set at. */
constexpr float kLabelHeight = 14.0F;
constexpr float kLabelScale = 0.62F;
/** The frame round the tile the viewer is showing. */
constexpr float kViewedThickness = 2.0F;
/** 40 bytes hold a row number, or a tag in hex. */
constexpr std::size_t kLabelCapacity = 40;
/** 160 bytes hold one line of the viewer's detail: a tag, a row and a package family. */
constexpr std::size_t kLineCapacity = 160;
/** Widths of the controls along the top of the page, and the gap the pager keeps. */
constexpr float kPackageFilterWidth = 240.0F;
constexpr float kFindWidth = 170.0F;
constexpr float kTileSliderWidth = 110.0F;
constexpr float kPagerGap = 10.0F;
/** Title of the icon viewer, used both to open it and to submit it. */
constexpr const char* kViewerTitle = "Icon";
/** Shown when the filter is off, which is how the browser opens. */
constexpr const char* kAllPackagesLabel = "All packages";
/** The viewer's well: its height, the margin inside it, and how far a small icon is enlarged. */
constexpr float kWellHeight = 230.0F;
constexpr float kWellMargin = 10.0F;
constexpr float kWellEnlargement = 4.0F;
/** One row of the list of what uses the icon: its height, its icon, and the gaps it sets. */
constexpr float kReferenceRowHeight = 30.0F;
constexpr float kReferenceIcon = 24.0F;
constexpr float kReferenceInset = 4.0F;
constexpr float kReferenceGap = 8.0F;
/** Share of a row's text the name may take before the detail after it gets the rest. */
constexpr float kReferenceNameShare = 0.6F;
/** Height the footer keeps under the list, in frame heights: its note line and its actions. */
constexpr float kFooterLines = 2.2F;
/** The close the viewer ends on, and the gap the footer keeps before its export. */
constexpr float kCloseWidth = 100.0F;
constexpr float kFooterGap = 16.0F;
/** Ammunition classes in `Catalog::ammoIconTags` order; index zero is never drawn. */
constexpr const char* kAmmoNames[]{"", "Primary ammunition", "Special ammunition", "Heavy ammunition"};

/** One thing that uses an icon, as the viewer lists it. */
struct Reference {
    /** The catalog item, which has a tooltip of its own; null for anything else. */
    const edit::CatalogItem* item{};
    std::string name;
    /** What the thing is, set muted after its name. */
    std::string detail;
    /** Definition hash, or zero for a stat or an ammunition class. */
    std::uint32_t hash{};
};

/**
 * @return Everything the editor reads that names one icon: catalog items and perks, the item
 * definitions the catalog leaves out, every stat, and the ammunition marks.
 */
[[nodiscard]] std::vector<Reference> references_of(std::uint32_t tag) {
    const edit::Catalog& catalog = model().catalog;
    std::vector<Reference> found;
    for (const auto& item : catalog.items) {
        if (item.iconTag != tag) {
            continue;
        }
        std::string detail = !item.type.empty() ? item.type : std::string(item.plug ? "Perk" : "Item");
        if (!item.plug && item.definition.tier != 0) {
            detail += "  /  ";
            detail += art::tier_name(item.definition.tier);
        }
        found.push_back({&item, item.name, std::move(detail), item.definition.definitionHash});
    }
    for (const auto& owner : catalog.iconOwners) {
        if (owner.tag == tag) {
            found.push_back({nullptr,
                             owner.name,
                             (owner.type.empty() ? std::string("Item") : owner.type)
                                 + "  /  not in the catalog",
                             owner.hash});
        }
    }
    for (const auto& [row, statTag] : catalog.statIcons) {
        if (statTag != tag) {
            continue;
        }
        const auto name = catalog.statNames.find(row);
        found.push_back({nullptr,
                         name != catalog.statNames.end() ? name->second
                                                         : "Stat row " + std::to_string(row),
                         "Stat",
                         0});
    }
    for (std::size_t i = 1; i < catalog.ammoIconTags.size(); ++i) {
        if (catalog.ammoIconTags[i] == tag) {
            found.push_back({nullptr, kAmmoNames[i], "Ammunition", 0});
        }
    }
    return found;
}

/**
 * Starts the icon sweep the first time the page is drawn, and takes its result once it is in.
 * The sweep reads every package's entry table, so it runs on a worker and only for this page;
 * until it lands the browser shows the investment icons the catalog loaded with.
 */
void advance_icon_sweep() noexcept {
    Model& state = model();
    const IconSweepPhase phase = state.iconSweep.load(std::memory_order_acquire);
    if (phase == IconSweepPhase::idle) {
        state.iconSweep.store(IconSweepPhase::running, std::memory_order_release);
        try {
            state.iconSweeper = std::thread([] {
                Model& worker = model();
                bool swept = false;
                try {
                    swept = edit::sweep_icons(
                        worker.catalog, worker.sweptIcons, worker.sweptPackages, worker.sweptOwners);
                } catch (...) {
                    swept = false;
                }
                worker.iconSweep.store(swept ? IconSweepPhase::done : IconSweepPhase::failed,
                                       std::memory_order_release);
            });
        } catch (...) {
            state.iconSweep.store(IconSweepPhase::failed, std::memory_order_release);
        }
        return;
    }
    if (phase != IconSweepPhase::done && phase != IconSweepPhase::failed) {
        return;
    }
    if (state.iconSweeper.joinable()) {
        state.iconSweeper.join();
    }
    if (phase == IconSweepPhase::done && !state.sweptIcons.empty()) {
        state.catalog.icons = std::move(state.sweptIcons);
        state.catalog.iconPackages = std::move(state.sweptPackages);
        state.catalog.iconOwners = std::move(state.sweptOwners);
        state.catalog.iconsSwept = true;
        state.iconFilterBuilt = -2;
        state.iconPackage = -1;
    }
    state.sweptIcons.clear();
    state.sweptPackages.clear();
    state.sweptOwners.clear();
    state.iconSweep.store(IconSweepPhase::finished, std::memory_order_release);
}

/** Rebuilds the filtered list when the chosen package changes. */
void refresh_filter() noexcept {
    Model& state = model();
    if (state.iconFilterBuilt == state.iconPackage) {
        return;
    }
    state.iconFilterBuilt = state.iconPackage;
    state.iconFiltered.clear();
    const auto& icons = state.catalog.icons;
    state.iconFiltered.reserve(icons.size());
    for (std::size_t i = 0; i < icons.size(); ++i) {
        if (state.iconPackage < 0 || icons[i].package == state.iconPackage) {
            state.iconFiltered.push_back(static_cast<std::uint32_t>(i));
        }
    }
    state.iconRow = 0;
}

/** Takes a finished export's outcome into the viewer's note. */
void collect_export() noexcept {
    Model& state = model();
    std::uint32_t tag{};
    bool saved{};
    std::string message;
    if (!preview::take_export(tag, saved, message)) {
        return;
    }
    state.iconExporting = 0;
    state.iconNote = std::move(message);
    state.iconNoteFailed = !saved;
}

/**
 * Finds one icon in the list the page shows and opens it. A tag is written with its 0x, or as
 * eight hex digits; anything else is read as an investment row.
 */
void find_icon() noexcept {
    Model& state = model();
    std::string text = state.iconFind;
    const std::size_t begin = text.find_first_not_of(' ');
    const std::size_t end = text.find_last_not_of(' ');
    text = begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    state.iconFindMissed = false;
    if (text.empty()) {
        return;
    }
    const bool tagged = text.starts_with("0x") || text.starts_with("0X") || text.size() == 8;
    char* stop = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &stop, tagged ? 16 : 10);
    if (stop == text.c_str() || *stop != '\0') {
        state.iconFindMissed = true;
        return;
    }
    const auto& filtered = state.iconFiltered;
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        const edit::IconRow& icon = state.catalog.icons[filtered[i]];
        if (tagged ? icon.tag == value : icon.row == value) {
            state.iconRow = static_cast<int>((i / kPageSize) * kPageSize);
            state.iconViewed = static_cast<int>(i);
            state.iconViewRequested = true;
            return;
        }
    }
    state.iconFindMissed = true;
}

/** Draws the package filter, which opens showing every package. */
void draw_package_filter() noexcept {
    Model& state = model();
    const auto& names = state.catalog.iconPackages;
    const bool chosen =
        state.iconPackage >= 0 && static_cast<std::size_t>(state.iconPackage) < names.size();
    if (!controls::begin_picker("##package",
                                chosen ? names[static_cast<std::size_t>(state.iconPackage)].c_str()
                                       : kAllPackagesLabel,
                                pixels(kPackageFilterWidth))) {
        return;
    }
    if (controls::picker_row(kAllPackagesLabel, state.iconPackage < 0)) {
        state.iconPackage = -1;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (controls::picker_row(names[i].c_str(), state.iconPackage == static_cast<int>(i))) {
            state.iconPackage = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    controls::end_picker();
}

/** @return The width a word button takes for its label. */
[[nodiscard]] float word_width(const char* label) noexcept {
    return ImGui::CalcTextSize(label).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
}

/**
 * Draws the row over the grid: the count, the package and the find field lead; the pager and the
 * tile size sit at the far end.
 * @return The first entry the grid draws.
 */
[[nodiscard]] std::size_t draw_controls(std::size_t total) noexcept {
    Model& state = model();
    const ImGuiStyle& style = ImGui::GetStyle();
    state.iconRow = std::clamp(state.iconRow, 0, (std::max)(0, static_cast<int>(total) - 1));
    const std::size_t pages = (std::max)(std::size_t{1}, (total + kPageSize - 1) / kPageSize);
    const std::size_t page = static_cast<std::size_t>(state.iconRow) / kPageSize;
    // The side workspace lies over the page's right edge, so the row keeps to what it leaves.
    const float right =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - overlay_width();

    char line[kLineCapacity]{};
    const bool scanning =
        state.iconSweep.load(std::memory_order_acquire) == IconSweepPhase::running;
    (void)std::snprintf(
        line, sizeof line, "%zu ICONS%s", total, scanning ? "  /  SCANNING PACKAGES" : "");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tooltip::muted(), "%s", line);
    ImGui::SameLine(0.0F, pixels(kPagerGap));
    draw_package_filter();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pixels(kFindWidth));
    if (ImGui::InputTextWithHint("##find",
                                 "Find a tag or row",
                                 state.iconFind,
                                 sizeof state.iconFind,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        find_icon();
    }
    if (state.iconFindMissed) {
        ImGui::SameLine();
        ImGui::TextColored(tooltip::pending(), "Not in this list");
    }

    char pageLine[kLabelCapacity]{};
    (void)std::snprintf(pageLine, sizeof pageLine, "PAGE %zu / %zu", page + 1, pages);
    const float trailing = word_width("Previous") + word_width("Next")
                           + ImGui::CalcTextSize(pageLine).x + pixels(kTileSliderWidth)
                           + (pixels(kPagerGap) * 2.0F) + style.ItemSpacing.x;
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(), right - trailing));
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button("Previous")) {
        state.iconRow = static_cast<int>((page - 1) * kPageSize);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, pixels(kPagerGap));
    ImGui::TextColored(tooltip::muted(), "%s", pageLine);
    ImGui::SameLine(0.0F, pixels(kPagerGap));
    ImGui::BeginDisabled(page + 1 >= pages);
    if (ImGui::Button("Next")) {
        state.iconRow = static_cast<int>((page + 1) * kPageSize);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pixels(kTileSliderWidth));
    ImGui::SliderFloat("##tile", &state.iconTile, kMinimumTile, kMaximumTile, "%.0f px");
    return static_cast<std::size_t>(state.iconRow);
}

/** Writes the label a tile carries: its investment row, or its tag when no record names it. */
void write_label(const edit::IconRow& icon, char (&text)[kLabelCapacity]) noexcept {
    if (icon.row != edit::kNoIconRow) {
        (void)std::snprintf(text, sizeof text, "%u", icon.row);
    } else {
        (void)std::snprintf(text, sizeof text, "%08X", icon.tag);
    }
}

/**
 * Shows what a tile holds under the pointer: an item's own tooltip when the icon is a catalog
 * item's, otherwise the tag, the row, the size and the package it came from.
 */
void draw_tile_tooltip(const edit::IconRow& icon) noexcept {
    const std::vector<Reference> references = references_of(icon.tag);
    if (!references.empty() && references.front().item != nullptr) {
        tooltip::draw(*references.front().item, nullptr);
        return;
    }
    const auto& names = model().catalog.iconPackages;
    ImGui::BeginTooltip();
    if (!references.empty()) {
        ImGui::TextUnformatted(references.front().name.c_str());
    }
    if (icon.row != edit::kNoIconRow) {
        ImGui::TextColored(tooltip::muted(), "0x%08X  /  row %u", icon.tag, icon.row);
    } else {
        ImGui::TextColored(tooltip::muted(), "0x%08X", icon.tag);
    }
    preview::Details facts{};
    if (preview::details(icon.tag, facts)) {
        ImGui::TextColored(tooltip::muted(), "%u x %u", facts.width, facts.height);
    }
    if (icon.package < names.size()) {
        ImGui::TextColored(tooltip::muted(), "%s", names[icon.package].c_str());
    }
    ImGui::EndTooltip();
}

/**
 * Draws one page of tiles. Each icon keeps its own proportions inside its tile and is only ever
 * shrunk to fit, never stretched to the tile's shape or blown up past its own size.
 * @param first Entry the page starts at.
 * @param viewing True while the viewer is open, which frames the tile it shows.
 */
void draw_grid(std::size_t first, bool viewing) noexcept {
    Model& state = model();
    const auto& filtered = state.iconFiltered;
    auto* draw = ImGui::GetWindowDrawList();
    const float tile = pixels(state.iconTile);
    const float gap = pixels(kTileGap);
    const float label = pixels(kLabelHeight);
    const float inset = pixels(kTileInset);
    const float rounding = pixels(controls::kRowRounding);
    const int columns = (std::max)(
        1,
        static_cast<int>((ImGui::GetContentRegionAvail().x - overlay_width() + gap) / (tile + gap)));
    const std::size_t last = (std::min)(filtered.size(), first + kPageSize);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{gap, gap});
    for (std::size_t i = first; i < last; ++i) {
        const edit::IconRow& icon = state.catalog.icons[filtered[i]];
        if ((i - first) % static_cast<std::size_t>(columns) != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // An invisible button over the tile and its label carries the hover and the click.
        const bool clicked = ImGui::InvisibleButton("icon", {tile, tile + label});
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 corner{at.x + tile, at.y + tile};
        draw->AddRectFilled(
            at, corner, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);
        (void)preview::draw_fitted(icon.tag,
                                   {at.x + inset, at.y + inset},
                                   {tile - (inset * 2.0F), tile - (inset * 2.0F)},
                                   pixels(1.0F),
                                   1.0F);
        if (viewing && state.iconViewed == static_cast<int>(i)) {
            draw->AddRect(at, corner, ImGui::GetColorU32(ImGuiCol_Text), rounding, 0, pixels(kViewedThickness));
        }

        char text[kLabelCapacity]{};
        write_label(icon, text);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kLabelScale);
        const float textWidth = ImGui::CalcTextSize(text).x;
        draw->AddText({at.x + (std::max)(0.0F, (tile - textWidth) * 0.5F), at.y + tile + pixels(1.0F)},
                      ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                      text);
        ImGui::PopFont();
        if (hovered) {
            draw_tile_tooltip(icon);
        }
        if (clicked) {
            state.iconViewed = static_cast<int>(i);
            state.iconViewRequested = true;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
}

/**
 * Draws one thing that uses the icon as a row of the viewer's list: its icon, its name, what it
 * is, and its hash at the far end. A catalog item shows its own tooltip under the pointer.
 */
void draw_reference(const Reference& reference, std::uint32_t tag, float width) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    const float height = pixels(kReferenceRowHeight);
    const float icon = pixels(kReferenceIcon);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("reference", {width, height});
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddRectFilled(origin,
                            {origin.x + width, origin.y + height},
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
                            pixels(controls::kRowRounding));
    }
    draw->AddLine({origin.x, origin.y + height},
                  {origin.x + width, origin.y + height},
                  ImGui::GetColorU32(tooltip::rule_color()));

    const ImVec2 iconAt{origin.x + pixels(kReferenceInset), origin.y + ((height - icon) * 0.5F)};
    if (reference.item != nullptr) {
        art::icon(*reference.item, iconAt, icon);
    } else {
        (void)preview::draw_fitted(tag, iconAt, {icon, icon}, pixels(1.0F), 1.0F);
    }

    const float top = origin.y + ((height - ImGui::GetTextLineHeight()) * 0.5F);
    float right = origin.x + width - pixels(kReferenceInset);
    if (reference.hash != 0) {
        char hash[kLabelCapacity]{};
        (void)std::snprintf(hash, sizeof hash, "0x%08X", reference.hash);
        right -= ImGui::CalcTextSize(hash).x;
        draw->AddText({right, top}, ImGui::GetColorU32(tooltip::muted()), hash);
        right -= pixels(kReferenceGap);
    }
    const float left = iconAt.x + icon + pixels(kReferenceGap);
    const float room = (std::max)(0.0F, right - left);
    const std::string name = reference.name.empty() ? std::string("Unnamed") : reference.name;
    const float nameWidth = (std::min)(ImGui::CalcTextSize(name.c_str()).x, room * kReferenceNameShare);
    art::clipped_text(name, {left, top}, room * kReferenceNameShare, ImGui::GetColorU32(ImGuiCol_Text));
    const float detailLeft = left + nameWidth + pixels(kReferenceGap);
    art::clipped_text(reference.detail,
                      {detailLeft, top},
                      (std::max)(0.0F, right - detailLeft),
                      ImGui::GetColorU32(tooltip::muted()));
    if (hovered && reference.item != nullptr) {
        tooltip::draw(*reference.item, nullptr);
    }
}

/** Moves the viewer by one icon, and the page under it with the icon it lands on. */
void step_viewer(int by) noexcept {
    Model& state = model();
    const int next = state.iconViewed + by;
    if (next < 0 || static_cast<std::size_t>(next) >= state.iconFiltered.size()) {
        return;
    }
    state.iconViewed = next;
    state.iconRow = static_cast<int>((static_cast<std::size_t>(next) / kPageSize) * kPageSize);
    if (state.iconExporting == 0) {
        state.iconNote.clear();
    }
}

/** Draws the viewer's footer: moving along, copying, exporting, and the close it ends on. */
void draw_viewer_footer(const edit::IconRow& icon) noexcept {
    Model& state = model();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const auto viewed = static_cast<std::size_t>(state.iconViewed);
    int by = 0;
    ImGui::BeginDisabled(viewed == 0);
    if (ImGui::Button("Previous")) {
        by = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(viewed + 1 >= state.iconFiltered.size());
    if (ImGui::Button("Next")) {
        by = 1;
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        by = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        by = 1;
    }
    ImGui::SameLine(0.0F, pixels(kFooterGap));
    if (ImGui::Button("Copy tag")) {
        char text[kLabelCapacity]{};
        (void)std::snprintf(text, sizeof text, "0x%08X", icon.tag);
        ImGui::SetClipboardText(text);
        state.iconNote = std::string("Copied ") + text + ".";
        state.iconNoteFailed = false;
    }
    ImGui::SameLine();
    const bool exporting = state.iconExporting != 0;
    ImGui::BeginDisabled(exporting);
    if (ImGui::Button(exporting ? "Exporting" : "Export PNG")) {
        if (preview::request_export(icon.tag)) {
            state.iconExporting = icon.tag;
            state.iconNote = "Exporting...";
            state.iconNoteFailed = false;
        } else {
            state.iconNote = "The export could not start.";
            state.iconNoteFailed = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(right - pixels(kCloseWidth));
    if (ImGui::Button("Close", {pixels(kCloseWidth), 0.0F}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    if (by != 0) {
        step_viewer(by);
    }
}

/**
 * Draws the viewer over one icon, in the sheet the editor's other tasks open in: what the icon is
 * called by what uses it, the icon itself at its own proportions, its size, and everything the
 * editor reads that names it. A tile only asks for this; the page opens it, because a tile that
 * scrolls out of the grid would otherwise take the modal with it.
 */
void draw_viewer() noexcept {
    Model& state = model();
    if (state.iconViewRequested) {
        state.iconViewRequested = false;
        if (state.iconExporting == 0) {
            state.iconNote.clear();
        }
        ImGui::OpenPopup(kViewerTitle);
    }
    if (!tooltip::begin_sheet(kViewerTitle)) {
        return;
    }
    const auto& filtered = state.iconFiltered;
    if (filtered.empty() || state.iconViewed < 0
        || static_cast<std::size_t>(state.iconViewed) >= filtered.size()) {
        ImGui::CloseCurrentPopup();
        tooltip::end_sheet();
        return;
    }
    const edit::IconRow& icon = state.catalog.icons[filtered[static_cast<std::size_t>(state.iconViewed)]];
    const std::vector<Reference> references = references_of(icon.tag);
    const auto& names = state.catalog.iconPackages;
    const char* package = icon.package < names.size() ? names[icon.package].c_str() : "";

    char line[kLineCapacity]{};
    if (icon.row != edit::kNoIconRow) {
        (void)std::snprintf(line, sizeof line, "0x%08X  /  row %u  /  %s", icon.tag, icon.row, package);
    } else {
        (void)std::snprintf(line, sizeof line, "0x%08X  /  %s", icon.tag, package);
    }
    const std::string title = references.empty()               ? std::string("Unreferenced icon")
                              : references.front().name.empty() ? std::string("Unnamed")
                                                                : references.front().name;
    tooltip::draw_sheet_head(title, line);
    controls::space(controls::kSectionSpacing);

    // The well the icon sits in, at its own proportions, enlarged a little when it is small.
    auto* draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float well = pixels(kWellHeight);
    const float margin = pixels(kWellMargin);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(
        at, {at.x + width, at.y + well}, ImGui::GetColorU32(ImGuiCol_FrameBg), pixels(controls::kRowRounding));
    if (!preview::draw_fitted(icon.tag,
                              {at.x + margin, at.y + margin},
                              {width - (margin * 2.0F), well - (margin * 2.0F)},
                              pixels(1.0F),
                              kWellEnlargement)) {
        const char* waiting = preview::unavailable(icon.tag) ? "No artwork in this container" : "Loading";
        const ImVec2 size = ImGui::CalcTextSize(waiting);
        draw->AddText({at.x + ((width - size.x) * 0.5F), at.y + ((well - size.y) * 0.5F)},
                      ImGui::GetColorU32(tooltip::muted()),
                      waiting);
    }
    ImGui::Dummy({width, well});
    preview::Details facts{};
    if (preview::details(icon.tag, facts)) {
        ImGui::TextColored(tooltip::muted(),
                           "%u x %u  /  %u %s",
                           facts.width,
                           facts.height,
                           facts.layers,
                           facts.layers == 1 ? "layer" : "layers");
    } else {
        ImGui::TextUnformatted("");
    }

    controls::space(controls::kSectionSpacing);
    (void)std::snprintf(line, sizeof line, "USED BY  /  %zu", references.size());
    ImGui::TextColored(tooltip::muted(), "%s", line);
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();

    const float listHeight =
        (std::max)(ImGui::GetFrameHeight(),
                   ImGui::GetContentRegionAvail().y - (ImGui::GetFrameHeightWithSpacing() * kFooterLines));
    // The rows set their own gaps, as the loadouts sheet's rows do.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0.0F});
    if (ImGui::BeginChild("icon_references", {0.0F, listHeight})) {
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        if (references.empty()) {
            controls::space(controls::kRowSpacing);
            ImGui::PushTextWrapPos(controls::kAutomaticWrapPosition);
            ImGui::TextColored(tooltip::muted(),
                               "No item, perk, stat or ammunition mark the editor reads uses this icon.");
            ImGui::TextColored(tooltip::muted(),
                               "%s",
                               icon.row != edit::kNoIconRow
                                   ? "It is in the investment icon table, which definitions the "
                                     "editor does not read also index."
                                   : "It is not in the investment icon table.");
            ImGui::PopTextWrapPos();
        }
        for (std::size_t i = 0; i < references.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            draw_reference(references[i], icon.tag, rowWidth);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // One line is always kept for the note, so the footer never jumps when one appears.
    controls::space(controls::kRowSpacing);
    ImGui::TextColored(state.iconNoteFailed ? tooltip::pending() : tooltip::muted(), "%s", state.iconNote.c_str());
    draw_viewer_footer(icon);
    tooltip::end_sheet();
}

} // namespace

void draw_icons_page() noexcept {
    Model& state = model();
    advance_icon_sweep();
    refresh_filter();
    collect_export();
    // Asked at the page's own scope, which is where the viewer is opened.
    const bool viewing = ImGui::IsPopupOpen(kViewerTitle);
    const std::size_t first = draw_controls(state.iconFiltered.size());
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();
    controls::space(controls::kRuleSpacing);
    if (ImGui::BeginChild("icon_browser")) {
        draw_grid(first, viewing);
    }
    ImGui::EndChild();
    draw_viewer();
}

} // namespace dawn::core::ui::modules::loadout::internal
