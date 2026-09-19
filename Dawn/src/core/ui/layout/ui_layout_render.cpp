#include <algorithm>
#include <array>
#include <imgui.h>
#include "../../../../resources/resource.h"
#include "../animation/transition/ui_transition_animation.h"
#include "../components/logo/ui_logo_component.h"
#include "../components/section/ui_section_component.h"
#include "../scaling/dpi/ui_dpi_scaling.h"
#include "navigation/ui_layout_navigation.h"
#include "credits/dawn_credits_badge.h"
#include "ui_layout_lifecycle.h"

namespace dawn::core::ui::layout {
namespace {
void draw_title() noexcept {
    const float scale = scaling::dpi::current();
    const float extent = 36.0F * scale;
    if (components::logo::draw(extent)) { ImGui::SameLine(0, 14.0F * scale); }
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5F);
    ImGui::TextUnformatted("D A W N");
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(0, 12.0F * scale);
    ImGui::TextDisabled(DAWN_DISPLAY_VERSION);
}
void draw_content(const navigation::Selection& selected) noexcept {
    if (!selected.moduleAvailable) { ImGui::TextDisabled("No pages are available."); return; }
    if (selected.descriptor.stable_id() != "client.mission_launch" && selected.descriptor.stable_id() != "core.loadout") {
        std::array<char, modules::kDisplayNameCapacity + 1> label{};
        const auto name = selected.descriptor.display_name();
        std::copy(name.begin(), name.end(), label.begin());
        components::section::header(label.data());
        ImGui::Spacing();
    }
    selected.descriptor.frame_callback()();
}
} // namespace

bool render(bool visible) noexcept {
    if (!internal::context_is_current()) { return false; }
    auto* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) { return false; }
    const float dpi = scaling::dpi::current();
    const auto layoutState = snapshot();
    const bool loadout = std::string_view(layoutState.selectedStableId.data(), layoutState.selectedStableIdLength) == "core.loadout";
    const float width = (std::min)((loadout ? 1360.0F : 1020.0F) * dpi, viewport->Size.x - 32.0F * dpi);
    const float height = (std::min)((loadout ? 920.0F : 800.0F) * dpi, viewport->Size.y - 32.0F * dpi);
    if (width < 420.0F * dpi || height < 300.0F * dpi) { return false; }
    const float progress = animation::transition::update(1, animation::transition::Lane::visibility,
        visible, {16.0F, 14.0F}, 0.0F);
    if (progress <= 0.0F) { return false; }
    const float scale = 0.98F + 0.02F * progress;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({width * scale, height * scale}, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, progress);
    constexpr auto flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("Dawn", nullptr, flags)) {
        const auto origin = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(origin,
            {origin.x + ImGui::GetWindowWidth(), origin.y + 3.0F * dpi},
            ImGui::GetColorU32(ImGuiCol_CheckMark));
        draw_title();
        const auto selected = navigation::draw(snapshot());
        // A settings page opens with its own heading, but the loadout page opens with a second
        // tab row, and a Spacing either side of the rule puts a hole between the two rows. The
        // rule alone carries the boundary.
        ImGui::Separator();
        const float contentHeight = (std::max)(1.0F,
            // The footer is one line of text, not a framed control, so reserving a frame
            // height for it left dead panel under every page.
            ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing());
        if (ImGui::BeginChild("##dawn_content", {0, contentHeight},
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoSavedSettings)) {
            draw_content(selected);
        }
        ImGui::EndChild();
        ImGui::TextDisabled("DAWN  /  CAMPAIGN ARCHIVE");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return true;
}
} // namespace dawn::core::ui::layout
