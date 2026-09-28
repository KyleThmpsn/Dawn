// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <imgui.h>

#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "art.h"
#include "controls.h"
#include "internal.h"
#include "tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/** View tabs, in the order of the View values. Sundial's own view names. */
constexpr const char* kViewLabels[]{"Characters & Loadouts",
                                    "Character Inventory",
                                    "Profile Inventory",
                                    "Armory",
                                    "Icons"};
static_assert(std::size(kViewLabels) == static_cast<std::size_t>(View::count),
              "Every view needs a tab.");

/**
 * The page runs tighter than the shared Dawn theme, which is sized for settings pages.
 * A grid of item cards reads as padding-heavy at those values, so the page pushes its own.
 * The vertical item spacing is controls::kRowSpacing, so a gap a layout adds by hand matches it.
 */
constexpr float kItemSpacingX = 6.0F;
constexpr float kFramePaddingX = 6.0F;
constexpr float kFramePaddingY = 2.0F;
constexpr float kItemInnerSpacing = 4.0F;

/** Width every action bar button shares. The buttons, and the gaps between them, are reserved on the right. */
constexpr float kBarButtonWidth = 92.0F;
/** The two history actions, set as words ahead of the live toggle, and the gap that sets them off. */
constexpr const char* kUndoLabel = "Undo";
constexpr const char* kRedoLabel = "Redo";
/** 160 bytes hold a history action's tooltip with the characters it would change. */
constexpr std::size_t kHistoryTipCapacity = 160;
/** Label of the live-apply toggle. It is measured rather than reserved at a guessed width. */
constexpr const char* kLiveToggleLabel = "Apply Live";
/** Rail under the character tab the game currently has in play. */
constexpr float kCharacterRailInset = 6.0F;
/** Extra width one view tab takes beyond its label. */
constexpr float kTabPadding = 26.0F;
/** Grab width of the workspace splitter, and the width of the rail drawn inside it on hover. */
constexpr float kSplitterWidth = 6.0F;
/** The shadow the side workspace throws onto the page under its left edge. */
constexpr float kWorkspaceShadowWidth = 18.0F;
constexpr float kWorkspaceShadowAlpha = 0.45F;
/**
 * The page's own widget palette: the game's flat, near-black fields and buttons with a hairline
 * edge, in place of the rounded, filled controls of the shared Dawn theme. Pushed over the
 * theme for the whole page, so every field, combo, button and popup on it takes the same look.
 */
constexpr ImVec4 kFieldFill{1.0F, 1.0F, 1.0F, 0.06F};
constexpr ImVec4 kFieldFillHovered{1.0F, 1.0F, 1.0F, 0.12F};
constexpr ImVec4 kFieldFillActive{1.0F, 1.0F, 1.0F, 0.18F};
constexpr ImVec4 kButtonFill{1.0F, 1.0F, 1.0F, 0.09F};
constexpr ImVec4 kButtonFillHovered{1.0F, 1.0F, 1.0F, 0.22F};
constexpr ImVec4 kButtonFillActive{1.0F, 1.0F, 1.0F, 0.32F};
constexpr ImVec4 kHairline{1.0F, 1.0F, 1.0F, 0.22F};
constexpr ImVec4 kPopupGround{0.09F, 0.09F, 0.10F, 0.98F};
constexpr ImVec4 kModalDim{0.0F, 0.0F, 0.0F, 0.55F};
constexpr ImVec4 kChoiceFill{1.0F, 1.0F, 1.0F, 0.16F};
constexpr ImVec4 kChoiceFillHovered{1.0F, 1.0F, 1.0F, 0.10F};
constexpr ImVec4 kChoiceFillActive{1.0F, 1.0F, 1.0F, 0.24F};
constexpr float kHairlineWidth = 1.0F;
/** Progress bar width on the loading view. */
constexpr float kProgressWidth = 360.0F;
/** The catalog worker reports whole percent, which the progress bar takes as a fraction. */
constexpr float kPercentScale = 100.0F;
/** Title of the discard confirmation, used by both the action and its modal. */
constexpr const char* kDiscardTitle = "Discard Edits?";
/** Color of the unapplied-edits marker and of the divergence banner. */
constexpr ImVec4 kPendingColor = controls::kPendingColor;
/** Color of an outcome the game or the editor refused, set apart from an ordinary result. */
constexpr ImVec4 kRefusedColor = controls::kRefusedColor;
/**
 * Draws the view tabs across the top, in place of a navigation sidebar.
 * @return The window X where the row of tabs ends, so a caller can fill the rest of the row.
 */
