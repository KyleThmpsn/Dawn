#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dawn::state::build_data::items {

/** Signed native definition indices give 32,768 item rows. */
inline constexpr std::size_t kDefinitionCapacity = 32768;
/** All bucket bits set mark a valid item row whose inventory bucket was not found. */
inline constexpr std::uint8_t kUnresolvedBucketId = 0xFF;
/** A plug with no authored insertion/enabled price carries all set-index bits. */
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;

/** One installed-build item identity, used to look up authored definition hashes. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{kUnresolvedBucketId};
    std::uint16_t insertionMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    std::uint16_t enabledMaterialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    /** Native rarity ladder: 1 common through 5 exotic; 0 outside the ladder. */
    std::uint8_t tier{};
    /** Plug category the definition declares, or 0 when it declares none. */
    std::uint32_t plugCategoryHash{};
    /**
     * The definition's first investment-stat value, clamped into a byte. A masterwork stat plug
     * declares its tier here (1..10); other plugs carry whatever their first stat says, which the
     * roll only reads to detect a Tier-1 masterwork. Zero when the definition declares no stat.
     */
    std::uint8_t actionStatValue{};
    /**
     * Stat table row of the first investment stat, naming the stat the definition contributes
     * to. A masterwork stat plug carries the stat it boosts here; a definition with no stat
     * carries no row, which never matches a weapon stat.
     */
    std::uint8_t actionStatRow{};
};

/**
 * The ten global masterwork-stat plug categories, one per weapon stat.
 * Every weapon's masterwork socket draws its candidates from these and never from the randomized
 * set, so a plug declaring one of them is that weapon's masterwork and the tier it reached is its
 * own `actionStatValue`. Both the roll that fills those sockets and the tooltip that reads one
 * back answer off this list, so it lives with the definitions it describes rather than twice.
 */
inline constexpr std::array<std::uint32_t, 10> kMasterworkStatCategories{
    199786516U,  // handling
    482070447U,  // draw time
    717646604U,  // reload speed
    1238043140U, // accuracy
    1392237582U, // range
    1762223024U, // stability
    1847616696U, // blast radius
    2321551094U, // projectile speed
    2458812152U, // impact
    2827428737U, // charge time
};

/** Tier a masterwork plug reaches when the weapon holding it counts as masterworked. */
inline constexpr std::uint8_t kMasterworkTier = 10;

/**
 * @return True when one definition is a weapon's masterwork plug at the top tier.
 * @param definition Plug definition being asked about.
 */
[[nodiscard]] constexpr bool masterwork_plug(const Definition& definition) noexcept {
    if (definition.actionStatValue < kMasterworkTier) {
        return false;
    }
    for (const std::uint32_t category : kMasterworkStatCategories) {
        if (definition.plugCategoryHash == category) {
            return true;
        }
    }
    return false;
}

/** Native item tiers, as the definition's rarity byte encodes them. */
enum class Tier : std::uint8_t {
    none = 0,
    common = 1,
    uncommon = 2,
    rare = 3,
    legendary = 4,
    exotic = 5,
};

/** Clears every generated item mapping. */
void clear() noexcept;

/**
 * Checks one complete dense item definition table.
 * @param definitions Candidate installed-build mappings.
 * @return True when every native index appears exactly once.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated item definition table in one step.
 * @param definitions Complete dense installed-build mappings.
 * @return True when all rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one authored hash with its expected inventory bucket.
 * @param definitionHash Authored item definition hash.
 * @param bucketId Expected inventory bucket id.
 * @param definition Receives the one matching mapping.
 * @return True only when exactly one mapping with a known bucket matches both keys.
 */
[[nodiscard]] bool
find(std::uint32_t definitionHash, std::uint8_t bucketId, Definition& definition) noexcept;

/**
 * Finds one authored hash without needing a known inventory bucket.
 * @param definitionHash Authored item or plug definition hash.
 * @param definition Receives the one matching installed-build mapping.
 * @return True only when exactly one native row carries the hash.
 */
[[nodiscard]] bool find_hash(std::uint32_t definitionHash, Definition& definition) noexcept;

/**
 * Finds one dense installed-build row by its native definition index.
 * @param definitionIndex Native item-definition row index.
 * @param definition Receives the matching installed-build mapping.
 * @return True when the table holds that row.
 */
[[nodiscard]] bool find_index(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies every mapping in native definition-index order.
 * @param output Caller-owned fixed mapping storage.
 * @param count Receives the copied row count.
 * @return False only when the caller's storage cannot hold the whole table.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of installed-build item mappings. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace dawn::state::build_data::items
