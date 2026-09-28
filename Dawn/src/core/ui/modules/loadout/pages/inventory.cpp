// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../card.h"
#include "../internal.h"
#include "../controls.h"
#include "../art.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Width of the search box each inventory page puts at the head of its filter row. */
constexpr float kSearchWidth = 220.0F;
/** 150 authored pixels fit the widest slot name in the slot picker. */
constexpr float kSlotPickerWidth = 150.0F;
/** Icon edge on an account stack row. */
constexpr float kProfileIconExtent = 28.0F;
constexpr float kProfileRowInset = 6.0F;
/** Vertical breathing room the row fill adds above and below its content. */
constexpr float kProfileRowPadding = 2.0F;
/** Narrowest an account stack row is drawn, which sets the column count. */
constexpr float kProfileRowMinimumWidth = 320.0F;
/**
 * Gap between two lines of account stacks.
 * The card grid's own column gap is narrower than the fill each row paints around its content, so
 * lines set at it ran their fills together into one block.
 */
constexpr float kProfileRowGap = 9.0F;
/** 64 authored pixels hold a five-figure stack without the field reading as an empty box. */
constexpr float kQuantityWidth = 64.0F;
/** Rule under each account stack row, as the tooltip rules its own rows. */
constexpr ImVec4 kProfileRowRule{1.0F, 1.0F, 1.0F, 0.08F};
/** The add action on each page's header row. */
constexpr const char* kAddItemLabel = "Add Item";
/** The removal says what it does. A glyph on its own read as decoration rather than a control. */
constexpr const char* kRemoveLabel = "Remove";
/** Section a stack falls under when its type is blank, or when the catalog does not carry it. */
constexpr const char* kUntypedGroup = "Other";
constexpr const char* kUnknownGroup = "Unknown";
/** Largest stack the editor offers when the catalog does not name a limit. */
constexpr int kUnknownStackLimit = 9999;
/** Title of the account stack removal, used for both the action and its modal. */
constexpr const char* kRemoveStackTitle = "Remove Account Stack?";
/**
 * The bar that stands over the grid while items are selected: a strip in the choice fill with the
 * accent's rail, holding the count and what can be done to all of them at once.
 */
constexpr float kPickBarPadding = 4.0F;
constexpr float kPickBarInset = 10.0F;
/**
 * Id of the removal of every selected item, whose title counts what it removes, and the words the
 * bar's send row leads with. Everything after ### names the popup, so the count can change in front.
 */
constexpr const char* kRemovePickedId = "###remove_selected";
constexpr const char* kSendLabel = "Send To";
/** 64 authored pixels hold a five-figure Power in the selection bar's field. */
constexpr float kPickPowerWidth = 64.0F;
/** Names the removal lists before it counts the rest. */
constexpr std::size_t kRemovalNamesShown = 6;
constexpr float kRemovalButtonWidth = 120.0F;
/** 192 bytes hold any outcome the bar reports, with every count it can give. */
constexpr std::size_t kMessageCapacity = 192;
/** The search's other results: the section's title, and the account's name for its own stacks. */
constexpr const char* kElsewhereLabel = "Elsewhere on the Account";
constexpr const char* kAccountLabel = "Account";
/** Where a result elsewhere is kept, after the name of whoever keeps it. */
constexpr const char* kEquippedWhere = "Equipped";
constexpr const char* kPostmasterWhere = "Postmaster";
/** A result that belongs to the account rather than a character. */
constexpr std::size_t kAccountItems = static_cast<std::size_t>(-1);

/**
 * Draws the page's add action at the far end of its header row, which opens the armory on the
 * category the page is made of. The armory is where an item is granted from; the page only has
 * to say where to go.
 * @param category Armory category the page opens on.
 */
