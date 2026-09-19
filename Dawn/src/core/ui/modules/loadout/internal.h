// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "state/editor/edit.h"

namespace dawn::core::ui::modules::loadout::internal {

namespace edit = state::editor;

/** 160 bytes hold any search a player types, including a full item name. */
inline constexpr std::size_t kSearchCapacity = 160;
/** The randomizer offers one checkbox per semantic equipment slot. */
inline constexpr std::size_t kSlotCount = 16;
static_assert(kSlotCount == state::account::inventory::kEquipmentSlotCount,
              "The randomizer slot mask must cover every equipment slot the account carries.");
/** Slot 11 is the subclass. It is named here because three pages reason about it. */
inline constexpr std::size_t kSubclassSlot = 11;
/** Slots 0 to 2 are weapons, 3 to 7 armor; the rest are cosmetic. Used to jump to a category. */
inline constexpr std::size_t kLastWeaponSlot = 2;
inline constexpr std::size_t kLastArmorSlot = 7;
/** Highest authored item level, which the game shows as ten times this. */
inline constexpr int kMaximumItemLevel = 9999;
/** Range the power field offers, which covers every installed reward tier. */
inline constexpr int kPowerSliderMaximum = 15000;

/**
 * @return The Power the game shows for one authored item level.
 * The account stores a level and the game displays ten Power per level, with a floor. Editing the
 * stored level directly showed 106 where the game shows 1060.
 * @param level Authored item level.
 */
[[nodiscard]] int power_of(int level) noexcept;

/**
 * @return The authored level that displays as the given Power.
 * @param power Power the player typed.
 */
[[nodiscard]] int level_of(int power) noexcept;
/** Armor stat plugs never exceed this, so the target sliders stop there. */
inline constexpr int kMaximumStatTarget = 50;

/**
 * Layout taken from Sundial, so the in-game editor reads as the same tool.
 * Sundial is an egui desktop app; these are its own authored values, scaled here for the display
 * like every other Dawn measurement. Names follow Sundial's.
 */
/**
 * At or above this width the inspector is a side workspace; below it, a bottom strip.
 * Sundial's own numbers were 980/540/420/760 over a 500-pixel page, sized for a workspace that
 * held the whole item editor. Power, lock, equip and the socket lanes have since moved on to the
 * card, leaving a header and a short column of actions, so these are re-cut for a slim pane and
 * for a page that must keep whole card columns rather than a band of dead space. The floor still
 * clears the stat editor, whose fixed label column leaves the sliders 129 pixels at 300.
 */
inline constexpr float kSideWorkspaceBreakpoint = 900.0F;
/**
 * The side workspace lies over the page rather than beside it, as the game's own item detail
 * lies over the inventory, so it can take the width a tooltip column needs without costing the
 * grid a column of cards.
 */
inline constexpr float kSideWorkspaceDefaultWidth = 440.0F;
inline constexpr float kSideWorkspaceMinWidth = 340.0F;
inline constexpr float kSideWorkspaceMaxWidth = 600.0F;
/** The page keeps at least this much width uncovered: two minimum-width cards and their gap. */
inline constexpr float kPrimaryWorkspaceMinWidth = 590.0F;
/** Workspace frame margin, symmetric(12, 8) in Sundial. */
inline constexpr float kWorkspaceMarginX = 12.0F;
inline constexpr float kWorkspaceMarginY = 8.0F;
/** Bottom workspace height: preferred 48% of the space, clamped, then capped at 70%. */
inline constexpr float kBottomWorkspacePreferredFraction = 0.48F;
inline constexpr float kBottomWorkspaceMaximumFraction = 0.70F;
inline constexpr float kBottomWorkspaceFloor = 260.0F;
inline constexpr float kBottomWorkspaceMinimum = 280.0F;
inline constexpr float kBottomWorkspaceMaximum = 360.0F;
/**
 * Item card width. Sundial offers Compact/Standard/Wide (285/335/430 minimums) for a window the
 * player sizes; the Dawn panel is fixed and smaller, so it takes the compact end to fit another
 * column instead of leaving a band of empty panel beside the grid.
 */
inline constexpr float kCardMinimumWidth = 285.0F;
/** Vertical gap between two cards in the same column. */
inline constexpr float kCardColumnSpacing = 3.0F;
/** Picker list heights, which Sundial clamps between these. */
inline constexpr float kPickerMinimumHeight = 320.0F;
inline constexpr float kPickerMaximumHeight = 420.0F;

/**
 * Works out the responsive card grid.
 * The column count is Sundial's: as many minimum-width cards as fit. The cards then fill the row
 * rather than stopping at the maximum width, which is where this departs from Sundial. Sundial
 * runs in a window the player resizes, so trailing space there is space they chose; the Dawn panel
 * is a fixed size, and capping the width left a wide band of dead space beside the cards.
 * @param available Width to fill, in framebuffer pixels.
 * @param spacing Horizontal gap between columns, in framebuffer pixels.
 * @param minimumWidth Narrowest a card may be drawn, which sets the column count.
 * @param columns Receives the column count, at least one.
 * @param cardWidth Receives the width of one card.
 */
void card_grid(float available,
               float spacing,
               float minimumWidth,
               int& columns,
               float& cardWidth) noexcept;

/** How far the background catalog load has got. The frame reads it and nothing else. */
enum class CatalogPhase : std::uint8_t {
    idle,
    loading,
    ready,
    failed,
};

/** How far the icon sweep has got. The page reads it and nothing else. */
enum class IconSweepPhase : std::uint8_t {
    idle,
    running,
    done,
    failed,
    /** The result has been moved into the catalog; nothing more to do this session. */
    finished,
};

/** Views of the editor, in the order their tabs appear. Named as Sundial names them. */
enum class View : std::uint8_t {
    /** Character identity and the equipped loadout, on one scrolling page. */
    characters,
    characterInventory,
    profileInventory,
    armory,
    /** The installed icon container, browsed by hand. Nothing else can name what is in it. */
    icons,
    count,
};

/** Item families the armory browses. They are not the catalog's own gear kinds. */
enum class Category : std::uint8_t {
    weapons,
    armor,
    cosmetics,
    perks,
    /** Account stacks: materials, currencies and consumables, which belong to no slot. */
    materials,
    count,
};

/** Order the armory grid lists its results in. */
enum class Sort : std::uint8_t {
    type,
    name,
    rarity,
    count,
};

/** What the inspector is bound to. An instance of zero means a catalog item nobody owns yet. */
struct Selection {
    std::uint32_t definitionHash{};
    std::uint64_t instanceSoid{};

