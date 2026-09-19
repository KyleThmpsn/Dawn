// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../controls.h"
#include "../internal.h"
#include "../preview.h"

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
constexpr float kTileGap = 3.0F;
/** Room under a tile for its label, and the size that label is set at. */
constexpr float kLabelHeight = 12.0F;
constexpr float kLabelScale = 0.62F;
/** 40 bytes hold a row number, or a tag in hex. */
constexpr std::size_t kLabelCapacity = 40;
/** Widths of the controls along the top of the page. */
constexpr float kRowFieldWidth = 140.0F;
constexpr float kTileSliderWidth = 160.0F;
constexpr float kPackageFilterWidth = 260.0F;
/** The viewer draws one icon at this edge, which is large enough to read its detail. */
constexpr float kViewerExtent = 320.0F;
/** Title of the icon viewer, used both to open it and to submit it. */
constexpr const char* kViewerTitle = "Icon";
/** Shown when the filter is off, which is how the browser opens. */
constexpr const char* kAllPackagesLabel = "All packages";
/** Most references the viewer lists by name before it counts the rest. */
constexpr std::size_t kReferenceLimit = 12;
/** Ammunition classes in `Catalog::ammoIconTags` order; index zero is never drawn. */
constexpr const char* kAmmoNames[]{"", "Primary", "Special", "Heavy"};

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
                    swept = edit::sweep_icons(worker.catalog, worker.sweptIcons, worker.sweptPackages);
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
        state.catalog.iconsSwept = true;
        state.iconFilterBuilt = -2;
        state.iconPackage = -1;
    }
    state.sweptIcons.clear();
    state.sweptPackages.clear();
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

