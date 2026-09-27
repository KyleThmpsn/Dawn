// SPDX-License-Identifier: GPL-3.0-only
#include "edit.h"
#include "../persistence/persistence.h"
#include "../account/inventory/placement.h"
#include "../equipment/light/definition.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

namespace dawn::state::editor {
namespace inv = account::inventory;
namespace {
/** The subclass's own equipment slot, which its abilities belong to. */
constexpr std::size_t kSubclassSlot = 11;
/** The character's ability entries, in the order the catalog lists a subclass's ability lanes. */
constexpr std::array<std::uint8_t CharacterState::*, 5> kAbilityFields{
    &CharacterState::movementAbilityEntry, &CharacterState::grenadeAbilityEntry, &CharacterState::superAbilityEntry,
    &CharacterState::meleeAbilityEntry, &CharacterState::classAbilityEntry};
static_assert(kAbilityFields.size() == std::tuple_size_v<decltype(CatalogItem::abilities)>, "Every catalog ability lane needs its character field.");
Stats contribution(const CatalogItem& item, const Catalog& catalog) {
    Stats result{};
    // The detail catalog does not bound this count, and a cache record carries whatever it was
    // written with, so the array's own size is the only limit that can be relied on.
    const std::size_t count = (std::min)(static_cast<std::size_t>(item.detail.statCount), item.detail.stats.size());
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t j = 0; j < result.size(); ++j)
            if (item.detail.stats[i].row == catalog.statRows[j]) result[j] += item.detail.stats[i].value;
    return result;
}
void sum(Stats& into, const Stats& values, int sign = 1) {
    for (std::size_t i = 0; i < into.size(); ++i) into[i] += sign * values[i];
}
bool nonzero(const Stats& values) { return std::any_of(values.begin(), values.end(), [](int v) { return v != 0; }); }
// The row generation is handed out, then advanced past. The family-four character encoder rejects
// a loadout whose item carries the serial the counter still points at, so the value assigned here
// must stay strictly below it. This matches how the runtime grants a serial on acquisition.
bool bump(CharacterState& character, Item& item) {
    if (character.nextInventorySerial >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) return false;
    // The character wire record requires every item revision to be strictly below next.
    item.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    return true;
}
// Repairs a character whose counter does not lead every row it owns. Accounts saved by the editor
// before the serial fix carry one such row, and the character object refuses to encode until the
// counter passes it, which leaves the game unable to publish that character at all. The encoder
// also refuses a counter below the number of rows it publishes, and that count is every equipped
// item plus every stored one, not the stored ones alone.
bool prepare_serial_counter(CharacterState& character) {
    auto next = character.nextInventorySerial;
    std::uint32_t count = 0;
    const auto include = [&](const Item& item) {
        ++count;
        if (item.mutationSerial < 0 || item.mutationSerial == INT32_MAX) return false;
        next = (std::max)(next, static_cast<std::uint32_t>(item.mutationSerial) + 1U);
        return true;
    };
    for (const auto& item : character.equipment.slots) if (item && !include(*item)) return false;
    for (std::size_t i = 0; i < character.inventory.count; ++i)
        if (!include(character.inventory.values[i])) return false;
    next = (std::max)(next, count);
    if (next > static_cast<std::uint32_t>(INT32_MAX)) return false;
    character.nextInventorySerial = next;
    return true;
}
bool exotic_conflict(const CharacterState& character, const CatalogItem& item, const Catalog& catalog) {
    if (item.definition.tier != 5 || (item.kind != GearKind::weapon && item.kind != GearKind::armor)) return false;
    for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
        if (slot == item.slot || !character.equipment.slots[slot]) continue;
        const auto* equipped = catalog.find(character.equipment.slots[slot]->definitionHash);
        if (equipped && equipped->kind == item.kind && equipped->definition.tier == 5) return true;
    }
    return false;
}
bool equip_character(CharacterState& character, const Catalog& catalog, std::uint64_t id, std::string& error) {
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        auto& source = character.inventory.values[i];
        if (source.instanceSoid != id) continue;
        const auto* definition = catalog.find(source.definitionHash);
        if (!definition || definition->slot >= inv::kEquipmentSlotCount || source.postmaster
            || !fits_class(*definition, character.characterClass)) { error = "This item cannot be equipped by this character."; return false; }
        if (exotic_conflict(character, *definition, catalog)) { error = "Only one exotic weapon and one exotic armor piece can be equipped."; return false; }
        auto& target = character.equipment.slots[definition->slot];
        if (target) {
            std::swap(*target, source);
            if (!bump(character, source)) { error = "Item revision limit reached."; return false; }
        } else { target = source; inv::erase(character, i); }
        if (!bump(character, *target)) { error = "Item revision limit reached."; return false; }
        return true;
    }
    error = "Select an inventory item first."; return false;
}
}
bool materialize(Item& item, const Catalog& catalog) {
    const auto* definition = catalog.find(item.definitionHash);
    if (!definition) return false;
    if (item.sockets.policy == inv::SocketPolicy::authored) return item.sockets.plugCount == definition->detail.ordinarySocketCount;
    inv::Sockets sockets;
    sockets.policy = inv::SocketPolicy::authored;
    sockets.plugCount = definition->detail.ordinarySocketCount;
    for (std::size_t i = 0; i < sockets.plugCount; ++i) {
        const auto id = definition->detail.initialPlugIndices[i];
        if (id == build_data::items::details::kUnavailableItemIndex) continue;
        const auto* plug = catalog.index(id);
        if (!plug) return false;
        sockets.plugs[i] = plug->definition.definitionHash;
    }
    item.sockets = sockets;
    return true;
}
bool set_plug(Item& item, const Catalog& catalog, std::size_t lane, std::uint16_t id, PlugScope scope) {
    const auto* definition = catalog.find(item.definitionHash);
    const auto* plug = catalog.index(id);
    if (!definition || !plug || !plug->plug || lane >= definition->detail.ordinarySocketCount) return false;
    const auto options = catalog.candidates(*definition, lane, scope);
    if (!std::binary_search(options.begin(), options.end(), id)) return false;
    Item staged = item;
    if (!materialize(staged, catalog)) return false;
    staged.sockets.plugs[lane] = plug->definition.definitionHash;
    // Authored plugs take precedence. A prior randomized offer must not mask the new choice.
    staged.rolledLaneMask &= static_cast<std::uint16_t>(~(1U << lane));
    staged.availablePlugRows[lane] = 0;
    staged.randomRoll = {};
    item = staged;
    return true;
}
std::string_view stat_label(const Catalog& catalog, std::size_t index) noexcept {
    if (index < catalog.statRows.size()) {
        const auto found = catalog.statNames.find(catalog.statRows[index]);
        if (found != catalog.statNames.end() && !found->second.empty()) return found->second;
    }
    return index < std::size(kStats) ? kStats[index] : std::string_view{};
}