float draw_view_tabs() noexcept {
    Model& state = model();
    float end = ImGui::GetCursorPosX();
    for (std::size_t i = 0; i < std::size(kViewLabels); ++i) {
        if (i != 0) {
            ImGui::SameLine();
            end += ImGui::GetStyle().ItemSpacing.x;
        }
        const float width = controls::tab_width(kViewLabels[i]) + pixels(kTabPadding);
        if (controls::tab(kViewLabels[i], state.view == static_cast<View>(i), width)) {
            state.view = static_cast<View>(i);
        }
        end += width;
    }
    return end;
}

/** @return The width the character tabs need, so the shell can right-align them in its row. */
[[nodiscard]] float character_tabs_width() noexcept {
    const state::AccountState& account = model().draft->after;
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0F;
    float total = 0.0F;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        total += ImGui::CalcTextSize(character_label(i).c_str()).x + padding;
        if (i != 0) {
            total += ImGui::GetStyle().ItemSpacing.x;
        }
    }
    return total;
}

/**
 * Draws the character tabs.
 * @param inlineRow True when they are riding the view tab row, which is taller than a Selectable,
 *        so they centre on it instead of sitting on its top edge.
 */
void draw_character_tabs(bool inlineRow) noexcept {
    Model& state = model();
    const state::AccountState& account = state.draft->after;
    const float lift =
        inlineRow ? (pixels(controls::kTabHeight) - ImGui::GetFrameHeight()) * 0.5F : 0.0F;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        const std::string label = character_label(i);
        if (i != 0) {
            ImGui::SameLine();
        }
        // ItemSize stores the row's own origin in CursorPosPrevLine, so every tab lifts from the
        // same baseline and the lift does not accumulate across the row.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
        ImGui::PushID(static_cast<int>(i));
        const float width = ImGui::CalcTextSize(label.c_str()).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
        if (ImGui::Selectable(label.c_str(), i == state.character, 0, {width, 0.0F})) {
            state.character = i;
            state.selection = {};
            state.browse.type.clear();
            state.results.key.clear();
        }
        // In play is a rail, being edited is the Selectable fill: two orthogonal states, and every
        // label stays at full contrast. Accent text on the window ground is ~3.4:1, the worst in
        // the row, on exactly the tab the player most needs to find.
        if (account.characters[i].selected) {
            const ImVec2 lo = ImGui::GetItemRectMin();
            const ImVec2 hi = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                {lo.x + pixels(kCharacterRailInset), hi.y - pixels(controls::kRailWidth)},
                {hi.x - pixels(kCharacterRailInset), hi.y},
                ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
        ImGui::PopID();
    }
}

/** @return True when the active view edits one character, which the character tabs pick. */
[[nodiscard]] bool view_has_characters() noexcept {
    const View view = model().view;
    return view == View::characters || view == View::characterInventory;
}

/**
 * Draws the character tabs, riding the view tab row when that row has the width for them.
 * Three short labels did not need a row of their own; they take one only if this one fills.
 * @param tabsEnd The window X where the view tabs end, from `draw_view_tabs`.
 */
