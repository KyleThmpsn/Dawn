// SPDX-License-Identifier: GPL-3.0-only
#include "art.h"

#include "../../fonts/runtime/ui_runtime_font_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "internal.h"
#include "tooltip.h"
#include "preview.h"
#include "state/account/inventory/placement.h"

namespace dawn::core::ui::modules::loadout::art {
namespace {

namespace inv = state::account::inventory;
using scaling::dpi::pixels;

/**
 * Rarity tints, indexed by tier. Tier 5 is Exotic and tier 0 is unclassified.
 * These are the game's own item header colors, so a band here reads as the band in game.
 */
constexpr ImVec4 kRarityTints[]{
    {0.765F, 0.737F, 0.706F, 1.0F},
    {0.765F, 0.737F, 0.706F, 1.0F},
    {0.212F, 0.435F, 0.259F, 1.0F},
    {0.314F, 0.463F, 0.639F, 1.0F},
    {0.322F, 0.184F, 0.396F, 1.0F},
    {0.808F, 0.682F, 0.200F, 1.0F},
};
/** Common and Exotic bands are light, so the game sets dark text on them. */
constexpr ImVec4 kDarkBandText{0.08F, 0.08F, 0.09F, 1.0F};
constexpr ImVec4 kLightBandText{1.0F, 1.0F, 1.0F, 1.0F};
/** Muted band text keeps this share of the text color. */
constexpr float kMutedBandTextOpacity = 0.72F;
/** Tiers at or below Common, and Exotic, take the dark text. */
constexpr std::uint8_t kLastLightBandTier = 1;
/** Tier names in the same order as the tints. */
constexpr const char* kTierNames[]{
    "Unclassified", "Common", "Uncommon", "Rare", "Legendary", "Exotic"};
/** Class names in stable authored order. */
constexpr const char* kClassNames[]{"Titan", "Hunter", "Warlock"};

/** The icon backdrop is the rarity tint at this weight, which keeps art legible on it. */
constexpr float kBackdropWeight = 0.17F;
/** 90 authored pixels is the smallest icon that fits a whole placeholder line. */
constexpr float kPlaceholderTextMinimumExtent = 90.0F;
/** Corner radius of the icon backdrop and its frame. */
constexpr float kIconBackdropRounding = 3.0F;
constexpr float kIconFrameRounding = 2.0F;
/** One extra strike per this many pixels of weight, which keeps a stroke solid with no blur. */
constexpr float kBoldStrikeStep = 0.6F;
/** 10 authored pixels of slack keep an ellipsis inside its measured column. */
constexpr float kClipSlack = 10.0F;
/** UTF-8 continuation bytes carry this tag, so a truncation steps back over them. */
constexpr unsigned char kContinuationMask = 0xC0U;
constexpr unsigned char kContinuationTag = 0x80U;

/** Separator between the two halves of a card subtitle. */
constexpr const char* kDetailSeparator = "  /  ";

/**
 * Armory result row geometry. The row is a rarity band, as the game's own item rows are: the
 * icon struck flush into its left edge, the name in capitals in the title cut, the type and slot
 * muted under it.
 */
constexpr float kResultRowHeight = 52.0F;
constexpr float kResultTextGap = 8.0F;
constexpr float kResultTextRightInset = 10.0F;
constexpr float kResultLineGap = 2.0F;
constexpr float kResultNameScale = 1.15F;
constexpr float kResultNameWeight = 0.8F;
constexpr float kResultSelectedThickness = 2.0F;
/** A row under the pointer lightens by this much over its band. */
constexpr ImVec4 kResultHoverWash{1.0F, 1.0F, 1.0F, 0.10F};

/** @return The tint scaled down to the backdrop weight, at full opacity. */
[[nodiscard]] ImU32 backdrop_color(const ImVec4& tint) noexcept {
    return ImGui::GetColorU32(
        {tint.x * kBackdropWeight, tint.y * kBackdropWeight, tint.z * kBackdropWeight, 1.0F});
}

/** Draws the text shown in place of an icon the package reader has not produced. */
void draw_icon_placeholder(const edit::CatalogItem& item, ImVec2 origin, float extent) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const bool loading = item.iconTag != 0 && !preview::unavailable(item.iconTag);
    if (extent >= pixels(kPlaceholderTextMinimumExtent)) {
        const char* text = loading ? "Loading image" : "No package image";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText({origin.x + ((extent - size.x) * 0.5F), origin.y + (extent * 0.5F)}, color, text);
        return;
    }
    const char* glyph = item.kind == edit::GearKind::weapon   ? "W"
                        : item.kind == edit::GearKind::armor  ? "A"
                                                              : "+";
    draw->AddText({origin.x + (extent * 0.4F), origin.y + (extent * 0.35F)}, color, glyph);
}

/**
 * Shows the game-style tooltip for a hovered card.
 * @param item Catalog definition behind the card.
 * @param instance Owned instance whose power and perks are shown, or zero for a catalog entry.
 */
void draw_card_tooltip(const edit::CatalogItem& item, std::uint64_t instance) noexcept {
    const edit::Item* owned = nullptr;
    if (instance != 0) {
        const edit::Item* selected = internal::selected_item();
        owned = selected != nullptr && selected->instanceSoid == instance ? selected : nullptr;
        if (owned == nullptr) {
            owned = internal::find_owned_item(instance);
        }
    }
    tooltip::draw(item, owned);
}

} // namespace

