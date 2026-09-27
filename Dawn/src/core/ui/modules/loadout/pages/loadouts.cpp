// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <utility>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../controls.h"
#include "../internal.h"
#include "../presets.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Popup id of the sheet, used by both the action that opens it and the sheet itself. */
constexpr const char* kLoadoutsTitle = "Saved loadouts";
/**
 * One loadout row, laid out as the game previews a loadout: the subclass stands as its emblem,
 * the name in the title cut beside it, and under the name the pieces it puts on, weapons then
 * armor, each framed in its rarity as the cards frame them.
 */
constexpr float kRowPadding = 7.0F;
constexpr float kEmblemExtent = 46.0F;
constexpr float kEmblemGap = 10.0F;
constexpr float kPieceExtent = 24.0F;
constexpr float kPieceGap = 3.0F;
constexpr float kPieceGroupGap = 10.0F;
constexpr float kLineGap = 5.0F;
/** Gap between the row's two quiet actions, and between the count and the row's word. */
constexpr float kActionGap = 6.0F;
constexpr float kWordGap = 10.0F;
/** A piece no longer available is veiled rather than left out, so the loadout still reads whole. */
constexpr ImVec4 kUnavailableVeil{0.0F, 0.0F, 0.0F, 0.62F};
/** The row's own word, set as a perk row sets its own: what pressing it does, or that it is done. */
constexpr const char* kEquipWord = "Equip";
constexpr const char* kEquippedWord = "Equipped";
/** The row's two quiet actions. Delete asks again, in place, before it deletes. */
constexpr const char* kUpdateLabel = "Update";
constexpr const char* kDeleteLabel = "Delete";
constexpr const char* kConfirmLabel = "Confirm";
/** Footer: the save action, the gap it keeps from the close, and the close the sheet ends on. */
constexpr float kSaveWidth = 140.0F;
constexpr float kFooterGap = 16.0F;
constexpr float kCloseWidth = 100.0F;
/** Height the list leaves under it for the footer, in lines, as the perk picker leaves its own. */
constexpr float kFooterLines = 1.6F;
/** 192 bytes hold any outcome the sheet reports, with a loadout name of the longest kind. */
constexpr std::size_t kMessageCapacity = 192;
/** 64 bytes hold the head's muted line or the count over the list. */
constexpr std::size_t kLineCapacity = 64;
/** Said when the file is there but could not be read, which is why nothing is saved over it. */
constexpr const char* kUnreadable =
    "The loadouts file could not be read, so nothing is saved over it.";
/** Said when the file could not be written. */
constexpr const char* kUnwritable = "Could not write the loadouts file.";
/** Said in place of the list while this character has saved nothing. */
constexpr const char* kEmpty = "No saved loadouts yet. Save what is equipped with the field below.";

/** What was pressed on one row this frame. */
enum class RowPress : std::uint8_t { none, equip, update, remove };

/** Reads the saved loadouts the first time the sheet needs them. */
void ensure_loaded() noexcept {
    Loadouts& loadouts = model().loadouts;
    if (loadouts.loaded) {
        return;
    }
    loadouts.loaded = true;
    loadouts.writable = presets::load(loadouts.entries);
}

/** Sets the bar's outcome line for a sheet action that reaches no game, so nothing overwrites it. */
void report(const char* text, bool failed) noexcept {
    Model& state = model();
    state.status = text;
    state.statusFailed = failed;
}

/** Writes the saved loadouts. @return False, with the reason in the bar, when they could not be. */
[[nodiscard]] bool persist() noexcept {
    Model& state = model();
    if (!state.loadouts.writable) {
        report(kUnreadable, true);
        return false;
    }
    if (!presets::save(state.loadouts.entries)) {
        report(kUnwritable, true);
        return false;
    }
    return true;
}

/** @return How many loadouts one character has saved. */
[[nodiscard]] std::size_t saved_by(std::uint64_t owner) noexcept {
    const Loadouts& loadouts = model().loadouts;
    return static_cast<std::size_t>(std::count_if(
        loadouts.entries.begin(), loadouts.entries.end(), [owner](const edit::SavedLoadout& entry) {
            return entry.character == owner;
        }));
}