Stats item_stats(const Item& item, const Catalog& catalog) {
    const auto* definition = catalog.find(item.definitionHash);
    if (!definition) return {};
    auto result = contribution(*definition, catalog);
    Item resolved = item;
    if (materialize(resolved, catalog)) for (std::size_t i = 0; i < resolved.sockets.plugCount; ++i)
        if (resolved.sockets.plugs[i]) if (const auto* plug = catalog.find(*resolved.sockets.plugs[i])) sum(result, contribution(*plug, catalog));
    return result;
}
/**
 * Armor 2.0 rolls its stats through two pairs of allocation sockets: two carrying a spread over
 * the top three stats (Mobility, Resilience, Recovery) and two over the bottom three. These are
 * the socket types the game gives them; Sundial identifies them the same way.
 */
constexpr std::uint16_t kTopAllocationSocketTypes[]{760, 761};
constexpr std::uint16_t kBottomAllocationSocketTypes[]{762, 763};
enum class Allocation { none, top, bottom };
Allocation allocation_of_socket(std::uint16_t socketType) {
    for (auto type : kTopAllocationSocketTypes) if (type == socketType) return Allocation::top;
    for (auto type : kBottomAllocationSocketTypes) if (type == socketType) return Allocation::bottom;
    return Allocation::none;
}
/** @return True when a plug spreads its stats over one allocation group and nothing outside it. */
bool allocation_plug(const CatalogItem& plug, const Catalog& catalog, Allocation group) {
    const Stats values = contribution(plug, catalog);
    bool inside = false;
    for (std::size_t shown = 0; shown < values.size(); ++shown) {
        const bool top = shown < 3;
        const int value = values[catalog.statOrder[shown]];
        if (value == 0) continue;
        if ((group == Allocation::top) != top) return false;
        inside = true;
    }
    return inside;
}
/**
 * @return The stat plugs one lane may take in place of the plug it holds.
 * An allocation socket draws on every allocation plug of its group the installed armor ever
 * rolls, since its own pool holds nothing but the roll it came with. Any other stat-bearing
 * socket stays inside its own pool: a stat mod trades for another stat mod, an archetype for
 * another archetype.
 */