void draw_add_action(Category category) noexcept {
    Model& state = model();
    const float width =
        ImGui::CalcTextSize(kAddItemLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    if (!ImGui::SmallButton(kAddItemLabel)) {
        return;
    }
    state.view = View::armory;
    state.browse.category = category;
    state.browse.type.clear();
    state.results.key.clear();
}

/**
 * A search as typed, split into the words matched against an item's name, type and description, and
 * the filters it must also pass. A filter the page does not know is kept as a word, so it matches the
 * way anything else typed would, rather than being dropped without a word.
 */
struct Query {
    std::string words;
    /** Rarity tier asked for, or -1. */
    int tier{-1};
    /** Weapon or armor asked for, when `kinded`. */
    edit::GearKind kind{edit::GearKind::other};
    bool kinded{};
    /** Element asked for, when `elemented`. */
    edit::Element element{edit::Element::none};
    bool elemented{};
    /** Each of these is -1 when not asked for, else 1 for yes and 0 for no. */
    int locked{-1};
    int equipped{-1};
    int postmaster{-1};
    /** Power comparison: 0 for none, else one of < > = and l for at most, g for at least. */
    char comparison{};
    int power{};

    /** @return True when a filter names something only an item a character holds can have. */
    [[nodiscard]] bool held_only() const noexcept {
        return locked >= 0 || equipped >= 0 || postmaster >= 0 || comparison != 0;
    }
};

/** Filter words and what each asks for, as a player types them after is: */
constexpr const char* kTierWords[]{"unclassified", "common", "uncommon", "rare", "legendary", "exotic"};

/** @return A search read into its words and its filters. The search is already folded to lower case. */
[[nodiscard]] Query read_query(const std::string& search) noexcept {
    Query query;
    const auto keep = [&query](const std::string& word) {
        query.words += query.words.empty() ? word : " " + word;
    };
    std::size_t begin = 0;
    while (begin < search.size()) {
        const std::size_t end = search.find(' ', begin);
        const std::string word = search.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        begin = end == std::string::npos ? search.size() : end + 1;
        if (word.empty()) {
            continue;
        }
        if (word.rfind("is:", 0) == 0) {
            const std::string what = word.substr(3);
            bool known = true;
            const auto tier = std::find_if(std::begin(kTierWords), std::end(kTierWords), [&what](const char* name) {
                return what == name;
            });
            if (tier != std::end(kTierWords)) {
                query.tier = static_cast<int>(tier - std::begin(kTierWords));
            } else if (what == "weapon" || what == "weapons") {
                query.kind = edit::GearKind::weapon;
                query.kinded = true;
            } else if (what == "armor") {
                query.kind = edit::GearKind::armor;
                query.kinded = true;
            } else if (what == "arc" || what == "solar" || what == "void") {
                query.element = what == "arc" ? edit::Element::arc : what == "solar" ? edit::Element::solar : edit::Element::void_;
                query.elemented = true;
            } else if (what == "locked" || what == "unlocked") {
                query.locked = what == "locked" ? 1 : 0;
            } else if (what == "equipped") {
                query.equipped = 1;
            } else if (what == "postmaster") {
                query.postmaster = 1;
            } else {
                known = false;
            }
            if (!known) {
                keep(word);
            }
            continue;
        }
        if (word.rfind("power:", 0) == 0) {
            std::string_view rest = std::string_view(word).substr(6);
            char comparison = '=';
            if (rest.rfind(">=", 0) == 0 || rest.rfind("<=", 0) == 0) {
                comparison = rest[0] == '>' ? 'g' : 'l';
                rest.remove_prefix(2);
            } else if (!rest.empty() && (rest[0] == '>' || rest[0] == '<' || rest[0] == '=')) {
                comparison = rest[0];
                rest.remove_prefix(1);
            }
            int power = 0;
            bool digits = !rest.empty() && rest.size() <= 6;
            for (const char c : rest) {
                digits = digits && c >= '0' && c <= '9';
                power = digits ? (power * 10) + (c - '0') : power;
            }
            if (digits) {
                query.comparison = comparison;
                query.power = power;
            } else {
                keep(word);
            }
            continue;
        }
        keep(word);
    }
    return query;
}

/** @return True when the game gives an item a Power, which only gear carries. */
[[nodiscard]] bool carries_power(const edit::CatalogItem& definition) noexcept {
    return definition.kind == edit::GearKind::weapon || definition.kind == edit::GearKind::armor;
}

/**
 * @return True when one item passes a search: its words and every filter.
 * @param item The item a character holds, or null for an account stack, which a filter only a held
 * item can pass leaves out.
 * @param equipped True when the character has the item on.
 */
[[nodiscard]] bool passes(const Query& query, const edit::CatalogItem& definition, const edit::Item* item, bool equipped) noexcept {
    if ((!query.words.empty() && !edit::matches(definition, query.words))
        || (query.tier >= 0 && definition.definition.tier != query.tier)
        || (query.kinded && definition.kind != query.kind)
        || (query.elemented && definition.element != query.element)) {
        return false;
    }
    if (item == nullptr) {
        return !query.held_only();
    }
    if ((query.locked >= 0 && ((item->flags & inv::kLockedItemFlag) != 0) != (query.locked == 1))
        || (query.equipped >= 0 && equipped != (query.equipped == 1))
        || (query.postmaster >= 0 && item->postmaster != (query.postmaster == 1))) {
        return false;
    }
    if (query.comparison == 0) {
        return true;
    }
    if (!carries_power(definition)) {
        return false;
    }
    const int power = power_of(item->level);
    switch (query.comparison) {
    case '<':
        return power < query.power;
    case '>':
        return power > query.power;
    case 'l':
        return power <= query.power;
    case 'g':
        return power >= query.power;
    default:
        return power == query.power;
    }
}

/** Said under the pointer over a search field: what it takes besides a name. */
constexpr const char* kSearchTip =
    "Name, type or description. Filters: is:exotic, is:legendary, is:weapon, is:armor, is:arc, "
    "is:solar, is:void, is:locked, is:unlocked, is:equipped, is:postmaster, power:>1000.";

/** @return True when one owned item passes the page's search and slot filters. */
[[nodiscard]] bool passes_filters(const edit::Item& item, const Query& query, bool equipped) noexcept {
    Model& state = model();
    const edit::CatalogItem* definition = state.catalog.find(item.definitionHash);
    if (definition == nullptr || !passes(query, *definition, &item, equipped)) {
        return false;
    }
    return state.inventorySlot < 0
           || definition->slot == static_cast<std::size_t>(state.inventorySlot);
}

/**
 * @return Every character item matching the page filters, equipped gear first, then by type and
 * name. The equipped pieces belong in their buckets too: the page is the whole of what the
 * character carries, not only what is stowed.
 */
[[nodiscard]] std::vector<const edit::Item*> filtered_character_items() noexcept {
    Model& state = model();
    const Query query = read_query(edit::searchable(state.inventorySearch));
    const state::CharacterState& owner = character();
    std::vector<const edit::Item*> items;
    for (const auto& slot : owner.equipment.slots) {
        if (slot && passes_filters(*slot, query, true)) {
            items.push_back(&*slot);
        }
    }
    const std::size_t equippedCount = items.size();
    for (std::size_t i = 0; i < owner.inventory.count; ++i) {
        if (passes_filters(owner.inventory.values[i], query, false)) {
            items.push_back(&owner.inventory.values[i]);
        }
    }
    // Only items passes_filters already resolved reach this list, so a missing definition sorts
    // last rather than being dereferenced on the strength of that invariant holding elsewhere.
    const auto byTypeThenName = [&state](const edit::Item* a, const edit::Item* b) {
        const edit::CatalogItem* first = state.catalog.find(a->definitionHash);
        const edit::CatalogItem* second = state.catalog.find(b->definitionHash);
        if (first == nullptr || second == nullptr) {
            return first != nullptr;
        }
        return first->type == second->type ? first->name < second->name : first->type < second->type;
    };
    std::sort(items.begin() + static_cast<std::ptrdiff_t>(equippedCount), items.end(), byTypeThenName);
    return items;
}

/** @return True when the instance is sitting in one of the character's equipment slots. */
[[nodiscard]] bool is_equipped(std::uint64_t instance) noexcept {
    for (const auto& slot : character().equipment.slots) {
        if (slot && slot->instanceSoid == instance) {
            return true;
        }
    }
    return false;
}

/** @return How many equipment slots the character has filled. */
[[nodiscard]] std::size_t equipped_count() noexcept {
    std::size_t count = 0;
    for (const auto& slot : character().equipment.slots) {
        count += slot ? 1 : 0;
    }
    return count;
}

/** Lets go of every selected item the character no longer holds, such as one just sent away. */
void prune_picks() noexcept {
    std::vector<std::uint64_t>& picked = model().picked;
    picked.erase(std::remove_if(picked.begin(),
                                picked.end(),
                                [](std::uint64_t instance) { return find_owned_item(instance) == nullptr; }),
                 picked.end());
}

/** Says what an action on the selected items came to, and carries it into the apply that follows. */
void report_bulk(const char* message, bool changed) noexcept {
    Model& state = model();
    state.status = message;
    state.statusFailed = false;
    if (changed) {
        state.editNote = message;
        mark_changed();
    }
}

/** Locks, or unlocks, every selected item that is not so already. */
void lock_picked(bool lock) noexcept {
    std::size_t changed = 0;
    for (const std::uint64_t instance : model().picked) {
        edit::Item* item = find_owned_item(instance);
        if (item == nullptr || ((item->flags & inv::kLockedItemFlag) != 0) == lock) {
            continue;
        }
        item->flags = lock ? item->flags | inv::kLockedItemFlag : item->flags & ~inv::kLockedItemFlag;
        ++changed;
    }
    char message[kMessageCapacity]{};
    if (changed == 0) {
        (void)std::snprintf(message, sizeof message, "All selected items are already %s.", lock ? "locked" : "unlocked");
    } else {
        (void)std::snprintf(message,
                            sizeof message,
                            "%s %zu %s.",
                            lock ? "Locked" : "Unlocked",
                            changed,
                            changed == 1 ? "item" : "items");
    }
    report_bulk(message, changed != 0);
}

/**
 * Sends every selected item that can go to another character. One that is equipped, at the
 * postmaster or of a class the other character cannot hold stays, and stays selected, so the page
 * shows what did not go.
 */
void send_picked(std::size_t target) noexcept {
    Model& state = model();
    const state::CharacterClass targetClass = state.draft->after.characters[target].characterClass;
    std::size_t sent = 0;
    std::size_t equipped = 0;
    std::size_t postmaster = 0;
    std::size_t otherClass = 0;
    std::size_t noRoom = 0;
    std::vector<std::uint64_t> stayed;
    const std::vector<std::uint64_t> picked = state.picked;
    for (const std::uint64_t instance : picked) {
        const edit::Item* item = find_owned_item(instance);
        const edit::CatalogItem* definition = item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
        if (item == nullptr || definition == nullptr) {
            continue;
        }
        std::size_t* reason = is_equipped(instance) ? &equipped
                              : item->postmaster     ? &postmaster
                              : !edit::fits_class(*definition, targetClass) ? &otherClass
                                                                            : nullptr;
        std::string refused;
        if (reason == nullptr && !edit::transfer(*state.draft, state.catalog, state.character, target, instance, refused)) {
            reason = &noRoom;
        }
        if (reason != nullptr) {
            ++*reason;
            stayed.push_back(instance);
            continue;
        }
        ++sent;
        if (state.selection.instanceSoid == instance) {
            clear_selection();
        }
    }
    state.picked = stayed;
    const std::string name = character_label(target);
    char message[kMessageCapacity]{};
    int written = sent == 0 ? std::snprintf(message, sizeof message, "Nothing was sent to %s.", name.c_str())
                            : std::snprintf(message, sizeof message, "Sent %zu %s to %s.", sent, sent == 1 ? "item" : "items", name.c_str());
    const auto note = [&](std::size_t count, const char* why) {
        if (count != 0 && written >= 0 && static_cast<std::size_t>(written) < sizeof message) {
            written += std::snprintf(message + written, sizeof message - static_cast<std::size_t>(written), " %zu %s.", count, why);
        }
    };
    note(equipped, equipped == 1 ? "is equipped" : "are equipped");
    note(postmaster, postmaster == 1 ? "is at the Postmaster" : "are at the Postmaster");
    note(otherClass, otherClass == 1 ? "belongs to another class" : "belong to another class");
    note(noRoom, "did not fit there");
    report_bulk(message, sent != 0);
    model().statusFailed = sent == 0;
}

/** Sets every selected weapon and armor piece to one Power. What carries no Power is left as it is. */
void set_power_picked(int power) noexcept {
    Model& state = model();
    const int level = std::clamp(level_of(power), 0, kMaximumItemLevel);
    std::size_t changed = 0;
    std::size_t powerless = 0;
    for (const std::uint64_t instance : state.picked) {
        edit::Item* item = find_owned_item(instance);
        const edit::CatalogItem* definition = item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
        if (definition == nullptr || !carries_power(*definition)) {
            ++powerless;
            continue;
        }
        if (item->level != level) {
            item->level = level;
            ++changed;
        }
    }
    char message[kMessageCapacity]{};
    int written = changed == 0
                      ? std::snprintf(message, sizeof message, "Already at %d Power.", power_of(level))
                      : std::snprintf(message,
                                      sizeof message,
                                      "Set %zu %s to %d Power.",
                                      changed,
                                      changed == 1 ? "piece" : "pieces",
                                      power_of(level));
    if (powerless != 0 && written >= 0 && static_cast<std::size_t>(written) < sizeof message) {
        (void)std::snprintf(message + written,
                            sizeof message - static_cast<std::size_t>(written),
                            " %zu %s no Power.",
                            powerless,
                            powerless == 1 ? "item carries" : "items carry");
    }
    report_bulk(message, changed != 0);
}

/**
 * Pulls every selected item at the postmaster into its own bucket, as the postmaster does. One whose
 * bucket is full, or that belongs to the account, stays there.
 */
void pull_picked() noexcept {
    Model& state = model();
    std::size_t pulled = 0;
    std::size_t stuck = 0;
    const std::vector<std::uint64_t> picked = state.picked;
    for (const std::uint64_t instance : picked) {
        const edit::Item* item = find_owned_item(instance);
        if (item == nullptr || !item->postmaster) {
            continue;
        }
        std::string refused;
        if (edit::pull_from_postmaster(*state.draft, state.catalog, state.character, instance, refused)) {
            ++pulled;
        } else {
            ++stuck;
        }
    }
    char message[kMessageCapacity]{};
    int written = std::snprintf(message,
                                sizeof message,
                                "Pulled %zu %s from the Postmaster.",
                                pulled,
                                pulled == 1 ? "item" : "items");
    if (stuck != 0 && written >= 0 && static_cast<std::size_t>(written) < sizeof message) {
        (void)std::snprintf(message + written,
                            sizeof message - static_cast<std::size_t>(written),
                            " %zu stayed at the Postmaster.",
                            stuck);
    }
    report_bulk(message, pulled != 0);
    state.statusFailed = pulled == 0;
}

/**
 * Removes every selected item that can be removed. A locked item stays, as the game keeps a locked
 * item, and an equipped one stays until something replaces it; both stay selected.
 */
void remove_picked() noexcept {
    Model& state = model();
    std::size_t removed = 0;
    std::size_t locked = 0;
    std::size_t equipped = 0;
    std::vector<std::uint64_t> stayed;
    const std::vector<std::uint64_t> picked = state.picked;
    for (const std::uint64_t instance : picked) {
        const edit::Item* item = find_owned_item(instance);
        if (item == nullptr) {
            continue;
        }
        if (is_equipped(instance)) {
            ++equipped;
            stayed.push_back(instance);
        } else if ((item->flags & inv::kLockedItemFlag) != 0) {
            ++locked;
            stayed.push_back(instance);
        } else if (erase_owned_item(instance)) {
            ++removed;
        }
    }
    state.picked = stayed;
    char message[kMessageCapacity]{};
    int written = std::snprintf(message,
                                sizeof message,
                                "Removed %zu %s.",
                                removed,
                                removed == 1 ? "item" : "items");
    if (locked + equipped != 0 && written >= 0 && static_cast<std::size_t>(written) < sizeof message) {
        (void)std::snprintf(message + written,
                            sizeof message - static_cast<std::size_t>(written),
                            " %zu locked or equipped %s stayed.",
                            locked + equipped,
                            locked + equipped == 1 ? "item" : "items");
    }
    report_bulk(message, removed != 0);
}

/** An action on every selected item, carried out once the page has drawn the items it would move. */
struct Bulk {
    enum class Kind : std::uint8_t { none, lock, unlock, send, power, pull, remove };
    Kind kind{Kind::none};
    /** Character a send goes to. */
    std::size_t target{};
};

/** Carries out one action on every selected item. */
void run_bulk(const Bulk& bulk) noexcept {
    switch (bulk.kind) {
    case Bulk::Kind::none:
        break;
    case Bulk::Kind::lock:
        lock_picked(true);
        break;
    case Bulk::Kind::unlock:
        lock_picked(false);
        break;
    case Bulk::Kind::send:
        send_picked(bulk.target);
        break;
    case Bulk::Kind::power:
        set_power_picked(model().pickPower);
        break;
    case Bulk::Kind::pull:
        pull_picked();
        break;
    case Bulk::Kind::remove:
        remove_picked();
        break;
    }
}

/**
 * Draws the confirmation the bar's Remove opens, which names what it would remove.
 * @return True when the removal was confirmed.
 */
[[nodiscard]] bool draw_remove_picked_modal() noexcept {
    Model& state = model();
    if (!ImGui::IsPopupOpen(kRemovePickedId)) {
        return false;
    }
    std::vector<std::string> names;
    std::size_t kept = 0;
    for (const std::uint64_t instance : state.picked) {
        const edit::Item* item = find_owned_item(instance);
        const edit::CatalogItem* definition = item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
        if (item == nullptr) {
            continue;
        }
        if (is_equipped(instance) || (item->flags & inv::kLockedItemFlag) != 0) {
            ++kept;
            continue;
        }
        names.push_back(definition != nullptr ? definition->name : std::string("Unknown Item"));
    }
    // The title says how many it removes; the id after it keeps the popup the same one whatever the count.
    char title[kMessageCapacity]{};
    (void)std::snprintf(title,
                        sizeof title,
                        "Remove %zu %s?%s",
                        names.size(),
                        names.size() == 1 ? "Item" : "Items",
                        kRemovePickedId);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return false;
    }
    ImGui::Text("Removes these from %s's inventory:", character_label(state.character).c_str());
    controls::space(controls::kRowSpacing);
    for (std::size_t i = 0; i < names.size() && i < kRemovalNamesShown; ++i) {
        ImGui::BulletText("%s", names[i].c_str());
    }
    if (names.size() > kRemovalNamesShown) {
        ImGui::TextDisabled("and %zu more", names.size() - kRemovalNamesShown);
    }
    if (names.empty()) {
        ImGui::TextDisabled("Locked and equipped items can't be removed.");
    }
    if (kept != 0 && !names.empty()) {
        ImGui::TextDisabled("%zu locked or equipped %s will stay.", kept, kept == 1 ? "item" : "items");
    }
    // A removal is an edit like any other, so it can be taken back; saying so is what makes it safe.
    if (!names.empty()) {
        ImGui::TextDisabled("Undo brings them back.");
    }
    controls::space(controls::kSectionSpacing);
    char label[kMessageCapacity]{};
    (void)std::snprintf(label, sizeof label, "Remove %zu", names.size());
    bool confirmed = false;
    ImGui::BeginDisabled(names.empty());
    if (controls::primary_button(label, {pixels(kRemovalButtonWidth), 0.0F})) {
        confirmed = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {pixels(kRemovalButtonWidth), 0.0F}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return confirmed;
}

/**
 * Draws the bar that stands over the grid while items are selected: how many, what can be done to
 * all of them, and at its far end the ways to widen the selection or let it go.
 * @param shown Items the page's filters show, which Select all takes.
 * @return The action pressed, which the page carries out once it has drawn its items: sending or
 * removing moves the items the grid is about to read.
 */
[[nodiscard]] Bulk draw_pick_bar(const std::vector<const edit::Item*>& shown) noexcept {
    Bulk bulk;
    Model& state = model();
    const state::AccountState& account = state.draft->after;
    const ImGuiStyle& style = ImGui::GetStyle();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = (std::max)(0.0F, ImGui::GetContentRegionAvail().x - overlay_width());
    const float padding = pixels(kPickBarPadding);
    const float inset = pixels(kPickBarInset);
    const float right = origin.x + width - inset;
    // Whether the far end fits beside the actions decides how tall the strip is, so the controls are
    // laid out first and the strip's fill goes in behind them afterwards.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);

    ImGui::SetCursorScreenPos({origin.x + inset, origin.y + padding});
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%zu Selected", state.picked.size());
    ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
    if (ImGui::Button("Lock")) {
        bulk.kind = Bulk::Kind::lock;
    }
    ImGui::SameLine();
    if (ImGui::Button("Unlock")) {
        bulk.kind = Bulk::Kind::unlock;
    }
    // Sending reads as the pane's own send row: the words, then a button for each other character.
    // One none of the selection could go to keeps its button, disabled, so the row says why.
    if (account.characterCount > 1) {
        ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
        ImGui::TextUnformatted(kSendLabel);
        for (std::size_t c = 0; c < account.characterCount; ++c) {
            if (c == state.character) {
                continue;
            }
            const bool fits = std::any_of(state.picked.begin(), state.picked.end(), [&](std::uint64_t instance) {
                const edit::Item* item = find_owned_item(instance);
                const edit::CatalogItem* definition = item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
                return definition != nullptr && !item->postmaster && !is_equipped(instance)
                       && edit::fits_class(*definition, account.characters[c].characterClass);
            });
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(c));
            ImGui::BeginDisabled(!fits);
            if (ImGui::Button(character_label(c).c_str())) {
                bulk = {Bulk::Kind::send, c};
            }
            ImGui::EndDisabled();
            if (!fits && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("None can go there: equipped, at the Postmaster or the wrong class.");
            }
            ImGui::PopID();
        }
    }
    // Power and the pull are offered only when the selection holds something they act on.
    const bool powered = std::any_of(state.picked.begin(), state.picked.end(), [&state](std::uint64_t instance) {
        const edit::Item* item = find_owned_item(instance);
        const edit::CatalogItem* definition = item != nullptr ? state.catalog.find(item->definitionHash) : nullptr;
        return definition != nullptr && carries_power(*definition);
    });
    const bool waiting = std::any_of(state.picked.begin(), state.picked.end(), [](std::uint64_t instance) {
        const edit::Item* item = find_owned_item(instance);
        return item != nullptr && item->postmaster;
    });
    if (powered) {
        ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
        ImGui::SetNextItemWidth(pixels(kPickPowerWidth));
        (void)ImGui::InputInt("##pick_power", &state.pickPower, 0, 0);
        // The field has no label of its own beside it; the button after it names what it is for.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Power to set the selected weapons and armor to.");
        }
        const bool entered = ImGui::IsItemDeactivated()
                             && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        state.pickPower = std::clamp(state.pickPower, 0, kPowerSliderMaximum);
        ImGui::SameLine();
        if (ImGui::Button("Set Power") || entered) {
            bulk.kind = Bulk::Kind::power;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Set selected gear to this Power.");
        }
    }
    if (waiting) {
        ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
        if (ImGui::Button("Pull")) {
            bulk.kind = Bulk::Kind::pull;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pull selected items from the Postmaster.");
        }
    }
    ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
    if (ImGui::Button("Remove")) {
        ImGui::OpenPopup(kRemovePickedId);
    }

    // Select all and Clear sit at the far end, apart from what acts on the items, or on a line of
    // their own when the actions leave them no room.
    const char* selectAll = "Select All Shown";
    const char* clear = "Clear Selection";
    const float trailing = ImGui::CalcTextSize(selectAll).x + ImGui::CalcTextSize(clear).x
                           + (style.FramePadding.x * 4.0F) + style.ItemSpacing.x;
    ImGui::SameLine();
    ImVec2 at = ImGui::GetCursorScreenPos();
    if (at.x + trailing > right) {
        at = {origin.x + inset, at.y + ImGui::GetFrameHeight() + style.ItemSpacing.y};
    } else {
        at.x = right - trailing;
    }
    ImGui::SetCursorScreenPos(at);
    if (ImGui::Button(selectAll)) {
        for (const edit::Item* item : shown) {
            if (!is_picked(item->instanceSoid)) {
                state.picked.push_back(item->instanceSoid);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(clear)) {
        state.picked.clear();
    }
    const float bottom = at.y + ImGui::GetFrameHeight() + padding;
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(origin, {origin.x + width, bottom}, ImGui::GetColorU32(ImGuiCol_Header), pixels(controls::kRowRounding));
    draw->AddRectFilled(origin, {origin.x + pixels(controls::kRailWidth), bottom}, ImGui::GetColorU32(ImGuiCol_CheckMark));
    draw->ChannelsMerge();
    if (draw_remove_picked_modal()) {
        bulk.kind = Bulk::Kind::remove;
    }
    ImGui::SetCursorScreenPos({origin.x, bottom});
    ImGui::Dummy({width, 0.0F});
    return bulk;
}

/** One search result kept somewhere other than the page's own list. */
struct Hit {
    /** Character that keeps it, or `kAccountItems` for an account stack. */
    std::size_t owner{kAccountItems};
    const edit::CatalogItem* definition{};
    /** The item a character keeps, or null for a stack. */
    const edit::Item* item{};
    /** Where the owner keeps it: equipped, at the postmaster, or neither. */
    bool equipped{};
    bool postmaster{};
    int quantity{};
};

/**
 * @return Every item matching a search that the page does not already list: what each other
 * character keeps, and the account's stacks when asked for.
 * @param skip Character whose items the page lists itself, or `kAccountItems` to leave none out.
 */
[[nodiscard]] std::vector<Hit> search_elsewhere(const std::string& search, std::size_t skip, bool stacks) noexcept {
    const Model& state = model();
    const state::AccountState& account = state.draft->after;
    const Query query = read_query(search);
    std::vector<Hit> hits;
    const auto consider = [&](std::size_t owner, const edit::Item& item, bool equipped) {
        const edit::CatalogItem* definition = state.catalog.find(item.definitionHash);
        if (definition != nullptr && passes(query, *definition, &item, equipped)) {
            hits.push_back({owner, definition, &item, equipped, item.postmaster, item.quantity});
        }
    };
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        if (c == skip) {
            continue;
        }
        const state::CharacterState& other = account.characters[c];
        for (const auto& slot : other.equipment.slots) {
            if (slot) {
                consider(c, *slot, true);
            }
        }
        for (std::size_t i = 0; i < other.inventory.count; ++i) {
            consider(c, other.inventory.values[i], false);
        }
    }
    for (std::size_t i = 0; stacks && i < account.profileItemCount; ++i) {
        const auto& stack = account.profileItems[i];
        const edit::CatalogItem* definition = state.catalog.find(stack.definitionHash);
        if (definition != nullptr && passes(query, *definition, nullptr, false)) {
            hits.push_back({kAccountItems, definition, nullptr, false, false, stack.quantity});
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.definition->name < b.definition->name;
    });
    return hits;
}