    /** @return True when the given card is the selected one. */
    [[nodiscard]] bool holds(std::uint32_t hash, std::uint64_t instance) const noexcept {
        return definitionHash == hash && instanceSoid == instance;
    }
};

/** Armory filters. Their combined key decides when the result list is rebuilt. */
struct Browse {
    Category category{Category::weapons};
    Sort sort{Sort::type};
    /** Zero shows every rarity; otherwise only this tier. */
    int rarity{};
    bool classOnly{true};
    bool includeInternal{};
    /** Empty shows every item type. */
    std::string type;
    char search[kSearchCapacity]{};

    /** @return Number of filters narrowing the results beyond the defaults. */
    [[nodiscard]] int narrowing() const noexcept {
        return (rarity != 0 ? 1 : 0) + (classOnly ? 0 : 1) + (includeInternal ? 1 : 0);
    }
};

/** Results for one exact set of armory filters, rebuilt only when that set changes. */
struct BrowseResults {
    std::vector<const edit::CatalogItem*> items;
    /** Items each type would show, counted before the type filter so the type picker can offer it. */
    std::map<std::string, std::size_t> types;
    /** Results across every type, which the type picker shows against "All types". */
    std::size_t total{};
    std::string key;
};

/** Values the grant controls carry for an item the character does not own yet. */
struct Grant {
    int power{1050};
    int quantity{1};
};

/** Perk picker state while it is open over one socket lane. */
struct SocketPicker {
    std::size_t lane{};
    /** Opens one step past the native pool: the socket's type across this gear type. */
    edit::PlugScope scope{edit::PlugScope::socketAndGear};
    std::vector<std::uint16_t> options;
    char search[kSearchCapacity]{};
    /** Filters over the offered plugs: a plug type or empty for all, a tier or zero for all. */
    std::string type;
    int rarity{};
    Sort sort{Sort::name};
    /** Offer the internal and placeholder plugs the catalog carries. */
    bool includeInternal{};
    /** Raised by a socket lane and consumed once the frame's widgets have all been submitted. */
    bool requested{};
};

/** Armor stat targets, kept for one item so switching items reloads them from that item. */
struct StatTargets {
    edit::Stats values{};
    std::uint64_t owner{};
};

/** Randomizer options, which are its slot mask, the power it rolls at, and its generator. */
struct Randomizer {
    std::array<bool, kSlotCount> slots{true, true, true, true, true, true, true, true};
    std::mt19937 engine{std::random_device{}()};
};

/**
 * Whole editor state, owned by the module and reachable only through `model()`.
 * The draft is the only thing here the game can see, and only once an apply commits it.
 */
struct Model {
    edit::Catalog catalog;
    std::unique_ptr<edit::Draft> draft;

