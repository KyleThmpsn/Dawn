// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
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
/**
 * The label under a tile: set at the size of the tooltip's smallest line, a perk's type, where a
 * run of figures still reads at a glance, and kept this far under the tile.
 */
constexpr float kLabelScale = 0.82F;
constexpr float kLabelGap = 1.0F;
/**
 * The shortest label a tile sets: four figures. A tile too narrow for even that goes without one,
 * since its tooltip names the tag and the row anyway.
 */
constexpr const char* kShortLabel = "0000";
/** 40 bytes hold a row number, a tag in hex, or the pager's line. */
constexpr std::size_t kLabelCapacity = 40;
/** 160 bytes hold one line of the viewer's detail: a tag, a row and a package family. */
constexpr std::size_t kLineCapacity = 160;
/** Widths of the controls along the top of the page. The find field widens if its hint needs it. */
constexpr float kPackageFilterWidth = 240.0F;
constexpr float kFindWidth = 170.0F;
constexpr float kTileSliderWidth = 110.0F;
/** What the find field asks for while it is empty. */
constexpr const char* kFindHint = "Tag or row, then Enter";
/** The pager's two actions, which the viewer's footer steps with too. */
constexpr const char* kPreviousLabel = "Previous";
constexpr const char* kNextLabel = "Next";
/** Title of the icon viewer, used both to open it and to submit it. */
constexpr const char* kViewerTitle = "Icon";
/** The viewer's tab that shows every layer stacked, as the icon is drawn. */
constexpr const char* kAllLayersLabel = "All Layers";
/** Shown when the filter is off, which is how the browser opens. */
constexpr const char* kAllPackagesLabel = "All Packages";
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
/** Ammunition classes in `Catalog::ammoIconTags` order; index zero is never drawn. */
constexpr const char* kAmmoNames[]{"", "Primary Ammunition", "Special Ammunition", "Heavy Ammunition"};

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

/** Stands for no page at all, so whichever page the grid draws next opens at its top. */
constexpr std::size_t kNoPage = (std::numeric_limits<std::size_t>::max)();

/**
 * What the grid keeps between frames that the model does not: the first entry of the page it drew
 * last, so a page it turns to opens at its top, and an entry to bring into view once it is drawn,
 * or -1.
 */
struct GridScroll {
    std::size_t shown{kNoPage};
    int reveal{-1};
};

GridScroll g_grid;

/**
 * The player's place, held by tag while the list under it is replaced: the icon the viewer shows,
 * the icon the grid's page opens on, and whether the viewer is open, which makes its icon the one
 * the grid stays with.
 */
