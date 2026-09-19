// SPDX-License-Identifier: GPL-3.0-only
#include "controls.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <imgui_internal.h>

#include "../../scaling/dpi/ui_dpi_scaling.h"

namespace dawn::core::ui::modules::loadout::controls {
namespace {

using scaling::dpi::pixels;

/** 8 authored pixels pad a banner away from its border. This is a text inset, not a card gap. */
constexpr float kBannerPadding = 8.0F;
/** One authored pixel keeps the banner outline sharp at common display scales. */
constexpr float kBannerBorderThickness = 1.0F;
/** Tab geometry: one clear pointer target, underlined while it owns the page. */
constexpr float kTabRailInset = 10.0F;
constexpr float kTabRailHeight = 2.0F;
/** A hovered tab shows its rail at this alpha: a preview under the pointer, not a press. */
constexpr float kTabHoverAlpha = 0.45F;
/** Letter spacing of the capitals a tab and a section heading are set in. */
constexpr float kTabTracking = 1.6F;
/** Section heading: the room over and under its line, the gap before its count, its rule. */
constexpr float kSectionHeaderPadding = 8.0F;
constexpr float kSectionCountGap = 8.0F;
constexpr float kSectionLabelAlpha = 0.85F;
constexpr float kSectionRuleAlpha = 0.18F;
/** Picker: the chevron in the field, the list's own padding and row gap, the rail on the choice. */
constexpr float kPickerChevronWidth = 8.0F;
constexpr float kPickerChevronHeight = 4.0F;
constexpr float kPickerChevronInset = 12.0F;
constexpr float kPickerChevronAlpha = 0.7F;
constexpr float kPickerListPadding = 6.0F;
constexpr float kPickerRowGap = 1.0F;
constexpr float kPickerRowIndent = 6.0F;
constexpr float kPickerRailWidth = 2.0F;
constexpr float kPickerHairline = 1.0F;
/** A primary action is the game's white button, so its label has to go dark on it. */
constexpr ImVec4 kPrimaryFill{0.92F, 0.92F, 0.92F, 1.0F};
constexpr ImVec4 kPrimaryFillHovered{1.0F, 1.0F, 1.0F, 1.0F};
constexpr ImVec4 kPrimaryFillActive{0.80F, 0.80F, 0.80F, 1.0F};
constexpr ImVec4 kPrimaryText{0.08F, 0.08F, 0.09F, 1.0F};

} // namespace

void space(float authoredPixels) noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0.0F});
    ImGui::Dummy({0.0F, pixels(authoredPixels)});
    ImGui::PopStyleVar();
}

bool primary_button(const char* label, ImVec2 size) noexcept {
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimaryFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryFillHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPrimaryFillActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kPrimaryText);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool disclosure(const char* label) noexcept {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{});
    const bool open = ImGui::CollapsingHeader(label);
    ImGui::PopStyleColor();
    return open;
}

namespace {

/** @return The visible part of a label, which Button cuts off at its id suffix. */
[[nodiscard]] std::string visible_label(const char* label) noexcept {
    const char* end = std::strstr(label, "##");
    return end != nullptr ? std::string(label, end) : std::string(label);
}

/**
 * Sets a run of capitals with the game's letter spacing, or only measures it.
 * Dear ImGui sets no tracking of its own, so the letters are laid one at a time, each advanced by
 * its own width and the tracking after it.
 * @param text Capitals to set.
 * @param at Top-left of the run in screen space, ignored when only measuring.
 * @param color Packed colour, ignored when only measuring.
 * @param measure True to return the width without drawing.
 * @return The width of the run in framebuffer pixels.
 */
float spaced_capitals(const std::string& text, ImVec2 at, ImU32 color, bool measure) noexcept {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tracking = pixels(kTabTracking);
    float x = 0.0F;
    const char* cursor = text.c_str();
    const char* end = cursor + text.size();
    while (cursor < end) {
        unsigned int code = 0;
        const int consumed = ImTextCharFromUtf8(&code, cursor, end);
        const char* next = cursor + (std::max)(consumed, 1);
        if (!measure) {
            draw->AddText({at.x + x, at.y}, color, cursor, next);
        }
        x += ImGui::CalcTextSize(cursor, next).x + tracking;
        cursor = next;
    }
    return (std::max)(0.0F, x - tracking);
}

/** @return The label as the tab sets it: ASCII folded to capitals. */
[[nodiscard]] std::string shouted(const std::string& text) noexcept {
    std::string result = text;
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80U) {
            character = static_cast<char>(std::toupper(byte));
        }
    }
    return result;
}

} // namespace

