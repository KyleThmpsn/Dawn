#pragma once
#include "catalog.h"
#include <iterator>
#include <random>
#include <tuple>

namespace dawn::state::editor {
using Item = account::inventory::Item;
using Stats = std::array<int, 6>;
inline constexpr int kMaximumItemLevel = 106;
struct Draft {
    AccountState before, after;
    bool dirty{};
};
inline constexpr const char* kSlots[]{"Kinetic", "Energy", "Power", "Helmet", "Gauntlets", "Chest", "Legs", "Class Item", "Ghost", "Sparrow", "Ship", "Subclass", "Clan Banner", "Emblem", "Emote", "Finisher"};
// The enum lives in another header, so a slot added there would otherwise read past this table
// on every page that names a slot.
static_assert(std::size(kSlots) == account::inventory::kEquipmentSlotCount, "Every equipment slot needs a name.");
/**
 * @return The name the installed build gives the character stat at one index.
 * The icon and the value for a stat both come from `Catalog::statRows[index]`, so its label is
 * taken from the same row rather than from the list above, which only agrees with them while the
 * build's own row order happens to match it. The list stands in when the bank names nothing.
 * @param catalog Loaded catalog holding the stat rows and their names.
 * @param index Character stat index, matching `Stats` and `kStats`.
 */
[[nodiscard]] std::string_view stat_label(const Catalog& catalog, std::size_t index) noexcept;
static_assert(std::size(kStats) == std::tuple_size_v<Stats>, "Every character stat needs a name.");
bool materialize(Item& item, const Catalog& catalog);
bool set_plug(Item& item, const Catalog& catalog, std::size_t lane, std::uint16_t plug, PlugScope scope);
Stats item_stats(const Item& item, const Catalog& catalog);
// Finds the closest supported stat-plug allocation and reports the values actually reached.
bool adjust_stats(Item& item, const Catalog& catalog, const Stats& targets, Stats& achieved);
// True when at least one of the item's stat-bearing sockets offers a different stat plug, which is
// the only case in which `adjust_stats` can change anything.
bool adjustable_stats(const Item& item, const Catalog& catalog);
bool give(Draft& draft, const Catalog& catalog, std::size_t character, std::uint32_t hash, int quantity, int power, bool equip, std::string& error);
bool equip(Draft& draft, const Catalog& catalog, std::size_t character, std::uint64_t id, std::string& error);
bool unequip(Draft& draft, const Catalog& catalog, std::size_t character, std::size_t slot, std::string& error);
bool randomize(Draft& draft, const Catalog& catalog, std::size_t character, const std::array<bool, account::inventory::kEquipmentSlotCount>& slots, int power, std::mt19937& random, std::string& error);
bool prepare_commit(const Draft& draft, const Catalog& catalog, AccountState& output, std::string& error);

/**
 * Repairs a freshly loaded draft the installed build would refuse to publish.
 * An account saved by an earlier editor carries one row whose generation the character counter
 * still points at, and the family-four character object will not encode until the counter leads
 * it. That leaves the game retrying the account refresh and never publishing that character.
 * @param draft Draft to repair in place. Only the working image is touched.
 * @param message Receives an explanation when a repair was staged.
 * @return True when the draft was changed and now needs applying.
 */
bool normalize(Draft& draft, std::string& message);
/**
 * Commits the draft and publishes it to the running game, with no restart.
 * The first apply of the process copies the player database aside, so one restore point covers
 * the whole editing session however many applies follow it.
 * @param draft Edits to commit. A committed draft is rebased onto the new account and made clean.
 * @param message Receives the user-facing outcome, whether or not the apply succeeds.
 * @param live Receives true when a signed-in peer took the change, false when it only saved.
 * @return True when the account now carries the draft. Nothing is changed otherwise.
 */
bool apply(Draft& draft, const Catalog& catalog, std::string& message, bool& live);
/**
 * Re-arms the running game with the account already committed.
 * An apply made before the player is signed in reaches nobody, so the editor calls this until a
 * peer takes it and the change shows in game.
 * @return True when at least one connected peer will reload the account.
 */
bool republish() noexcept;
}
