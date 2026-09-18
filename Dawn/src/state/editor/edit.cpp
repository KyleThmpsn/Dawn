// SPDX-License-Identifier: GPL-3.0-only
#include "edit.h"
#include "../persistence/persistence.h"
#include "../account/inventory/placement.h"
#include <algorithm>
#include <limits>
#include <memory>

namespace dawn::state::editor {
namespace inv = account::inventory;
namespace {
Stats contribution(const CatalogItem& item, const Catalog& catalog) {
    Stats result{};
    for (std::size_t i = 0; i < item.detail.statCount; ++i)
        for (std::size_t j = 0; j < result.size(); ++j)
            if (item.detail.stats[i].row == catalog.statRows[j]) result[j] += item.detail.stats[i].value;
    return result;
}
void sum(Stats& into, const Stats& values, int sign = 1) {
    for (std::size_t i = 0; i < into.size(); ++i) into[i] += sign * values[i];
}
bool nonzero(const Stats& values) { return std::any_of(values.begin(), values.end(), [](int v) { return v != 0; }); }
bool bump(CharacterState& character, Item& item) {
    if (character.nextInventorySerial >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) return false;
    // The character wire record requires every item revision to be strictly below next.
    item.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    return true;
}
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
    // Repair drafts loaded from the old editor without rewriting any item or revision.
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
Stats item_stats(const Item& item, const Catalog& catalog) {
    const auto* definition = catalog.find(item.definitionHash);
    if (!definition) return {};
    auto result = contribution(*definition, catalog);
    Item resolved = item;
    if (materialize(resolved, catalog)) for (std::size_t i = 0; i < resolved.sockets.plugCount; ++i)
        if (resolved.sockets.plugs[i]) if (const auto* plug = catalog.find(*resolved.sockets.plugs[i])) sum(result, contribution(*plug, catalog));
    return result;
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
        // Stay in the native socket pool; stat editing never replaces a gameplay perk with an arbitrary stat plug.
        std::vector<std::uint16_t> choices;
        for (auto id : definition->compatible[lane]) {
            const auto* choice = catalog.index(id);
            if (choice && choice->definition.plugCategoryHash == current->definition.plugCategoryHash
                && nonzero(contribution(*choice, catalog))) choices.push_back(id);
        }
        if (choices.empty()) continue;
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
    if (characterIndex >= draft.after.characterCount || !definition || quantity <= 0 || power < 0 || power > kMaximumItemLevel) { error = "Choose a valid item, quantity and item level (0-106)."; return false; }
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
bool randomize(Draft& draft, const Catalog& catalog, std::size_t characterIndex, const std::array<bool, 16>& slots, int power, std::mt19937& random, std::string& error) {
    if (power < 0 || power > kMaximumItemLevel) { error = "Item level must be between 0 and 106."; return false; }
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
        auto& equipped = *staged->after.characters[characterIndex].equipment.slots[slot];
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
            // Preserve existing saves; enforce the cap when authoring a new level.
            if (item.level > kMaximumItemLevel && (!old || item.level != old->level)) {
                error = "Item level must be between 0 and 106."; return false;
            }
            if (old && item != *old && item.mutationSerial <= old->mutationSerial && !bump(character, item)) {
                error = "Item revision limit reached."; return false;
            }
            return true;
        };
        for (auto& item : character.equipment.slots) if (item && !check(*item)) return false;
        for (std::size_t i = 0; i < character.inventory.count; ++i) if (!check(character.inventory.values[i])) return false;
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
    return account::valid(output);
}
}
