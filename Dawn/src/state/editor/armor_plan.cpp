// SPDX-License-Identifier: GPL-3.0-only
// The armor planner follows Sundial's armor stat adjuster by KyleThmpsn. See vendor/sundial/NOTICE.md.
#include "armor_plan.h"
#include <algorithm>
#include <memory>
#include <numeric>
#include <string_view>
#include <unordered_map>

namespace dawn::state::editor {
namespace {
namespace inv = account::inventory;

/** Whole-set states kept between two slots, refit states kept per socket, and plans kept per slot: Sundial's bounds. */
constexpr std::size_t kSearchStates = 1000;
constexpr std::size_t kPieceStates = 128;
constexpr std::size_t kSlotPlans = 512;
/** The planner looks up from its work this often to see whether anyone still wants the plan. */
constexpr std::size_t kCancelStride = 256;

/** Six capped totals, with zero for every stat that has no minimum, which is how a state is told apart. */
using Key = std::array<std::uint16_t, 6>;
struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint16_t value : key) {
            hash = (hash ^ value) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(hash);
    }
};

[[nodiscard]] int capped(int value) noexcept {
    return std::clamp(value, 0, kStatCap);
}

[[nodiscard]] Key key_of(const Stats& totals, const Stats& minimums) noexcept {
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = minimums[i] > 0 ? static_cast<std::uint16_t>(capped(totals[i])) : std::uint16_t{0};
    }
    return key;
}

/** How far a set falls short of its minimums: in all, then at the worst stat. Less is better. */
struct Shortfall {
    int total{};
    int largest{};
    friend auto operator<=>(const Shortfall&, const Shortfall&) = default;
};

[[nodiscard]] Shortfall shortfall(const Stats& totals, const Stats& minimums) noexcept {
    Shortfall result;
    for (std::size_t i = 0; i < totals.size(); ++i) {
        if (minimums[i] <= 0) {
            continue;
        }
        const int missing = (std::max)(0, minimums[i] - capped(totals[i]));
        result.total += missing;
        result.largest = (std::max)(result.largest, missing);
    }
    return result;
}

/** @return Points past the cap, which the game counts for nothing. */
[[nodiscard]] int waste(const Stats& totals) noexcept {
    int result = 0;
    for (const int value : totals) {
        result += (std::max)(0, value - kStatCap);
    }
    return result;
}

/** @return Points past each minimum, up to the cap. */
[[nodiscard]] int excess(const Stats& totals, const Stats& minimums) noexcept {
    int result = 0;
    for (std::size_t i = 0; i < totals.size(); ++i) {
        result += minimums[i] > 0 ? (std::max)(0, capped(totals[i]) - minimums[i]) : 0;
    }
    return result;
}

/** @return What the stats with no minimum add up to, up to the cap: more of them is better. */
[[nodiscard]] int spare(const Stats& totals, const Stats& minimums) noexcept {
    int result = 0;
    for (std::size_t i = 0; i < totals.size(); ++i) {
        result += minimums[i] > 0 ? 0 : capped(totals[i]);
    }
    return result;
}

[[nodiscard]] bool nonzero(const Stats& values) noexcept {
    return std::any_of(values.begin(), values.end(), [](int value) { return value != 0; });
}

[[nodiscard]] bool positive(const Stats& values) noexcept {
    return std::any_of(values.begin(), values.end(), [](int value) { return value > 0; });
}

void add(Stats& into, const Stats& values, int sign = 1) noexcept {
    for (std::size_t i = 0; i < into.size(); ++i) {
        into[i] += sign * values[i];
    }
}

/** @return What one piece shows for stored totals, each stat through the piece's own curve. */
[[nodiscard]] Stats shown_of(const ArmorCandidate& candidate, const Stats& stored) noexcept {
    Stats shown{};
    for (std::size_t i = 0; i < shown.size(); ++i) {
        shown[i] = display_stat(candidate.curves[i] ? &*candidate.curves[i] : nullptr, stored[i]);
    }
    return shown;
}

/** Energy socket types, which a masterwork fills. Sundial names the same. */
[[nodiscard]] bool masterwork_socket(std::uint16_t type) noexcept {
    return (type >= 29 && type <= 43) || type == 520 || type == 678 || type == 679;
}