/** Saves what the character has equipped now, under the typed name or a numbered one. */
void save_current() noexcept {
    Loadouts& loadouts = model().loadouts;
    const state::CharacterState& owner = character();
    std::string name = loadouts.name;
    const std::size_t first = name.find_first_not_of(" \t");
    const std::size_t last = name.find_last_not_of(" \t");
    name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    if (name.empty()) {
        name = "Loadout " + std::to_string(saved_by(owner.soid) + 1);
    }
    loadouts.entries.push_back(edit::capture_loadout(owner, name));
    if (!persist()) {
        loadouts.entries.pop_back();
        return;
    }
    loadouts.name[0] = '\0';
    char message[kMessageCapacity]{};
    (void)std::snprintf(message, sizeof message, "Saved %s.", name.c_str());
    report(message, false);
}

/** Replaces what one loadout holds with what is equipped now, keeping its name. */
void update_saved(edit::SavedLoadout& entry) noexcept {
    edit::SavedLoadout previous = entry;
    entry = edit::capture_loadout(character(), previous.name);
    if (!persist()) {
        entry = std::move(previous);
        return;
    }
    char message[kMessageCapacity]{};
    (void)std::snprintf(message, sizeof message, "Updated %s.", entry.name.c_str());
    report(message, false);
}

/** Deletes one loadout once its Delete has been confirmed. */
void delete_saved(std::size_t index) noexcept {
    Loadouts& loadouts = model().loadouts;
    const auto at = loadouts.entries.begin() + static_cast<std::ptrdiff_t>(index);
    edit::SavedLoadout removed = std::move(*at);
    loadouts.entries.erase(at);
    loadouts.pendingDelete = -1;
    if (!persist()) {
        loadouts.entries.insert(loadouts.entries.begin() + static_cast<std::ptrdiff_t>(index),
                                std::move(removed));
        return;
    }
    char message[kMessageCapacity]{};
    (void)std::snprintf(message, sizeof message, "Deleted %s.", removed.name.c_str());
    report(message, false);
}

/**
 * Equips one saved loadout and says what it came to. A piece the loadout could not put back, gone
 * from the character or from the build, is counted in the message rather than refusing the rest.
 * @return True when the sheet should close, so the outcome can be read in the bar behind it.
 */
bool equip_saved(const edit::SavedLoadout& loadout) noexcept {
    Model& state = model();
    edit::LoadoutResult result;
    std::string refused;
    const bool changed = edit::apply_loadout(
        *state.draft, state.catalog, state.character, loadout, result, refused);
    char message[kMessageCapacity]{};
    if (result.equipped == 0) {
        (void)std::snprintf(message,
                            sizeof message,
                            "None of %s's pieces are available on this character.",
                            loadout.name.c_str());
        report(message, true);
        return false;
    }
    if (result.unavailable == 0) {
        (void)std::snprintf(message,
                            sizeof message,
                            changed ? "Equipped %s." : "%s is already equipped.",
                            loadout.name.c_str());
    } else {
        (void)std::snprintf(message,
                            sizeof message,
                            "%s %s; %zu of %zu pieces are no longer available.",
                            changed ? "Equipped" : "Already equipped:",
                            loadout.name.c_str(),
                            result.unavailable,
                            result.saved);
    }
    if (!changed) {
        report(message, false);
        return true;
    }
    // The apply that publishes this writes its own outcome over the bar, so what the loadout came
    // to is carried in front of it rather than lost under it.
    state.status = message;
    state.editNote = message;
    record_edit(true);
    return true;
}

/**
 * Draws one saved piece: its icon framed in its rarity, veiled when the piece is no longer
 * available, or a recess where the slot was empty or the build no longer carries the item. Under
 * the pointer it shows the item's own tooltip, as a card does, from the character's own copy when
 * it still holds one, so the tooltip shows that copy's rolls.
 */