/**
 * Takes the page to where one result is kept: the character that keeps it, with the item open in
 * the pane and the search carried over, or the account page for a stack.
 * @param search The search that found it, which the page it goes to is given.
 */
void go_to(const Hit& hit, const char* search) noexcept {
    Model& state = model();
    if (hit.owner == kAccountItems) {
        state.view = View::profileInventory;
        (void)std::snprintf(state.profileSearch, sizeof state.profileSearch, "%s", search);
        return;
    }
    state.character = hit.owner;
    state.view = View::characterInventory;
    (void)std::snprintf(state.inventorySearch, sizeof state.inventorySearch, "%s", search);
    state.inventorySlot = -1;
    state.picked.clear();
    state.browse.type.clear();
    state.results.key.clear();
    select(*hit.definition, hit.item->instanceSoid);
}

/**
 * Draws one result kept elsewhere as a compact row: the icon, the name, and at the far end who
 * keeps it and where. The item's own tooltip is under the pointer, and the row goes to it.
 * @return True when the row was pressed.
 */
[[nodiscard]] bool draw_hit(const Hit& hit, float width, float rowHeight) noexcept {
    auto* draw = ImGui::GetWindowDrawList();
    const float icon = pixels(kProfileIconExtent);
    const float inset = pixels(kProfileRowInset);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 corner{origin.x + width, origin.y + rowHeight};
    const bool clicked = ImGui::InvisibleButton("hit", {width, rowHeight});
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddRectFilled(origin, corner, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), pixels(controls::kRowRounding));
    }
    draw->AddLine({origin.x, corner.y}, {corner.x, corner.y}, ImGui::GetColorU32(kProfileRowRule));
    art::icon(*hit.definition, {origin.x + inset, origin.y + ((rowHeight - icon) * 0.5F)}, icon);

    std::string where = hit.owner == kAccountItems ? std::string(kAccountLabel) : character_label(hit.owner);
    if (hit.equipped) {
        where += std::string("  /  ") + kEquippedWhere;
    } else if (hit.postmaster) {
        where += std::string("  /  ") + kPostmasterWhere;
    } else if (hit.item == nullptr || hit.quantity > 1) {
        where += "  /  " + std::to_string(hit.quantity);
    }
    const float whereWidth = ImGui::CalcTextSize(where.c_str()).x;
    const float text = origin.y + ((rowHeight - ImGui::GetTextLineHeight()) * 0.5F);
    draw->AddText({corner.x - inset - whereWidth, text}, ImGui::GetColorU32(tooltip::muted()), where.c_str());
    const float nameLeft = origin.x + inset + icon + inset;
    art::clipped_text(hit.definition->name,
                      {nameLeft, text},
                      (std::max)(0.0F, corner.x - inset - whereWidth - inset - nameLeft),
                      ImGui::GetColorU32(ImGuiCol_Text));
    if (hovered) {
        tooltip::draw(*hit.definition, hit.item);
    }
    return clicked;
}