/** @return True when a plug's name marks it a masterwork, as Sundial reads the installed names. */
[[nodiscard]] bool masterwork_name(std::string_view name) noexcept {
    return name.find("Masterwork") != std::string_view::npos || name.ends_with(" Energy 10") || name.starts_with("Tier 10 Armor");
}

/** @return True when a plug is a stat mod: named a mod, and adding to exactly one stat. */
[[nodiscard]] bool stat_mod(const CatalogItem& plug, const Stats& values) noexcept {
    return plug.name.ends_with(" Mod") && std::count_if(values.begin(), values.end(), [](int value) { return value != 0; }) == 1;
}

/** Adds a choice, unless one adding the same stats is offered already; the plug on now wins a tie, then the lower hash. */
void offer(std::vector<StatChoice>& choices, std::uint32_t plug, const Stats& values, std::uint32_t current) {
    for (StatChoice& choice : choices) {
        if (choice.values != values) {
            continue;
        }
        if (plug == current || (choice.plug != current && plug < choice.plug)) {
            choice.plug = plug;
        }
        return;
    }
    choices.push_back({plug, values});
}

/** @return The plugs a socket type takes, which is every pool the installed armor gives it. */
[[nodiscard]] std::vector<std::uint16_t> socket_pool(const CatalogItem& definition, std::size_t lane, const Catalog& catalog) {
    return catalog.candidates(definition, lane, PlugScope::socket);
}

/**
 * @return The socket a plan may refit in one lane, or none. An allocation lane takes any allocation
 * plug of its group, empty or not; a mod socket takes any stat mod, or its empty plug; an energy
 * socket short of a masterwork takes the best masterwork of its own energy, and one already
 * masterworked is kept.
 */
[[nodiscard]] std::optional<StatSocket> stat_socket(const Item& item, const CatalogItem& definition, std::size_t lane, const Catalog& catalog) {
    StatSocket socket;
    socket.lane = static_cast<std::uint8_t>(lane);
    socket.current = item.sockets.plugs[lane].value_or(0U);
    const CatalogItem* current = socket.current != 0 ? catalog.find(socket.current) : nullptr;
    const Stats now = current != nullptr ? plug_stats(*current, catalog) : Stats{};
    offer(socket.choices, socket.current, now, socket.current);
    if (allocation_lane(definition, lane)) {
        // An empty allocation lane, as a piece given from the build has, takes any plug of its group.
        socket.kind = StatSocketKind::allocation;
        for (const std::uint16_t id : stat_plug_choices(definition, lane, current, catalog)) {
            const CatalogItem* choice = catalog.index(id);
            if (choice == nullptr) {
                continue;
            }
            const Stats values = plug_stats(*choice, catalog);
            if (nonzero(values)) {
                offer(socket.choices, choice->definition.definitionHash, values, socket.current);
            }
        }
    } else if (masterwork_socket(definition.detail.socketTypes[lane])) {
        if (current == nullptr || (masterwork_name(current->name) && positive(now))) {
            return std::nullopt;
        }
        socket.kind = StatSocketKind::masterwork;
        // A masterwork of the energy the piece has now keeps the mods it takes; another energy's is
        // only used when the build gives this one none.
        const CatalogItem* best = nullptr;
        int bestSum = 0;
        bool bestSameEnergy = false;
        for (const std::uint16_t id : socket_pool(definition, lane, catalog)) {
            const CatalogItem* choice = catalog.index(id);
            if (choice == nullptr || !masterwork_name(choice->name)) {
                continue;
            }
            const Stats values = plug_stats(*choice, catalog);
            const int sum = std::accumulate(values.begin(), values.end(), 0);
            const bool sameEnergy = choice->definition.plugCategoryHash == current->definition.plugCategoryHash;
            if (sum <= 0) {
                continue;
            }
            if (best == nullptr || (sameEnergy && !bestSameEnergy) || (sameEnergy == bestSameEnergy && (sum > bestSum
                || (sum == bestSum && choice->definition.definitionHash < best->definition.definitionHash)))) {
                best = choice;
                bestSum = sum;
                bestSameEnergy = sameEnergy;
            }
        }
        if (best == nullptr) {
            return std::nullopt;
        }
        offer(socket.choices, best->definition.definitionHash, plug_stats(*best, catalog), socket.current);
    } else {
        std::vector<const CatalogItem*> mods;
        for (const std::uint16_t id : socket_pool(definition, lane, catalog)) {
            const CatalogItem* choice = catalog.index(id);
            if (choice != nullptr && stat_mod(*choice, plug_stats(*choice, catalog))) {
                mods.push_back(choice);
            }
        }
        if (mods.empty()) {
            return std::nullopt;
        }
        socket.kind = StatSocketKind::mod;
        // A stat mod can also come out, leaving the socket as the build leaves it.
        if (nonzero(now)) {
            const auto initial = definition.detail.initialPlugIndices[lane];
            const CatalogItem* empty = initial != build_data::items::details::kUnavailableItemIndex ? catalog.index(initial) : nullptr;
            if (empty == nullptr || !nonzero(plug_stats(*empty, catalog))) {
                offer(socket.choices, empty != nullptr ? empty->definition.definitionHash : 0U, Stats{}, socket.current);
            }
        }
        for (const CatalogItem* mod : mods) {
            offer(socket.choices, mod->definition.definitionHash, plug_stats(*mod, catalog), socket.current);
        }
    }
    const bool refittable = std::any_of(socket.choices.begin(), socket.choices.end(), [&](const StatChoice& choice) {
        return choice.plug != socket.current && positive(choice.values);
    });
    return refittable ? std::optional<StatSocket>(std::move(socket)) : std::nullopt;
}

