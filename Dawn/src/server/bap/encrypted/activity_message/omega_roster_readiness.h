#pragma once

#include <array>
#include "../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../state/activity/omega_intro_rules.h"
#include "../../../../state/activity/omega_enemy_crown_catalog.h"
#include "../../../../state/activity/omega_lair_full_roster_catalog.h"

namespace dawn::server::bap::encrypted::activity_message::omega_roster_readiness {
namespace service = middleware::bap::activity_message;

/** Exact measured Omega roster topology acknowledged by both recovered bootstrap forms. */
constexpr std::array<std::uint32_t, 6> kOmegaRosterKeys{{
    0x4786C0E0U,
    0x29D7B029U,
    0x82FB58B7U,
    0xBA5F26EFU,
    0xD00142CFU,
    0xF7A6CE7FU,
}};
constexpr std::uint16_t kOmegaTopLevelGroups = 3;
constexpr std::int16_t kOmegaBubble = 15;
/** The forest map-generator group is appended to the published roster (stable group set across
 *  the tunnel crossing keeps the authored objects alive); the client's acknowledgement then
 *  carries one extra entry for it, on the forest bubble. */
constexpr std::uint32_t kOmegaForestGeneratorKey = 0x2763EC97U;
constexpr std::int16_t kOmegaForestBubble = 11;
constexpr std::uint32_t kOmegaLairKey = state::activity::omega_presentation::kIntroRegistry;
constexpr std::int16_t kOmegaLairBubble = 14;
/** Installed Omega also registers these two Forest areas before the opening.
 * Their exact identities/scopes were acknowledged in the sixteen-entry startup. */
constexpr std::array<service::sense_update::RosterEntry, 2> kOmegaForestAreas{{
    {0x0A7A8608U, 12, 0x83U, true},
    {0x34D23982U, 13, 0x83U, true},
}};
struct ExpectedSenseGroup final {
    std::uint32_t key;
    std::uint16_t firstObject;
    std::uint16_t objectCount;
};

struct ExpectedSenseObject final {
    std::uint32_t key;
    std::uint16_t slotIndex;
    std::uint8_t slotType;
};

/** Required object topology shared by the recovered 235-byte and 236-byte bootstraps. */
constexpr std::array<ExpectedSenseGroup, 2> kOmegaInitialGroups{{
    {0xBA5F26EFU, 0, 2},
    {0xD00142CFU, 2, 4},
}};
constexpr std::array<ExpectedSenseObject, 6> kOmegaInitialObjects{{
    {0xBA5F26EFU, 1, 23},
    {0xBA5F26EFU, 2, 70},
    {0xD00142CFU, 0, 1},
    {0xD00142CFU, 1, 43},
    {0xD00142CFU, 16, 23},
    {0xD00142CFU, 17, 70},
}};
constexpr std::uint32_t kOmegaFirstSenseGroupBits = 334;
/** Four object headers, Scene/mission/monitor bodies and one group terminator. */
constexpr std::uint32_t kOmegaSecondSenseGroupFixedBits = 521;

[[nodiscard]] inline bool exact_omega_roster(
    const service::sense_update::SenseUpdate& update) noexcept {
    // Preserve the recovered 6..14-entry forms. The installed full roster adds
    // exactly two Forest areas to the complete combat set, not arbitrary keys.
    // All six opening sensor bodies remain mandatory in the report check below.
    const bool withForestAreas = update.rosterEntryCount == kOmegaRosterKeys.size() + 4
        + state::activity::omega_lair_full_roster::kCombatGroups.size() + kOmegaForestAreas.size();
    const auto openingCount = update.rosterEntryCount - (withForestAreas ? kOmegaForestAreas.size() : 0U);
    const auto extraCount=openingCount>kOmegaRosterKeys.size()+4
        ? openingCount-kOmegaRosterKeys.size()-4 : 0U;
    if(extraCount>state::activity::omega_lair_full_roster::kCombatGroups.size()) { return false; }
    const bool withCrown = openingCount >= kOmegaRosterKeys.size() + 4;
    const bool withBoss = openingCount == kOmegaRosterKeys.size() + 3 || withCrown;
    const bool withLair = openingCount == kOmegaRosterKeys.size() + 2 || withBoss;
    const bool withGenerator =
        openingCount == kOmegaRosterKeys.size() + 1 || withLair;
    if (!update.hasRosterAcknowledgement
        || update.topLevelRosterCount != kOmegaTopLevelGroups
        || (openingCount != kOmegaRosterKeys.size() && !withGenerator)
        || update.bubbleBlockCount != (withLair ? 3U : withGenerator ? 2U : 1U)
            + (withForestAreas ? kOmegaForestAreas.size() : 0U)) {
        return false;
    }
    // The client orders bubble entries by bubble number, so the generator (bubble 11) sits
    // BEFORE the bubble-15 trio (measured: index 3 of 7). Accept it at any position while the
    // six measured entries keep their own relative order.
    std::size_t expectedIndex = 0;
    bool generatorSeen = !withGenerator;
    bool lairSeen = !withLair;
    bool bossSeen = !withBoss;
    bool crownSeen = !withCrown;
    std::array<bool,4> combatSeen{};
    std::array<bool, kOmegaForestAreas.size()> forestAreasSeen{};
    for (std::size_t index = 0; index < update.rosterEntryCount; ++index) {
        const service::sense_update::RosterEntry& entry = update.rosterEntries[index];
        if (!entry.active || entry.state != 0x83U) {
            return false;
        }
        bool forestArea = false;
        for (std::size_t i = 0; withForestAreas && i < kOmegaForestAreas.size(); ++i) {
            const auto& expected = kOmegaForestAreas[i];
            if (entry.registryKey != expected.registryKey) { continue; }
            if (forestAreasSeen[i] || entry.bubble != expected.bubble) { return false; }
            forestAreasSeen[i] = true;
            forestArea = true;
            break;
        }
        if (forestArea) { continue; }
        if (!generatorSeen && entry.registryKey == kOmegaForestGeneratorKey
            && entry.bubble == kOmegaForestBubble) {
            generatorSeen = true;
            continue;
        }
        if (!lairSeen && entry.registryKey == kOmegaLairKey && entry.bubble == kOmegaLairBubble) {
            lairSeen = true;
            continue;
        }
        if (!bossSeen && entry.registryKey == state::activity::omega_presentation::kBossRegistry
            && entry.bubble == kOmegaLairBubble) {
            bossSeen = true;
            continue;
        }
        if (!crownSeen && entry.registryKey == state::activity::omega_enemy_crown::kRegistry
            && entry.bubble == kOmegaLairBubble) {
            crownSeen = true;
            continue;
        }
        bool combat=false;
        for(std::size_t i=0;i<extraCount;++i) {
            const auto& expected=state::activity::omega_lair_full_roster::kCombatGroups[i];
            if(entry.registryKey!=expected.key) { continue; }
            if(combatSeen[i] || entry.bubble!=kOmegaLairBubble) { return false; }
            combatSeen[i]=true;
            combat=true;
            break;
        }
        if(combat) { continue; }
        if (expectedIndex >= kOmegaRosterKeys.size()) {
            return false;
        }
        const std::int16_t expectedBubble =
            expectedIndex < kOmegaTopLevelGroups ? -1 : kOmegaBubble;
        if (entry.registryKey != kOmegaRosterKeys[expectedIndex]
            || entry.bubble != expectedBubble) {
            return false;
        }
        ++expectedIndex;
    }
    for(std::size_t i=0;i<extraCount;++i) { if(!combatSeen[i]) { return false; } }
    for (bool seen : forestAreasSeen) { if (withForestAreas && !seen) { return false; } }
    return generatorSeen && lairSeen && bossSeen && crownSeen && expectedIndex == kOmegaRosterKeys.size();
}

[[nodiscard]] inline bool exact_body(const service::sense_update::SenseObject& object,
                              std::uint32_t bodyBits,
                              std::uint64_t first,
                              std::uint64_t second,
                              std::uint64_t third = 0) noexcept {
    return object.bodyBits == bodyBits && object.bodyFirst == first
           && object.bodySecond == second && object.bodyThird == third;
}

/** The initial type-1 object has two measured optional-field layouts with the same role. */
[[nodiscard]] inline bool omega_initial_type1_body(
    const service::sense_update::SenseObject& object) noexcept {
    return exact_body(object, 85, 0x8093180000000000ULL, 1)
           || exact_body(object, 92, 0x87F9263000000000ULL, 1);
}

[[nodiscard]] inline bool omega_initial_object_body(
    std::size_t index,
    const service::sense_update::SenseObject& object) noexcept {
    if (index == 0 || index == 4) {
        return exact_body(object,
                          167,
                          0xAFFFFFFFF3F80000ULL,
                          0x0BFFFFFFFAFFFFFFULL,
                          0x7F00000001ULL);
    }
    if (index == 1 || index == 5) {
        return exact_body(object, 54, 0x20800100000001ULL, 0);
    }
    if (index == 2) {
        return omega_initial_type1_body(object);
    }
    return index == 3 && exact_body(object, 75, 0xC000000048000000ULL, 1);
}

[[nodiscard]] inline bool exact_omega_initial_report(
    const service::sense_update::SenseUpdate& update) noexcept {
    if (!exact_omega_roster(update) || update.groupCount != kOmegaInitialGroups.size()
        || update.objectCount != kOmegaInitialObjects.size()) {
        return false;
    }
    for (std::size_t index = 0; index < kOmegaInitialGroups.size(); ++index) {
        const service::sense_update::SenseGroup& actual = update.groups[index];
        const ExpectedSenseGroup& expected = kOmegaInitialGroups[index];
        if (actual.registryKey != expected.key || actual.firstObject != expected.firstObject
            || actual.objectCount != expected.objectCount) {
            return false;
        }
    }
    if (update.groups[0].bodyBits != kOmegaFirstSenseGroupBits
        || update.groups[1].bodyBits
               != kOmegaSecondSenseGroupFixedBits + update.objects[2].bodyBits) {
        return false;
    }
    for (std::size_t index = 0; index < kOmegaInitialObjects.size(); ++index) {
        const service::sense_update::SenseObject& actual = update.objects[index];
        const ExpectedSenseObject& expected = kOmegaInitialObjects[index];
        if (actual.registryKey != expected.key || actual.slotType != expected.slotType
            || actual.slotIndex != expected.slotIndex
            || !omega_initial_object_body(index, actual)) {
            return false;
        }
    }
    return true;
}

} // namespace dawn::server::bap::encrypted::activity_message::omega_roster_readiness