/**
 * Draws what a search finds elsewhere on the account, under the page's own results, in a section
 * of its own that folds like any other. Pressing a result takes the page to it.
 * @param query The search, already folded for matching.
 * @param search The search as typed, which the page a result is on is given.
 * @param skip Character whose items the page lists itself, or `kAccountItems`.
 * @param stacks True to include the account's stacks, which the account page lists itself.
 * @return True when anything was found.
 */
bool draw_elsewhere(const std::string& query, const char* search, std::size_t skip, bool stacks) noexcept {
    if (query.empty()) {
        return false;
    }
    const std::vector<Hit> hits = search_elsewhere(query, skip, stacks);
    if (hits.empty()) {
        return false;
    }
    controls::space(controls::kSectionSpacing);
    if (!controls::section_header(kElsewhereLabel, hits.size())) {
        return true;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int columns = 1;
    float width = 0.0F;
    card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kProfileRowMinimumWidth), columns, width);
    const auto perRow = static_cast<std::size_t>(columns);
    const std::size_t lines = (hits.size() + perRow - 1) / perRow;
    const float rowHeight = pixels(kProfileIconExtent) + (pixels(kProfileRowPadding) * 2.0F);
    const Hit* pressed = nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, 0.0F});
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines), rowHeight);
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            for (std::size_t column = 0; column < perRow; ++column) {
                const std::size_t index = (static_cast<std::size_t>(line) * perRow) + column;
                if (index >= hits.size()) {
                    break;
                }
                if (column != 0) {
                    ImGui::SameLine(0.0F, spacing);
                }
                ImGui::PushID(static_cast<int>(index));
                if (draw_hit(hits[index], width, rowHeight)) {
                    pressed = &hits[index];
                }
                ImGui::PopID();
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    // The page changes character or view on a press, so the jump waits until the list is drawn.
    if (pressed != nullptr) {
        const std::string typed = search;
        go_to(*pressed, typed.c_str());
    }
    return true;
}