/** @return One piece as a plan may use it, with its fixed stats and every socket it may refit. */
[[nodiscard]] ArmorCandidate candidate_of(const Item& item, const CatalogItem& definition, const Catalog& catalog, std::size_t holder, bool equipped) {
    ArmorCandidate candidate;
    candidate.instance = item.instanceSoid;
    candidate.definition = item.definitionHash;
    candidate.holder = holder;
    candidate.equipped = equipped;
    candidate.exotic = definition.definition.tier == 5;
    candidate.locked = (item.flags & inv::kLockedItemFlag) != 0;
    candidate.shown = shown_stats(item, catalog);
    for (std::size_t i = 0; i < candidate.curves.size(); ++i) {
        const ScaledStat* scaled = scaled_stat(catalog, definition.statGroupIndex, catalog.statRows[i]);
        if (scaled != nullptr && !scaled->curve.empty()) {
            candidate.curves[i] = *scaled;
        }
    }
    candidate.fixed = item_stats(item, catalog);
    Item resolved = item;
    if (candidate.locked || !materialize(resolved, catalog)) {
        return candidate;
    }
    const std::size_t lanes = (std::min)(resolved.sockets.plugCount, static_cast<std::size_t>(definition.detail.ordinarySocketCount));
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        if (auto socket = stat_socket(resolved, definition, lane, catalog)) {
            // The plug in it now is part of what the piece gives; the plan chooses it again, or another.
            add(candidate.fixed, socket->choices.front().values, -1);
            candidate.sockets.push_back(std::move(*socket));
        }
    }
    return candidate;
}

/** The plug picked for each socket of one piece, in socket order. A piece has no more sockets than lanes. */
using Picks = std::array<std::uint32_t, inv::kPlugCapacity>;

/** A refit of one piece under way: its stored totals, what it shows, the plug picked per socket so far, and its changes. */
struct PieceState {
    Stats stored{};
    Stats shown{};
    Picks picks{};
    unsigned changes{};
    unsigned masterworks{};
};

/** One way to fill a slot: a piece and one refit of it. */
struct PiecePlan {
    std::size_t candidate{};
    Stats shown{};
    Picks picks{};
    unsigned changes{};
    unsigned masterworks{};
    bool exotic{};
};

[[nodiscard]] bool piece_less(const PieceState& a, const PieceState& b, const Stats& minimums) noexcept {
    if (const auto order = shortfall(a.shown, minimums) <=> shortfall(b.shown, minimums); order != 0) return order < 0;
    if (const int wa = waste(a.shown), wb = waste(b.shown); wa != wb) return wa < wb;
    if (a.changes + a.masterworks != b.changes + b.masterworks) return a.changes + a.masterworks < b.changes + b.masterworks;
    if (a.changes != b.changes) return a.changes < b.changes;
    return a.picks < b.picks;
}