    std::thread loader;
    std::atomic<CatalogPhase> phase{CatalogPhase::idle};
    std::atomic_bool cancelLoad{false};
    std::atomic_uint loadProgress{};
    std::string loadError;

    /** Last outcome shown under the footer. */
    std::string status;
    /** Set when the running account moved on while the draft held unapplied edits. */
    bool accountDiverged{};
    /** Player setting: commit each edit to the running game as soon as it is made. */
    bool applyInstantly{true};
    /** Raised by an edit and consumed once the frame's widgets have all been submitted. */
    bool applyRequested{};
    /** Tick of the last account divergence check, which is throttled well below frame rate. */
    std::uint64_t lastDivergenceCheckTick{};
    /** While non-zero, the last apply reached no signed-in peer and is re-offered until this tick. */
    std::uint64_t republishUntilTick{};

    View view{View::characters};
    std::size_t character{};
    /** Width of the side inspector, which the player can drag. Sundial's workspace is resizable. */
    float inspectorWidth{};
    Selection selection;
    Browse browse;
    BrowseResults results;
    Grant grant;
    SocketPicker picker;
    StatTargets targets;
    Randomizer randomizer;

    /**
     * The icon sweep, which reads every package's entry table and so runs on its own thread, and
     * only once the browser is opened. The worker fills the two vectors and then flips the phase;
     * the page moves them into the catalog on its own thread.
     */
    std::thread iconSweeper;
    std::atomic<IconSweepPhase> iconSweep{IconSweepPhase::idle};
    std::vector<edit::IconRow> sweptIcons;
    std::vector<std::string> sweptPackages;
    /** Icon browser: the first row on the page, and the edge it draws each tile at. */
    int iconRow{};
    /** 52 is the tooltip band's icon edge, so a browsed icon is seen at the size it will be used. */
    float iconTile{52.0F};
    /** Package the browser is filtered to, as an index into the catalog's, or -1 for all. */
    int iconPackage{-1};
    /** Icons passing the filter, and the filter the list was built for. */
    std::vector<std::uint32_t> iconFiltered;
    int iconFilterBuilt{-2};
    /** Icon the viewer is showing, as an index into the catalog's icons, or -1 for none. */
    int iconViewed{-1};
    /** Raised by a tile and consumed by the page, which is the scope that owns the viewer. */
    bool iconViewRequested{};