float tab_width(const char* label) noexcept {
    return spaced_capitals(shouted(visible_label(label)), {}, 0, true);
}

bool tab(const char* label, bool active, float width) noexcept {
    // A themed Button answers a hover with an opaque panel fill over the whole row and leaves an
    // inactive label muted, which reads as a pressed button rather than a tab. The row is drawn
    // by hand instead, in the spaced capitals the game sets its own tabs in, so the label can
    // brighten and the rail fade in under the pointer. EnableNav keeps the keyboard and gamepad
    // reach a Button would have given the tab.
    const float height = pixels(kTabHeight);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 corner{origin.x + width, origin.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::string text = shouted(visible_label(label));
    const float textWidth = spaced_capitals(text, {}, 0, true);
    const float lineHeight = ImGui::GetTextLineHeight();
    spaced_capitals(text,
                    {origin.x + ((width - textWidth) * 0.5F), origin.y + ((height - lineHeight) * 0.5F)},
                    ImGui::GetColorU32(active || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    false);
    // The game underlines the open tab in white, and a tab under the pointer in a fainter white.
    if (active || hovered) {
        draw->AddRectFilled({origin.x + pixels(kTabRailInset), corner.y - pixels(kTabRailHeight)},
                            {corner.x - pixels(kTabRailInset), corner.y},
                            ImGui::GetColorU32(ImGuiCol_Text, active ? 1.0F : kTabHoverAlpha));
    }
    return pressed;
}

bool begin_picker(const char* id, const char* preview, float width) noexcept {
    // The field is the combo without its arrow box; the chevron is drawn over the field by hand
    // so it sits in the field rather than in a button beside it. The list gets the page's popup
    // ground, room around its rows, and rows that centre their text.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{pixels(kPickerListPadding), pixels(kPickerListPadding)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2{ImGui::GetStyle().ItemSpacing.x, pixels(kPickerRowGap)});
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2{0.0F, 0.5F});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, pixels(kPickerHairline));
    const bool open = ImGui::BeginCombo(
        id, preview, ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
    const float half = pixels(kPickerChevronWidth) * 0.5F;
    const float rise = pixels(kPickerChevronHeight);
    const ImVec2 centre{at.x + width - pixels(kPickerChevronInset), at.y + (height * 0.5F)};
    draw->AddTriangleFilled({centre.x - half, centre.y - (rise * 0.5F)},
                            {centre.x + half, centre.y - (rise * 0.5F)},
                            {centre.x, centre.y + (rise * 0.5F)},
                            ImGui::GetColorU32(ImGuiCol_Text, kPickerChevronAlpha));
    if (!open) {
        ImGui::PopStyleVar(4);
    }
    return open;
}

void end_picker() noexcept {
    ImGui::EndCombo();
    ImGui::PopStyleVar(4);
}

bool picker_row(const char* label, bool selected) noexcept {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    // The row indents its text by the field padding, so the list lines up under the field.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2{pixels(kPickerRowIndent), ImGui::GetStyle().FramePadding.y});
    const std::string text = std::string("  ") + label;
    const bool pressed = ImGui::Selectable(text.c_str(), selected, 0, {0.0F, height});
    ImGui::PopStyleVar();
    if (selected) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            {at.x, at.y}, {at.x + pixels(kPickerRailWidth), at.y + height},
            ImGui::GetColorU32(ImGuiCol_Text));
    }
    return pressed;
}