/** Draws the slot picker that narrows the character item list. */
void draw_slot_picker() noexcept {
    Model& state = model();
    const char* preview =
        state.inventorySlot < 0 ? "All Slots" : edit::kSlots[state.inventorySlot];
    if (!controls::begin_picker("##inventory_slot", preview, pixels(kSlotPickerWidth))) {
        return;
    }
    if (controls::picker_row("All Slots", state.inventorySlot < 0)) {
        state.inventorySlot = -1;
    }
    for (int slot = 0; slot < static_cast<int>(kSlotCount); ++slot) {
        if (controls::picker_row(edit::kSlots[slot], state.inventorySlot == slot)) {
            state.inventorySlot = slot;
        }
    }
    controls::end_picker();
}

/**
 * @return The group one stored item belongs to.
 * Buckets carry no name in the installed build, so gear is grouped by the equipment slot its
 * bucket feeds and everything else by its own item type, which is what the game calls it.
 */
[[nodiscard]] std::string item_group(const edit::Item& item,
                                     const edit::CatalogItem& definition) noexcept {
    if (item.postmaster) {
        return "Postmaster";
    }
    if (definition.slot < inv::kEquipmentSlotCount) {
        return edit::kSlots[definition.slot];
    }
    return definition.type.empty() ? std::string("Other") : definition.type;
}

