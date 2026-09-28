// SPDX-License-Identifier: GPL-3.0-only
// The armor planner follows Sundial's armor stat adjuster by KyleThmpsn. See vendor/sundial/NOTICE.md.
#pragma once
#include "edit.h"
#include <atomic>
#include <optional>

namespace dawn::state::editor {
/** Armor slots a plan fills, helmet to class item, which are equipment slots 3 to 7. */
inline constexpr std::size_t kPlanSlots = 5;
inline constexpr std::size_t kFirstPlanSlot = 3;
/** The game counts a character stat up to this; whatever a set adds past it does nothing. */
inline constexpr int kStatCap = 100;

/** What a plan may do besides refitting the armor on now. */
struct ArmorOptions {
    /** Put on a stowed piece in place of the one on, where that comes closer. */
    bool swaps{true};
    /** Reach the stowed armor of the other characters of this class too; a swap brings it over. */
    bool others{true};
    /** Slots, helmet to class item, whose piece on now stays on. Its plugs may still change. */
    std::array<bool, kPlanSlots> kept{};
};

/** What a socket a plan refits holds, which the preview names a change by. */
enum class StatSocketKind : std::uint8_t {
    /** One of the four sockets an armor piece's stats roll in. */
    allocation,
    /** The general armor mod socket, which takes a single-stat mod or nothing. */
    mod,
    /** The energy socket, which a plan only ever raises to a masterwork. */
    masterwork,
};

/** One plug a socket can take, by hash, with zero for an empty lane, and the stats it adds. */
struct StatChoice {
    std::uint32_t plug{};
    Stats values{};
};

/** A socket a plan may refit: its lane, the plug in it now, and every plug it may take instead. */
struct StatSocket {
    std::uint8_t lane{};
    std::uint32_t current{};
    StatSocketKind kind{};
    std::vector<StatChoice> choices;
};

/** One armor piece a plan may use for a slot, and everything the plan needs to know of it. */
struct ArmorCandidate {
    /** Zero for a slot with nothing on, which only a swap can fill. */
    std::uint64_t instance{};
    std::uint32_t definition{};
    /** Character that holds the piece. Another character's piece is brought over by a swap. */
    std::size_t holder{};
    bool equipped{}, exotic{}, locked{};
    /** Stats the piece gives whatever the plan does, as stored. */
    Stats fixed{};
    /** Stats the piece shows now, as its tooltip shows them. */
    Stats shown{};
    /** The curve each stat is shown through, from the piece's stat group, or none for a stored value. */
    std::array<std::optional<ScaledStat>, 6> curves;
    std::vector<StatSocket> sockets;
};

/** Everything a plan is worked out from, taken on the editor's thread so a worker can plan from it. */
struct ArmorInput {
    std::size_t character{};
    /** Per slot, the piece on now first, then every stowed piece a swap may put on. */
    std::array<std::vector<ArmorCandidate>, kPlanSlots> slots;
    /** The six stats the armor on now adds up to, as the character screen shows them. */
    Stats current{};
};

/** The closest armor a plan found for a set of minimums. */
struct ArmorPlan {
    struct Piece {
        /** The chosen piece, as an index into its slot's candidates; zero is the piece on now. */
        std::size_t candidate{};
        /** What the piece shows once refitted. */
        Stats shown{};
        /** Each lane the plan refits and the plug it takes there, zero for empty. */
        std::vector<std::pair<std::uint8_t, std::uint32_t>> plugs;
        unsigned allocations{}, mods{}, masterworks{};
    };
    std::array<Piece, kPlanSlots> pieces{};
    /** The six stats the plan's armor adds up to, as the character screen will show them. */
    Stats totals{};
    /** How far each stat falls short of its minimum, counted up to the cap; zero where it is met. */
    Stats shortfalls{};
    unsigned swaps{};
    /** False when the plan was abandoned before it finished, so it says nothing. */
    bool finished{};
    [[nodiscard]] bool exact() const noexcept;
    [[nodiscard]] bool changes() const noexcept;
};

/**
 * Takes what a plan for one character is worked out from: the armor on now, and when swaps are
 * allowed, every stowed piece that could replace it. Locked armor is never changed: a locked piece on
 * now keeps its slot and its plugs, and a locked piece stowed is never put on.
 */
ArmorInput armor_input(const AccountState& account, const Catalog& catalog, std::size_t character, const ArmorOptions& options);

/**
 * Finds the armor closest to a set of minimums, as Sundial's adjuster does: each slot's pieces with
 * every refit of their stat sockets are searched together, keeping the best of every distinct set of
 * totals. Sets that meet the minimums, or come closest, win; then those wasting least past the cap;
 * then those swapping and refitting least. At most one exotic is worn.
 * @param minimums Least of each stat, in `Stats` order; zero leaves a stat free.
 * @param cancel Checked as the search runs, so a plan no one will read stops early.
 */
ArmorPlan plan_armor(const ArmorInput& input, const Stats& minimums, const std::atomic_bool& cancel);

/**
 * Puts one plan on: brings over what other characters hold, puts on each swapped piece with an
 * exotic last, then refits the plugs. It is all or nothing; on refusal the draft is left as it was.
 * @param input The input the plan was worked out from.
 */
bool apply_armor_plan(Draft& draft, const Catalog& catalog, const ArmorInput& input, const ArmorPlan& plan, std::string& error);
} // namespace dawn::state::editor