/** Draws the package filter, which opens showing every package. */
void draw_package_filter() noexcept {
    Model& state = model();
    const auto& names = state.catalog.iconPackages;
    const bool chosen =
        state.iconPackage >= 0 && static_cast<std::size_t>(state.iconPackage) < names.size();
    ImGui::SetNextItemWidth(pixels(kPackageFilterWidth));
    if (!ImGui::BeginCombo("##package",
                           chosen ? names[static_cast<std::size_t>(state.iconPackage)].c_str()
                                  : kAllPackagesLabel)) {
        return;
    }
    if (ImGui::Selectable(kAllPackagesLabel, state.iconPackage < 0)) {
        state.iconPackage = -1;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(names[i].c_str(), state.iconPackage == static_cast<int>(i))) {
            state.iconPackage = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/** Draws the row of controls and returns the first entry the grid should draw. */
[[nodiscard]] std::size_t draw_controls(std::size_t total) noexcept {
    Model& state = model();

    ImGui::AlignTextToFramePadding();
    if (state.iconSweep.load(std::memory_order_acquire) == IconSweepPhase::running) {
        ImGui::TextDisabled("%zu icons, scanning packages", total);
    } else {
        ImGui::TextDisabled("%zu icons", total);
    }
    ImGui::SameLine();
    draw_package_filter();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pixels(kRowFieldWidth));
    // Stepping by a page is what the browser is actually driven by, so that is the fast step.
    ImGui::InputInt("##row", &state.iconRow, 1, static_cast<int>(kPageSize));
    ImGui::SameLine();
    if (ImGui::Button("<")) {
        state.iconRow -= static_cast<int>(kPageSize);
    }
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        state.iconRow += static_cast<int>(kPageSize);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pixels(kTileSliderWidth));
    ImGui::SliderFloat("##tile", &state.iconTile, kMinimumTile, kMaximumTile, "%.0f px");

    state.iconRow = std::clamp(state.iconRow, 0, (std::max)(0, static_cast<int>(total) - 1));
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
 * Draws what points at one icon: every catalog item whose icon it is, the character stat it
 * marks, or the ammunition class it stands for. An icon nothing names is said to be unreferenced,
 * which is exactly the case the browser exists to find.
 * @param tag Icon container tag being viewed.
 */
void draw_references(std::uint32_t tag) noexcept {
    const edit::Catalog& catalog = model().catalog;
    controls::space(controls::kRowSpacing);
    std::size_t shown = 0;
    std::size_t total = 0;
    for (const auto& item : catalog.items) {
        if (item.iconTag != tag) {
            continue;
        }
        ++total;
        if (shown < kReferenceLimit) {
            ImGui::TextDisabled("%s", item.plug ? "Perk" : "Item");
            ImGui::SameLine();
            ImGui::Text("%s  0x%08X", item.name.c_str(), item.definition.definitionHash);
            ++shown;
        }
    }
    for (std::size_t i = 0; i < catalog.statIconTags.size(); ++i) {
        if (catalog.statIconTags[i] != tag) {
            continue;
        }
        ++total;
        const std::string_view label = edit::stat_label(catalog, i);
        ImGui::TextDisabled("Stat");
        ImGui::SameLine();
        ImGui::Text("%.*s", static_cast<int>(label.size()), label.data());
    }
    for (std::size_t i = 1; i < catalog.ammoIconTags.size(); ++i) {
        if (catalog.ammoIconTags[i] != tag) {
            continue;
        }
        ++total;
        ImGui::TextDisabled("Ammunition");
        ImGui::SameLine();
        ImGui::TextUnformatted(kAmmoNames[i]);
    }
    if (total > kReferenceLimit) {
        ImGui::TextDisabled("and %zu more", total - kReferenceLimit);
    }
    if (total == 0) {
        ImGui::TextDisabled("Nothing in the catalog references this icon.");
    }
}

/**
 * Draws the viewer over one icon, large, with where it came from.
 * A tile only asks for this; the page opens it, because a tile that scrolls out of the grid would
 * otherwise take the modal with it.
 */
void draw_viewer() noexcept {
    Model& state = model();
    if (state.iconViewRequested) {
        state.iconViewRequested = false;
        ImGui::OpenPopup(kViewerTitle);
    }
    if (!ImGui::BeginPopupModal(kViewerTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const auto& filtered = state.iconFiltered;
    const auto viewed = static_cast<std::size_t>((std::max)(0, state.iconViewed));
    if (filtered.empty() || viewed >= filtered.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const edit::IconRow& icon = state.catalog.icons[filtered[viewed]];
    const float extent = pixels(kViewerExtent);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    (void)preview::draw(icon.tag, at, extent);
    ImGui::Dummy({extent, extent});

    const auto& names = state.catalog.iconPackages;
    if (icon.package < names.size()) {
        ImGui::TextDisabled("%s", names[icon.package].c_str());
    }
    if (icon.row != edit::kNoIconRow) {
        ImGui::Text("row %u", icon.row);
        ImGui::SameLine();
    }
    ImGui::Text("tag 0x%08X", icon.tag);
    draw_references(icon.tag);

    if (ImGui::Button("<") && state.iconViewed > 0) {
        --state.iconViewed;
    }
    ImGui::SameLine();
    if (ImGui::Button(">") && viewed + 1 < filtered.size()) {
        ++state.iconViewed;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy tag")) {
        char text[kLabelCapacity]{};
        (void)std::snprintf(text, sizeof text, "0x%08X", icon.tag);
        ImGui::SetClipboardText(text);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void draw_icons_page() noexcept {
    Model& state = model();
    advance_icon_sweep();
    refresh_filter();
    const auto& filtered = state.iconFiltered;
    const std::size_t first = draw_controls(filtered.size());
    if (!ImGui::BeginChild("icon_browser")) {
        ImGui::EndChild();
        return;
    }
    const float tile = pixels(state.iconTile);
    const float gap = pixels(kTileGap);
    const float label = pixels(kLabelHeight);
    const int columns =
        (std::max)(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
    const std::size_t last = (std::min)(filtered.size(), first + kPageSize);

    for (std::size_t i = first; i < last; ++i) {
        const edit::IconRow& icon = state.catalog.icons[filtered[i]];
        if ((i - first) % static_cast<std::size_t>(columns) != 0) {
            ImGui::SameLine(0.0F, gap);
        }
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // An invisible button over the tile carries the hover and the click.
        const bool clicked = ImGui::InvisibleButton("icon", {tile, tile + label});
        (void)preview::draw(icon.tag, at, tile);

        char text[kLabelCapacity]{};
        write_label(icon, text);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kLabelScale);
        ImGui::GetWindowDrawList()->AddText({at.x, at.y + tile},
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            text);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            const auto& names = state.catalog.iconPackages;
            ImGui::SetTooltip("tag 0x%08X  %s\nClick to open it larger.",
                              icon.tag,
                              icon.package < names.size() ? names[icon.package].c_str() : "");
        }
        if (clicked) {
            state.iconViewed = static_cast<int>(i);
            state.iconViewRequested = true;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    draw_viewer();
}

} // namespace dawn::core::ui::modules::loadout::internal