struct Place {
    std::uint32_t viewed{};
    std::uint32_t page{};
    bool viewing{};
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
            detail += controls::kDetailSeparator;
            detail += art::tier_name(item.definition.tier);
        }
        found.push_back({&item, item.name, std::move(detail), item.definition.definitionHash});
    }
    for (const auto& owner : catalog.iconOwners) {
        if (owner.tag == tag) {
            std::string detail = owner.type.empty() ? std::string("Item") : owner.type;
            detail += controls::kDetailSeparator;
            detail += "Not in the catalog";
            found.push_back({nullptr, owner.name, std::move(detail), owner.hash});
        }
    }
    for (const auto& [row, statTag] : catalog.statIcons) {
        if (statTag != tag) {
            continue;
        }
        const auto name = catalog.statNames.find(row);
        found.push_back({nullptr,
                         name != catalog.statNames.end() ? name->second
                                                         : "Stat Row " + std::to_string(row),
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

/** @return The tag of one entry of the list the page shows, or zero for no entry. */
[[nodiscard]] std::uint32_t tag_at(int entry) noexcept {
    const Model& state = model();
    if (entry < 0 || static_cast<std::size_t>(entry) >= state.iconFiltered.size()) {
        return 0;
    }
    const std::uint32_t index = state.iconFiltered[static_cast<std::size_t>(entry)];
    return index < state.catalog.icons.size() ? state.catalog.icons[index].tag : 0;
}

/** @return Where a tag falls in the list the page shows, or -1 when the list leaves it out. */
[[nodiscard]] int entry_of(std::uint32_t tag) noexcept {
    const Model& state = model();
    if (tag == 0) {
        return -1;
    }
    for (std::size_t i = 0; i < state.iconFiltered.size(); ++i) {
        if (state.catalog.icons[state.iconFiltered[i]].tag == tag) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/** @return The first entry of the page an entry falls on. */
[[nodiscard]] int page_start(std::size_t entry) noexcept {
    return static_cast<int>((entry / kPageSize) * kPageSize);
}

/** @return The page the grid is on, kept inside the pages the list fills. */
[[nodiscard]] std::size_t current_page(std::size_t pages) noexcept {
    const auto row = static_cast<std::size_t>((std::max)(model().iconRow, 0));
    return (std::min)(row / kPageSize, pages - 1);
}

/**
 * Fills the list the page shows from the catalog's icons and the chosen package. The list is new to
 * the grid, so it opens at its top even on the page number the old one was on.
 */
void build_filter() noexcept {
    Model& state = model();
    g_grid.shown = kNoPage;
    state.iconFilterBuilt = state.iconPackage;
    state.iconFiltered.clear();
    const auto& icons = state.catalog.icons;
    state.iconFiltered.reserve(icons.size());
    for (std::size_t i = 0; i < icons.size(); ++i) {
        if (state.iconPackage < 0 || icons[i].package == state.iconPackage) {
            state.iconFiltered.push_back(static_cast<std::uint32_t>(i));
        }
    }
}

/** @return The place as the list stands, taken before the list is replaced. */
[[nodiscard]] Place hold_place(bool viewing) noexcept {
    const Model& state = model();
    return {tag_at(state.iconViewed), tag_at(state.iconRow), viewing};
}

/**
 * Finds the place again once the list is replaced: the viewer keeps its icon, and the grid turns to
 * the page holding the icon it stays with and brings that one into view. The page's own icon
 * stands in when the viewer is shut, or its icon is gone from the new list.
 */
void restore_place(const Place& place) noexcept {
    Model& state = model();
    state.iconViewed = entry_of(place.viewed);
    const int kept = place.viewing && state.iconViewed >= 0 ? state.iconViewed : entry_of(place.page);
    state.iconRow = kept >= 0 ? page_start(static_cast<std::size_t>(kept)) : 0;
    g_grid.reveal = kept;
}

/**
 * Starts the icon sweep the first time the page is drawn, and takes its result once it is in.
 * The sweep reads every package's entry table, so it runs on a worker and only for this page;
 * until it lands the browser shows the investment icons the catalog loaded with.
 * @param viewing True while the viewer is open, whose icon the grid then stays with.
 */
void advance_icon_sweep(bool viewing) noexcept {
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
        // The swept list is ordered by package, so no index into the old one holds in it. The place
        // is held by tag across the swap and found again after it, so an open viewer keeps its icon
        // and the grid its page.
        const Place place = hold_place(viewing);
        state.catalog.icons = std::move(state.sweptIcons);
        state.catalog.iconPackages = std::move(state.sweptPackages);
        state.catalog.iconOwners = std::move(state.sweptOwners);
        state.catalog.iconsSwept = true;
        state.iconPackage = -1;
        build_filter();
        restore_place(place);
    }
    state.sweptIcons.clear();
    state.sweptPackages.clear();
    state.sweptOwners.clear();
    state.iconSweep.store(IconSweepPhase::finished, std::memory_order_release);
}

/**
 * Rebuilds the list when the player picks another package, from its first page. The icon the
 * viewer was on is found again by its tag, so the index never comes to name another icon.
 */
void refresh_filter() noexcept {
    Model& state = model();
    if (state.iconFilterBuilt == state.iconPackage) {
        return;
    }
    const std::uint32_t viewed = tag_at(state.iconViewed);
    build_filter();
    state.iconViewed = entry_of(viewed);
    state.iconRow = 0;
    g_grid.reveal = -1;
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

/** Takes a copied tag's outcome into the viewer's note, once the game window has written it. */
void collect_copy() noexcept {
    Model& state = model();
    bool copied = false;
    if (state.iconCopying == 0 || !take_copy_result(copied)) {
        return;
    }
    char text[kLineCapacity]{};
    (void)std::snprintf(
        text, sizeof text, copied ? "Copied 0x%08X." : "Couldn't copy 0x%08X.", state.iconCopying);
    state.iconNote = text;
    state.iconNoteFailed = !copied;
    state.iconCopying = 0;
}

/**
 * Finds one icon in the list the page shows, opens it, and brings its tile into view. A tag is
 * written with its 0x, or as eight hex digits; anything else is read as an investment row.
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
            state.iconRow = page_start(i);
            state.iconViewed = static_cast<int>(i);
            state.iconViewRequested = true;
            g_grid.reveal = static_cast<int>(i);
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
 * Sets text into a width: whole when it fits, and cut short by `art::clipped_text` when it does not.
 * @return True when the text was cut, which is when the pointer should be offered the whole of it.
 */
bool fit_text(const std::string& text, ImVec2 at, float width, ImU32 color) noexcept {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        ImGui::GetWindowDrawList()->AddText(at, color, text.c_str());
        return false;
    }
    art::clipped_text(text, at, width, color);
    return true;
}

/**
 * Sets a muted label on the control row, level with the text inside the fields beside it.
 * `tooltip::draw_label` sets its capitals at the cursor, so a group lowers them by the fields'
 * padding and then hands the row its line back, as `AlignTextToFramePadding` does for plain text.
 */
void draw_row_label(const char* text) noexcept {
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y);
    tooltip::draw_label(text);
    ImGui::EndGroup();
}

/**
 * Sets a refusal at the cursor on the control row, level with the text in the fields before it and
 * cut short at the room the pager leaves, with the whole of it under the pointer when it is cut.
 * @param text Message, in sentence case.
 * @param end Window X the message stops short of.
 */
void draw_row_refusal(const char* text, float end) noexcept {
    const float room = end - ImGui::GetCursorPosX();
    if (room <= 0.0F) {
        return;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool cut = fit_text(text,
                              {at.x, at.y + ImGui::GetStyle().FramePadding.y},
                              room,
                              ImGui::GetColorU32(controls::kRefusedColor));
    ImGui::Dummy({(std::min)(ImGui::CalcTextSize(text).x, room), ImGui::GetFrameHeight()});
    if (cut && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

/**
 * Draws the row over the grid: the count, the package and the find field lead, with what a find or
 * the package scan came to after them; the pager and the tile size keep to the far end. PageUp and
 * PageDown turn the page as the pager does.
 * @return The first entry the grid draws.
 */
[[nodiscard]] std::size_t draw_controls(std::size_t total) noexcept {
    Model& state = model();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = pixels(controls::kGroupGap);
    const std::size_t pages = (std::max)(std::size_t{1}, (total + kPageSize - 1) / kPageSize);
    // The keys are left alone while a field is taking them or a popup lies over the page.
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        const std::size_t page = current_page(pages);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && page > 0) {
            state.iconRow = static_cast<int>((page - 1) * kPageSize);
        } else if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && page + 1 < pages) {
            state.iconRow = static_cast<int>((page + 1) * kPageSize);
        }
    }
    // The side workspace lies over the page's right edge, so the row keeps to what it leaves.
    const float right =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - overlay_width();

    const IconSweepPhase sweep = state.iconSweep.load(std::memory_order_acquire);
    const bool scanning = sweep == IconSweepPhase::running || sweep == IconSweepPhase::done;
    char line[kLineCapacity]{};
    (void)std::snprintf(line,
                        sizeof line,
                        "%zu %s%s%s",
                        total,
                        total == 1 ? "Icon" : "Icons",
                        scanning ? controls::kDetailSeparator : "",
                        scanning ? "Scanning Packages" : "");
    draw_row_label(line);
    ImGui::SameLine(0.0F, gap);
    draw_package_filter();
    ImGui::SameLine();
    ImGui::SetNextItemWidth((std::max)(pixels(kFindWidth), word_width(kFindHint)));
    const bool entered = ImGui::InputTextWithHint(
        "##find", kFindHint, state.iconFind, sizeof state.iconFind, ImGuiInputTextFlags_EnterReturnsTrue);
    // A miss answers the text that was asked about, so editing the text lets it go.
    if (ImGui::IsItemEdited()) {
        state.iconFindMissed = false;
    }
    if (entered) {
        find_icon();
    }

    // The pager is measured after the find, which can move the page. Its line is a muted label, so
    // it is measured in the spaced capitals `tooltip::draw_label` sets.
    const std::size_t page = current_page(pages);
    char pageLine[kLabelCapacity]{};
    (void)std::snprintf(pageLine, sizeof pageLine, "Page %zu of %zu", page + 1, pages);
    const float trailing = word_width(kPreviousLabel) + controls::spaced_width(pageLine)
                           + word_width(kNextLabel) + pixels(kTileSliderWidth) + (gap * 2.0F)
                           + style.ItemSpacing.x;
    const float pager = right - trailing;
    // What the find and the package scan came to, in the room the pager leaves.
    if (state.iconFindMissed) {
        ImGui::SameLine();
        draw_row_refusal("No icon with that tag or row.", pager - gap);
    }
    if (sweep == IconSweepPhase::failed || (sweep == IconSweepPhase::finished && !state.catalog.iconsSwept)) {
        ImGui::SameLine(0.0F, gap);
        draw_row_refusal("Package scan failed. Showing investment icons only.", pager - gap);
    }

    // The pager keeps to the far end, or follows what leads it when the row runs short.
    ImGui::SameLine();
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(), pager));
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button(kPreviousLabel)) {
        state.iconRow = static_cast<int>((page - 1) * kPageSize);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, gap);
    draw_row_label(pageLine);
    ImGui::SameLine(0.0F, gap);
    ImGui::BeginDisabled(page + 1 >= pages);
    if (ImGui::Button(kNextLabel)) {
        state.iconRow = static_cast<int>((page + 1) * kPageSize);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pixels(kTileSliderWidth));
    ImGui::SliderFloat("##tile", &state.iconTile, kMinimumTile, kMaximumTile, "%.0f px");
    const std::size_t first = current_page(pages) * kPageSize;
    state.iconRow = static_cast<int>(first);
    return first;
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
 * Sets a tile's label under it, centred. A label wider than the tile is cut short at the tile's
 * edge rather than run into its neighbour's.
 * @param at Top-left of the tile in screen space.
 * @param tile Edge of the tile in framebuffer pixels.
 * @param size Font size the label is set at, before display scaling.
 * @param lit True under the pointer, which brightens the label.
 */
void draw_tile_label(const edit::IconRow& icon, ImVec2 at, float tile, float size, bool lit) noexcept {
    char text[kLabelCapacity]{};
    write_label(icon, text);
    ImGui::PushFont(nullptr, size);
    const float width = ImGui::CalcTextSize(text).x;
    const ImVec2 top{at.x, at.y + tile + pixels(kLabelGap)};
    const ImU32 color = ImGui::GetColorU32(lit ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (width <= tile) {
        ImGui::GetWindowDrawList()->AddText({top.x + ((tile - width) * 0.5F), top.y}, color, text);
    } else {
        art::clipped_text(text, top, tile, color);
    }
    ImGui::PopFont();
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
        ImGui::TextColored(tooltip::muted(), "0x%08X%sRow %u", icon.tag, controls::kDetailSeparator, icon.row);
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
    // A page turned to opens at its top; one entry asked for is then brought into view below.
    const bool turned = first != g_grid.shown;
    const int reveal = g_grid.reveal;
    g_grid.shown = first;
    g_grid.reveal = -1;
    if (turned) {
        ImGui::SetScrollY(0.0F);
    }
    if (filtered.empty()) {
        ImGui::TextColored(tooltip::muted(), "No icons.");
        return;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const float tile = pixels(state.iconTile);
    const float gap = pixels(kTileGap);
    const float inset = pixels(kTileInset);
    const float rounding = pixels(controls::kCardRounding);
    // The label is measured once, at its own size: whether a tile this narrow has room for even a
    // short one, and the height it then adds under every tile.
    const float labelSize = ImGui::GetStyle().FontSizeBase * kLabelScale;
    ImGui::PushFont(nullptr, labelSize);
    const bool labelled = ImGui::CalcTextSize(kShortLabel).x <= tile;
    const float label = labelled ? ImGui::GetTextLineHeight() + pixels(kLabelGap) : 0.0F;
    ImGui::PopFont();
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
        // A tile found or stepped to is brought into view when the grid would leave it out of
        // sight: from the page's top on a page just turned to, and from where it is otherwise.
        if (static_cast<int>(i) == reveal) {
            const float origin = ImGui::GetWindowPos().y - ImGui::GetScrollY();
            const float scroll = turned ? 0.0F : ImGui::GetScrollY();
            if (ImGui::GetItemRectMin().y - origin < scroll
                || ImGui::GetItemRectMax().y - origin > scroll + ImGui::GetWindowHeight()) {
                ImGui::SetScrollHereY(0.5F);
            }
        }
        const ImVec2 corner{at.x + tile, at.y + tile};
        draw->AddRectFilled(
            at, corner, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);
        (void)preview::draw_fitted(icon.tag,
                                   {at.x + inset, at.y + inset},
                                   {tile - (inset * 2.0F), tile - (inset * 2.0F)},
                                   pixels(1.0F),
                                   1.0F);
        if (viewing && state.iconViewed == static_cast<int>(i)) {
            draw->AddRect(at, corner, ImGui::GetColorU32(ImGuiCol_Text), rounding, 0, pixels(controls::kRailWidth));
        }
        if (labelled) {
            draw_tile_label(icon, at, tile, labelSize, hovered);
        }
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
 * is, and its hash at the far end. A catalog item shows its own tooltip under the pointer; any other
 * row shows its whole name and detail there when the row has cut them short.
 */
void draw_reference(const Reference& reference, std::uint32_t tag, float width) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    const float height = pixels(kReferenceRowHeight);
    const float icon = pixels(kReferenceIcon);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("reference", {width, height});
    const bool hovered = ImGui::IsItemHovered();
    // Only a catalog item answers the pointer, with its tooltip, so only its row lights under it.
    if (hovered && reference.item != nullptr) {
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
    const float nameRoom = (std::max)(0.0F, right - left) * kReferenceNameShare;
    const std::string name = reference.name.empty() ? std::string("Unnamed") : reference.name;
    const bool nameCut = fit_text(name, {left, top}, nameRoom, ImGui::GetColorU32(ImGuiCol_Text));
    const float detailLeft =
        left + (std::min)(ImGui::CalcTextSize(name.c_str()).x, nameRoom) + pixels(kReferenceGap);
    const bool detailCut = fit_text(reference.detail,
                                    {detailLeft, top},
                                    (std::max)(0.0F, right - detailLeft),
                                    ImGui::GetColorU32(tooltip::muted()));
    if (hovered && reference.item != nullptr) {
        tooltip::draw(*reference.item, nullptr);
    } else if (hovered && (nameCut || detailCut)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextColored(tooltip::muted(), "%s", reference.detail.c_str());
        ImGui::EndTooltip();
    }
}

/** Moves the viewer by one icon, and the page under it with the icon it lands on, into view. */
void step_viewer(int by) noexcept {
    Model& state = model();
    const int next = state.iconViewed + by;
    if (next < 0 || static_cast<std::size_t>(next) >= state.iconFiltered.size()) {
        return;
    }
    state.iconViewed = next;
    state.iconRow = page_start(static_cast<std::size_t>(next));
    state.iconLayer = -1;
    g_grid.reveal = next;
    if (state.iconExporting == 0) {
        state.iconNote.clear();
    }
}

/** @return The image record of the layer shown alone, or zero while every layer is shown. */
[[nodiscard]] std::uint32_t shown_layer_tag(std::uint32_t tag) noexcept {
    preview::LayerDetails layer{};
    const int index = model().iconLayer;
    return index >= 0 && preview::layer_details(tag, static_cast<unsigned>(index), layer) ? layer.tag : 0;
}

/**
 * Draws a tab for every layer an icon stacks, after one for the whole stack, when it has more than
 * one. Picking a layer shows it alone in the well, and the tag, the copy and the export follow it.
 * Each tab is its label and the page's tab padding, as the view tabs are. A stack a little wider
 * than the sheet gives up padding to stay on one row; one too wide even bare wraps instead of
 * running off the sheet's edge.
 */
void draw_layer_tabs(std::uint32_t tag, unsigned layers) noexcept {
    Model& state = model();
    if (layers < 2) {
        state.iconLayer = -1;
        return;
    }
    // Every tab is gathered first, so the row can be measured before it is drawn.
    std::vector<std::pair<int, const char*>> tabs{{-1, kAllLayersLabel}};
    for (unsigned i = 0; i < layers; ++i) {
        preview::LayerDetails layer{};
        if (preview::layer_details(tag, i, layer)) {
            tabs.emplace_back(static_cast<int>(i), layer.role);
        }
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float room = ImGui::GetContentRegionAvail().x;
    float bare = spacing * static_cast<float>(tabs.size() - 1);
    for (const auto& entry : tabs) {
        bare += controls::tab_width(entry.second);
    }
    const float fitted = (room - bare) / static_cast<float>(tabs.size());
    const float padding =
        fitted < 0.0F ? pixels(controls::kTabPadding) : (std::min)(fitted, pixels(controls::kTabPadding));
    float used = 0.0F;
    for (const auto& [index, label] : tabs) {
        const float width = controls::tab_width(label) + padding;
        if (used > 0.0F && used + spacing + width <= room) {
            ImGui::SameLine();
            used += spacing + width;
        } else {
            used = width;
        }
        ImGui::PushID(index);
        if (controls::tab(label, state.iconLayer == index, width)) {
            state.iconLayer = index;
        }
        ImGui::PopID();
    }
}

/** Draws the viewer's footer: moving along, copying and exporting. Escape closes the viewer. */
void draw_viewer_footer(const edit::IconRow& icon) noexcept {
    Model& state = model();
    const auto viewed = static_cast<std::size_t>(state.iconViewed);
    int by = 0;
    ImGui::BeginDisabled(viewed == 0);
    if (ImGui::Button(kPreviousLabel)) {
        by = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(viewed + 1 >= state.iconFiltered.size());
    if (ImGui::Button(kNextLabel)) {
        by = 1;
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        by = -1;
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        by = 1;
    }
    ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
    const std::uint32_t layerTag = shown_layer_tag(icon.tag);
    if (ImGui::Button("Copy Tag")) {
        const std::uint32_t copied = layerTag != 0 ? layerTag : icon.tag;
        char text[kLabelCapacity]{};
        (void)std::snprintf(text, sizeof text, "0x%08X", copied);
        if (request_copy(text)) {
            state.iconCopying = copied;
        } else {
            state.iconNote = "Couldn't copy the tag.";
            state.iconNoteFailed = true;
        }
    }
    ImGui::SameLine();
    const bool exporting = state.iconExporting != 0;
    ImGui::BeginDisabled(exporting);
    if (ImGui::Button(exporting ? "Exporting" : "Export PNG")) {
        if (preview::request_export(icon.tag, layerTag)) {
            state.iconExporting = icon.tag;
            state.iconNote = "Exporting...";
            state.iconNoteFailed = false;
        } else {
            state.iconNote = "Export failed.";
            state.iconNoteFailed = true;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
        state.iconLayer = -1;
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
        (void)std::snprintf(line,
                            sizeof line,
                            "0x%08X%sRow %u%s%s",
                            icon.tag,
                            controls::kDetailSeparator,
                            icon.row,
                            controls::kDetailSeparator,
                            package);
    } else {
        (void)std::snprintf(line, sizeof line, "0x%08X%s%s", icon.tag, controls::kDetailSeparator, package);
    }
    const std::string title = references.empty()               ? std::string("Unreferenced Icon")
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
        at, {at.x + width, at.y + well}, ImGui::GetColorU32(ImGuiCol_FrameBg), pixels(controls::kCardRounding));
    if (!preview::draw_fitted(icon.tag,
                              {at.x + margin, at.y + margin},
                              {width - (margin * 2.0F), well - (margin * 2.0F)},
                              pixels(1.0F),
                              kWellEnlargement,
                              IM_COL32_WHITE,
                              state.iconLayer)) {
        const char* waiting = preview::unavailable(icon.tag) ? "No artwork in this container." : "Loading";
        const ImVec2 size = ImGui::CalcTextSize(waiting);
        draw->AddText({at.x + ((width - size.x) * 0.5F), at.y + ((well - size.y) * 0.5F)},
                      ImGui::GetColorU32(tooltip::muted()),
                      waiting);
    }
    ImGui::Dummy({width, well});
    preview::Details facts{};
    const bool drawn = preview::details(icon.tag, facts);
    draw_layer_tabs(icon.tag, drawn ? facts.layers : 0);
    preview::LayerDetails layer{};
    if (state.iconLayer >= 0 && preview::layer_details(icon.tag, static_cast<unsigned>(state.iconLayer), layer)) {
        ImGui::TextColored(tooltip::muted(),
                           "%s%s0x%08X%s%u x %u",
                           layer.role,
                           controls::kDetailSeparator,
                           layer.tag,
                           controls::kDetailSeparator,
                           layer.width,
                           layer.height);
    } else if (drawn) {
        ImGui::TextColored(tooltip::muted(),
                           "%u x %u%s%u %s",
                           facts.width,
                           facts.height,
                           controls::kDetailSeparator,
                           facts.layers,
                           facts.layers == 1 ? "layer" : "layers");
    } else {
        ImGui::TextUnformatted("");
    }

    controls::space(controls::kSectionSpacing);
    // The list is headed as a section heads its rows: its name, and the count after it.
    tooltip::draw_label("Used By");
    ImGui::SameLine();
    ImGui::TextColored(tooltip::muted(), "%zu", references.size());
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();

    // The footer stands on the sheet's bottom edge however long the list is: the note line, then the
    // actions. The content region ends at the window's height less its padding, so the footer ends
    // there, and the list takes the height left between the rule and the footer.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float bottom = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y;
    const float footerTop =
        bottom - (ImGui::GetTextLineHeight() + style.ItemSpacing.y + ImGui::GetFrameHeight());
    const float gap = pixels(controls::kRowSpacing);
    const float listHeight = (std::max)(ImGui::GetFrameHeight(), footerTop - gap - ImGui::GetCursorPosY());
    // The rows set their own gaps, as the loadouts sheet's rows do.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{style.ItemSpacing.x, 0.0F});
    if (ImGui::BeginChild("icon_references", {0.0F, listHeight})) {
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        if (references.empty()) {
            controls::space(controls::kRowSpacing);
            ImGui::PushTextWrapPos(controls::kAutomaticWrapPosition);
            ImGui::TextColored(tooltip::muted(),
                               "Nothing in the catalog uses this icon.");
            ImGui::TextColored(tooltip::muted(),
                               "%s",
                               icon.row != edit::kNoIconRow
                                   ? "Listed in the investment icon table."
                                   : "Not in the investment icon table.");
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

    // One line is always kept for the note, so the actions never move when one appears.
    ImGui::SetCursorPosY((std::max)(footerTop, ImGui::GetCursorPosY() + gap));
    ImGui::TextColored(
        state.iconNoteFailed ? controls::kRefusedColor : tooltip::muted(), "%s", state.iconNote.c_str());
    draw_viewer_footer(icon);
    tooltip::end_sheet();
}

} // namespace

void draw_icons_page() noexcept {
    Model& state = model();
    // Asked at the page's own scope, which is where the viewer is opened.
    const bool viewing = ImGui::IsPopupOpen(kViewerTitle);
    advance_icon_sweep(viewing);
    refresh_filter();
    collect_export();
    collect_copy();
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