[[nodiscard]] bool plan_less(const PiecePlan& a, const PiecePlan& b, const Stats& minimums) noexcept {
    if (const auto order = shortfall(a.shown, minimums) <=> shortfall(b.shown, minimums); order != 0) return order < 0;
    if (const int wa = waste(a.shown), wb = waste(b.shown); wa != wb) return wa < wb;
    if ((a.candidate != 0) != (b.candidate != 0)) return a.candidate == 0;
    if (a.changes + a.masterworks != b.changes + b.masterworks) return a.changes + a.masterworks < b.changes + b.masterworks;
    if (a.changes != b.changes) return a.changes < b.changes;
    if (a.candidate != b.candidate) return a.candidate < b.candidate;
    return a.picks < b.picks;
}

/**
 * @return The choices a socket is weighed with for these minimums. Choices that add the same to every
 * stat with a minimum only differ in the stats left free, so of each such group only the one on now,
 * the one adding least elsewhere and the one adding most elsewhere are kept.
 */
[[nodiscard]] std::vector<StatChoice> choices_for(const StatSocket& socket, const Stats& minimums) {
    const auto free_total = [&](const StatChoice& choice) {
        int sum = 0;
        for (std::size_t i = 0; i < minimums.size(); ++i) {
            sum += minimums[i] > 0 ? 0 : choice.values[i];
        }
        return sum;
    };
    std::unordered_map<Key, std::vector<const StatChoice*>, KeyHash> groups;
    for (const StatChoice& choice : socket.choices) {
        Key key{};
        for (std::size_t i = 0; i < key.size(); ++i) {
            key[i] = minimums[i] > 0 ? static_cast<std::uint16_t>(std::clamp(choice.values[i] + 0x8000, 0, 0xFFFF)) : std::uint16_t{0};
        }
        groups[key].push_back(&choice);
    }
    std::vector<StatChoice> kept;
    for (const auto& [key, group] : groups) {
        (void)key;
        std::vector<const StatChoice*> keep;
        const auto retain = [&](const StatChoice* choice) {
            if (choice != nullptr && std::find(keep.begin(), keep.end(), choice) == keep.end()) {
                keep.push_back(choice);
            }
        };
        const auto on = std::find_if(group.begin(), group.end(), [&](const StatChoice* choice) { return choice->plug == socket.current; });
        retain(on != group.end() ? *on : nullptr);
        retain(*std::min_element(group.begin(), group.end(), [&](const StatChoice* a, const StatChoice* b) {
            const int fa = free_total(*a), fb = free_total(*b);
            return fa != fb ? fa < fb : a->plug < b->plug;
        }));
        retain(*std::max_element(group.begin(), group.end(), [&](const StatChoice* a, const StatChoice* b) {
            const int fa = free_total(*a), fb = free_total(*b);
            return fa != fb ? fa < fb : a->plug > b->plug;
        }));
        for (const StatChoice* choice : keep) {
            kept.push_back(*choice);
        }
    }
    std::sort(kept.begin(), kept.end(), [&](const StatChoice& a, const StatChoice& b) {
        if ((a.plug != socket.current) != (b.plug != socket.current)) return a.plug == socket.current;
        if (a.values != b.values) return a.values < b.values;
        return a.plug < b.plug;
    });
    return kept;
}