std::vector<std::uint16_t> stat_choices(const CatalogItem& definition, std::size_t lane, const CatalogItem& current, const Catalog& catalog) {
    std::vector<std::uint16_t> choices;
    const Allocation group = lane < definition.detail.socketTypes.size()
        ? allocation_of_socket(definition.detail.socketTypes[lane]) : Allocation::none;
    if (group != Allocation::none) {
        for (auto type : group == Allocation::top ? kTopAllocationSocketTypes : kBottomAllocationSocketTypes) {
            const auto pool = catalog.socketPools.find(type);
            if (pool == catalog.socketPools.end()) continue;
            for (auto id : pool->second) {
                const auto* choice = catalog.index(id);
                if (choice && choice->definition.plugCategoryHash == current.definition.plugCategoryHash
                    && allocation_plug(*choice, catalog, group)) choices.push_back(id);
            }
        }
        choices.push_back(current.definition.definitionIndex);
        std::sort(choices.begin(), choices.end());
        choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
        return choices;
    }
    for (auto id : definition.compatible[lane]) {
        const auto* choice = catalog.index(id);
        if (choice && choice->definition.plugCategoryHash == current.definition.plugCategoryHash
            && nonzero(contribution(*choice, catalog))) choices.push_back(id);
    }
    return choices;
}
bool adjustable_stats(const Item& item, const Catalog& catalog) {
    const auto* definition = catalog.find(item.definitionHash);
    if (!definition || definition->kind != GearKind::armor) return false;
    Item resolved = item;
    if (!materialize(resolved, catalog)) return false;
    for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) {
        const auto* current = resolved.sockets.plugs[lane] ? catalog.find(*resolved.sockets.plugs[lane]) : nullptr;
        if (!current || !nonzero(contribution(*current, catalog))) continue;
        for (auto id : stat_choices(*definition, lane, *current, catalog)) {
            const auto* choice = catalog.index(id);
            if (choice && choice->definition.definitionHash != current->definition.definitionHash) return true;
        }
    }
    return false;
}
bool adjust_stats(Item& item, const Catalog& catalog, const Stats& targets, Stats& achieved) {
    const auto* definition = catalog.find(item.definitionHash);
    if (!definition || definition->kind != GearKind::armor) return false;
    Item original = item;
    if (!materialize(original, catalog)) return false;
    struct Plan { Item item; Stats values; unsigned changes{}; };
    std::vector<Plan> plans{{original, item_stats(original, catalog), 0}};
    bool mutableLane = false;
    const auto cost = [&](const Plan& p) {
        std::int64_t result = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) { const auto delta = std::int64_t(p.values[i]) - targets[i]; result += delta * delta; }
        return result;
    };
    for (std::size_t lane = 0; lane < original.sockets.plugCount; ++lane) {
        const auto* current = original.sockets.plugs[lane] ? catalog.find(*original.sockets.plugs[lane]) : nullptr;
        if (!current || !nonzero(contribution(*current, catalog))) continue;
        // Stat editing never replaces a gameplay perk with an arbitrary stat plug: a lane only
        // trades within the set `stat_choices` says it rolls from.
        const std::vector<std::uint16_t> choices = stat_choices(*definition, lane, *current, catalog);
        if (choices.size() < 2) continue;
        mutableLane = true;
        std::vector<Plan> next;
        for (const auto& plan : plans) for (auto id : choices) {
            const auto* choice = catalog.index(id);
            Plan candidate = plan;
            candidate.item.sockets.plugs[lane] = choice->definition.definitionHash;
            sum(candidate.values, contribution(*current, catalog), -1);
            sum(candidate.values, contribution(*choice, catalog));
            candidate.changes += choice->definition.definitionHash != current->definition.definitionHash;
            next.push_back(std::move(candidate));
        }
        std::sort(next.begin(), next.end(), [&](const Plan& a, const Plan& b) {
            const auto ca = cost(a), cb = cost(b);
            return ca == cb ? a.changes < b.changes : ca < cb;
        });
        // Deduplicate stat outcomes and retain a bounded beam between socket columns.
        plans.clear();
        for (auto& p : next) {
            if (std::none_of(plans.begin(), plans.end(), [&](const Plan& q) { return p.values == q.values; })) plans.push_back(std::move(p));
            if (plans.size() == 128) break;
        }
    }
    if (!mutableLane || plans.empty()) return false;
    item = plans.front().item; item.rolledLaneMask = 0; item.availablePlugRows = {}; item.randomRoll = {};
    achieved = plans.front().values;
    return true;
}
bool give(Draft& draft, const Catalog& catalog, std::size_t characterIndex, std::uint32_t hash, int quantity, int power, bool shouldEquip, std::string& error) {
    const auto* definition = catalog.find(hash);
    if (characterIndex >= draft.after.characterCount || !definition || quantity <= 0 || power < 0 || power > kMaximumItemLevel) {
        error = "Choose a valid item, quantity and item level."; return false;
    }
    auto staged = std::make_unique<AccountState>(draft.after);
    auto& character = staged->characters[characterIndex];
    build_data::inventory::buckets::Descriptor bucket{};
    if (!build_data::find_inventory_bucket_descriptor(definition->definition.bucketId, bucket)) { error = "This definition is a perk; insert it into a socket."; return false; }
    if (bucket.arraySelector == build_data::inventory::buckets::ArraySelector::profile) {
        if (shouldEquip || quantity > definition->detail.maxStackSize) { error = "This is an account item; check its stack limit."; return false; }
        auto existing = staged->profileItemCount;
        for (std::size_t i = 0; i < staged->profileItemCount; ++i) if (staged->profileItems[i].definitionHash == hash
            && staged->profileItems[i].quantity <= definition->detail.maxStackSize - quantity) { existing = i; break; }
        if (existing == staged->profileItemCount) {
            if (inv::profile_room(*staged, hash) < quantity || existing >= staged->profileItems.size()) { error = "This account inventory bucket is full."; return false; }
            auto& stack = staged->profileItems[staged->profileItemCount++];
            stack.definitionHash = hash;
            if (build_data::is_profile_action_source(definition->definition.definitionIndex, definition->definition.bucketId)
                && !persistence::next_profile_item_instance_soid(*staged, stack.instanceSoid)) { error = "Could not allocate an item identity."; return false; }
        }
        auto& stack = staged->profileItems[existing];
        if (stack.mutationSerial == (std::numeric_limits<std::int32_t>::max)()) { error = "Item revision limit reached."; return false; }
        stack.quantity += quantity; ++stack.mutationSerial;
    } else if (bucket.arraySelector == build_data::inventory::buckets::ArraySelector::character) {
        if (!fits_class(*definition, character.characterClass)) { error = "This item belongs to another class."; return false; }
        const bool instanced = definition->detail.instancedDefinitionState == build_data::items::details::InstancedDefinitionState::instanced;
        if (quantity > (instanced ? 1 : definition->detail.maxStackSize)) { error = "The quantity exceeds this item's stack limit."; return false; }
        if (character.inventory.count >= character.inventory.values.size() || !inv::has_room(character, bucket.bucketId)) { error = "This inventory slot is full. Free a space before adding an item."; return false; }
        Item item; item.definitionHash = hash; item.level = power; item.quantity = quantity;
        if (!persistence::next_item_instance_soid(*staged, item.instanceSoid) || !bump(character, item)) { error = "Could not allocate an item identity."; return false; }
        character.inventory.values[character.inventory.count++] = item;
        if (shouldEquip && !equip_character(character, catalog, item.instanceSoid, error)) return false;
    } else { error = "This item cannot be placed in the editable inventories."; return false; }
    draft.after = *staged; draft.dirty = true; error = "Added to draft."; return true;
}
bool equip(Draft& draft, const Catalog& catalog, std::size_t character, std::uint64_t id, std::string& error) {
    if (character >= draft.after.characterCount) return false;
    auto staged = std::make_unique<CharacterState>(draft.after.characters[character]);
    if (!equip_character(*staged, catalog, id, error)) return false;
    draft.after.characters[character] = *staged; draft.dirty = true; error = "Equipment updated in draft."; return true;
}
bool unequip(Draft& draft, const Catalog&, std::size_t character, std::size_t slot, std::string& error) {
    if (character >= draft.after.characterCount || slot >= inv::kEquipmentSlotCount) return false;
    auto& target = draft.after.characters[character];
    if (!target.equipment.slots[slot] || target.inventory.count >= target.inventory.values.size()) { error = "No room in inventory."; return false; }
    if (slot <= 7 || slot == 11) { error = "Replace this required equipment slot by equipping another item."; return false; }
    auto item = *target.equipment.slots[slot];
    if (!bump(target, item)) { error = "Item revision limit reached."; return false; }
    target.inventory.values[target.inventory.count++] = item; target.equipment.slots[slot].reset(); draft.dirty = true;
    error = "Moved to inventory in draft."; return true;
}
bool pull_from_postmaster(Draft& draft, const Catalog& catalog, std::size_t characterIndex, std::uint64_t id, std::string& error) {
    if (characterIndex >= draft.after.characterCount) return false;
    auto& character = draft.after.characters[characterIndex];
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        auto& item = character.inventory.values[i];
        if (item.instanceSoid != id) continue;
        if (!item.postmaster) { error = "This item is not at the postmaster."; return false; }
        const auto* definition = catalog.find(item.definitionHash);
        build_data::inventory::buckets::Descriptor bucket{};
        if (!definition || !build_data::find_inventory_bucket_descriptor(definition->definition.bucketId, bucket)) { error = "This item is missing from the installed build."; return false; }
        if (bucket.arraySelector != build_data::inventory::buckets::ArraySelector::character) { error = "This is an account item. Collect it from the postmaster in game."; return false; }
        // A postmaster row counts against the postmaster, so its own bucket is measured without it.
        if (!inv::has_room(character, definition->definition.bucketId)) { error = "Its inventory slot is full. Free a space before pulling it."; return false; }
        auto pulled = item; pulled.postmaster = false;
        if (!bump(character, pulled)) { error = "Item revision limit reached."; return false; }
        item = pulled; draft.dirty = true; error = "Pulled from the postmaster."; return true;
    }
    error = "This item is no longer on this character."; return false;
}
bool transfer(Draft& draft, const Catalog& catalog, std::size_t from, std::size_t to, std::uint64_t id, std::string& error) {
    if (from >= draft.after.characterCount || to >= draft.after.characterCount || from == to) return false;
    auto staged = std::make_unique<AccountState>(draft.after);
    auto& source = staged->characters[from];
    auto& target = staged->characters[to];
    for (std::size_t i = 0; i < source.inventory.count; ++i) {
        const auto item = source.inventory.values[i];
        if (item.instanceSoid != id) continue;
        if (item.postmaster) { error = "Pull it from the postmaster before sending it."; return false; }
        const auto* definition = catalog.find(item.definitionHash);
        if (!definition) { error = "This item is missing from the installed build."; return false; }
        if (!fits_class(*definition, target.characterClass)) { error = "That character's class cannot hold this item."; return false; }
        if (target.inventory.count >= target.inventory.values.size() || !inv::has_room(target, definition->definition.bucketId)) { error = "That character has no room for it. Free a space there first."; return false; }
        auto moved = item;
        // The receiving character's counter runs on its own and can be behind the item's revision,
        // which would move the item backwards. It is lifted past it first, so the item's own
        // revision only ever rises, as it does for every other edit.
        const auto previous = static_cast<std::uint32_t>(item.mutationSerial);
        if (target.nextInventorySerial <= previous) target.nextInventorySerial = previous + 1U;
        if (!bump(target, moved)) { error = "Item revision limit reached."; return false; }
        inv::erase(source, i);
        target.inventory.values[target.inventory.count++] = moved;
        draft.after = *staged; draft.dirty = true; error = "Sent to the other character."; return true;
    }
    error = "Only a stowed item can be sent. Unequip it first."; return false;
}
PieceState piece_state(const CharacterState& character, const Catalog& catalog, const SavedPiece& piece) noexcept {
    if (piece.instance == 0) return PieceState::empty;
    // A definition the installed build no longer carries, as a removed package leaves behind, has
    // nothing left to make it from, and one of another class could not be held if it were made.
    const CatalogItem* definition = catalog.find(piece.definition);
    if (!definition || !fits_class(*definition, character.characterClass)) return PieceState::unavailable;
    // An instance id only names one item within one account, so the item it names now has to be the
    // one saved: after a reset the same id can belong to something else entirely.
    for (const auto& slot : character.equipment.slots)
        if (slot && slot->instanceSoid == piece.instance && slot->definitionHash == piece.definition) return PieceState::equipped;
    for (std::size_t i = 0; i < character.inventory.count; ++i) {
        const auto& stowed = character.inventory.values[i];
        if (stowed.instanceSoid == piece.instance && stowed.definitionHash == piece.definition)
            return stowed.postmaster ? PieceState::unavailable : PieceState::stowed;
    }
    return PieceState::missing;
}
SavedLoadout capture_loadout(const CharacterState& character, const Catalog& catalog, std::string name) {
    SavedLoadout loadout; loadout.character = character.soid; loadout.name = std::move(name);
    for (std::size_t slot = 0; slot < loadout.pieces.size(); ++slot) {
        const auto& item = character.equipment.slots[slot];
        if (!item) continue;
        SavedPiece& piece = loadout.pieces[slot];
        piece = {item->instanceSoid, item->definitionHash, item->level, {}};
        // The plugs are kept as fitted, so a copy made again from them is the one that was saved.
        Item resolved = *item;
        if (materialize(resolved, catalog))
            for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) piece.plugs.push_back(resolved.sockets.plugs[lane].value_or(0U));
    }
    for (std::size_t lane = 0; lane < kAbilityFields.size(); ++lane) loadout.abilities[lane] = character.*kAbilityFields[lane];
    return loadout;
}
namespace {
/**
 * Makes one saved piece again and stows it: a new instance at the given level, with each saved plug
 * the build still carries fitted into its lane. The account gives the instance its id.
 * @return The new instance id, or zero when the piece cannot be made or has no room.
 */
std::uint64_t recreate(AccountState& account, CharacterState& character, const Catalog& catalog, const SavedPiece& piece, int level) {
    const CatalogItem* definition = catalog.find(piece.definition);
    build_data::inventory::buckets::Descriptor bucket{};
    if (!definition || !fits_class(*definition, character.characterClass)
        || !build_data::find_inventory_bucket_descriptor(definition->definition.bucketId, bucket)
        || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::character
        || character.inventory.count >= character.inventory.values.size() || !inv::has_room(character, bucket.bucketId)) return 0;
    Item item; item.definitionHash = piece.definition; item.level = std::clamp(level, 0, kMaximumItemLevel); item.quantity = 1;
    if (!piece.plugs.empty() && materialize(item, catalog)) {
        for (std::size_t lane = 0; lane < piece.plugs.size() && lane < item.sockets.plugCount; ++lane) {
            const CatalogItem* plug = piece.plugs[lane] != 0 ? catalog.find(piece.plugs[lane]) : nullptr;
            // A plug the build no longer carries, or one the lane no longer offers, keeps the default.
            if (plug != nullptr) (void)set_plug(item, catalog, lane, plug->definition.definitionIndex, PlugScope::all);
        }
    }
    if (!persistence::next_item_instance_soid(account, item.instanceSoid) || !bump(character, item)) return 0;
    character.inventory.values[character.inventory.count++] = item;
    return item.instanceSoid;
}
}
bool apply_loadout(Draft& draft, const Catalog& catalog, std::size_t characterIndex, const SavedLoadout& loadout,
                   int fallbackLevel, LoadoutResult& result, std::string& error) {
    result = {};
    if (characterIndex >= draft.after.characterCount) return false;
    // The whole account is staged, because a piece made again takes its id from the account.
    auto staged = std::make_unique<AccountState>(draft.after);
    CharacterState& character = staged->characters[characterIndex];
    std::vector<std::uint64_t> plain, exotic;
    bool changed = false;
    for (std::size_t slot = 0; slot < loadout.pieces.size(); ++slot) {
        const SavedPiece& piece = loadout.pieces[slot];
        const PieceState standing = piece_state(character, catalog, piece);
        if (standing == PieceState::empty) continue;
        ++result.saved;
        if (standing == PieceState::unavailable) { ++result.unavailable; continue; }
        if (standing == PieceState::equipped) { ++result.equipped; continue; }
        std::uint64_t id = piece.instance;
        if (standing == PieceState::missing) {
            // The level it was saved at; for a loadout saved before levels were kept, the level of
            // what it replaces.
            const auto& current = character.equipment.slots[slot];
            const int level = piece.level > 0 ? piece.level : current ? current->level : fallbackLevel;
            id = recreate(*staged, character, catalog, piece, level);
            if (id == 0) { ++result.unavailable; continue; }
            result.replaced[slot] = id;
            ++result.recreated;
            changed = true;
        }
        const CatalogItem* definition = catalog.find(piece.definition);
        (definition->definition.tier == static_cast<std::uint8_t>(build_data::items::Tier::exotic) ? exotic : plain).push_back(id);
    }
    // Plain pieces go on first. One can only lower the exotic count, so by the time an exotic goes
    // on, the exotic it would have clashed with has already been replaced.
    for (const auto* group : {&plain, &exotic}) for (const std::uint64_t id : *group) {
        std::string refused;
        if (equip_character(character, catalog, id, refused)) { ++result.equipped; changed = true; } else ++result.unavailable;
    }
    // The abilities belong to the subclass: they are put back only when the saved subclass is the
    // one on now, and only an entry that subclass offers for its lane, or the apply would refuse.
    const std::uint64_t savedSubclass = result.replaced[kSubclassSlot] != 0 ? result.replaced[kSubclassSlot] : loadout.pieces[kSubclassSlot].instance;
    const auto& subclassSlot = character.equipment.slots[kSubclassSlot];
    const CatalogItem* subclass = savedSubclass != 0 && subclassSlot && subclassSlot->instanceSoid == savedSubclass
        ? catalog.find(subclassSlot->definitionHash) : nullptr;
    for (std::size_t lane = 0; subclass && lane < kAbilityFields.size(); ++lane) {
        for (const auto& choice : subclass->abilities[lane]) {
            if (choice.entry != loadout.abilities[lane]) continue;
            if (character.*kAbilityFields[lane] != choice.entry) { character.*kAbilityFields[lane] = choice.entry; changed = true; }
            break;
        }
    }
    if (!changed) { error = result.equipped != 0 ? "This loadout is already equipped." : "None of this loadout's pieces are available."; return false; }
    draft.after = *staged; draft.dirty = true; error = "Loadout equipped in draft."; return true;
}
bool randomize(Draft& draft, const Catalog& catalog, std::size_t characterIndex, const std::array<bool, inv::kEquipmentSlotCount>& slots, int power, std::mt19937& random, std::string& error) {
    if (power < 0 || power > kMaximumItemLevel) { error = "That item level is out of range."; return false; }
    if (characterIndex >= draft.after.characterCount) return false;
    auto staged = std::make_unique<Draft>(draft);
    auto& character = staged->after.characters[characterIndex];
    bool any = false;
    // Clearing chosen slots first allows exactly one exotic per category across the complete result.
    for (std::size_t slot = 0; slot < slots.size(); ++slot) if (slots[slot] && character.equipment.slots[slot]) {
        if (character.inventory.count >= character.inventory.values.size()) { error = "Free inventory space before randomizing."; return false; }
        auto item = *character.equipment.slots[slot];
        if (!bump(character, item)) return false;
        character.inventory.values[character.inventory.count++] = item;
        character.equipment.slots[slot].reset();
    }
    for (std::size_t slot = 0; slot < slots.size(); ++slot) if (slots[slot]) {
        std::vector<const CatalogItem*> options;
        // Reuse owned gear when the bucket is full. Otherwise draw from the complete catalog.
        for (const auto& definition : catalog.items) if (definition.slot == slot && !definition.plug && !definition.internal
            && fits_class(definition, character.characterClass) && !exotic_conflict(character, definition, catalog)
            && inv::has_room(character, definition.definition.bucketId)) options.push_back(&definition);
        if (options.empty()) {
            std::vector<std::uint64_t> owned;
            for (std::size_t i = 0; i < character.inventory.count; ++i) {
                const auto* definition = catalog.find(character.inventory.values[i].definitionHash);
                if (definition && definition->slot == slot && fits_class(*definition, character.characterClass)
                    && !exotic_conflict(character, *definition, catalog)) owned.push_back(character.inventory.values[i].instanceSoid);
            }
            if (owned.empty() || !equip(*staged, catalog, characterIndex, owned[random() % owned.size()], error)) { error = "No valid random choice for one of the selected slots."; return false; }
        } else if (!give(*staged, catalog, characterIndex, options[random() % options.size()]->definition.definitionHash, 1, power, true, error)) return false;
        // Both branches above place the item in the slot the options were filtered by, but a null
        // here would crash the randomizer rather than refuse, so it is checked as a refusal.
        auto& target = staged->after.characters[characterIndex].equipment.slots[slot];
        if (!target) { error = "No valid random choice for one of the selected slots."; return false; }
        auto& equipped = *target;
        equipped.level = power;
        const auto* definition = catalog.find(equipped.definitionHash);
        for (std::size_t lane = 0; definition && lane < definition->compatible.size(); ++lane) {
            const auto& optionsForLane = definition->compatible[lane];
            if (!optionsForLane.empty()) (void)set_plug(equipped, catalog, lane, optionsForLane[random() % optionsForLane.size()], PlugScope::compatible);
        }
        any = true;
    }
    if (!any) { error = "Choose at least one slot to randomize."; return false; }
    draft.after = staged->after; draft.dirty = true; error = "Random loadout staged. Your previous equipment is in inventory."; return true;
}
bool normalize(Draft& draft, std::string& message) {
    bool serials = false;
    for (std::size_t c = 0; c < draft.after.characterCount; ++c) {
        auto& character = draft.after.characters[c];
        const auto before = character.nextInventorySerial;
        if (!prepare_serial_counter(character)) continue;
        serials |= character.nextInventorySerial != before;
    }
    if (!serials) return false;
    draft.dirty = true;
    message = "Repaired an item revision this account could not publish. Apply to finish the fix.";
    return true;
}
bool prepare_commit(const Draft& draft, const Catalog& catalog, AccountState& output, std::string& error) {
    output = draft.after;
    if (!account::valid(output)) { error = "The draft contains an invalid character or inventory value."; return false; }
    const auto prior = [&](std::uint64_t id) -> const Item* {
        for (std::size_t c = 0; c < draft.before.characterCount; ++c) {
            const auto& character = draft.before.characters[c];
            for (const auto& item : character.equipment.slots) if (item && item->instanceSoid == id) return &*item;
            for (std::size_t i = 0; i < character.inventory.count; ++i) if (character.inventory.values[i].instanceSoid == id) return &character.inventory.values[i];
        }
        return nullptr;
    };
    for (std::size_t c = 0; c < output.characterCount; ++c) {
        auto& character = output.characters[c];
        if (!prepare_serial_counter(character)) {
            error = "Item revision limit reached."; return false;
        }
        const auto check = [&](Item& item) {
            const auto* definition = catalog.find(item.definitionHash);
            if (!definition || item.quantity > (definition->detail.instancedDefinitionState == build_data::items::details::InstancedDefinitionState::instanced ? 1 : definition->detail.maxStackSize)) {
                error = "An item exceeds its installed stack limit or is missing from the catalog."; return false;
            }
            const auto* old = prior(item.instanceSoid);
            // A level whose Power the game cannot hold is refused, unless the save already had it.
            if (item.level > kMaximumItemLevel && (!old || item.level != old->level)) {
                error = "That item level is out of range."; return false;
            }
            if (old && item != *old && item.mutationSerial <= old->mutationSerial && !bump(character, item)) {
                error = "Item revision limit reached."; return false;
            }
            return true;
        };
        for (auto& item : character.equipment.slots) if (item && !check(*item)) return false;
        for (std::size_t i = 0; i < character.inventory.count; ++i) if (!check(character.inventory.values[i])) return false;
        if (!prepare_serial_counter(character)) { error = "Item revision limit reached."; return false; }
    }
    std::array<std::size_t, 256> occupied{};
    for (std::size_t i = 0; i < output.profileItemCount; ++i) {
        auto& item = output.profileItems[i];
        const auto* definition = catalog.find(item.definitionHash);
        build_data::inventory::buckets::Descriptor bucket{};
        if (!definition || !build_data::find_inventory_bucket_descriptor(definition->definition.bucketId, bucket)
            || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile
            || ++occupied[bucket.bucketId] > bucket.slotCount || item.quantity > definition->detail.maxStackSize) {
            error = "An account inventory bucket or item stack exceeds its installed limit."; return false;
        }
        for (std::size_t j = 0; j < draft.before.profileItemCount; ++j) {
            const auto& old = draft.before.profileItems[j];
            if (old.instanceSoid != item.instanceSoid || old.definitionHash != item.definitionHash) continue;
            if (item != old && item.mutationSerial <= old.mutationSerial) {
                if (old.mutationSerial == (std::numeric_limits<std::int32_t>::max)()) { error = "Item revision limit reached."; return false; }
                item.mutationSerial = old.mutationSerial + 1;
            }
            break;
        }
    }
    // The serial repairs above can only have made the image less valid, and a refusal that says
    // nothing leaves the action bar showing the outcome of the apply before this one.
    if (!account::valid(output)) { error = "The draft contains an invalid character or inventory value."; return false; }
    return true;
}
}