    /** Inventory page filter: a slot index, or -1 for every slot. */
    int inventorySlot{-1};
    char inventorySearch[kSearchCapacity]{};
    /** Profile page search, kept apart so switching pages does not carry one filter to the other. */
    char profileSearch[kSearchCapacity]{};
};

/** @return The module state. It exists for the whole time the page is registered. */
[[nodiscard]] Model& model() noexcept;

/** @return True while the module state exists, which the frame entry checks first. */
[[nodiscard]] bool live() noexcept;

/** @return The character the pages are editing. Only called once a draft exists. */
[[nodiscard]] state::CharacterState& character() noexcept;

/**
 * @return The character's item with this instance id, or null when it owns none.
 * @param instance Owned instance id.
 */
[[nodiscard]] edit::Item* find_owned_item(std::uint64_t instance) noexcept;

/** @return The selected owned item, or null when the selection names a catalog item only. */
[[nodiscard]] edit::Item* selected_item() noexcept;

/** Points the inspector at one catalog item, and optionally at one instance the character owns. */
void select(const edit::CatalogItem& item, std::uint64_t instance = 0) noexcept;

/** Clears the inspector selection. */
void clear_selection() noexcept;

/**
 * Removes one stowed item from the character's inventory and marks the draft changed.
 * An equipped item is not in that array and is left alone; unequip it first.
 * @param instance Owned instance id.
 * @return True when an item was removed.
 */
bool erase_owned_item(std::uint64_t instance) noexcept;

/**
 * Marks the draft changed and, in instant mode, queues the apply that publishes it.
 * @param publish False for an edit that cannot stand on its own yet, such as a class change
 *        before the matching armor and subclass are equipped. The draft still shows it.
 */
void mark_changed(bool publish = true) noexcept;

/**
 * Records the outcome of one edit call that maintains the draft itself.
 * @param succeeded True when the edit changed the draft.
 */
void record_edit(bool succeeded) noexcept;

/**
 * Records an edit made through a control the player holds, such as a slider or a stepper.
 * The draft follows every frame, and the apply waits until the control is released, so a drag
 * commits once instead of once per frame. Call it immediately after the control.
 * @param changed True when the control reported a change this frame.
 */
void record_scalar_edit(bool changed) noexcept;

/** Rebases the draft onto the running account, discarding any unapplied edits. */
void reload_account() noexcept;

/** Starts, or restarts, the background catalog load. */
void start_catalog_load() noexcept;

/** Draws the whole page inside the active Core UI frame. */
void draw() noexcept;

/** Releases the catalog worker and clears state between UI lifecycles. */
void reset() noexcept;

/** View bodies. Each draws into the region the frame has already sized. */

/** Identity and progression fields, drawn above the equipped loadout on the Characters view. */
void draw_character_fields() noexcept;
/** The equipped armor stat totals, drawn as one row under the loadout heading. */
void draw_armor_totals() noexcept;

/** The equipped loadout cards, drawn below the identity fields on the Characters view. */
void draw_equipment() noexcept;
void draw_armory_page() noexcept;
void draw_character_inventory_page() noexcept;
void draw_profile_inventory_page() noexcept;

/** Draws the icon container browser, which is how an unnamed icon is found. */
void draw_icons_page() noexcept;

/** The subclass and path pickers, drawn as the second group of the Characters view. */
void draw_subclass_group(float labelWidth, float controlWidth) noexcept;

/** The five ability pickers, drawn as the third group of the Characters view. */
void draw_ability_group(float labelWidth, float controlWidth) noexcept;

/** Draws the item detail pane beside, or instead of, the page body. */
void draw_inspector() noexcept;

/**
 * @return How much of the page's right edge the side workspace lies over, in framebuffer pixels.
 * A row of controls that must stay reachable lays itself out inside the width this leaves.
 */
[[nodiscard]] float overlay_width() noexcept;

/**
 * @return The armor stat targets bound to one owned armor piece.
 * The targets follow the item the inspector shows: a new item reloads them from its own roll, and
 * the tooltip's stat block then draws and moves them in place.
 * @param item Owned armor piece being edited.
 */
[[nodiscard]] edit::Stats& stat_targets_for(const edit::Item& item) noexcept;

/**
 * Rolls the armor to its targets once a bar has been let go of, and settles the targets on what
 * the roll reached.
 * @param item Owned armor piece being edited.
 * @param released True on the frame a stat bar was released.
 */
void settle_stat_targets(edit::Item& item, bool released) noexcept;

/** Draws the perk picker modal when a socket row has opened it. */
void draw_perk_picker() noexcept;

/**
 * Draws every socket of one owned item as an editable row: the fitted plug's icon, its name, its
 * type and what it does, with the row itself opening the picker for that lane.
 * @param definition Catalog definition of the item being edited.
 * @param item Owned instance whose sockets are shown.
 * @param width Content width in framebuffer pixels.
 */
void draw_perk_editor(const edit::CatalogItem& definition,
                      const edit::Item& item,
                      float width) noexcept;

/** Opens the perk picker over one socket lane of the selected item. */
void open_perk_picker(const edit::CatalogItem& definition, std::size_t lane) noexcept;

} // namespace dawn::core::ui::modules::loadout::internal