/** @return Every refit of one piece worth keeping for these minimums, best first. */
[[nodiscard]] std::vector<PiecePlan> piece_plans(const ArmorCandidate& candidate, std::size_t index, const Stats& minimums,
                                                 const std::atomic_bool& cancel) {
    std::vector<PieceState> states{PieceState{candidate.fixed, shown_of(candidate, candidate.fixed), {}, 0, 0}};
    for (std::size_t depth = 0; depth < candidate.sockets.size() && depth < Picks{}.size(); ++depth) {
        const StatSocket& socket = candidate.sockets[depth];
        if (cancel.load(std::memory_order_relaxed)) {
            return {};
        }
        const std::vector<StatChoice> choices = choices_for(socket, minimums);
        std::unordered_map<Key, PieceState, KeyHash> next;
        for (const PieceState& state : states) {
            for (const StatChoice& choice : choices) {
                PieceState grown = state;
                add(grown.stored, choice.values);
                grown.shown = shown_of(candidate, grown.stored);
                grown.picks[depth] = choice.plug;
                if (choice.plug != socket.current) {
                    (socket.kind == StatSocketKind::masterwork ? grown.masterworks : grown.changes) += 1;
                }
                const auto [at, added] = next.try_emplace(key_of(grown.shown, minimums), grown);
                if (!added && piece_less(grown, at->second, minimums)) {
                    at->second = std::move(grown);
                }
            }
        }
        states.clear();
        states.reserve(next.size());
        for (auto& [key, state] : next) {
            (void)key;
            states.push_back(std::move(state));
        }
        std::sort(states.begin(), states.end(), [&](const PieceState& a, const PieceState& b) { return piece_less(a, b, minimums); });
        if (states.size() > kPieceStates) {
            states.resize(kPieceStates);
        }
    }
    std::vector<PiecePlan> plans;
    plans.reserve(states.size());
    for (PieceState& state : states) {
        plans.push_back({index, state.shown, state.picks, state.changes, state.masterworks, candidate.exotic});
    }
    return plans;
}

/** @return Every way to fill one slot worth keeping, across its pieces, best first. */
[[nodiscard]] std::vector<PiecePlan> slot_plans(const std::vector<ArmorCandidate>& candidates, const Stats& minimums,
                                                const std::atomic_bool& cancel) {
    // The same totals from an exotic and from another piece are both kept, since the exotic rule may
    // turn one of them away.
    std::array<std::unordered_map<Key, PiecePlan, KeyHash>, 2> byResult;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        // An empty slot stays empty only when nothing can fill it: a piece that adds nothing is still
        // better than a slot the game needs filled going without.
        if (candidates[index].instance == 0 && candidates.size() > 1) {
            continue;
        }
        for (PiecePlan& plan : piece_plans(candidates[index], index, minimums, cancel)) {
            auto& found = byResult[plan.exotic ? 1 : 0];
            const Key key = key_of(plan.shown, minimums);
            const auto at = found.find(key);
            if (at == found.end()) {
                found.emplace(key, std::move(plan));
            } else if (plan_less(plan, at->second, minimums)) {
                at->second = std::move(plan);
            }
        }
        if (cancel.load(std::memory_order_relaxed)) {
            return {};
        }
    }
    std::vector<PiecePlan> plans;
    for (auto& found : byResult) {
        for (auto& [key, plan] : found) {
            (void)key;
            plans.push_back(std::move(plan));
        }
    }
    std::sort(plans.begin(), plans.end(), [&](const PiecePlan& a, const PiecePlan& b) { return plan_less(a, b, minimums); });
    if (plans.size() > kSlotPlans) {
        plans.resize(kSlotPlans);
    }
    return plans;
}

/** A whole set under way: its totals, the plan picked for each slot so far, and what it would change. */
struct SearchState {
    Stats totals{};
    std::array<std::uint16_t, kPlanSlots> plans{};
    unsigned swaps{};
    unsigned changes{};
    unsigned masterworks{};
    unsigned exotics{};
};

[[nodiscard]] bool search_less(const SearchState& a, const SearchState& b, const Stats& minimums) noexcept {
    if (const auto order = shortfall(a.totals, minimums) <=> shortfall(b.totals, minimums); order != 0) return order < 0;
    if (const int wa = waste(a.totals), wb = waste(b.totals); wa != wb) return wa < wb;
    if (a.swaps != b.swaps) return a.swaps < b.swaps;
    if (a.changes + a.masterworks != b.changes + b.masterworks) return a.changes + a.masterworks < b.changes + b.masterworks;
    if (a.changes != b.changes) return a.changes < b.changes;
    if (const int ea = excess(a.totals, minimums), eb = excess(b.totals, minimums); ea != eb) return ea < eb;
    if (const int sa = spare(a.totals, minimums), sb = spare(b.totals, minimums); sa != sb) return sa > sb;
    return a.plans < b.plans;
}
} // namespace

bool ArmorPlan::exact() const noexcept {
    return finished && !nonzero(shortfalls);
}

bool ArmorPlan::changes() const noexcept {
    return finished && (swaps != 0 || std::any_of(pieces.begin(), pieces.end(), [](const Piece& piece) { return !piece.plugs.empty(); }));
}

