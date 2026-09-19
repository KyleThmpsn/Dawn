#include "state_item_random_roll.h"

#include <array>
#include <cstddef>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"

namespace dawn::state::runtime::detail {
namespace {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace items = build_data::items;

/** Cosmetic and mod buckets whose lanes carry an owned choice, never a rolled one. */
constexpr std::uint8_t kOrnamentBucketId = 13;
constexpr std::uint8_t kShaderBucketId = 14;
constexpr std::uint8_t kModBucketId = 37;
/** One lane offers at most this many plugs before the pool is treated as full. */
constexpr std::size_t kMaxLaneCandidates = 64;
/**
 * Game socket-type index of the trait columns (the manifest's trait socket-type hash
 * 2614797986, whitelisted plug category 7906839 "frames"). A trait column is retail-shaped:
 * exactly one owned plug, the rolled trait. Other perk columns offer two distinct choices
 * from their installed randomized sets when the pool has enough members.
 */
constexpr std::uint16_t kTraitSocketType = 92;
/**
 * Native equipment slots of the three character weapon slots. The slot numbers are not the
 * semantic order the UI shows: kinetic is 7, energy 8, heavy 9.
 */
constexpr std::int8_t kFirstWeaponEquipmentSlot = 7;
constexpr std::int8_t kLastWeaponEquipmentSlot = 9;

/**
 * Only a character weapon carries the randomized perk columns a roll is meant to fill. Armour,
 * cosmetics and every profile-owned row keep the curated plugs their definition ships with, so
 * they are refused here rather than in each caller.
 * @param bucketId Installed inventory bucket of the acquired definition.
 * @return True when the bucket is one of the three character weapon slots.
 */
[[nodiscard]] bool weapon_bucket(std::uint8_t bucketId) noexcept {
    namespace buckets = build_data::inventory::buckets;
    buckets::Descriptor descriptor{};
    if (!build_data::find_inventory_bucket_descriptor(bucketId, descriptor) ||
        descriptor.arraySelector != buckets::ArraySelector::character) {
        return false;
    }
    return descriptor.equipmentSlot >= kFirstWeaponEquipmentSlot &&
           descriptor.equipmentSlot <= kLastWeaponEquipmentSlot;
}

/**
 * A lane is rollable only when the plug it ships with is a perk. A shader, an ornament or a mod
 * is an owned choice the account applies, so rolling one hands out cosmetics and mods the account
 * never earned and changes the weapon's appearance on every drop.
 * @param plugDefinitionIndex Installed definition of the lane's initial plug.
 * @return True when the lane may be rolled.
 */
[[nodiscard]] bool is_perk_lane(std::uint16_t plugDefinitionIndex) noexcept {
    items::Definition plug{};
    if (!build_data::find_item_definition_index(plugDefinitionIndex, plug)) {
        return false;
    }
    return plug.bucketId != kShaderBucketId && plug.bucketId != kOrnamentBucketId &&
           plug.bucketId != kModBucketId;
}

/// A masterwork socket's candidates live in its reusable set under the global masterwork-stat
/// categories, never in the randomized set, so the roll pool alone cannot see them.
using build_data::items::kMasterworkStatCategories;

/** One masterwork socket's Tier-1 stat plugs, at most one per stat family. */
struct MasterworkScan {
    std::array<std::uint16_t, kMasterworkStatCategories.size()> candidates{};
    std::size_t count{};
    std::uint64_t seenCategories{};
};

/// Keeps the first Tier-1 plug of every masterwork-stat family the lane's allowed pool carries.
[[nodiscard]] bool collect_masterwork_candidate(void* context,
                                                std::uint16_t plugDefinitionIndex) noexcept {
    auto* scan = static_cast<MasterworkScan*>(context);
    if (scan->count >= scan->candidates.size()) {
        return true;
    }
    items::Definition plug{};
    if (!build_data::find_item_definition_index(plugDefinitionIndex, plug) ||
        plug.actionStatValue != 1) {
        return true;
    }
    std::size_t category = kMasterworkStatCategories.size();
    for (std::size_t index = 0; index < kMasterworkStatCategories.size(); ++index) {
        if (plug.plugCategoryHash == kMasterworkStatCategories[index]) {
            category = index;
            break;
        }
    }
    if (category == kMasterworkStatCategories.size()) {
        return true;
    }
    const std::uint64_t bit = std::uint64_t{1} << category;
    if ((scan->seenCategories & bit) != 0) {
        return true;
    }
    scan->seenCategories |= bit;
    scan->candidates[scan->count++] = plugDefinitionIndex;
    return true;
}

/** One lane's native-order randomized draw set, kept exactly as the Client indexes it. */
struct RollPool {
    std::array<std::uint16_t, kMaxLaneCandidates> indices{};
    std::size_t count{};
};

/** Appends one native-order randomized member of one lane's draw set. */
[[nodiscard]] bool collect_roll_member(void* context, std::uint16_t plugDefinitionIndex) noexcept {
    auto* pool = static_cast<RollPool*>(context);
    if (pool->count >= pool->indices.size()) {
        return false;
    }
    pool->indices[pool->count++] = plugDefinitionIndex;
    return true;
}

/** SplitMix64 finalizer, which decorrelates neighbouring seeds into unrelated byte lanes. */
[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

} // namespace

bool roll_random_bytes(authored_inventory::Item& item, const items::Definition& itemDefinition,
                       const item_details::Definition& itemDetail, std::uint64_t seed) noexcept {
    if (itemDetail.ordinarySocketState != item_details::OrdinarySocketState::present ||
        itemDetail.ordinarySocketCount == 0) {
        return false;
    }
    if (!weapon_bucket(itemDefinition.bucketId)) {
        return false;
    }
    // An exotic ships one authored roll, so leaving its bytes zero is the correct outcome.
    if (itemDefinition.tier == static_cast<std::uint8_t>(items::Tier::exotic)) {
        return false;
    }
    // Every refusal has to come before the bytes are written. A refused item keeps the curated
    // native defaults, and roll bytes left on it would have the Client randomize its sockets from
    // entropy no authored plug agrees with.
    if (itemDetail.ordinarySocketCount > authored_inventory::kPlugCapacity) {
        return false;
    }

    // The instance is folded in after the caller's seed is mixed, never XORed into it raw. A
    // caller that already put the instance in its seed would otherwise cancel it back out and
    // leave the roll keyed on the seed's remaining entropy alone.
    std::uint64_t stream = mix(mix(seed) ^ item.instanceSoid);
    for (std::size_t index = 0; index < item.randomRoll.size(); ++index) {
        stream = mix(stream ^ (static_cast<std::uint64_t>(index) << 56U));
        // The high byte carries the most avalanche, so it is the one worth keeping.
        item.randomRoll[index] = static_cast<std::uint8_t>(stream >> 56U);
    }

    // The Client reads an occupied socket lane straight out and never consults that lane's plug
    // set, and it leaves an empty lane empty rather than filling it from the set. So the plug has
    // to be authored here: every lane offering more than one plug takes a rolled pick and every
    // fixed lane keeps its authored plug. Sockets with no initial plug in definition (0xFFFF) are
    // populated from their plug pool.
    authored_inventory::Sockets sockets{};
    sockets.policy = authored_inventory::SocketPolicy::authored;
    sockets.plugCount = itemDetail.ordinarySocketCount;
    std::uint32_t rolledLanes = 0;
    item.rolledLaneMask = 0;
    for (std::size_t lane = 0; lane < itemDetail.ordinarySocketCount; ++lane) {
        const std::uint16_t initialPlugIndex = itemDetail.initialPlugIndices[lane];
        items::Definition initialPlug{};
        const bool hasInitialPlug =
            initialPlugIndex != item_details::kUnavailableItemIndex &&
            build_data::find_item_definition_index(initialPlugIndex, initialPlug);

        if (hasInitialPlug) {
            sockets.plugs[lane] = initialPlug.definitionHash;
        }

        RollPool roll{};
        if (!build_data::visit_socket_roll_pool(itemDefinition.definitionIndex,
                                                static_cast<std::uint8_t>(lane),
                                                &collect_roll_member, &roll)) {
            // A pool wider than the 64 owned-row bits cannot be represented faithfully. The
            // visitor fails at capacity, so discard its prefix and leave this lane authored.
            roll = {};
        }
        // A masterwork socket is an ordinary lane whose allowed pool carries the global
        // masterwork-stat plugs under their ten fixed categories. Its members never join the
        // randomized set, so the roll pool alone cannot see it. One Tier-1 plug per stat family
        // is collected; the rolled pick among the weapon-valid ones is the drop's masterwork
        // stat.
        MasterworkScan masterworkScan{};
        (void)build_data::visit_socket_plug_pool(itemDefinition.definitionIndex,
                                                 static_cast<std::uint8_t>(lane),
                                                 &collect_masterwork_candidate, &masterworkScan);
        const bool masterworkSocket = masterworkScan.count != 0;
        if (!masterworkSocket) {
            if (roll.count < 1) {
                continue;
            }
            if (hasInitialPlug && !is_perk_lane(initialPlugIndex)) {
                continue;
            }
        }

        // A masterwork lane is authored directly: one random stat's Tier-1 plug from the scanned
        // reusable candidates replaces the definition's initial plug in the lane. Perk lanes are
        // authored from the randomized set's native row order, and the selected row is marked owned
        // below. The authored plug drives inspection while the owned-row mask drives hover and
        // keeps the rolled perk reachable after a curated plug replaces it.
        std::uint16_t chosenPlugIndex = 0;
        std::size_t chosenRow = 0;
        std::uint64_t ownedRows = 0;
        // Retail restricts a masterwork stat to one the weapon itself declares: the manifest
        // carries all ten stat families in every weapon's reusable masterwork pool, yet a scout
        // never rolls Velocity and a launcher never rolls Impact. The candidates are narrowed
        // to plugs whose boosted stat row sits in the weapon's own stat block before the pick.
        std::size_t validCount = 0;
        std::array<std::uint16_t, kMasterworkStatCategories.size()> validCandidates{};
        // The declared count is a byte and the stat block is fixed, so the walk takes whichever
        // is smaller rather than trusting a detail row that outran its own array.
        const std::size_t statCount = itemDetail.statCount < itemDetail.stats.size()
                                          ? itemDetail.statCount
                                          : itemDetail.stats.size();
        if (masterworkSocket) {
            for (std::size_t index = 0; index < masterworkScan.count; ++index) {
                items::Definition plug{};
                if (!build_data::find_item_definition_index(masterworkScan.candidates[index],
                                                            plug) ||
                    plug.actionStatRow == item_details::kEmptyStatRow) {
                    continue;
                }
                for (std::size_t stat = 0; stat < statCount; ++stat) {
                    if (itemDetail.stats[stat].row == plug.actionStatRow) {
                        validCandidates[validCount++] = masterworkScan.candidates[index];
                        break;
                    }
                }
            }
            if (validCount == 0) {
                // No stat the weapon declares is masterworkable: keep the definition's default
                // plug rather than authoring an invalid one, the safe-skip outcome.
                continue;
            }
            const std::size_t pick =
                lane < item.randomRoll.size()
                    ? static_cast<std::size_t>(item.randomRoll[lane] % validCount)
                    : (stream = mix(stream ^ (static_cast<std::uint64_t>(lane) << 32U)),
                       static_cast<std::size_t>(stream % validCount));
            chosenPlugIndex = validCandidates[pick];
            // No owned rows: the client renders the lane's plug directly and never offers the
            // masterwork stat for insertion, matching retail drops.
            ownedRows = 0;
        } else {
            chosenRow = lane < item.randomRoll.size()
                            ? static_cast<std::size_t>(item.randomRoll[lane] % roll.count)
                            : (stream = mix(stream ^ (static_cast<std::uint64_t>(lane) << 32U)),
                               static_cast<std::size_t>(stream % roll.count));
            // Retail column shape: trait columns own exactly one rolled trait. Non-trait columns
            // (barrel, magazine, sight) roll multiple choices (typically 2) drawn exclusively from
            // the lane's randomized pool.
            ownedRows = std::uint64_t{1} << chosenRow;
            if (itemDetail.socketTypes[lane] != kTraitSocketType && roll.count >= 2) {
                stream = mix(stream ^ (static_cast<std::uint64_t>(lane + 16U) << 32U));
                const std::size_t secondRow =
                    (chosenRow + 1 + static_cast<std::size_t>(stream % (roll.count - 1))) %
                    roll.count;
                ownedRows |= std::uint64_t{1} << secondRow;
            }
            chosenPlugIndex = roll.indices[chosenRow];
        }
        items::Definition chosenPlug{};
        if (!build_data::find_item_definition_index(chosenPlugIndex, chosenPlug)) {
            continue;
        }
        sockets.plugs[lane] = chosenPlug.definitionHash;
        item.availablePlugRows[lane] = ownedRows;
        item.rolledLaneMask |= static_cast<std::uint16_t>(1U << lane);
        ++rolledLanes;

        std::array<char, core::log::kLineCapacity> laneLine{};
        const int laneCount = std::snprintf(
            laneLine.data(), laneLine.size(),
            "ev=item_roll stage=lane lane=%zu rows=%zu "
            "row=%zu stats=%zu valid=%zu "
            "byte_val=0x%02X socket_type=%u category=0x%08X "
            "masterwork=%u "
            "initial=0x%08X chosen=0x%08X owned_mask=0x%016llX",
            lane, roll.count, chosenRow, masterworkScan.count, validCount,
            static_cast<unsigned>(lane < item.randomRoll.size() ? item.randomRoll[lane] : 0U),
            itemDetail.socketTypes[lane], hasInitialPlug ? initialPlug.plugCategoryHash : 0U,
            static_cast<unsigned>(masterworkSocket ? 1U : 0U),
            hasInitialPlug ? initialPlug.definitionHash : 0U, chosenPlug.definitionHash,
            static_cast<unsigned long long>(item.availablePlugRows[lane]));
        if (laneCount > 0) {
            core::log::write(core::log::Channel::state, core::log::Level::debug,
                             {laneLine.data(), static_cast<std::size_t>(laneCount)});
        }
    }
    item.sockets = sockets;

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(),
        "ev=item_roll stage=random_bytes result=ok instance=0x%llX "
        "weapon=0x%08X seed=0x%llX rolled_lanes=%u lanes=%u "
        "bytes=%02X%02X%02X%02X%02X%02X%02X%02X",
        static_cast<unsigned long long>(item.instanceSoid), itemDefinition.definitionHash,
        static_cast<unsigned long long>(seed), rolledLanes, itemDetail.ordinarySocketCount,
        item.randomRoll[0], item.randomRoll[1], item.randomRoll[2], item.randomRoll[3],
        item.randomRoll[4], item.randomRoll[5], item.randomRoll[6], item.randomRoll[7]);
    if (count > 0) {
        core::log::write(core::log::Channel::state, core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

} // namespace dawn::state::runtime::detail