ImVec4 rarity_color(std::uint8_t tier) noexcept {
    return kRarityTints[(std::min)(static_cast<std::size_t>(tier), std::size(kRarityTints) - 1)];
}

ImU32 band_text(std::uint8_t tier, bool muted) noexcept {
    ImVec4 color = tier <= kLastLightBandTier || tier == kExoticTier ? kDarkBandText : kLightBandText;
    if (muted) {
        color.w = kMutedBandTextOpacity;
    }
    return ImGui::GetColorU32(color);
}

const char* tier_name(std::uint8_t tier) noexcept {
    return kTierNames[(std::min)(static_cast<std::size_t>(tier), std::size(kTierNames) - 1)];
}

float collection_card_height() noexcept {
    return std::floor(pixels(kResultRowHeight));
}

std::string shout(const std::string& value) noexcept {
    std::string result = value;
    // Only ASCII is folded: names come from the localized bank, and a byte-wise toupper on a
    // UTF-8 sequence is not safe.
    for (char& character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80U) {
            character = static_cast<char>(std::toupper(byte));
        }
    }
    return result;
}

const char* class_name(state::CharacterClass value) noexcept {
    return kClassNames[(std::min)(static_cast<std::size_t>(value), std::size(kClassNames) - 1)];
}

void icon(const edit::CatalogItem& item, ImVec2 origin, float extent, bool framed) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec4 tint = rarity_color(item.definition.tier);
    const ImVec2 corner{origin.x + extent, origin.y + extent};
    draw->AddRectFilled(origin, corner, backdrop_color(tint), pixels(kIconBackdropRounding));
    if (!preview::draw(item.iconTag, origin, extent)) {
        draw_icon_placeholder(item, origin, extent);
    }
    if (framed) {
        draw->AddRect(origin, corner, ImGui::GetColorU32(tint), pixels(kIconFrameRounding));
    }
}

/**
 * Pushes one heavier cut, or the regular face when the build ships none.
 * @return The weight the caller is still left to strike itself.
 */
[[nodiscard]] float push_weight(fonts::runtime::Weight requested,
                                float size,
                                float fallbackWeight) noexcept {
    ImFont* face = fonts::runtime::weight(requested);
    ImGui::PushFont(face, size);
    return face != nullptr ? 0.0F : fallbackWeight;
}

float push_title(float size, float fallbackWeight) noexcept {
    return push_weight(fonts::runtime::Weight::medium, size, fallbackWeight);
}

float push_figure(float size, float fallbackWeight) noexcept {
    return push_weight(fonts::runtime::Weight::displayBold, size, fallbackWeight);
}