void draw_character_tabs_row(float tabsEnd) noexcept {
    if (!view_has_characters()) {
        return;
    }
    const float row = character_tabs_width();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const bool shares = right - row > tabsEnd + ImGui::GetStyle().ItemSpacing.x;
    if (shares) {
        ImGui::SameLine(right - row);
    } else {
        controls::space(controls::kRowSpacing);
    }
    draw_character_tabs(shares);
}

/**
 * Draws one history action: its word, disabled while there is nothing to retrace, and under the
 * pointer what it would change and the key that does the same.
 * @param forward False for Undo, true for Redo.
 */
void draw_history_action(bool forward) noexcept {
    const std::string subject = history_subject(forward);
    ImGui::BeginDisabled(subject.empty());
    // The bar's buttons share one width, so the row reads as one set of controls.
    if (ImGui::Button(forward ? kRedoLabel : kUndoLabel, {pixels(kBarButtonWidth), 0.0F})) {
        retrace_edit(forward);
    }
    ImGui::EndDisabled();
    if (!subject.empty() && ImGui::IsItemHovered()) {
        char tip[kHistoryTipCapacity]{};
        (void)std::snprintf(tip,
                            sizeof tip,
                            forward ? "Redo the change to %s  (Ctrl+Y)" : "Undo the last change to %s  (Ctrl+Z)",
                            subject.c_str());
        ImGui::SetTooltip("%s", tip);
    }
}

/**
 * Takes the history keys: Ctrl+Z undoes, Ctrl+Y or Ctrl+Shift+Z redoes. A field being typed in keeps
 * them for its own text, and a control still held is an edit not yet recorded.
 */
void take_history_keys() noexcept {
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
        return;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        retrace_edit(false);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)
               || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
        retrace_edit(true);
    }
}