ArmorInput armor_input(const AccountState& account, const Catalog& catalog, std::size_t character, const ArmorOptions& options) {
    ArmorInput input;
    input.character = character;
    if (character >= account.characterCount) {
        return input;
    }
    const CharacterState& owner = account.characters[character];
    const auto armor_in = [&](const Item& item, std::size_t slot) -> const CatalogItem* {
        const CatalogItem* definition = catalog.find(item.definitionHash);
        return definition != nullptr && definition->kind == GearKind::armor && !definition->plug && definition->slot == slot
            && fits_class(*definition, owner.characterClass) ? definition : nullptr;
    };
    for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
        const std::size_t equipment = kFirstPlanSlot + slot;
        auto& candidates = input.slots[slot];
        const auto& worn = owner.equipment.slots[equipment];
        const CatalogItem* wornDefinition = worn ? armor_in(*worn, equipment) : nullptr;
        if (wornDefinition != nullptr) {
            candidates.push_back(candidate_of(*worn, *wornDefinition, catalog, character, true));
            add(input.current, candidates.front().shown);
        } else {
            // Nothing on, or nothing the planner can read: the slot adds nothing unless a swap fills it.
            ArmorCandidate empty;
            empty.holder = character;
            empty.equipped = true;
            empty.locked = worn.has_value();
            candidates.push_back(empty);
        }
        if (!options.swaps || options.kept[slot] || candidates.front().locked) {
            continue;
        }
        const auto stowed = [&](const CharacterState& holder, std::size_t index) {
            for (std::size_t i = 0; i < holder.inventory.count; ++i) {
                const Item& item = holder.inventory.values[i];
                const CatalogItem* definition = armor_in(item, equipment);
                if (definition != nullptr && !item.postmaster && (item.flags & inv::kLockedItemFlag) == 0) {
                    candidates.push_back(candidate_of(item, *definition, catalog, index, false));
                }
            }
        };
        stowed(owner, character);
        for (std::size_t other = 0; options.others && other < account.characterCount; ++other) {
            if (other != character && account.characters[other].characterClass == owner.characterClass) {
                stowed(account.characters[other], other);
            }
        }
    }
    return input;
}

ArmorPlan plan_armor(const ArmorInput& input, const Stats& minimums, const std::atomic_bool& cancel) {
    ArmorPlan result;
    std::array<std::vector<PiecePlan>, kPlanSlots> plans;
    std::vector<SearchState> states{SearchState{}};
    for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
        plans[slot] = slot_plans(input.slots[slot], minimums, cancel);
        if (plans[slot].empty() || cancel.load(std::memory_order_relaxed)) {
            return result;
        }
        std::unordered_map<Key, SearchState, KeyHash> next;
        std::size_t weighed = 0;
        for (const SearchState& state : states) {
            for (std::size_t index = 0; index < plans[slot].size(); ++index) {
                if (++weighed % kCancelStride == 0 && cancel.load(std::memory_order_relaxed)) {
                    return result;
                }
                const PiecePlan& plan = plans[slot][index];
                const unsigned exotics = state.exotics + (plan.exotic ? 1U : 0U);
                if (exotics > 1) {
                    continue;
                }
                SearchState grown = state;
                add(grown.totals, plan.shown);
                grown.plans[slot] = static_cast<std::uint16_t>(index);
                grown.swaps += plan.candidate != 0 ? 1U : 0U;
                grown.changes += plan.changes;
                grown.masterworks += plan.masterworks;
                grown.exotics = exotics;
                const auto [at, added] = next.try_emplace(key_of(grown.totals, minimums), grown);
                if (!added && search_less(grown, at->second, minimums)) {
                    at->second = grown;
                }
            }
        }
        states.clear();
        states.reserve(next.size());
        for (const auto& [key, state] : next) {
            (void)key;
            states.push_back(state);
        }
        if (states.size() > kSearchStates) {
            std::partial_sort(states.begin(), states.begin() + kSearchStates, states.end(),
                              [&](const SearchState& a, const SearchState& b) { return search_less(a, b, minimums); });
            states.resize(kSearchStates);
        }
        if (states.empty()) {
            return result;
        }
    }
    const SearchState& best = *std::min_element(states.begin(), states.end(),
                                                [&](const SearchState& a, const SearchState& b) { return search_less(a, b, minimums); });
    for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
        const PiecePlan& chosen = plans[slot][best.plans[slot]];
        const ArmorCandidate& candidate = input.slots[slot][chosen.candidate];
        ArmorPlan::Piece& piece = result.pieces[slot];
        piece.candidate = chosen.candidate;
        piece.shown = chosen.shown;
        for (std::size_t s = 0; s < candidate.sockets.size() && s < chosen.picks.size(); ++s) {
            const StatSocket& socket = candidate.sockets[s];
            if (chosen.picks[s] == socket.current) {
                continue;
            }
            piece.plugs.emplace_back(socket.lane, chosen.picks[s]);
            switch (socket.kind) {
            case StatSocketKind::allocation: ++piece.allocations; break;
            case StatSocketKind::mod: ++piece.mods; break;
            case StatSocketKind::masterwork: ++piece.masterworks; break;
            }
        }
        add(result.totals, piece.shown);
        result.swaps += chosen.candidate != 0 ? 1U : 0U;
    }
    for (std::size_t i = 0; i < minimums.size(); ++i) {
        result.shortfalls[i] = minimums[i] > 0 ? (std::max)(0, minimums[i] - capped(result.totals[i])) : 0;
    }
    result.finished = true;
    return result;
}