void draw_piece(const edit::SavedPiece& piece,
                edit::PieceState standing,
                ImVec2 at,
                float extent,
                bool rowHovered) noexcept {
    const Model& state = model();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner{at.x + extent, at.y + extent};
    const edit::CatalogItem* definition =
        standing == edit::PieceState::empty ? nullptr : state.catalog.find(piece.definition);
    if (definition == nullptr) {
        draw->AddRectFilled(at, corner, ImGui::GetColorU32(ImGuiCol_FrameBg), pixels(controls::kRowRounding));
        return;
    }
    art::icon(*definition, at, extent);
    if (standing == edit::PieceState::unavailable) {
        draw->AddRectFilled(at, corner, ImGui::GetColorU32(kUnavailableVeil));
    }
    if (rowHovered && ImGui::IsMouseHoveringRect(at, corner)) {
        const edit::Item* owned = find_owned_item(piece.instance);
        tooltip::draw(*definition,
                      owned != nullptr && owned->definitionHash == piece.definition ? owned : nullptr);
    }
}

/** @return The width a word button takes for its label, at the height of the line it sits on. */
[[nodiscard]] float word_width(const char* label) noexcept {
    return ImGui::CalcTextSize(label).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
}

/**
 * Draws one loadout row and reports what was pressed on it.
 * The whole row is one control, which equips the loadout, as a perk row applies its perk. Update
 * and Delete sit over the far end of the piece strip and take the pointer before the row does.
 * @param entry Saved loadout the row shows.
 * @param confirming True when this row's Delete was pressed once and now asks to be confirmed.
 * @param width Row width in framebuffer pixels.
 */