bool picker(const char* id, int& index, const char* const* items, int count, float width) noexcept {
    const char* preview = index >= 0 && index < count ? items[index] : "";
    if (!begin_picker(id, preview, width)) {
        return false;
    }
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (picker_row(items[i], i == index) && i != index) {
            index = i;
            changed = true;
        }
    }
    end_picker();
    return changed;
}

bool picker(const char* id, int& index, const char* items, float width) noexcept {
    std::vector<const char*> list;
    for (const char* item = items; *item != '\0'; item += std::strlen(item) + 1) {
        list.push_back(item);
    }
    return picker(id, index, list.data(), static_cast<int>(list.size()), width);
}

void heading(const char* text) noexcept {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const std::string capitals = shouted(visible_label(text));
    const float width = spaced_capitals(
        capitals, at, ImGui::GetColorU32(ImGuiCol_Text, kSectionLabelAlpha), false);
    ImGui::Dummy({width, ImGui::GetTextLineHeight()});
}

bool section_header(const char* label, std::size_t count) noexcept {
    // The heading is a divider that folds, not a button: the game heads a column with a line of
    // muted capitals over a rule. Open state lives in the window's own storage under the label.
    const ImGuiID id = ImGui::GetID(label);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(id, true);
    const float height = ImGui::GetTextLineHeight() + pixels(kSectionHeaderPadding);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    if (ImGui::InvisibleButton("section", {width, height})) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(open || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled,
                                           open || hovered ? kSectionLabelAlpha : 1.0F);
    const float textLeft = origin.x;
    const float textTop = origin.y + (pixels(kSectionHeaderPadding) * 0.5F);
    const float used = spaced_capitals(shouted(visible_label(label)), {textLeft, textTop}, color, false);
    char figure[24]{};
    (void)std::snprintf(figure, sizeof figure, "%zu", count);
    draw->AddText({textLeft + used + pixels(kSectionCountGap), textTop},
                  ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  figure);
    draw->AddLine({origin.x, origin.y + height - 1.0F},
                  {origin.x + width, origin.y + height - 1.0F},
                  ImGui::GetColorU32(ImGuiCol_Text, open ? kSectionRuleAlpha : kSectionRuleAlpha * 0.5F));
    space(kRuleSpacing);
    return open;
}

bool search(const char* id, const char* hint, char* buffer, std::size_t capacity) noexcept {
    return ImGui::InputTextWithHint(id, hint, buffer, capacity);
}

void field_label(const char* text, float width) noexcept {
    const float startX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    const float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0F, width - textWidth));
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::SetCursorPosX(startX + width + pixels(kFieldColumnGap));
}

float small_button_height() noexcept {
    return ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y;
}

void align_to_title(float controlHeight) noexcept {
    const float titleHeight = ImGui::GetItemRectSize().y;
    if (titleHeight > controlHeight) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ((titleHeight - controlHeight) * 0.5F));
    }
}

void title(const char* text, float scale) noexcept {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void banner(const ImVec4& color, const char* text) noexcept {
    const float padding = pixels(kBannerPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, pixels(kCardRounding));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, pixels(kBannerBorderThickness));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{padding, padding});
    ImGui::PushStyleColor(ImGuiCol_Border, color);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const float height = ImGui::CalcTextSize(text,
                                             nullptr,
                                             false,
                                             ImGui::GetContentRegionAvail().x - (padding * 2.0F))
                             .y
                         + (padding * 2.0F);
    if (ImGui::BeginChild("##banner",
                          {0.0F, height},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        ImGui::PushTextWrapPos(kAutomaticWrapPosition);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

} // namespace dawn::core::ui::modules::loadout::controls