bool apply_armor_plan(Draft& draft, const Catalog& catalog, const ArmorInput& input, const ArmorPlan& plan, std::string& error) {
    if (!plan.finished || input.character >= draft.after.characterCount) {
        error = "There is no finished plan to put on.";
        return false;
    }
    const std::size_t who = input.character;
    auto staged = std::make_unique<Draft>(draft);
    const auto chosen = [&](std::size_t slot) -> const ArmorCandidate* {
        const std::size_t index = plan.pieces[slot].candidate;
        return index < input.slots[slot].size() ? &input.slots[slot][index] : nullptr;
    };
    // Another character's piece is brought over first, as a transfer brings it.
    for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
        const ArmorCandidate* candidate = chosen(slot);
        if (candidate == nullptr) {
            error = "The armor changed since this plan was worked out.";
            return false;
        }
        if (plan.pieces[slot].candidate != 0 && candidate->holder != who
            && !transfer(*staged, catalog, candidate->holder, who, candidate->instance, error)) {
            return false;
        }
    }
    // Then each swap, an exotic last, so the exotic it replaces is already off.
    for (const bool exotic : {false, true}) {
        for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
            const ArmorCandidate* candidate = chosen(slot);
            if (plan.pieces[slot].candidate != 0 && candidate->exotic == exotic
                && !equip(*staged, catalog, who, candidate->instance, error)) {
                return false;
            }
        }
    }
    // Then the refits, each checked against the plug the plan found there.
    for (std::size_t slot = 0; slot < kPlanSlots; ++slot) {
        const ArmorPlan::Piece& piece = plan.pieces[slot];
        if (piece.plugs.empty()) {
            continue;
        }
        const ArmorCandidate* candidate = chosen(slot);
        auto& worn = staged->after.characters[who].equipment.slots[kFirstPlanSlot + slot];
        Item item = worn ? *worn : Item{};
        if (!worn || worn->instanceSoid != candidate->instance || !materialize(item, catalog)) {
            error = "The armor changed since this plan was worked out.";
            return false;
        }
        for (const auto& [lane, plug] : piece.plugs) {
            const auto socket = std::find_if(candidate->sockets.begin(), candidate->sockets.end(),
                                             [&](const StatSocket& known) { return known.lane == lane; });
            if (lane >= item.sockets.plugCount || socket == candidate->sockets.end()
                || item.sockets.plugs[lane].value_or(0U) != socket->current) {
                error = "The armor changed since this plan was worked out.";
                return false;
            }
            item.sockets.plugs[lane] = plug != 0 ? std::optional<std::uint32_t>(plug) : std::nullopt;
            // Authored plugs take precedence, as a perk picked by hand does.
            item.rolledLaneMask &= static_cast<std::uint16_t>(~(1U << lane));
            item.availablePlugRows[lane] = 0;
        }
        item.randomRoll = {};
        *worn = item;
    }
    staged->dirty = true;
    draft = std::move(*staged);
    return true;
}
} // namespace dawn::state::editor