[[nodiscard]] RowPress draw_row(const edit::SavedLoadout& entry,
                                bool confirming,
                                float width) noexcept {
    const Model& state = model();
    const state::CharacterState& owner = character();
    const ImGuiStyle& style = ImGui::GetStyle();
    auto* draw = ImGui::GetWindowDrawList();
    const float padding = pixels(kRowPadding);
    const float emblem = pixels(kEmblemExtent);
    const float piece = pixels(kPieceExtent);
    const float rowHeight = emblem + (padding * 2.0F);

    // Where every piece stands, which both the strip and the row's word read. The preview and the
    // equip read a piece the same way, so the row never promises a piece the equip will skip.
    std::array<edit::PieceState, inv::kEquipmentSlotCount> standing{};
    std::size_t stowed = 0;
    std::size_t equipped = 0;
    std::size_t unavailable = 0;
    for (std::size_t slot = 0; slot < standing.size(); ++slot) {
        standing[slot] = edit::piece_state(owner, state.catalog, entry.pieces[slot]);
        stowed += standing[slot] == edit::PieceState::stowed ? 1U : 0U;
        equipped += standing[slot] == edit::PieceState::equipped ? 1U : 0U;
        unavailable += standing[slot] == edit::PieceState::unavailable ? 1U : 0U;
    }
    // Nothing left to put on: every piece still held is already on.
    const bool inPlace = stowed == 0 && equipped != 0;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("loadout", {width, rowHeight});
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddRectFilled(origin,
                            {origin.x + width, origin.y + rowHeight},
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
                            pixels(controls::kRowRounding));
    }
    draw->AddLine({origin.x, origin.y + rowHeight},
                  {origin.x + width, origin.y + rowHeight},
                  ImGui::GetColorU32(tooltip::rule_color()));

    // The subclass stands as the loadout's emblem, at the height of the row, as the band's icon
    // stands at the height of the band.
    draw_piece(entry.pieces[kSubclassSlot],
               standing[kSubclassSlot],
               {origin.x + padding, origin.y + padding},
               emblem,
               hovered);

    // The name and the strip hang as one block, centred on the emblem.
    const float textLeft = origin.x + padding + emblem + pixels(kEmblemGap);
    const float right = origin.x + width - padding;
    const float nameSize = style.FontSizeBase * controls::kSubheadingScale;
    const float nameWeight = art::push_title(nameSize, 0.0F);
    const float nameLine = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const float top = origin.y + ((rowHeight - (nameLine + pixels(kLineGap) + piece)) * 0.5F);

    // The row's word sits at the far end of the name line, on its baseline, as a perk row's does,
    // and what equipping would leave behind is said just before it, in the colour a pending value
    // takes. Both belong on this line: the strip under it keeps its far end for the two actions.
    const float baseline = top + (nameLine - ImGui::GetTextLineHeight());
    const char* word = inPlace ? kEquippedWord : kEquipWord;
    float end = right - ImGui::CalcTextSize(word).x;
    draw->AddText({end, baseline},
                  ImGui::GetColorU32(hovered && !inPlace ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                         : tooltip::muted()),
                  word);
    if (unavailable != 0) {
        char count[kLineCapacity]{};
        (void)std::snprintf(count, sizeof count, "%zu unavailable", unavailable);
        end -= pixels(kWordGap) + ImGui::CalcTextSize(count).x;
        draw->AddText({end, baseline}, ImGui::GetColorU32(tooltip::pending()), count);
    }
    (void)art::push_title(nameSize, 0.0F);
    art::clipped_text(art::shout(entry.name),
                      {textLeft, top},
                      (std::max)(0.0F, end - pixels(kWordGap) - textLeft),
                      ImGui::GetColorU32(ImGuiCol_Text),
                      nameWeight);
    ImGui::PopFont();

    // The strip: the three weapons, a gap, then the five armor pieces.
    const float stripTop = top + nameLine + pixels(kLineGap);
    float x = textLeft;
    for (std::size_t slot = 0; slot <= kLastArmorSlot; ++slot) {
        if (slot == kLastWeaponSlot + 1) {
            x += pixels(kPieceGroupGap) - pixels(kPieceGap);
        }
        draw_piece(entry.pieces[slot], standing[slot], {x, stripTop}, piece, hovered);
        x += piece + pixels(kPieceGap);
    }

    // Update and Delete, set as quiet words over the far end of the strip. Delete keeps its width
    // when it turns into Confirm, so nothing shifts under the pointer between the two presses.
    RowPress press = clicked ? RowPress::equip : RowPress::none;
    const float updateWidth = word_width(kUpdateLabel);
    const float deleteWidth = (std::max)(word_width(kDeleteLabel), word_width(kConfirmLabel));
    const float buttonHeight = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{style.FramePadding.x, 0.0F});
    ImGui::SetCursorScreenPos({right - deleteWidth - pixels(kActionGap) - updateWidth,
                               stripTop + ((piece - buttonHeight) * 0.5F)});
    ImGui::BeginDisabled(!model().loadouts.writable);
    if (ImGui::Button(kUpdateLabel, {updateWidth, buttonHeight})) {
        press = RowPress::update;
    }
    ImGui::SameLine(0.0F, pixels(kActionGap));
    if (ImGui::Button(confirming ? kConfirmLabel : kDeleteLabel, {deleteWidth, buttonHeight})) {
        press = RowPress::remove;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    // The cursor is put back under the row, and an item submitted there so the list measures it.
    ImGui::SetCursorScreenPos({origin.x, origin.y + rowHeight});
    ImGui::Dummy({width, 0.0F});
    return press;
}

/** Draws the footer: a name and the save that uses it, then the close the sheet ends on. */
[[nodiscard]] bool draw_footer() noexcept {
    Loadouts& loadouts = model().loadouts;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float save = pixels(kSaveWidth);
    const float closeWidth = pixels(kCloseWidth);
    if (!loadouts.writable) {
        ImGui::PushTextWrapPos(controls::kAutomaticWrapPosition);
        ImGui::TextColored(tooltip::pending(), "%s", kUnreadable);
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!loadouts.writable);
    ImGui::SetNextItemWidth((std::max)(0.0F,
                                       right - ImGui::GetCursorPosX() - save - closeWidth
                                           - style.ItemSpacing.x - pixels(kFooterGap)));
    // A text field reports Enter itself; it is only the number fields that cannot.
    const bool entered = ImGui::InputTextWithHint("##loadout_name",
                                                  "Name what is equipped now",
                                                  loadouts.name,
                                                  sizeof loadouts.name,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (controls::primary_button("Save current", {save, 0.0F}) || entered) {
        save_current();
    }
    ImGui::EndDisabled();
    // The one way out sits at the far end, as the game ends its sheets.
    ImGui::SameLine(right - closeWidth);
    return ImGui::Button("Close", {closeWidth, 0.0F});
}

} // namespace