/** The +1 and +2 bucket ranks only sit past the equipment run while the two counts agree. */
static_assert(kSlotCount == inv::kEquipmentSlotCount,
              "Bucket ranks assume the module and the account agree on the slot count.");

/** Draws everything the character carries, equipped and stowed, divided into its buckets. */
void draw_character_items() noexcept {
    Model& state = model();
    prune_picks();
    ImGui::SetNextItemWidth(pixels(kSearchWidth));
    (void)controls::search("##inventory_search",
                           "Search Inventory...",
                           state.inventorySearch,
                           sizeof state.inventorySearch);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kSearchTip);
    }
    ImGui::SameLine();
    draw_slot_picker();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    // The list is built once, before the child, so the count and the grid agree on the filters.
    const std::vector<const edit::Item*> items = filtered_character_items();
    const std::size_t held = character().inventory.count + equipped_count();
    if (items.size() == held) {
        ImGui::TextDisabled("%zu Items", held);
    } else {
        ImGui::TextDisabled("%zu of %zu", items.size(), held);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ctrl+click items to select several at once.");
    }
    draw_add_action(Category::weapons);
    // Sending or removing moves the items the grid is about to read, so an action on the selection
    // waits until the grid is drawn.
    Bulk bulk;
    if (!state.picked.empty()) {
        controls::space(controls::kRowSpacing);
        bulk = draw_pick_bar(items);
    }

    if (!ImGui::BeginChild("owned")) {
        ImGui::EndChild();
        run_bulk(bulk);
        return;
    }
    std::map<std::string, std::vector<const edit::Item*>> groups;
    std::map<std::string, std::size_t> order;
    for (const edit::Item* item : items) {
        const edit::CatalogItem* definition = state.catalog.find(item->definitionHash);
        if (definition == nullptr) {
            continue;
        }
        const std::string group = item_group(*item, *definition);
        groups[group].push_back(item);
        // Rank is decided from the same three cases the name is, so one cannot disagree with the
        // other. `emplace` is first-wins, and the Postmaster is the one bucket whose name is fixed
        // while its rank was taken from whichever item reached it first: a postmastered hand
        // cannon ranked it 0, which floated it up beside Kinetic.
        order.emplace(group,
                      item->postmaster ? kSlotCount + 2
                      : definition->slot < inv::kEquipmentSlotCount
                          ? definition->slot
                      : definition->type.empty() ? kSlotCount + 1
                                                 : kSlotCount);
    }
    std::vector<std::pair<std::string, std::vector<const edit::Item*>>> ordered;
    ordered.reserve(groups.size());
    for (auto& entry : groups) {
        ordered.emplace_back(entry.first, std::move(entry.second));
    }
    std::sort(ordered.begin(), ordered.end(), [&order](const auto& a, const auto& b) {
        const std::size_t left = order[a.first];
        const std::size_t right = order[b.first];
        return left == right ? a.first < b.first : left < right;
    });

    // One bucket per collapsing section, each holding the same responsive card grid the loadout
    // uses. Sundial lays its character inventory out this way; splitting the page into fixed
    // columns of single-column lists wasted most of the width whatever the tiles were sized at.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    for (const auto& [group, members] : ordered) {
        if (!controls::section_header(group.c_str(), members.size())) {
            continue;
        }
        int columns = 1;
        float width = 0.0F;
        card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kCardMinimumWidth), columns, width);
        // Every card is resolved before the grid runs. Re-resolving inside it meant a null could
        // `continue` after SameLine had already fired, which left the next card taking the skipped
        // card's cell and the column accounting drifting for the rest of the bucket; and the row
        // height was being measured on the pre-edit pointer while the card drew from the resolved
        // one, so an equip inside the bucket could clip a card's own socket row.
        std::vector<edit::Item*> live;
        live.reserve(members.size());
        for (const edit::Item* member : members) {
            if (edit::Item* resolved = find_owned_item(member->instanceSoid)) {
                live.push_back(resolved);
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, pixels(kCardColumnSpacing)});
        const auto perRow = static_cast<std::size_t>(columns);
        float rowHeight = 0.0F;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (i % perRow == 0) {
                // The row is drawn at its tallest card, so a bucket of socketless items packs in.
                rowHeight = 0.0F;
                for (std::size_t j = i; j < live.size() && j < i + perRow; ++j) {
                    rowHeight = (std::max)(rowHeight, card::natural_height(live[j], width));
                }
            } else {
                ImGui::SameLine(0.0F, spacing);
            }
            edit::Item* item = live[i];
            const edit::CatalogItem* definition = state.catalog.find(item->definitionHash);
            const bool equipped = is_equipped(item->instanceSoid);
            // The bucket already names the slot, so the card only says when the item is equipped.
            card::draw(item,
                       equipped ? "Equipped" : nullptr,
                       definition != nullptr ? definition->slot : kSlotCount,
                       equipped         ? card::Action::swap
                       : item->postmaster ? card::Action::pull
                                          : card::Action::equip,
                       width,
                       rowHeight);
        }
        ImGui::PopStyleVar();
        controls::space(controls::kSectionSpacing);
    }
    const std::string query = edit::searchable(state.inventorySearch);
    if (items.empty()) {
        ImGui::TextDisabled(query.empty() ? "No items match." : "Nothing on this character matches.");
    }
    // A search reaches past this character: what the others keep, and the account's stacks.
    (void)draw_elsewhere(query, state.inventorySearch, state.character, true);
    ImGui::EndChild();
    run_bulk(bulk);
}