void bold_text(const char* text, ImVec2 at, ImU32 color, float weight) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    if (weight <= 0.0F) {
        draw->AddText(at, color, text);
        return;
    }
    // Striking only to the right grew every glyph off its right edge alone: the upright of a digit
    // thickened while its bowl stayed thin, and the ink it gained came out of the gap before the
    // next figure, which read as a line of badly spaced numbers. The strikes are laid in a ring
    // around the glyph instead, so it thickens evenly and its own centre stays where it was.
    // The ring is centred half a weight in, which leaves the line occupying the same box a caller
    // reserved for it, rather than bleeding a half weight past its left edge.
    const float radius = weight * 0.5F;
    const ImVec2 centre{at.x + radius, at.y};
    const int strikes = (std::max)(1, static_cast<int>(radius / kBoldStrikeStep));
    for (int ring = 1; ring <= strikes; ++ring) {
        const float offset = radius * static_cast<float>(ring) / static_cast<float>(strikes);
        draw->AddText({centre.x - offset, centre.y}, color, text);
        draw->AddText({centre.x + offset, centre.y}, color, text);
        draw->AddText({centre.x, centre.y - offset}, color, text);
        draw->AddText({centre.x, centre.y + offset}, color, text);
    }
    draw->AddText(centre, color, text);
}

void clipped_text(const std::string& text,
                  ImVec2 at,
                  float width,
                  ImU32 color,
                  float weight) noexcept {
    std::string value = text;
    const float limit = width - pixels(kClipSlack);
    bool shortened = false;
    while (!value.empty() && ImGui::CalcTextSize(value.c_str()).x > limit) {
        std::size_t end = value.size() - 1;
        while (end != 0 && (static_cast<unsigned char>(value[end]) & kContinuationMask) == kContinuationTag) {
            --end;
        }
        value.resize(end);
        shortened = true;
    }
    if (shortened) {
        value += "...";
    }
    bold_text(value.c_str(), at, color, weight);
}

void collection_card(const edit::CatalogItem& item, float width) noexcept {
    const float height = pixels(kResultRowHeight);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(item.definition.definitionIndex));
    const bool clicked = ImGui::InvisibleButton("collection_item", {width, height});
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = internal::model().selection.holds(item.definition.definitionHash, 0);

    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner{origin.x + width, origin.y + height};
    const std::uint8_t tier = item.definition.tier;
    draw->AddRectFilled(origin, corner, ImGui::GetColorU32(rarity_color(tier)));
    if (hovered) {
        draw->AddRectFilled(origin, corner, ImGui::GetColorU32(kResultHoverWash));
    }
    icon(item, origin, height);

    // The name and the detail are centred as a pair against the icon, as they are on a card.
    const float textLeft = origin.x + height + pixels(kResultTextGap);
    const float textWidth = corner.x - pixels(kResultTextRightInset) - textLeft;
    const float nameSize = ImGui::GetStyle().FontSizeBase * kResultNameScale;
    const float nameWeight = push_title(nameSize, pixels(kResultNameWeight));
    const float nameLine = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const float line = ImGui::GetTextLineHeight();
    const float gap = pixels(kResultLineGap);
    float top = origin.y + ((height - nameLine - gap - line) * 0.5F);
    (void)push_title(nameSize, 0.0F);
    clipped_text(shout(item.name), {textLeft, top}, textWidth, band_text(tier), nameWeight);
    ImGui::PopFont();
    top += nameLine + gap;
    // The detail line is the type and the slot, or whichever of the two the item has.
    std::string detail = item.type;
    if (item.slot < inv::kEquipmentSlotCount) {
        detail += detail.empty() ? edit::kSlots[item.slot] : kDetailSeparator + std::string(edit::kSlots[item.slot]);
    }
    if (!detail.empty()) {
        clipped_text(detail, {textLeft, top}, textWidth, band_text(tier, true));
    }

    if (selected) {
        draw->AddRect(origin,
                      corner,
                      ImGui::GetColorU32(ImGuiCol_Text),
                      0.0F,
                      0,
                      pixels(kResultSelectedThickness));
    }
    if (hovered) {
        draw_card_tooltip(item, 0);
    }
    if (clicked) {
        internal::select(item);
    }
    ImGui::PopID();
}

} // namespace dawn::core::ui::modules::loadout::art