void open_loadouts() noexcept {
    ensure_loaded();
    model().loadouts.pendingDelete = -1;
    ImGui::OpenPopup(kLoadoutsTitle);
}

void draw_loadouts_modal() noexcept {
    // The sheet the perk picker is, so the editor's two sheets are one frame, head, ground and size.
    if (!tooltip::begin_sheet(kLoadoutsTitle)) {
        return;
    }
    Model& state = model();
    Loadouts& loadouts = state.loadouts;
    const state::CharacterState& owner = character();
    const std::size_t held = saved_by(owner.soid);

    // The head names the sheet and the character it is for; the count heads the list, as the
    // picker counts its perks.
    char line[kLineCapacity]{};
    (void)std::snprintf(line,
                        sizeof line,
                        "%s %zu  /  %zu saved",
                        art::class_name(owner.characterClass),
                        state.character + 1,
                        held);
    tooltip::draw_sheet_head(kLoadoutsTitle, line);
    controls::space(controls::kSectionSpacing);
    (void)std::snprintf(line, sizeof line, "%zu %s", held, held == 1 ? "loadout" : "loadouts");
    ImGui::TextColored(tooltip::muted(), "%s", art::shout(line).c_str());
    controls::space(controls::kRuleSpacing);
    ImGui::Separator();

    bool close = false;
    int removal = -1;
    const float listHeight =
        (std::max)(ImGui::GetFrameHeight(),
                   ImGui::GetContentRegionAvail().y - (ImGui::GetFrameHeightWithSpacing() * kFooterLines)
                       - (loadouts.writable ? 0.0F : ImGui::GetTextLineHeightWithSpacing()));
    // The rows set their own gaps, as the tooltip's rows do.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0.0F});
    if (ImGui::BeginChild("loadout_rows", {0.0F, listHeight})) {
        const float width = ImGui::GetContentRegionAvail().x;
        for (std::size_t index = 0; index < loadouts.entries.size(); ++index) {
            edit::SavedLoadout& entry = loadouts.entries[index];
            if (entry.character != owner.soid) {
                continue;
            }
            ImGui::PushID(static_cast<int>(index));
            const RowPress press =
                draw_row(entry, loadouts.pendingDelete == static_cast<int>(index), width);
            ImGui::PopID();
            if (press == RowPress::equip) {
                close = equip_saved(entry) || close;
            } else if (press == RowPress::update) {
                update_saved(entry);
            } else if (press == RowPress::remove) {
                if (loadouts.pendingDelete == static_cast<int>(index)) {
                    removal = static_cast<int>(index);
                } else {
                    loadouts.pendingDelete = static_cast<int>(index);
                }
            }
        }
        if (held == 0) {
            const ImVec2 size = ImGui::CalcTextSize(kEmpty);
            const ImVec2 room = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos({ImGui::GetCursorPosX() + (std::max)(0.0F, (room.x - size.x) * 0.5F),
                                 ImGui::GetCursorPosY() + (std::max)(0.0F, (room.y - size.y) * 0.5F)});
            ImGui::TextColored(tooltip::muted(), "%s", kEmpty);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // The list is left alone while it is drawn, and a confirmed removal is taken out after it.
    if (removal >= 0) {
        delete_saved(static_cast<std::size_t>(removal));
    }

    controls::space(controls::kRowSpacing);
    close = draw_footer() || close;
    if (close || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    tooltip::end_sheet();
}

} // namespace dawn::core::ui::modules::loadout::internal
