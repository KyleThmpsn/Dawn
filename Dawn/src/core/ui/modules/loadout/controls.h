// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <imgui.h>

namespace dawn::core::ui::modules::loadout::controls {

/**
 * The page's own tokens. Every gap, radius and type size a loadout page draws by hand comes from
 * here, so one visual value is one constant rather than the same number typed in five files.
 * These are the gap ADDED beyond the ambient row spacing: `space()` suppresses only the spacing
 * after its own Dummy, so the preceding item has already advanced the cursor.
 */
/** The gap that hugs a horizontal rule, above it and below it. */
inline constexpr float kRuleSpacing = 3.0F;
/** One row gap, which is the page's own vertical item spacing, for a gap a layout adds itself. */
inline constexpr float kRowSpacing = 4.0F;
/** The gap between two stacked sections of a page. */
inline constexpr float kSectionSpacing = 6.0F;
/**
 * Corner radius of every card-like surface: item cards, result rows, stack rows and banners. The
 * game's surfaces are square-cornered, as its tooltip is.
 */
inline constexpr float kCardRounding = 0.0F;
/** Corner radius of the fill a list row shows under the pointer, square as the rows under it are. */
inline constexpr float kRowRounding = 0.0F;
/** Set between the parts of one detail line, such as an item's type and its slot, or a tag and its row. */
inline constexpr const char* kDetailSeparator = "  |  ";
/** Room a tab adds round its label, so the spaced capitals of neighbouring tabs read as separate words. */
inline constexpr float kTabPadding = 26.0F;
/** Width of every slot picker, which holds the longest slot name. */
inline constexpr float kSlotPickerWidth = 140.0F;
/** Width of the buttons a confirmation ends on and of the action bar's, so the pairs all match. */
inline constexpr float kConfirmButtonWidth = 92.0F;
/**
 * Gap between a field label column and the control beside it. Sundial uses 18.
 * `field_label` advances the cursor by exactly this, so a caller sizing a control from its group
 * width subtracts this same value.
 */
inline constexpr float kFieldColumnGap = 18.0F;
/** Height of a navigation tab row, which the shell centres the rest of the row against. */
inline constexpr float kTabHeight = 34.0F;
/** A zero wrap position wraps at the current content edge. */
inline constexpr float kAutomaticWrapPosition = 0.0F;
/** Type ramp, in multiples of the body size. Sundial's heading is one third larger than body. */
inline constexpr float kHeadingScale = 1.35F;
/** An item name inside a pane, which sits between the heading and the body. */
inline constexpr float kSubheadingScale = 1.15F;
/** Colour of an outcome the game or the editor refused, and of an action that cannot be taken back. */
inline constexpr ImVec4 kRefusedColor{0.94F, 0.48F, 0.42F, 1.0F};
/** Colour of a value waiting on the game, such as unapplied edits or a target a roll has not reached. */
inline constexpr ImVec4 kPendingColor{0.88F, 0.76F, 0.47F, 1.0F};
/**
 * Width of every rail and mark the editor draws beside or under something to say what it is: the
 * rail beside a chosen row, under an open tab or the character in play, and a chosen card's outline.
 */
inline constexpr float kRailWidth = 2.0F;
/** Gap that sets one group of controls on a row apart from the next, wider than the gap inside one. */
inline constexpr float kGroupGap = 14.0F;
/**
 * Height of the actions a pane or a sheet is for, such as adding an item or equipping a loadout: a
 * target above the 24 pixels a pointer needs, set apart from the page's shorter field controls.
 */
inline constexpr float kActionHeight = 28.0F;

/**
 * Adds vertical space without the surrounding item spacing doubling it.
 * @param authoredPixels Gap height before display scaling, from the tokens above.
 */
void space(float authoredPixels) noexcept;

/**
 * Draws a button in the accent color, for the one action a view is built around.
 * @param label Visible label with an optional Dear ImGui ID suffix.
 * @param size Final framebuffer size, following Dear ImGui button conventions.
 * @return True when the button fires.
 */
[[nodiscard]] bool primary_button(const char* label, ImVec2 size) noexcept;

/**
 * Draws a checkbox in the game's own box and tick, with its label after it, in place of Dear
 * ImGui's drawn one.
 * @param label Visible label with an optional Dear ImGui ID suffix.
 * @param value The state, flipped when the box is pressed.
 * @return True when the box was pressed this frame.
 */
bool checkbox(const char* label, bool* value) noexcept;

/**
 * Draws a flat navigation tab, set in the game's spaced capitals and underlined while it owns
 * the page.
 * @param label Visible label, folded to capitals here.
 * @param active True when this tab owns the current content.
 * @param width Final framebuffer width.
 * @return True when the tab fires.
 */
[[nodiscard]] bool tab(const char* label, bool active, float width) noexcept;

/** @return The width one tab's label takes in the spaced capitals `tab` sets it in. */
[[nodiscard]] float tab_width(const char* label) noexcept;

/**
 * Sets words in the game's spaced capitals, as its tabs, headings and status words are set, into
 * the current window's draw list. The cursor does not move.
 * @param text Words, folded to capitals here.
 * @param at Top-left of the run in screen space.
 * @param color Packed text colour.
 * @return The width the run took, in framebuffer pixels.
 */
float spaced(const char* text, ImVec2 at, ImU32 color) noexcept;

/** @return The width `spaced` would take for this text, without drawing it. */
[[nodiscard]] float spaced_width(const char* text) noexcept;

/**
 * Draws a page heading in the game's spaced capitals, one line tall, and advances past it.
 * @param text Heading, folded to capitals here.
 */
void heading(const char* text) noexcept;

/**
 * Draws a section heading over a run of rows, as the game heads a column of its inventory: the
 * name in muted capitals, the count after it, and a rule under both. Pressing it folds the
 * section, and the state is kept by the heading's own id.
 * @param label Section name, folded to capitals here.
 * @param count Rows the section holds.
 * @return True while the section is open.
 */
[[nodiscard]] bool section_header(const char* label, std::size_t count) noexcept;

/**
 * Opens a picker: a flat field showing the chosen value with a chevron at its end, which drops a
 * dark list. Every selector on the page is one of these, so they all read the same.
 * Between this and `end_picker` the caller draws its rows with `picker_row`.
 * @param id Stable Dear ImGui id.
 * @param preview Value shown in the field.
 * @param width Field width in framebuffer pixels.
 * @return True while the list is open. `end_picker` is called only when it is.
 */
[[nodiscard]] bool begin_picker(const char* id, const char* preview, float width) noexcept;

/** Closes a list opened by `begin_picker`. */
void end_picker() noexcept;

/**
 * Opens a menu, such as a card's right-click menu: the popup ground in a hairline, close round its
 * words, with each row lit from edge to edge under the pointer. Every menu on the page is one of
 * these, so they all read the same.
 * @param id Popup id, which the caller opens with `ImGui::OpenPopup` from the same scope.
 * @return True while the menu is open. `end_menu` is called only when it is.
 */
[[nodiscard]] bool begin_menu(const char* id) noexcept;

/** Closes a menu opened by `begin_menu`. */
void end_menu() noexcept;

/**
 * Draws a row inside a menu that opens a menu of its own beside it, in the same frame.
 * @param label Row label, which is also its id.
 * @return True while the submenu is open. `end_submenu` is called only when it is.
 */
[[nodiscard]] bool begin_submenu(const char* label) noexcept;

/** Closes a submenu opened by `begin_submenu`. */
void end_submenu() noexcept;

/**
 * Draws one row of an open picker list, with a rail beside the chosen one.
 * @param label Row text.
 * @param selected True for the row that is the current value.
 * @return True when the row is pressed. The list closes on its own.
 */
[[nodiscard]] bool picker_row(const char* label, bool selected) noexcept;

/**
 * Draws a whole picker over a fixed list of labels.
 * @param id Stable Dear ImGui id.
 * @param index Chosen row, updated when another is pressed.
 * @param items Row labels.
 * @param count Number of labels.
 * @param width Field width in framebuffer pixels.
 * @return True when the choice changed this frame.
 */
bool picker(const char* id, int& index, const char* const* items, int count, float width) noexcept;

/** The same over a null-separated list, as ImGui::Combo takes one. */
bool picker(const char* id, int& index, const char* items, float width) noexcept;

/**
 * Draws a search box with a hint, sized by the caller.
 * @param id Stable Dear ImGui scope ID.
 * @param hint Hint shown while the buffer is empty.
 * @param buffer Caller-owned null-terminated text storage.
 * @param capacity Writable buffer size in bytes.
 * @return True when this frame changed the buffer.
 */
bool search(const char* id, const char* hint, char* buffer, std::size_t capacity) noexcept;

/**
 * Draws a field label to the left of the control that follows, right-aligned in a fixed column.
 * Dear ImGui puts a widget's label after it, which reads as "Hunter (v) Class" and leaves the
 * labels ragged. Sundial right-aligns them into a label column instead, so the controls line up.
 * @param text Visible label.
 * @param width Label column width in framebuffer pixels.
 */
void field_label(const char* text, float width) noexcept;

/** @return The height of a Dear ImGui small button, in framebuffer pixels. */
[[nodiscard]] float small_button_height() noexcept;

/**
 * Centers the control about to be drawn against the heading beside it.
 * Call it straight after `SameLine`, while the heading is still the last item. A heading is taller
 * than a control, so without this the control sits against the top of the row.
 * @param controlHeight Height of the control that follows, in framebuffer pixels.
 */
void align_to_title(float controlHeight) noexcept;

/**
 * Draws one line of text at a larger size than the body.
 * @param text Text to draw.
 * @param scale Multiplier over the base font size.
 */
void title(const char* text, float scale) noexcept;

/**
 * Draws a full-width banner in the caller's color, used for warnings the page cannot ignore.
 * @param color Text and border color.
 * @param text Message, wrapped to the content width.
 */
void banner(const ImVec4& color, const char* text) noexcept;

/** How a confirmation was answered this frame. */
enum class Answer : unsigned char { none, confirm, cancel };

/**
 * Draws the two buttons every confirmation ends on: its action as the primary button, then Cancel,
 * both `kConfirmButtonWidth` wide. Enter answers with the action and Escape with Cancel.
 * @param action The action's label, such as "Remove".
 * @param enabled False when the action cannot be taken, which leaves Cancel the only answer.
 * @param destructive True for an action nothing can take back, which is set in kRefusedColor.
 */
[[nodiscard]] Answer confirm_footer(const char* action, bool enabled = true, bool destructive = false) noexcept;

} // namespace dawn::core::ui::modules::loadout::controls