/** Removes one account stack from the draft, keeping the remaining rows packed. */
void erase_profile_item(std::size_t index) noexcept {
    state::AccountState& account = model().draft->after;
    for (std::size_t row = index + 1; row < account.profileItemCount; ++row) {
        account.profileItems[row - 1] = account.profileItems[row];
    }
    account.profileItems[--account.profileItemCount] = {};
    mark_changed();
}

/**
 * Draws one account-wide stack as a single compact row.
 * Everything is laid out through the normal cursor: the icon, a spacer that reserves the name
 * column, then the controls. Positioning any of it with SetCursorPos left the parent unable to
 * size itself, which Dear ImGui reports and which broke the later columns.
 * @param index Row in the draft profile list.
 * @param width Card width in framebuffer pixels.
 * @param rowHeight Content height in framebuffer pixels, shared with the caller's clipper so the
 * lines it skips are the same height as the lines it draws.
 * @return True when the card's removal was confirmed. The caller erases it after the loop.
 */
[[nodiscard]] bool draw_profile_item(std::size_t index, float width, float rowHeight) noexcept {
    Model& state = model();
    state::account::inventory::ProfileItem& item = state.draft->after.profileItems[index];
    const edit::CatalogItem* definition = state.catalog.find(item.definitionHash);
    const float icon = pixels(kProfileIconExtent);
    const float inset = pixels(kProfileRowInset);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float quantityWidth = pixels(kQuantityWidth);
    const float removeWidth =
        ImGui::CalcTextSize(kRemoveLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    const float nameWidth = (std::max)(
        0.0F, width - (inset * 3.0F) - icon - quantityWidth - removeWidth - (spacing * 2.0F));

    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginGroup();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // The row's extent is known before its content, so the fill, the hover and the tooltip can all
    // be settled up front. The fill is painted first because the draw list paints in order, and
    // filling after the group had measured itself covered the icon and the name with a blank card.
    const ImVec2 fillMin{origin.x, origin.y - pixels(kProfileRowPadding)};
    const ImVec2 fillMax{origin.x + width, origin.y + rowHeight + pixels(kProfileRowPadding)};
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(fillMin, fillMax);
    // The game lists its currencies and materials as plain rows ruled off from one another, with
    // nothing filled in until the pointer reaches a row.
    if (hovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            fillMin, fillMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    }
    ImGui::GetWindowDrawList()->AddLine({fillMin.x, fillMax.y},
                                        {fillMax.x, fillMax.y},
                                        ImGui::GetColorU32(kProfileRowRule));

    ImGui::Dummy({inset, icon});
    ImGui::SameLine(0.0F, 0.0F);
    const ImVec2 iconAt = ImGui::GetCursorScreenPos();
    if (definition != nullptr) {
        art::icon(*definition, iconAt, icon);
    }
    ImGui::Dummy({icon, icon});

    ImGui::SameLine(0.0F, inset);
    const ImVec2 nameAt = ImGui::GetCursorScreenPos();
    art::clipped_text(definition != nullptr ? definition->name : std::string("Unknown Item"),
                      {nameAt.x, nameAt.y + ((icon - ImGui::GetTextLineHeight()) * 0.5F)},
                      nameWidth,
                      ImGui::GetColorU32(definition != nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    ImGui::Dummy({nameWidth, icon});
    // The item reads the same here as it does anywhere else in the editor. The controls at the far
    // end are excluded, so the tooltip does not stand over the field the player is reaching for.
    if (hovered && definition != nullptr
        && ImGui::GetIO().MousePos.x < nameAt.x + nameWidth) {
        tooltip::draw(*definition, nullptr);
    }

    ImGui::SameLine(0.0F, spacing);
    const int limit = definition != nullptr ? (std::max)(1, definition->detail.maxStackSize)
                                            : kUnknownStackLimit;
    ImGui::SetNextItemWidth(quantityWidth);
    // An empty label keeps Dear ImGui from printing one after the stepper buttons.
    const bool changed = ImGui::InputInt("##quantity", &item.quantity, 0, 0);
    if (changed) {
        item.quantity = std::clamp(item.quantity, 1, limit);
    }
    record_scalar_edit(changed);

    ImGui::SameLine(0.0F, spacing);
    // The removal is offered only on the row under the pointer, so a page of stacks does not read
    // as a page of Remove buttons. Its width is reserved either way, so the columns hold still.
    if (hovered || ImGui::IsPopupOpen(kRemoveStackTitle)) {
        if (ImGui::Button(kRemoveLabel)) {
            ImGui::OpenPopup(kRemoveStackTitle);
        }
    } else {
        ImGui::Dummy({removeWidth, ImGui::GetFrameHeight()});
    }

    bool removed = false;
    if (ImGui::BeginPopupModal(kRemoveStackTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(definition != nullptr ? definition->name.c_str() : "Unknown Item");
        if (ImGui::Button("Remove Stack")) {
            removed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return removed;
}

/**
 * @return The section one account stack belongs under, which is what the game calls the item.
 * The profile holds currencies, materials, mods and shaders all in one run of rows, so its own
 * order says nothing a player is looking for; the type does.
 */
[[nodiscard]] std::string profile_group(const edit::CatalogItem* definition) noexcept {
    if (definition == nullptr) {
        return kUnknownGroup;
    }
    return definition->type.empty() ? std::string(kUntypedGroup) : definition->type;
}

/** One section of the account page: its heading, and the draft rows filed under it. */
struct ProfileGroup {
    std::string name;
    std::vector<std::size_t> rows;
};

/**
 * @return Every account stack matching the page search, gathered into its sections.
 * Sections read alphabetically, with the two catch-alls last so a named type is never buried
 * under them, and each section's rows read by name.
 */
[[nodiscard]] std::vector<ProfileGroup> grouped_profile_items() noexcept {
    Model& state = model();
    const std::string search = edit::searchable(state.profileSearch);
    const Query query = read_query(search);
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < state.draft->after.profileItemCount; ++i) {
        const edit::CatalogItem* definition =
            state.catalog.find(state.draft->after.profileItems[i].definitionHash);
        // A stack the catalog does not carry still has to be reachable, so an empty search keeps
        // it and any typed search drops it: there is no name to match it against.
        if (!search.empty() && (definition == nullptr || !passes(query, *definition, nullptr, false))) {
            continue;
        }
        groups[profile_group(definition)].push_back(i);
    }

    const auto rank = [](const std::string& name) {
        return name == kUntypedGroup ? 1 : name == kUnknownGroup ? 2 : 0;
    };
    const auto byName = [&state](std::size_t left, std::size_t right) {
        const edit::CatalogItem* first =
            state.catalog.find(state.draft->after.profileItems[left].definitionHash);
        const edit::CatalogItem* second =
            state.catalog.find(state.draft->after.profileItems[right].definitionHash);
        if (first == nullptr || second == nullptr) {
            return first != nullptr;
        }
        return first->name < second->name;
    };
    std::vector<ProfileGroup> ordered;
    ordered.reserve(groups.size());
    for (auto& [name, rows] : groups) {
        std::sort(rows.begin(), rows.end(), byName);
        ordered.push_back({name, std::move(rows)});
    }
    std::sort(ordered.begin(), ordered.end(), [&rank](const ProfileGroup& a, const ProfileGroup& b) {
        const int left = rank(a.name);
        const int right = rank(b.name);
        return left == right ? a.name < b.name : left < right;
    });
    return ordered;
}

/**
 * Draws one section of account stacks as a responsive grid.
 * @param rows Draft indices filed under this section.
 * @param rowHeight Row content height in framebuffer pixels.
 * @param removal Receives the draft index whose removal was confirmed, if any.
 */
void draw_profile_group(const std::vector<std::size_t>& rows,
                        float rowHeight,
                        std::size_t& removal) noexcept {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int columns = 1;
    float width = 0.0F;
    card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kProfileRowMinimumWidth), columns, width);
    const auto perRow = static_cast<std::size_t>(columns);
    const std::size_t lines = (rows.size() + perRow - 1) / perRow;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, pixels(kProfileRowGap)});
    // The account holds up to 701 stacks. Building all of them cost more per frame than the whole
    // of the rest of the page, so only the lines the player can see are submitted.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines), rowHeight + pixels(kProfileRowGap));
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            const std::size_t first = static_cast<std::size_t>(line) * perRow;
            float top = 0.0F;
            for (std::size_t column = 0; column < perRow && first + column < rows.size(); ++column) {
                if (column == 0) {
                    top = ImGui::GetCursorPosY();
                } else {
                    // SameLine carries the baseline of whatever the last row ended on, which left
                    // the columns of one line sitting a couple of pixels apart. Only the X it
                    // works out is wanted, so the line's own top is put back afterwards.
                    ImGui::SameLine(0.0F, spacing);
                    ImGui::SetCursorPosY(top);
                }
                if (draw_profile_item(rows[first + column], width, rowHeight)) {
                    removal = rows[first + column];
                }
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
}

/** Draws every account-wide stack, which belongs to the profile rather than a character. */
void draw_profile_items() noexcept {
    Model& state = model();
    const std::size_t held = state.draft->after.profileItemCount;
    const std::vector<ProfileGroup> groups = grouped_profile_items();
    std::size_t shown = 0;
    for (const ProfileGroup& group : groups) {
        shown += group.rows.size();
    }

    ImGui::SetNextItemWidth(pixels(kSearchWidth));
    (void)controls::search("##profile_search",
                           "Search Account Items...",
                           state.profileSearch,
                           sizeof state.profileSearch);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kSearchTip);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (shown == held) {
        ImGui::TextDisabled("%zu Stacks", held);
    } else {
        ImGui::TextDisabled("%zu of %zu", shown, held);
    }
    draw_add_action(Category::materials);

    if (!ImGui::BeginChild("profile_items")) {
        ImGui::EndChild();
        return;
    }
    const std::string query = edit::searchable(state.profileSearch);
    if (groups.empty()) {
        ImGui::TextDisabled(query.empty() ? "No items match." : "No account items match.");
        // A search reaches the characters too, whose items this page does not list.
        (void)draw_elsewhere(query, state.profileSearch, kAccountItems, false);
        ImGui::EndChild();
        return;
    }
    const float rowHeight = (std::max)(pixels(kProfileIconExtent), ImGui::GetFrameHeight());
    // A confirmed removal moves every later row, so the list is left and erased afterwards.
    std::size_t removal = held;
    for (const ProfileGroup& group : groups) {
        if (!controls::section_header(group.name.c_str(), group.rows.size())) {
            continue;
        }
        draw_profile_group(group.rows, rowHeight, removal);
        controls::space(controls::kSectionSpacing);
    }
    if (removal < held) {
        erase_profile_item(removal);
    }
    (void)draw_elsewhere(query, state.profileSearch, kAccountItems, false);
    ImGui::EndChild();
}

} // namespace

bool is_picked(std::uint64_t instance) noexcept {
    const std::vector<std::uint64_t>& picked = model().picked;
    return std::find(picked.begin(), picked.end(), instance) != picked.end();
}

void toggle_pick(std::uint64_t instance) noexcept {
    std::vector<std::uint64_t>& picked = model().picked;
    const auto at = std::find(picked.begin(), picked.end(), instance);
    if (at == picked.end()) {
        picked.push_back(instance);
    } else {
        picked.erase(at);
    }
}

void draw_character_inventory_page() noexcept {
    draw_character_items();
}

void draw_profile_inventory_page() noexcept {
    draw_profile_items();
}

} // namespace dawn::core::ui::modules::loadout::internal