/** Draws the action bar: the state and last outcome left, the apply actions right. */
void draw_action_bar() noexcept {
    Model& state = model();
    ImGui::Separator();
    controls::space(controls::kRuleSpacing);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    // With live apply on and nothing waiting, the bar is the toggle alone: every edit has already
    // gone to the game, so there is nothing to apply and nothing to reload. The two actions come
    // back when live apply is off, or when an edit is still waiting on the game, which is also
    // the one state worth a word about.
    const bool manual = !state.applyInstantly || state.draft->dirty;
    const float actionsWidth =
        manual ? (pixels(kBarButtonWidth) * 2.0F) + (style.ItemSpacing.x * 2.0F) : 0.0F;
    // Measured, not reserved: the installed Destiny face decides how wide the label runs, and a
    // guessed 104 let an over-wide checkbox overdraw the Reload button beside it.
    const float toggleWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x
                              + ImGui::CalcTextSize(kLiveToggleLabel).x;
    // Undo and Redo lead the toggle, set off from it, so the three read as two groups.
    const float historyWidth = (pixels(kBarButtonWidth) * 2.0F) + style.ItemSpacing.x + pixels(controls::kGroupGap);
    ImGui::AlignTextToFramePadding();
    if (state.draft->dirty) {
        ImGui::TextColored(kPendingColor, "Unsaved Changes");
    } else {
        ImGui::Dummy({0.0F, ImGui::GetFrameHeight()});
    }
    // What the last edit or apply came to. Without it a refused edit was indistinguishable from one
    // still waiting to go: both left only "Unsaved Changes" with nothing to say why. A refusal is
    // set in its own colour, and the whole message is under the pointer when the row clips it.
    if (!state.status.empty()) {
        ImGui::SameLine();
        const float room = (std::max)(0.0F,
                                      right - actionsWidth - toggleWidth - historyWidth
                                          - style.ItemSpacing.x - ImGui::GetCursorPosX());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        art::clipped_text(state.status,
                          {at.x, at.y + style.FramePadding.y},
                          room,
                          state.statusFailed ? ImGui::GetColorU32(kRefusedColor)
                                             : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::Dummy({room, ImGui::GetFrameHeight()});
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", state.status.c_str());
        }
    }

    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(), right - actionsWidth - toggleWidth - historyWidth));
    draw_history_action(false);
    ImGui::SameLine();
    draw_history_action(true);
    ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
    bool live = state.applyInstantly;
    if (ImGui::Checkbox(kLiveToggleLabel, &live)) {
        state.applyInstantly = live;
        // Turning it on adopts the draft the player built while it was off.
        if (live && state.draft->dirty) {
            state.applyRequested = true;
        }
    }
    if (!manual) {
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Reload", {pixels(kBarButtonWidth), 0.0F})) {
        if (state.draft->dirty) {
            ImGui::OpenPopup(kDiscardTitle);
        } else {
            reload_account();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.draft->dirty);
    if (controls::primary_button("Apply", {pixels(kBarButtonWidth), 0.0F})) {
        state.applyRequested = true;
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopupModal(kDiscardTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::Button("Discard and Reload")) {
            reload_account();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Editing")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/** Draws the selected view's body. */
void draw_active_view() noexcept {
    switch (model().view) {
    case View::characters:
        if (ImGui::BeginChild("characters_scroll")) {
            draw_character_fields();
            draw_equipment();
        }
        ImGui::EndChild();
        break;
    case View::characterInventory:
        draw_character_inventory_page();
        break;
    case View::profileInventory:
        draw_profile_inventory_page();
        break;
    case View::armory:
        draw_armory_page();
        break;
    case View::icons:
        draw_icons_page();
        break;
    case View::count:
        break;
    }
}

/** @return True when the active view has an item detail workspace to show. */
[[nodiscard]] bool view_has_inspector() noexcept {
    const View view = model().view;
    return view == View::characters || view == View::armory || view == View::characterInventory;
}

/** Draws the warning shown when the game changed the account under unapplied edits. */
void draw_divergence_banner() noexcept {
    if (!model().accountDiverged) {
        return;
    }
    controls::banner(kPendingColor,
                     "Account changed in game. Reloading discards your unapplied edits.");
    controls::space(controls::kRowSpacing);
}

/**
 * Draws the draggable edge of the side workspace at the cursor, and the shadow the workspace
 * throws onto the page it lies over.
 */
void draw_splitter(float height, float lowerBound, float upperBound) noexcept {
    Model& state = model();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float shadow = pixels(kWorkspaceShadowWidth);
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        {at.x + pixels(kSplitterWidth) - shadow, at.y},
        {at.x + pixels(kSplitterWidth), at.y + height},
        ImGui::GetColorU32({0.0F, 0.0F, 0.0F, 0.0F}),
        ImGui::GetColorU32({0.0F, 0.0F, 0.0F, kWorkspaceShadowAlpha}),
        ImGui::GetColorU32({0.0F, 0.0F, 0.0F, kWorkspaceShadowAlpha}),
        ImGui::GetColorU32({0.0F, 0.0F, 0.0F, 0.0F}));
    ImGui::InvisibleButton("workspace_splitter", {pixels(kSplitterWidth), height});
    if (ImGui::IsItemActive()) {
        // Dragging right narrows the workspace, because the page owns the left side.
        state.inspectorWidth =
            std::clamp(state.inspectorWidth - ImGui::GetIO().MouseDelta.x, lowerBound, upperBound);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        // Both widths scale, so the rail keeps its share of the grab band at every DPI.
        const float inset = (pixels(kSplitterWidth) - pixels(controls::kRailWidth)) * 0.5F;
        ImGui::GetWindowDrawList()->AddRectFilled({lo.x + inset, lo.y},
                                                  {hi.x - inset, hi.y},
                                                  ImGui::GetColorU32(ImGuiCol_SeparatorActive));
    }
}

/** Opens a workspace frame with Sundial's margins. */
[[nodiscard]] bool begin_workspace(const char* id, ImVec2 size) noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{pixels(kWorkspaceMarginX), pixels(kWorkspaceMarginY)});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    return ImGui::BeginChild(
        id, size, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}

/** Closes a workspace frame opened by `begin_workspace`. */
void end_workspace() noexcept {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/** @return The height Sundial gives the bottom workspace for the space available. */
[[nodiscard]] float bottom_workspace_height(float available) noexcept {
    const float maximum =
        (std::max)(available * kBottomWorkspaceMaximumFraction, pixels(kBottomWorkspaceFloor));
    const float preferred = std::clamp(available * kBottomWorkspacePreferredFraction,
                                       pixels(kBottomWorkspaceMinimum),
                                       pixels(kBottomWorkspaceMaximum));
    return (std::min)(maximum, preferred);
}

} // namespace

void draw_sheet_outcome(const char* forced) noexcept {
    const Model& state = model();
    const std::string outcome = forced != nullptr                     ? std::string(forced)
                                : state.status != state.statusAtSheet ? state.status
                                                                      : std::string();
    const bool refused = forced != nullptr || state.statusFailed;
    const float width = ImGui::GetContentRegionAvail().x;
    art::clipped_text(outcome,
                      ImGui::GetCursorScreenPos(),
                      width,
                      ImGui::GetColorU32(refused ? controls::kRefusedColor : tooltip::muted()));
    ImGui::Dummy({width, ImGui::GetTextLineHeight()});
    if (!outcome.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", outcome.c_str());
    }
}

float overlay_width() noexcept {
    const Model& state = model();
    const bool shown = view_has_inspector() && state.selection.definitionHash != 0;
    if (!shown || ImGui::GetContentRegionAvail().x < pixels(kSideWorkspaceBreakpoint)) {
        return 0.0F;
    }
    return state.inspectorWidth + pixels(kSplitterWidth);
}

namespace {

/** Draws the page body, and beside or below it the item workspace. */
void draw_workspace(float height) noexcept {
    Model& state = model();
    const bool showInspector = view_has_inspector() && state.selection.definitionHash != 0;
    const float available = ImGui::GetContentRegionAvail().x;

    if (!showInspector) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
        if (ImGui::BeginChild("page", {0.0F, height})) {
            draw_active_view();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    if (available < pixels(kSideWorkspaceBreakpoint)) {
        const float workspaceHeight = bottom_workspace_height(height);
        const float pageHeight =
            (std::max)(0.0F, height - workspaceHeight - ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
        if (ImGui::BeginChild("page", {0.0F, pageHeight})) {
            draw_active_view();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (begin_workspace("item_workspace", {0.0F, workspaceHeight})) {
            draw_inspector();
        }
        end_workspace();
        return;
    }

    const float upperBound = std::clamp(available - pixels(kPrimaryWorkspaceMinWidth),
                                        pixels(kSideWorkspaceMinWidth),
                                        pixels(kSideWorkspaceMaxWidth));
    const float lowerBound = (std::min)(pixels(kSideWorkspaceMinWidth), upperBound);
    if (state.inspectorWidth <= 0.0F) {
        state.inspectorWidth = pixels(kSideWorkspaceDefaultWidth);
    }
    state.inspectorWidth = std::clamp(state.inspectorWidth, lowerBound, upperBound);

    // The page keeps the whole width and the workspace is laid over its right edge, as the game
    // lays an item's detail over the inventory behind it. The workspace is submitted after the
    // page, which puts it in front for both drawing and the pointer.
    const ImVec2 top = ImGui::GetCursorPos();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
    if (ImGui::BeginChild("page", {0.0F, height})) {
        draw_active_view();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SetCursorPos({top.x + available - state.inspectorWidth - pixels(kSplitterWidth), top.y});
    draw_splitter(height, lowerBound, upperBound);
    ImGui::SameLine(0.0F, 0.0F);
    if (begin_workspace("item_workspace", {0.0F, height})) {
        draw_inspector();
    }
    end_workspace();
}

/** Draws the catalog progress view, which owns the frame until the catalog is ready. */
void draw_catalog_progress(CatalogPhase phase) noexcept {
    Model& state = model();
    if (phase == CatalogPhase::loading) {
        // The bar sits in the middle of the page, which is otherwise empty while the catalog loads.
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float width = pixels(kProgressWidth);
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + ((available.x - width) * 0.5F),
                             ImGui::GetCursorPosY() + ((available.y - ImGui::GetFrameHeight()) * 0.5F)});
        ImGui::ProgressBar(static_cast<float>(state.loadProgress.load()) / kPercentScale,
                           {width, 0.0F});
        return;
    }
    ImGui::TextWrapped("%s", state.loadError.c_str());
    controls::space(controls::kRowSpacing);
    if (ImGui::Button("Retry")) {
        start_catalog_load();
    }
}

/** Draws the message shown while the account has no character to edit. */
void draw_empty_account() noexcept {
    ImGui::TextDisabled("No Characters");
    controls::space(controls::kRowSpacing);
    if (ImGui::Button("Reload Account")) {
        reload_account();
    }
}

/** Draws the whole page, under the page's own spacing. */
void draw_page() noexcept {
    Model& state = model();
    const CatalogPhase phase = state.phase.load(std::memory_order_acquire);
    if (phase == CatalogPhase::idle) {
        start_catalog_load();
        return;
    }
    if (phase != CatalogPhase::ready) {
        draw_catalog_progress(phase);
        return;
    }
    if (!state.draft) {
        reload_account();
    }
    if (state.draft->after.characterCount == 0) {
        draw_empty_account();
        return;
    }

    const float tabsEnd = draw_view_tabs();
    draw_character_tabs_row(tabsEnd);
    controls::space(controls::kRowSpacing);
    draw_divergence_banner();

    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    // The gap under the body, the separator (a hairline, which takes no layout height at all in
    // 1.92.6), the gap under it, the bar padding and one row of controls.
    const float barHeight =
        ImGui::GetFrameHeight() + pixels(controls::kRuleSpacing) + (spacing * 2.0F);
    const float bodyHeight =
        (std::max)(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y - barHeight);
    draw_workspace(bodyHeight);
    // The modals belong to the page window, the one scope every card and the pane can reach.
    draw_perk_picker();
    draw_removal_confirm();
    draw_action_bar();
    take_history_keys();
}

} // namespace

void card_grid(float available,
               float spacing,
               float minimumWidth,
               int& columns,
               float& cardWidth) noexcept {
    const float room = (std::max)(available, 0.0F);
    const float minimum = (std::max)(minimumWidth, 1.0F);
    columns = (std::max)(1, static_cast<int>((room + spacing) / (minimum + spacing)));
    const float totalSpacing = spacing * static_cast<float>(columns - 1);
    cardWidth = (std::max)((room - totalSpacing) / static_cast<float>(columns), 0.0F);
}

void draw() noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2{pixels(kItemSpacingX), pixels(controls::kRowSpacing)});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2{pixels(kFramePaddingX), pixels(kFramePaddingY)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
                        ImVec2{pixels(kItemInnerSpacing), pixels(kItemInnerSpacing)});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, pixels(kHairlineWidth));
    // Modals opened from the page are square dark sheets with a flat title, as the picker is.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, pixels(kHairlineWidth));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kPopupGround);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kPopupGround);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, kPopupGround);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, kModalDim);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kFieldFill);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kFieldFillHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kFieldFillActive);
    ImGui::PushStyleColor(ImGuiCol_Button, kButtonFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kButtonFillHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonFillActive);
    ImGui::PushStyleColor(ImGuiCol_Border, kHairline);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPopupGround);
    ImGui::PushStyleColor(ImGuiCol_Header, kChoiceFill);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kChoiceFillHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kChoiceFillActive);
    draw_page();
    ImGui::PopStyleColor(15);
    ImGui::PopStyleVar(8);
}

} // namespace dawn::core::ui::modules::loadout::internal
