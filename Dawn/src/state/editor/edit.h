#pragma once
#include "catalog.h"
#include <random>

namespace dawn::state::editor {
using Item = account::inventory::Item;
using Stats = std::array<int, 6>;
inline constexpr int kMaximumItemLevel = 106;
struct Draft {
    AccountState before, after;
    bool dirty{};
};
inline constexpr const char* kSlots[]{"Kinetic", "Energy", "Power", "Helmet", "Gauntlets", "Chest", "Legs", "Class item", "Ghost", "Sparrow", "Ship", "Subclass", "Clan banner", "Emblem", "Emote", "Finisher"};
inline constexpr const char* kStats[]{"Mobility", "Resilience", "Recovery", "Discipline", "Intellect", "Strength"};
bool materialize(Item& item, const Catalog& catalog);
bool set_plug(Item& item, const Catalog& catalog, std::size_t lane, std::uint16_t plug, PlugScope scope);
Stats item_stats(const Item& item, const Catalog& catalog);
// Finds the closest supported stat-plug allocation and reports the values actually reached.
bool adjust_stats(Item& item, const Catalog& catalog, const Stats& targets, Stats& achieved);
bool give(Draft& draft, const Catalog& catalog, std::size_t character, std::uint32_t hash, int quantity, int power, bool equip, std::string& error);
bool equip(Draft& draft, const Catalog& catalog, std::size_t character, std::uint64_t id, std::string& error);
bool unequip(Draft& draft, const Catalog& catalog, std::size_t character, std::size_t slot, std::string& error);
bool randomize(Draft& draft, const Catalog& catalog, std::size_t character, const std::array<bool, 16>& slots, int power, std::mt19937& random, std::string& error);
bool prepare_commit(const Draft& draft, const Catalog& catalog, AccountState& output, std::string& error);
bool save(Draft& draft, const Catalog& catalog, std::string& error);
}
