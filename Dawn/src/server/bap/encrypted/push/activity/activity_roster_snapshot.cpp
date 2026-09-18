#include "../../../../../state/activity/coo/native_mission_forest_authority.h"
#include "../../../../../state/activity/vendors/lifetime.h"
#include "../../../../../state/activity/Newlight/launchpad/transit.h"
#include <Windows.h>
#include "../../../../../client/player/player_position.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <span>
#include <memory>
#include <new>
#include "native_roster_lifetime_projection.h"
#include "tower_spawn_recovery.h"
#include "native_activity_publisher.h"

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/bap/activity_message/tower_watch_cue_manifest.h"
#include "../../../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/destination/activity_destination_spawn_binding.h"
#include "../../../../../state/activity/events/activity_event_selection.h"
#include "../../../../../state/activity/forced/activity_forced_destination.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/activity/nightfall/rules.h"
#include "../../../../../state/activity/progress/mission_progress.h"
#include "../../../../../state/activity/omega_presentation.h"
#include "../../../../../state/activity/coo/omega_projection.h"
#include "../../../../../state/activity/coo/omega_opening_projection.h"
#include "../../../../../state/activity/omega_first_lair_runtime.h"
#include "../../../../../state/activity/omega_ending.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/build_data/scenarios/scenario_catalog.h"
#include "../../../../../state/runtime/runtime.h"
#include "activity_arrival.h"
#include "activity_region_snapshot.h"
#include "internal.h"
#include "omega_lair_roster.h"
#include "omega_opening_publication.h"
#include "gateway_roster.h"
#include "deadly_trial_roster.h"
#include "beyond_infinity_roster.h"
#include "deep_storage_roster.h"
#include "hijacked_roster.h"
#include "vendor_roster.h"
#include "launchpad_roster.h"
#include "newlight_tower_roster.h"
#include "../../../../../state/activity/Newlight/launchpad/runtime.h"
#include "../../../../../state/activity/beyond_infinity/runtime.h"
#include "../../../../../state/activity/deep_storage/runtime.h"
#include "../../../../../state/activity/hijacked/runtime.h"
#include "../../../../../state/activity/beyond_infinity/transit.h"
#include "../../../../../state/activity/deadly_trial/runtime.h"
#include "../../../../../state/activity/gateway/runtime.h"
#include "../../../../../state/activity/strike_pact/runtime.h"
#include "strike_pact_roster.h"
#include "../../../../runtime/activity/native_activity_profiles.h"
#include "../../../../runtime/activity/native_activity_runtime.h"
#include "../../../../runtime/activity/native_round_projection.h"
#include "../../../../runtime/activity/native_activity_transit.h"
#include "../../../../runtime/activity/haunted_forest_lifetime_profile.h"
#include "../../../../runtime/activity/adventure_opening_publication.h"
#include "strike_bond_roster.h"
#include "../../../../../state/activity/strike_bond/runtime.h"
#include "../../../../../middleware/bap/activity_message/tower_watch_cue_manifest.h"

namespace dawn::server::bap::encrypted::push::activity {
namespace {

namespace layouts = state::build_data::scenarios;
namespace membership_message = middleware::bap::activity_message::replicate_membership;
namespace tower_watch = middleware::bap::activity_message::tower_watch;

/**
 * The type-17 lifetime state the roster reports.
 * Only 3, 6 and 10 are safe: spawn gate G4 indexes a jump table with no bounds check, so any other
 * value is a wild jump, not a refusal. 3 is what a live activity measured.
 */
constexpr std::uint8_t kLifetimeState = 3;
/** Sends whose state byte moves regardless. A body absorbed while the world loads needs them. */
constexpr std::uint8_t kWarmupSends = 3;
/** The state byte is stored biased into one signed byte, so the sequence stays inside this. */
constexpr std::uint8_t kStateSequenceWrap = 128;
/** Standard 32-bit FNV-1a basis and prime fold the group set into one comparable value. */
constexpr std::uint32_t kFoldBasis = 2166136261U;
constexpr std::uint32_t kFoldPrime = 16777619U;
/** Only a type-13 slot binds the player, so only a group holding one may carry the key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
constexpr std::uint8_t kSlotTypeMissionDirector = 35;
/** The join request names its character in the low half of the SOID, so compare on that half. */
constexpr std::uint64_t kIdentityLowMask = 0xFFFFFFFFULL;
constexpr std::uint16_t kNoRosterGroup = 0xFFFFU;
/** Scenario whose authored Tower event groups are selected by the event configuration. */
constexpr std::string_view kTowerScenarioName = "city_tower_social_d2";
/** Actual Farm scenario name from the recovered activity catalog. */
constexpr std::string_view kFarmScenarioName = "campaign_social_space_d2";

std::atomic_uint64_t g_lastTowerfallLayoutTrace{UINT64_MAX};
std::atomic_uint64_t g_lastTowerfallBuilderTrace{UINT64_MAX};

[[nodiscard]] std::int64_t utc_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static_assert(message::kGroupCapacity == layouts::kDestinationWireGroupCapacity);

/**
 * Finds the full authored SOID for the character the join request named.
 * The client sends a short identity form. Publishing that form binds no object, so the full SOID
 * goes out instead.
 * @param joinCharacter Character id the join request carried, or zero when it carried none.
 * @return Authored SOID of the named character, or of the selected character when nothing matches.
 */
[[nodiscard]] std::uint64_t roster_player_key(const state::AccountState& account,
                                              std::uint64_t joinCharacter,
                                              bool allowSelectedFallback) noexcept {
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (joinCharacter == 0) {
        return allowSelectedFallback ? selected : 0;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const std::uint64_t soid = account.characters[index].soid;
        if ((soid & kIdentityLowMask) == (joinCharacter & kIdentityLowMask)) {
            return soid;
        }
    }
    return allowSelectedFallback ? selected : 0;
}

[[nodiscard]] bool
load_group(std::uint16_t tableIndex, Scratch& scratch, std::size_t slot) noexcept {
    return slot < scratch.rosterGroups.size()
           && state::build_data::find_roster_group(tableIndex, scratch.rosterGroups[slot]);
}

/** @return True when the event roster selection applies to this destination. */
[[nodiscard]] constexpr bool event_layout_destination(std::string_view name) noexcept {
    return name == kTowerScenarioName || name == kFarmScenarioName;
}

enum class EventGroupDisposition : std::uint8_t { keep, remove, missing };

/**
 * Compacts resolved group indices while distinguishing an excluded event from a missing lookup.
 * @param indices Group table indices, compacted in place.
 * @param count Number of entries in indices, updated to the compacted count.
 * @param classify Resolves one index and classifies its registry key.
 * @return False when a referenced index could not be resolved.
 */
template <typename Classify>
[[nodiscard]] constexpr bool compact_event_groups(std::span<std::uint16_t> indices,
                                                   std::size_t& count,
                                                   Classify&& classify) noexcept {
    if (count > indices.size()) {
        return false;
    }
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const EventGroupDisposition disposition = classify(indices[index]);
        if (disposition == EventGroupDisposition::missing) {
            return false;
        }
        if (disposition == EventGroupDisposition::remove) {
            continue;
        }
        indices[kept++] = indices[index];
    }
    for (std::size_t index = kept; index < count; ++index) {
        indices[index] = 0;
    }
    count = kept;
    return true;
}

/** Compacts bubble group indices and their masks as one positional domain. */
template <typename Classify>
[[nodiscard]] constexpr bool compact_event_bubble_groups(
    std::span<std::uint16_t> indices,
    std::span<std::uint64_t> masks,
    std::size_t& count,
    Classify&& classify) noexcept {
    if (count > indices.size() || count > masks.size()) {
        return false;
    }
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const EventGroupDisposition disposition = classify(indices[index]);
        if (disposition == EventGroupDisposition::missing) {
            return false;
        }
        if (disposition == EventGroupDisposition::remove) {
            continue;
        }
        indices[kept] = indices[index];
        masks[kept] = masks[index];
        ++kept;
    }
    for (std::size_t index = kept; index < count; ++index) {
        indices[index] = 0;
        masks[index] = 0;
    }
    count = kept;
    return true;
}

/** Synthetic contract checks for the compaction invariants; no installed data is involved. */
[[nodiscard]] constexpr bool synthetic_event_compaction_coverage() noexcept {
    constexpr auto classify = [](std::uint16_t index) noexcept {
        return index == 20 ? EventGroupDisposition::remove : EventGroupDisposition::keep;
    };
    std::array<std::uint16_t, 4> topLevel{10, 20, 30, 40};
    std::size_t topLevelCount = topLevel.size();
    if (!compact_event_groups(topLevel, topLevelCount, classify)
        || topLevelCount != 3 || topLevel[0] != 10 || topLevel[1] != 30
        || topLevel[2] != 40 || topLevel[3] != 0) {
        return false;
    }

    std::array<std::uint16_t, 3> bubbleGroups{10, 20, 30};
    std::array<std::uint64_t, 3> bubbleMasks{1, 2, 4};
    std::size_t bubbleCount = bubbleGroups.size();
    if (!compact_event_bubble_groups(bubbleGroups, bubbleMasks, bubbleCount, classify)
        || bubbleCount != 2 || bubbleGroups[0] != 10 || bubbleGroups[1] != 30
        || bubbleMasks[0] != 1 || bubbleMasks[1] != 4 || bubbleGroups[2] != 0
        || bubbleMasks[2] != 0) {
        return false;
    }

    std::array<std::uint16_t, 3> authored{10, 20, 99};
    std::size_t authoredCount = authored.size();
    if (!compact_event_groups(authored, authoredCount, classify)
        || authoredCount != 2 || authored[0] != 10 || authored[1] != 99 || authored[2] != 0) {
        return false;
    }

    std::array<std::uint16_t, 2> otherMission{10, 99};
    std::size_t otherMissionCount = otherMission.size();
    constexpr auto keep_all = [](std::uint16_t) noexcept { return EventGroupDisposition::keep; };
    return !event_layout_destination("infinite_abyss")
           && compact_event_groups(otherMission, otherMissionCount, keep_all)
           && otherMissionCount == 2 && otherMission[0] == 10 && otherMission[1] == 99;
}

static_assert(synthetic_event_compaction_coverage());

/**
 * Filters one caller-owned scenario layout by the selected seasonal event keys.
 * Every table index is resolved before it can be removed, so an unavailable row fails the layout
 * rather than being mistaken for an unknown key. The three compacted domains keep their paired
 * masks and per-bubble counts aligned with the surviving table indices.
 * @param name Scenario package name.
 * @param layout Caller-owned layout copy; the cached scenario definition is never passed here.
 * @return True when the layout was unchanged or all referenced groups were resolved.
 */
[[nodiscard]] bool filter_event_layout(std::string_view name, layouts::Definition& layout) noexcept {
    if (!event_layout_destination(name)) {
        return true;
    }
    const auto classify = [name](std::uint16_t tableIndex) noexcept {
        layouts::RosterGroup group{};
        if (!state::build_data::find_roster_group(tableIndex, group)) {
            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(
                line.data(), line.size(),
                "ev=events stage=roster_filter result=missing_group dest=%.*s index=%u",
                static_cast<int>(name.size()), name.data(), static_cast<unsigned>(tableIndex));
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            return EventGroupDisposition::missing;
        }
        // events::withheld only returns true for a mapped seasonal key; ordinary, root, player,
        // and vendor groups therefore remain untouched even when the file contains other keys.
        return state::activity::events::withheld(group.registryKey)
                   ? EventGroupDisposition::remove
                   : EventGroupDisposition::keep;
    };
    std::size_t rosterGroupCount = layout.rosterGroupCount;
    if (!compact_event_groups(layout.rosterGroups, rosterGroupCount, classify)) {
        return false;
    }
    layout.rosterGroupCount = static_cast<std::uint8_t>(rosterGroupCount);
    std::size_t bubbleGroupCount = layout.bubbleGroupCount;
    if (!compact_event_bubble_groups(
            layout.bubbleGroups, layout.bubbleGroupMasks, bubbleGroupCount, classify)) {
        return false;
    }
    layout.bubbleGroupCount = static_cast<std::uint8_t>(bubbleGroupCount);

    for (std::size_t bubble = 0; bubble < layout.authoredGroups.size(); ++bubble) {
        std::size_t authoredCount = layout.authoredGroupCounts[bubble];
        if (!compact_event_groups(layout.authoredGroups[bubble], authoredCount, classify)) {
            return false;
        }
        layout.authoredGroupCounts[bubble] = static_cast<std::uint8_t>(authoredCount);
    }
    return true;
}

void expose_group(const layouts::RosterGroup& group, message::Group& output) noexcept {
    output.key = group.registryKey;
    output.slotTypes = std::span<const std::uint8_t>(group.slotTypes.data(), group.slotCount);
    output.slotFlags = std::span<const std::uint8_t>(group.slotFlags.data(), group.slotCount);
    output.slotIndices = std::span<const std::uint16_t>(group.slotIndices.data(), group.slotCount);
}

// The state-121 shells share these existing global keys. Only the native
// bookend has a new network descriptor; its type61 companion has none.
bool terminal_roster(Scratch& scratch,message::Roster& roster) noexcept {
    constexpr std::array<std::uint32_t,4> globals{0x4786C0E0,0x82FB58B7,0x29D7B029,0x96E0A5E5};
    std::size_t count{};bool root=false;
    for(std::size_t i=0;i<roster.groupCount;++i) {
        const auto group=roster.groups[i];
        if(std::find(globals.begin(),globals.end(),group.key)==globals.end()) continue;
        root|=group.key==0x4786C0E0;roster.groups[count++]=group;
    }
    if(!root || count>=roster.groups.size()) return false;
    static constexpr std::array<std::uint8_t,1> types{6},flags{2};
    static constexpr std::array<std::uint16_t,1> indices{0};
    roster.playerKeyGroup=0;
    for(std::size_t i=0;i<count && !roster.playerKeyGroup;++i)
        if(std::find(roster.groups[i].slotTypes.begin(),roster.groups[i].slotTypes.end(),std::uint8_t{13})!=roster.groups[i].slotTypes.end())
            roster.playerKeyGroup=roster.groups[i].key;
    if(!roster.playerKeyGroup) return false;
    roster.topLevelGroupCount=count;
    roster.groups[count++]={0x3A6CE17A,types,flags,indices};
    roster.groupCount=count;
    scratch.rosterSubBlockKeys[0][0]=0x3A6CE17A;
    scratch.rosterSubBlocks[0]={15,std::span(scratch.rosterSubBlockKeys[0]).first(1)};
    roster.bubbleSubBlocks=std::span(scratch.rosterSubBlocks).first(1);
    return true;
}

enum class OrdinaryCoverage : std::uint8_t { absent, active, inactiveBubble };

[[nodiscard]] OrdinaryCoverage ordinary_coverage(const layouts::Definition& layout,
                                                  std::uint16_t tableIndex,
                                                  std::size_t selectedSlice,
                                                  std::size_t& scratchSlot) noexcept {
    scratchSlot = 0;
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (layout.rosterGroups[index] == tableIndex) {
            scratchSlot = index;
            return OrdinaryCoverage::active;
        }
    }
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        if (layout.bubbleGroups[index] == tableIndex) {
            scratchSlot = std::size_t{layout.rosterGroupCount} + index;
            return (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << selectedSlice)) != 0
                       ? OrdinaryCoverage::active
                       : OrdinaryCoverage::inactiveBubble;
        }
    }
    return OrdinaryCoverage::absent;
}

[[nodiscard]] std::span<const message::BubbleSubBlock> fill_sub_blocks(
    const layouts::Definition& layout,
    Scratch& scratch,
    const message::Roster& roster,
    std::size_t ordinaryBubbleStart,
    std::size_t selectedSlice,
    std::span<const std::uint32_t> authoredLocalKeys) noexcept {
    std::size_t published = 0;
    for (std::size_t bubble = 0; bubble < scratch.rosterSubBlocks.size(); ++bubble) {
        std::size_t keyCount = 0;
        for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
            if ((layout.bubbleGroupMasks[index] & (std::uint64_t{1} << bubble)) == 0) {
                continue;
            }
            scratch.rosterSubBlockKeys[published][keyCount] =
                roster.groups[ordinaryBubbleStart + index].key;
            ++keyCount;
        }
        if (bubble == selectedSlice) {
            for (const std::uint32_t key : authoredLocalKeys) {
                if (keyCount == scratch.rosterSubBlockKeys[published].size()) {
                    return {};
                }
                scratch.rosterSubBlockKeys[published][keyCount++] = key;
            }
        }
        if (keyCount == 0) {
            continue;
        }
        scratch.rosterSubBlocks[published].bubble = static_cast<std::uint32_t>(bubble);
        scratch.rosterSubBlocks[published].keys = std::span<const std::uint32_t>(
            scratch.rosterSubBlockKeys[published].data(), keyCount);
        scratch.rosterSubBlocks[published].presence = {};
        scratch.rosterSubBlocks[published].states = {};
        ++published;
    }
    return std::span(scratch.rosterSubBlocks).first(published);
}

[[nodiscard]] bool fill_roster(const layouts::Definition& layout,
                               Scratch& scratch,
                               message::Roster& roster,
                               std::int32_t region) noexcept {
    roster = {};
    const std::size_t ordinaryCount =
        std::size_t{layout.rosterGroupCount} + std::size_t{layout.bubbleGroupCount};
    if (layout.rosterGroupCount == 0 || ordinaryCount > scratch.rosterGroups.size()
        || ordinaryCount > roster.groups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (!load_group(layout.rosterGroups[index], scratch, index)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        if (!load_group(layout.bubbleGroups[index],
                        scratch,
                        std::size_t{layout.rosterGroupCount} + index)) {
            return false;
        }
    }
    for (std::size_t left = 0; left < ordinaryCount; ++left) {
        for (std::size_t right = left + 1; right < ordinaryCount; ++right) {
            if (scratch.rosterGroups[left].registryKey
                == scratch.rosterGroups[right].registryKey) {
                return false;
            }
        }
    }

    namespace tables = middleware::content::packages::tables;
    std::size_t selectedSlice = layouts::kBubbleCapacity;
    if (region >= 0 && region % static_cast<std::int32_t>(tables::kSliceSetIndexFactor) == 0) {
        const std::size_t ordinal =
            static_cast<std::size_t>(region) / tables::kSliceSetIndexFactor;
        if (ordinal < layouts::kBubbleCapacity) {
            selectedSlice = ordinal;
        }
    }
    std::uint16_t authoredRoot = kNoRosterGroup;
    std::array<std::uint16_t, layouts::kDestinationAuthoredGroupCapacity> authoredLocals{};
    std::size_t authoredLocalCount = 0;
    std::array<std::uint32_t, layouts::kDestinationAuthoredGroupCapacity> inactiveAuthoredKeys{};
    std::size_t inactiveAuthoredCount = 0;
    if (selectedSlice < layouts::kBubbleCapacity) {
        const std::size_t authoredCount = layout.authoredGroupCounts[selectedSlice];
        if (authoredCount != 0 && authoredCount <= layouts::kDestinationAuthoredGroupCapacity) {
            const std::uint16_t root = layout.authoredGroups[selectedSlice][0];
            std::size_t ordinarySlot = 0;
            const OrdinaryCoverage rootCoverage =
                ordinary_coverage(layout, root, selectedSlice, ordinarySlot);
            if (rootCoverage == OrdinaryCoverage::absent) {
                authoredRoot = root;
            } else if (rootCoverage == OrdinaryCoverage::inactiveBubble) {
                inactiveAuthoredKeys[inactiveAuthoredCount++] =
                    scratch.rosterGroups[ordinarySlot].registryKey;
            }
            for (std::size_t index = 1; index < authoredCount; ++index) {
                const std::uint16_t local = layout.authoredGroups[selectedSlice][index];
                const OrdinaryCoverage localCoverage =
                    ordinary_coverage(layout, local, selectedSlice, ordinarySlot);
                if (localCoverage == OrdinaryCoverage::absent) {
                    authoredLocals[authoredLocalCount++] = local;
                } else if (localCoverage == OrdinaryCoverage::inactiveBubble) {
                    inactiveAuthoredKeys[inactiveAuthoredCount++] =
                        scratch.rosterGroups[ordinarySlot].registryKey;
                }
            }
        }
    }
    const std::size_t authoredAdditionalCount =
        (authoredRoot != kNoRosterGroup ? 1U : 0U) + authoredLocalCount;
    bool publishAuthored = (authoredAdditionalCount != 0 || inactiveAuthoredCount != 0)
                           && ordinaryCount + authoredAdditionalCount
                                  <= layouts::kDestinationWireGroupCapacity;
    std::size_t loadedAuthored = 0;
    if (publishAuthored && authoredRoot != kNoRosterGroup) {
        publishAuthored = load_group(authoredRoot, scratch, ordinaryCount + loadedAuthored);
        loadedAuthored += publishAuthored ? 1U : 0U;
    }
    for (std::size_t index = 0; publishAuthored && index < authoredLocalCount; ++index) {
        publishAuthored =
            load_group(authoredLocals[index], scratch, ordinaryCount + loadedAuthored);
        loadedAuthored += publishAuthored ? 1U : 0U;
    }
    for (std::size_t authored = 0; publishAuthored && authored < loadedAuthored; ++authored) {
        const layouts::RosterGroup& candidate = scratch.rosterGroups[ordinaryCount + authored];
        for (std::size_t ordinary = 0; ordinary < ordinaryCount; ++ordinary) {
            if (candidate.registryKey == scratch.rosterGroups[ordinary].registryKey) {
                publishAuthored = false;
                break;
            }
        }
        for (std::size_t earlier = 0; publishAuthored && earlier < authored; ++earlier) {
            if (candidate.registryKey
                == scratch.rosterGroups[ordinaryCount + earlier].registryKey) {
                publishAuthored = false;
                break;
            }
        }
    }
    if (!publishAuthored) {
        authoredRoot = kNoRosterGroup;
        authoredLocalCount = 0;
        inactiveAuthoredCount = 0;
        loadedAuthored = 0;
    }

    std::size_t output = 0;
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        expose_group(scratch.rosterGroups[index], roster.groups[output++]);
    }
    const bool addsRoot = authoredRoot != kNoRosterGroup;
    if (addsRoot) {
        expose_group(scratch.rosterGroups[ordinaryCount], roster.groups[output++]);
    }
    const std::size_t ordinaryBubbleStart = output;
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        expose_group(scratch.rosterGroups[layout.rosterGroupCount + index],
                     roster.groups[output++]);
    }
    std::array<std::uint32_t, layouts::kDestinationAuthoredGroupCapacity> authoredLocalKeys{};
    std::copy_n(inactiveAuthoredKeys.begin(), inactiveAuthoredCount, authoredLocalKeys.begin());
    for (std::size_t index = 0; index < authoredLocalCount; ++index) {
        const std::size_t scratchIndex = ordinaryCount + (addsRoot ? 1U : 0U) + index;
        expose_group(scratch.rosterGroups[scratchIndex], roster.groups[output]);
        authoredLocalKeys[inactiveAuthoredCount + index] = roster.groups[output].key;
        ++output;
    }
    roster.topLevelGroupCount = std::size_t{layout.rosterGroupCount} + (addsRoot ? 1U : 0U);
    roster.groupCount = output;
    roster.bubbleSubBlocks = fill_sub_blocks(
        layout,
        scratch,
        roster,
        ordinaryBubbleStart,
        selectedSlice,
        std::span(authoredLocalKeys).first(inactiveAuthoredCount + authoredLocalCount));
    for (std::size_t index = 0; index < roster.topLevelGroupCount && roster.playerKeyGroup == 0;
         ++index) {
        const message::Group& group = roster.groups[index];
        for (const std::uint8_t slotType : group.slotTypes) {
            if (slotType == kSlotTypeParticipation) {
                roster.playerKeyGroup = group.key;
                break;
            }
        }
    }
    return roster.playerKeyGroup != 0;
}

/** @param roster Published groups. @return One value that changes when the group set changes. */
[[nodiscard]] std::uint32_t fold_groups(const message::Roster& roster) noexcept {
    std::uint32_t folded = kFoldBasis;
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        folded = (folded ^ roster.groups[index].key) * kFoldPrime;
    }
    return folded;
}

/** @return True when one published group owns the mission-director slot. */
[[nodiscard]] bool carries_mission_director(const message::Roster& roster) noexcept {
    for (std::size_t group = 0; group < roster.groupCount; ++group) {
        for (const std::uint8_t slotType : roster.groups[group].slotTypes) {
            if (slotType == kSlotTypeMissionDirector) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Picks the per-entry state byte and advances the connection's counters.
 * A burst send leaves the byte and the latched group set alone past the warm-up, so a group change
 * during a load is published by the next keepalive send instead.
 * @param session Connection-owned roster counters.
 * @param folded Current group set.
 * @param burst True for a send on the loading cadence.
 * @return The state byte to send.
 */
[[nodiscard]] std::uint8_t
next_state_sequence(Session& session, std::uint32_t folded, bool burst) noexcept {
    if (session.activity.rosterSends < kWarmupSends
        || (!burst && session.activity.rosterGroups != folded)) {
        session.activity.rosterState =
            static_cast<std::uint8_t>((session.activity.rosterState + 1) % kStateSequenceWrap);
        session.activity.rosterGroups = folded;
    }
    if (session.activity.rosterSends < kWarmupSends) {
        ++session.activity.rosterSends;
    }
    return session.activity.rosterState;
}

} // namespace

/** Purely resolves one copied destination/source pair. */
EffectiveRegion resolve_region(
    const state::activity::defaults::DefaultDestination& defaults,
    const state::activity::destination::DestinationSelection& selection,
    std::int32_t reportedRegion,
    bool allowArrival,
    std::string_view name,
    const layouts::Definition& layout) noexcept {
    EffectiveRegion region{};
    region.arrival = arrival_slice_set(defaults, selection, name, layout);
    region.reported = reportedRegion >= 0;
    if (region.reported) {
        region.index = reportedRegion;
        region.valid = true;
    } else if (allowArrival) {
        region.index = static_cast<std::int32_t>(region.arrival);
        region.valid = true;
    }
    return region;
}

/** Compatibility entry point: copy exact self inputs once, then use the pure builder. */
RosterOutcome build_roster_snapshot(Session& session,
                                    Scratch& scratch,
                                    message::Snapshot& snapshot,
                                    std::span<char> destination,
                                    std::size_t& destinationLength,
    bool burst) noexcept {
    state::activity::membership::RegionSnapshotInputs copied{};
    if (!region_lineage_is_current_locked(session, session.activity.lineage)
        || !state::activity::membership::snapshot_region_inputs(session.activity.lineage.bound,
                                                              session.activity.lineage.source,
                                                              {},
                                                              copied)) {
        return RosterOutcome::noLayout;
    }
    const std::string_view name(
        reinterpret_cast<const char*>(copied.destination.packageName.data()),
        copied.destination.packageNameLength);
    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return RosterOutcome::noLayout;
    }
    const EffectiveRegion region = resolve_region(copied.defaults.defaultDestination,
                                                  copied.destination,
                                                  copied.sourceMembership.region.index,
                                                  session.activity.lineage.kind
                                                          == RegionLineageKind::ownedActivity
                                                      && session.activity.lineage.bound
                                                             == session.activity.lineage.source,
                                                  name,
                                                  layout);
    if (!region.valid) {
        return RosterOutcome::noGroups;
    }
    const RosterSnapshotInputs inputs{
        copied.destination,
        copied.defaults,
        copied.sourceMembership,
        copied.grantBefore,
        region.index,
        region.arrival,
        copied.sourceDestination,
    };
    return build_roster_snapshot(
        session, scratch, inputs, snapshot, destination, destinationLength, burst);
}

/** Builds from one immutable copied region plan. */
RosterOutcome build_roster_snapshot(Session& session,
                                    Scratch& scratch,
                                    const RosterSnapshotInputs& inputs,
                                    message::Snapshot& snapshot,
                                    std::span<char> destination,
                                    std::size_t& destinationLength,
                                    bool burst) noexcept {
    snapshot = {};
    destinationLength = 0;
    const auto& defaults = inputs.defaults;
    const auto& selection = inputs.destination;
    layouts::Definition layout{};
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    // Persistence must remain bound to this authenticated connection's character. Resolve its
    // short join identity against one captured account snapshot; never re-read mutable selection
    // state from a delayed mission callback.
    const state::AccountState rosterAccount = state::account_snapshot();
    const std::uint64_t missionCharacterSoid =
        roster_player_key(rosterAccount, session.activity.characterSoid, false);
    const bool omegaDestination = name == "mission_scot";
    // Mission ownership survives ordinary z-legs (including 120 -> 128). Using the
    // current region here resets the controller and removes its roster on the return
    // trip. Bubble sub-blocks still scope native objects to their authored bubble.
    const bool gatewayDestination = name == "mission_abs"
        && !session.activity.joinedForeignSession;
    const bool gatewayPrepared = !session.activity.joinedForeignSession
        && state::activity::gateway::prepare(
        state::activity::mission_run_generation(), gatewayDestination);
    if(gatewayDestination && !gatewayPrepared) { return RosterOutcome::noGroups; }
    const bool trialDestination=name=="adventure_ginger" && !session.activity.joinedForeignSession;
    const bool trialPrepared=!session.activity.joinedForeignSession && state::activity::deadly_trial::prepare(state::activity::mission_run_generation(),trialDestination);
    if(trialDestination && !trialPrepared) { return RosterOutcome::noGroups; }
    const bool beyondDestination=name=="adventure_vod" && !session.activity.joinedForeignSession;
    const bool beyondPrepared=state::activity::beyond_infinity::prepare(state::activity::mission_run_generation(),beyondDestination);
    if(beyondDestination && !beyondPrepared) { return RosterOutcome::noGroups; }
    const bool deepDestination=name=="adventure_whisk" && !session.activity.joinedForeignSession;
    const bool deepPrepared=state::activity::deep_storage::prepare(state::activity::mission_run_generation(),deepDestination);
    if(deepDestination && !deepPrepared) { return RosterOutcome::noGroups; }
    const bool gardenDestination=(name=="strike_bond" || name=="mission_bond") && !session.activity.joinedForeignSession;
    const bool gardenPrepared=state::activity::strike_bond::prepare(state::activity::mission_run_generation(),gardenDestination,name=="mission_bond");
    if(gardenDestination && !gardenPrepared) return RosterOutcome::noGroups;
    const bool strikeDestination=(name=="strike_pact" || name=="mission_pact") && !session.activity.joinedForeignSession;
    const bool strikePrepared=!session.activity.joinedForeignSession && state::activity::strike_pact::prepare(state::activity::mission_run_generation(),strikeDestination,name=="mission_pact");
    if(strikeDestination && !strikePrepared) { return RosterOutcome::noGroups; }
    const bool launchpadDestination=name=="mission_launchpad" && !session.activity.joinedForeignSession;
    const bool launchpadPrepared=state::activity::newlight::launchpad::prepare(state::activity::mission_run_generation(),launchpadDestination);
    if(launchpadDestination && !launchpadPrepared) {return RosterOutcome::noGroups;}
    const bool hijackedDestination=name=="adventure_rumba" && !session.activity.joinedForeignSession;
    const bool hijackedPrepared=!session.activity.joinedForeignSession && state::activity::hijacked::prepare(state::activity::mission_run_generation(),hijackedDestination);
    if(hijackedDestination && !hijackedPrepared) { return RosterOutcome::noGroups; }
    const auto& omegaExperiments = core::settings::get().omegaExperiments;
    const bool syntheticOmega = omegaDestination;
    // Forest-D's native encounter classifier requires the selected race global
    // before loading its per-piece populations. Keep it stable across portal
    // quiescence; this does not request individual actors or override gate state.
    snapshot.omegaForestVexEncounters = syntheticOmega;
    if (!syntheticOmega && !session.activity.joinedForeignSession) {
        state::activity::omega_presentation::reset();
    }
    // Once the type-7 forest transition is issued, every Omega body goes silent so nothing
    // applies into components the slice teardown is freeing (index-heap double-free).
    const bool omegaQuiesced = state::activity::omega_authority_quiesced();
    snapshot.archiveOmega = name == "mission_scot";
    snapshot.omegaSceneAuthority = syntheticOmega && !omegaQuiesced;
    const bool cooOpening = syntheticOmega && !session.activity.joinedForeignSession
        && state::activity::coo::omega::select(state::activity::mission_run_generation(),
            omegaExperiments.cooExecutor);
    // Reconstructed Omega policy: the authenticated pm_weapondown monitor (30/20,
    // backed by authored tv_weapondown 60/29) advances Ikora's waiting orb animation.
    // This is distinct from pt_start_ikora_vignette 31/18 -> 60/28; the original
    // retail host join to C7ECAA77 is not recovered. Keep the accepted approach
    // latch for the whole run so backtracking cannot remove/reinsert the event.
    // A direct Forest/Lair launch has no accepted opening edge and leaves it absent.
    snapshot.omegaIkoraPortalRequested = snapshot.omegaSceneAuthority
        && session.activity.sensorObservation.omegaOpeningTriggered;
    // A received native scene output stays latched even when authority publication is delayed.
    // Initialization is the exact gate's closed state; snapshot construction never resets it.
    const auto lattice = session.activity.sensorObservation.omegaIkoraLattice.plan(
        session.activity.key.generation.value, snapshot.omegaSceneAuthority);
    snapshot.omegaIkoraLatticeReleased = lattice.active && lattice.position.revision == 2;
    // This hash is initial player data, independent of the later portal carrier activation.
    // The encoder preserves native participation ownership; do not resend a neutral player
    // record when the lattice opens merely to supply the previously missing predicate input.
    snapshot.omegaPortalPlayerHash = syntheticOmega && !omegaQuiesced;
    snapshot.omegaPortalEntry = snapshot.omegaSceneAuthority && snapshot.omegaIkoraLatticeReleased;
    if (cooOpening) {
        state::activity::coo::omega::opening::project(session.activity.sensorObservation.omegaOpeningExecutor, snapshot);
    }
    // Arm the one-shot Ghost line only once the client is IN WORLD (the same latch that gates
    // the authored seed). Run 6 proved the hazard: the record dispatched at t=60.7 during the
    // load screen, its 10 s eligibility deadline expired exactly at the t=70.7 fade-in, and
    // the consumed generation is never retried. Roster-ack is NOT an in-world signal (it
    // arrived 10 s early that run); mission_seed_armed() is. The scan re-runs on every apply,
    // so arming after component start is safe.
    snapshot.omegaDialogueArm =
        syntheticOmega && !omegaQuiesced && state::activity::mission_seed_armed();
    // The gate-hop moment: the entrance sense latched, the player stands in the shared tunnel.
    snapshot.omegaTunnelDialogue =
        snapshot.omegaDialogueArm
        && session.activity.sensorObservation.omegaForestEntranceTriggered;
    // The banner switches to the forest objective at the gate, like the tunnel Ghost line: a
    // walked z-leg reports region 88 client-side but the server membership never commits it
    // (advertised region stays 120), so region-gating never fired. Use the persistent entrance
    // sense instead, plus the region for a direct forest launch that skips the gate.
    snapshot.omegaForestBanner =
        snapshot.omegaDialogueArm
        && (session.activity.sensorObservation.omegaForestEntranceTriggered
            || (inputs.regionIndex >= 64 && inputs.regionIndex <= 112));
    if (snapshot.omegaDialogueArm && !session.activity.joinedForeignSession) {
        const auto mission = state::activity::coo::omega::update({
            state::activity::mission_run_generation(), GetTickCount64(), inputs.regionIndex,
            session.activity.sensorObservation.omegaForestEntranceTriggered, omegaExperiments.cooExecutor});
        state::activity::coo::omega::project(mission, snapshot);
    }
    // Activate the map generator once the player has ever entered the gate (persistent sense
    // latch), or by region for a direct forest launch. Region alone flaps during walked z-legs
    // (same failure the banner hit), and an unarmed window while the component constructs means
    // the body and the component never overlap; a record for a not-yet-created component is
    // skipped harmlessly, so arming wide is safe.
    snapshot.omegaForestGenerator =
        syntheticOmega
        && (session.activity.sensorObservation.omegaForestEntranceTriggered
            || (inputs.regionIndex >= 64 && inputs.regionIndex <= 104));
    snapshot.omegaForestSeed=state::activity::coo::native_generator::mission_seed(state::activity::mission_run_generation(),0,1);
    snapshot.omegaGateAuthority = syntheticOmega && omegaExperiments.gateAuthority;
    snapshot.omegaPortalMutation = syntheticOmega && omegaExperiments.portalMutation;
    // TEMP diagnostic: which arm flag drops at the gate (dialogue/directive applies stop there).
    // Throttled to a change-only line so it cannot flood.
    {
        static std::uint32_t s_lastArmState = 0xFFFFFFFFu;
        const std::uint32_t armState =
            (snapshot.omegaDialogueArm ? 1u : 0u)
            | (snapshot.omegaSceneAuthority ? 2u : 0u)
            | (state::activity::mission_seed_armed() ? 4u : 0u)
            | (omegaQuiesced ? 8u : 0u)
            | (session.activity.sensorObservation.omegaForestEntranceTriggered ? 16u : 0u)
            | (snapshot.omegaTunnelDialogue ? 32u : 0u)
            | (snapshot.omegaForestBanner ? 64u : 0u)
            | (static_cast<std::uint32_t>(inputs.regionIndex & 0x3FF) << 16);
        if (s_lastArmState != armState) {
            s_lastArmState = armState;
            std::array<char, 200> line{};
            const int written = std::snprintf(
                line.data(), line.size(),
                "ev=omega_arm dialogue=%u scene=%u seed=%u quiesced=%u entrance=%u tunnel=%u "
                "banner=%u region=%d stage=%u",
                snapshot.omegaDialogueArm ? 1u : 0u, snapshot.omegaSceneAuthority ? 1u : 0u,
                state::activity::mission_seed_armed() ? 1u : 0u, omegaQuiesced ? 1u : 0u,
                session.activity.sensorObservation.omegaForestEntranceTriggered ? 1u : 0u,
                snapshot.omegaTunnelDialogue ? 1u : 0u, snapshot.omegaForestBanner ? 1u : 0u,
                inputs.regionIndex, static_cast<unsigned>(session.activity.omegaOpeningStage));
            if (written > 0) {
                core::log::write(core::log::Channel::server, core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    destinationLength = (std::min)(name.size(), destination.size());
    std::copy_n(name.begin(), destinationLength, destination.begin());
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return RosterOutcome::noLayout;
    }
    if(beyondPrepared && !beyond_infinity_roster::prepare_layout(layout,
        [](std::uint32_t key,std::uint32_t tag,std::uint16_t& index) noexcept { return layouts::find_group_index(key,tag,index); },
        [](std::size_t index,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group(index,group); })) { return RosterOutcome::noGroups; }
    if(deepPrepared && !deep_storage_roster::prepare_layout(layout,
        [](std::uint32_t key,std::uint32_t tag,std::uint16_t& index) noexcept { return layouts::find_group_index(key,tag,index); },
        [](std::size_t index,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group(index,group); })) { return RosterOutcome::noGroups; }
    if(hijackedPrepared && !hijacked_roster::prepare_layout(layout,
        [](std::uint32_t key,std::uint16_t& index) noexcept { return layouts::find_group_index(key,index); },
        [](std::size_t index,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group(index,group); })) { return RosterOutcome::noGroups; }
    if(trialPrepared && !deadly_trial_roster::prepare_layout(layout,
        [](std::uint32_t key,std::uint32_t tag,std::uint16_t& index) noexcept { return layouts::find_group_index(key,tag,index); },
        [](std::size_t index,layouts::RosterGroup& group) noexcept {
            return state::build_data::find_roster_group(index,group);
        })) { return RosterOutcome::noGroups; }
    // Publish the map generator group for the WHOLE activity, not per-region: a group set that
    // changes at the tunnel crossing advances the roster state sequence, which destroys and
    // recreates every authored object mid-run (killing the dialogue/directive components and the
    // objective banner). The readiness check accepts the extra acknowledgement entry, and the
    // bubble-11 mask keeps the generator's objects out of the lighthouse sub-block.
    if (name == "mission_scot") {
        state::build_data::amend_omega_forest_generator(layout);
    }
    // Free-roam scenarios overflow the fixed intersection capacities, so their extracted rows
    // hold zero groups; a zero-group row otherwise vetoes the whole periodic bundle and the
    // client starves into the activity-host timeout. Publish the global participation group
    // alone, which is part of every activity launch and safe in every slice set.
    if (layout.rosterGroupCount == 0) {
        const bool amended = state::build_data::amend_participation_fallback(layout);
        static std::atomic<std::uint32_t> s_lastFallback{0xFFFFFFFFU};
        const std::uint32_t observed = (layout.tag << 1) | (amended ? 1U : 0U);
        if (s_lastFallback.exchange(observed) != observed) {
            std::array<char, 160> line{};
            const int written = std::snprintf(
                line.data(), line.size(),
                "ev=activity stage=roster_fallback dest=%.*s tag=0x%X amended=%u",
                static_cast<int>(destinationLength), destination.data(), layout.tag,
                amended ? 1U : 0U);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server,
                                 amended ? core::log::Level::info : core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    if (!filter_event_layout(name, layout)) {
        return RosterOutcome::noGroups;
    }
    if (inputs.regionIndex < 0
        || !fill_roster(layout, scratch, snapshot.roster,
            omega_lair::roster_region(inputs.regionIndex,snapshot.omegaEndingRetire))) {
        return RosterOutcome::noGroups;
    }
    namespace vendorWorld=state::activity::vendors::presentation;
    const bool towerVendors=!vendorWorld::groups(layout.tag).empty();
    const auto ownsVendor=[scenario=layout.tag](std::uint32_t key) noexcept {return vendorWorld::owns(key,scenario);};
    if(!session.activity.joinedForeignSession) {
        state::activity::newlight::launchpad::quest::selected(
            state::activity::mission_run_generation(),selection.activityIndex,layout.tag);
    }
    if(towerVendors) {
        const auto role=native_publisher::prepare(session.activity.instance,session.activity.lineage,
            session.activity.rosterLifetimes,scratch,snapshot.roster,
            ownsVendor);
        if(role==native_publisher::Role::invalid) {return RosterOutcome::noGroups;}
        if(role==native_publisher::Role::creator) {
            if(!tower_spawn_recovery::retain_global_bodies(scratch,snapshot.roster,
                session.activity.rosterLifetimes,session.activity.instance,layout.tag,
                [](std::uint32_t key,layouts::RosterGroup& group) noexcept {
                    std::uint16_t index{};
                    return layouts::find_group_index(key,index) && state::build_data::find_roster_group(index,group);
                })) {return RosterOutcome::noGroups;}
            if(!vendor_roster::admit(scratch,snapshot.roster,layout.tag,
                [](std::uint32_t key,layouts::RosterGroup& group) noexcept {
                    std::uint16_t index{};
                    return layouts::find_group_index(key,index) && state::build_data::find_roster_group(index,group);
                })) {return RosterOutcome::noGroups;}
            namespace welcome=state::activity::vendors::presentation;
            const auto player=client::player::position::snapshot();
            const auto bubble=static_cast<std::uint8_t>(inputs.regionIndex/8);
            session.activity.towerVendorPresence=welcome::presence(session.activity.towerVendorPresence,bubble,
                player.position,player.present,layout.tag);
            snapshot.vendorPresentation={true,bubble,session.activity.towerVendorPresence,layout.tag};
            for(const auto& group:welcome::groups(layout.tag)) for(const auto& binding:group.slots) {
                const auto& source=binding.asset;
                if(source.type!=1) continue;
                const auto index=welcome::source_index(layout.tag,source.registry,1,source.slot);
                if(index>=snapshot.vendorPresentation.population.size()) continue;
                state::activity::vendors::lifetime::Authority authority;
                if(!state::activity::vendors::lifetime::prepare(session.activity.instance,source,group.bubble,authority))
                    return RosterOutcome::noGroups;
                snapshot.vendorPresentation.population[index]={authority.generation,authority.occupied,authority.suspended};
            }
            const auto now=GetTickCount64();
            if(!session.activity.vendorClockOrigin) {session.activity.vendorClockOrigin=now;}
            snapshot.gameplayClockTicks=state::activity::coo::native_activity_ticks(now-session.activity.vendorClockOrigin);
        }
    }
    if(name==state::activity::newlight::launchpad::kPackage
        && layout.tag==state::activity::newlight::launchpad::kScenario
        && native_publisher::prepare(session.activity.instance,session.activity.lineage,
            session.activity.rosterLifetimes,scratch,snapshot.roster,launchpad_roster::owns_key)
            ==native_publisher::Role::invalid) {return RosterOutcome::noGroups;}
    if (name == "mission_scot") {
        const auto admission = omega_lair::admit(layout, scratch, snapshot.roster,
            [](std::size_t index, layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group(index, group);
            });
        static std::atomic_int lastAdmission{-1};
        if (lastAdmission.exchange(static_cast<int>(admission)) != static_cast<int>(admission)) {
            const bool admitted = admission == omega_lair::Admission::added
                                  || admission == omega_lair::Admission::present;
            std::array<char, 160> line{};
            const int written = std::snprintf(line.data(), line.size(),
                "ev=omega_intro stage=roster admitted=%u result=%u registry=F4D0E0B2 bubble=14 groups=%zu",
                admitted ? 1U : 0U, static_cast<unsigned>(admission), snapshot.roster.groupCount);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server,
                    admitted ? core::log::Level::info : core::log::Level::warn,
                    {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    if (name == "mission_scot") {
        const auto admission = omega_lair::admit(layout, scratch, snapshot.roster,
            [](std::size_t index, layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group(index, group);
            }, true);
        static std::atomic_int lastBossAdmission{-1};
        if (lastBossAdmission.exchange(static_cast<int>(admission)) != static_cast<int>(admission)) {
            std::array<char, 160> line{};
            const int written = std::snprintf(line.data(), line.size(),
                "ev=omega_boss stage=roster result=%u registry=95FB2E01 bubble=14 groups=%zu",
                static_cast<unsigned>(admission), snapshot.roster.groupCount);
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server, core::log::Level::info,
                    {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    if (name == "mission_scot") {
        const auto admission = omega_lair::admit_crown(layout, scratch, snapshot.roster,
            [](std::size_t index, layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group(index, group);
            });
        static std::atomic_int lastCrownAdmission{-1};
        if (lastCrownAdmission.exchange(static_cast<int>(admission)) != static_cast<int>(admission)) {
            std::array<char,160> line{};
            const int written = std::snprintf(line.data(),line.size(),
                "ev=omega_crown stage=roster result=%u registry=0040BF06 bubble=14 groups=%zu",
                static_cast<unsigned>(admission),snapshot.roster.groupCount);
            if (written>0 && static_cast<std::size_t>(written)<line.size()) {
                core::log::write(core::log::Channel::server,core::log::Level::info,
                    {line.data(),static_cast<std::size_t>(written)});
            }
        }
    }
    if(name=="mission_scot") {
        for(const auto& group:state::activity::omega_lair_full_roster::kCombatGroups) {
            const auto admission=omega_lair::admit_full(layout,scratch,snapshot.roster,
                [](std::size_t index,layouts::RosterGroup& row) noexcept {
                    return state::build_data::find_roster_group(index,row);
                },group);
            if(admission!=omega_lair::Admission::added && admission!=omega_lair::Admission::present) {
                return RosterOutcome::noGroups;
            }
        }
    }
    if(gatewayPrepared) {
        std::uint32_t failedKey{};
        const bool admitted=gateway_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group_by_key(key,tag,group);
            },&failedKey);
        const auto gatewayRun=state::activity::mission_run_generation();
        static std::atomic_uint64_t lastGatewayAdmission{UINT64_MAX};
        const auto stamp=(gatewayRun<<1)|(admitted?1ULL:0ULL);
        if(lastGatewayAdmission.exchange(stamp)!=stamp) {
            std::array<char,256> line{};
            std::snprintf(line.data(),line.size(),"ev=gateway stage=roster result=%s run=%llu groups=%zu failed_registry=%08X scope=lighthouse_15_state_0",
                admitted?"admitted":"failed",static_cast<unsigned long long>(gatewayRun),snapshot.roster.groupCount,failedKey);
            core::log::write(core::log::Channel::server,admitted?core::log::Level::info:core::log::Level::error,line.data());
        }
        if(!admitted) { return RosterOutcome::noGroups; }
        snapshot.gateway=state::activity::gateway::snapshot(state::activity::mission_run_generation(),
            GetTickCount64(),state::activity::mission_seed_armed());
        if(snapshot.gateway.enabled) {
            if (!state::activity::progress::observe(missionCharacterSoid, name,
                    selection.activityIndex, 0, -1, snapshot.gateway.section,
                    snapshot.gateway.finished)) return RosterOutcome::noGroups;
            snapshot.missionCompletion=snapshot.gateway.completion;
        }
    }
    if(trialPrepared) {
        std::uint32_t failedKey{};
        const bool admitted=deadly_trial_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group_by_key(key,tag,group);
            },&failedKey);
        const auto deadly_trialRun=state::activity::mission_run_generation();
        static std::atomic_uint64_t lastDeadlyTrialAdmission{UINT64_MAX};
        const auto stamp=(deadly_trialRun<<1)|(admitted?1ULL:0ULL);
        if(lastDeadlyTrialAdmission.exchange(stamp)!=stamp) {
            std::array<char,256> line{};
            std::snprintf(line.data(),line.size(),"ev=deadly_trial stage=roster result=%s run=%llu groups=%zu failed_registry=%08X scope=town_51_alleys_0_1",
                admitted?"admitted":"failed",static_cast<unsigned long long>(deadly_trialRun),snapshot.roster.groupCount,failedKey);
            core::log::write(core::log::Channel::server,admitted?core::log::Level::info:core::log::Level::error,line.data());
        }
        if(!admitted) { return RosterOutcome::noGroups; }
        snapshot.deadly_trial=state::activity::deadly_trial::snapshot(state::activity::mission_run_generation(),
            GetTickCount64(),state::activity::mission_seed_armed());
        if(snapshot.deadly_trial.enabled) {
            if (!state::activity::progress::observe(missionCharacterSoid, name,
                    selection.activityIndex, 0, -1, snapshot.deadly_trial.section,
                    snapshot.deadly_trial.finished)) return RosterOutcome::noGroups;
            snapshot.missionCompletion=snapshot.deadly_trial.completion;
        }
    }
    if(beyondPrepared) {
        std::uint32_t failedKey{};
        const bool admitted=beyond_infinity_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group_by_key(key,tag,group); },&failedKey);
        if(!admitted) {
            std::array<char,160> line{};std::snprintf(line.data(),line.size(),"ev=beyond_infinity stage=roster result=failed registry=%08X",failedKey);
            core::log::write(core::log::Channel::server,core::log::Level::error,line.data());return RosterOutcome::noGroups;
        }
        snapshot.beyond_infinity=state::activity::beyond_infinity::snapshot(state::activity::mission_run_generation(),GetTickCount64(),state::activity::mission_seed_armed());
        if(snapshot.beyond_infinity.enabled) {
            if (!state::activity::progress::observe(missionCharacterSoid, name,
                    selection.activityIndex, 0, -1, snapshot.beyond_infinity.section,
                    snapshot.beyond_infinity.finished)) return RosterOutcome::noGroups;
            snapshot.missionCompletion=snapshot.beyond_infinity.completion;
            snapshot.gameplayClockTicks=snapshot.beyond_infinity.gameplayClockTicks;
        }
    }
    if(deepPrepared) {
        std::uint32_t failedKey{};
        const bool admitted=deep_storage_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group_by_key(key,tag,group); },&failedKey);
        if(!admitted) {
            std::array<char,160> line{};std::snprintf(line.data(),line.size(),"ev=deep_storage stage=roster result=failed registry=%08X",failedKey);
            core::log::write(core::log::Channel::server,core::log::Level::error,line.data());return RosterOutcome::noGroups;
        }
        snapshot.deep_storage=state::activity::deep_storage::snapshot(state::activity::mission_run_generation(),GetTickCount64(),state::activity::mission_seed_armed());
        if(snapshot.deep_storage.enabled) {
            if (!state::activity::progress::observe(missionCharacterSoid, name,
                    selection.activityIndex, 0, -1, snapshot.deep_storage.section,
                    snapshot.deep_storage.finished)) return RosterOutcome::noGroups;
            snapshot.missionCompletion=snapshot.deep_storage.completion;
            snapshot.gameplayClockTicks=snapshot.deep_storage.gameplayClockTicks;
        }
    }
    if(gardenPrepared) {
        strike_bond_roster::Report report{};
        if(!strike_bond_roster::admit(layout,scratch,snapshot.roster,inputs.regionIndex,
            [](std::size_t i,layouts::RosterGroup& g) noexcept {return state::build_data::find_roster_group(i,g);},report)) {
            // Admission is all-or-nothing across twelve refusal points, and this call site used to
            // return with no line at all: the log proved the stage failed but never which authored
            // assumption was wrong. Name the refusal and dump what the layout actually resolved to.
            // Diagnostic only - the refusal itself is unchanged.
            static std::atomic_uint64_t lastGardenRoster{UINT64_MAX};
            std::uint64_t stamp=static_cast<std::uint64_t>(report.detail)<<24;
            for(const char c:report.stage) { stamp=stamp*131U+static_cast<unsigned char>(c); }
            stamp^=static_cast<std::uint64_t>(report.lastMissing)<<32;
            if(lastGardenRoster.exchange(stamp)!=stamp) {
                std::array<char,512> line{};
                int used=std::snprintf(line.data(),line.size(),
                    "ev=strike_bond stage=roster result=failed refusal=%.*s detail=%08X detail2=%u "
                    "walk=%u resolved=%zu roots=%zu present=%u missing=%u full=%u "
                    "layout_roster=%zu layout_bubble=%zu layout_authored=%zu bubbles=%zu wire_capacity=%zu",
                    static_cast<int>(report.stage.size()),report.stage.data(),report.detail,report.detail2,
                    report.lastMissing<=4?static_cast<unsigned>(report.lastMissing):0U,
                    report.resolved,report.roots,report.present,report.missing,report.full,
                    report.layoutRosterGroups,report.layoutBubbleGroups,report.layoutAuthoredGroups,
                    report.layoutBubbles,middleware::bap::activity_message::sensor_auth_update::kGroupCapacity);
                if(used>0 && static_cast<std::size_t>(used)<line.size()) {
                    core::log::write(core::log::Channel::server,core::log::Level::error,
                        {line.data(),static_cast<std::size_t>(used)});
                }
                if(report.rowKey) {
                    std::array<char,384> row{};
                    const int n=std::snprintf(row.data(),row.size(),
                        "ev=strike_bond stage=roster_row key=%08X expected_tag=%08X actual_tag=%08X "
                        "key_found=%u tag_matched=%u slots_valid=%u expected_bubble=%u expected_root=%u "
                        "actual_root=%u actual_owners=%016llX",
                        report.rowKey,report.rowExpectedTag,report.rowActualTag,
                        report.rowKeyFound?1U:0U,report.rowTagMatched?1U:0U,report.rowSlotsValid?1U:0U,
                        static_cast<unsigned>(report.rowExpectedBubble),report.rowExpectedRoot?1U:0U,
                        report.rowRoot?1U:0U,static_cast<unsigned long long>(report.rowOwners));
                    if(n>0 && static_cast<std::size_t>(n)<row.size()) {
                        core::log::write(core::log::Channel::server,core::log::Level::error,
                            {row.data(),static_cast<std::size_t>(n)});
                    }
                }
                for(std::size_t base=0;base<report.bubbleStates;base+=5) {
                    std::array<char,512> bl{};
                    int at=std::snprintf(bl.data(),bl.size(),"ev=strike_bond stage=roster_bubbles first=%zu of=%zu rows=",base,report.bubbleStates);
                    for(std::size_t i=base;i<report.bubbleStates && i<base+5 && at>0
                        && static_cast<std::size_t>(at)<bl.size()-72;++i) {
                        const auto& b=report.bubbles[i];
                        at+=std::snprintf(bl.data()+at,bl.size()-static_cast<std::size_t>(at),
                            "b%u:state%u/of%u/authored%u/hash%08X ",static_cast<unsigned>(b.bubble),
                            static_cast<unsigned>(b.state),static_cast<unsigned>(b.stateCount),
                            static_cast<unsigned>(b.authored),b.hash);
                    }
                    if(at>0 && static_cast<std::size_t>(at)<bl.size()) {
                        core::log::write(core::log::Channel::server,core::log::Level::error,
                            {bl.data(),static_cast<std::size_t>(at)});
                    }
                }
                for(std::size_t base=0;base<report.observedCount;base+=4) {
                    std::array<char,512> dump{};
                    int at=std::snprintf(dump.data(),dump.size(),
                        "ev=strike_bond stage=roster_observed first=%zu of=%zu rows=",base,report.observedCount);
                    for(std::size_t i=base;i<report.observedCount && i<base+4 && at>0
                        && static_cast<std::size_t>(at)<dump.size()-96;++i) {
                        const auto& o=report.observed[i];
                        at+=std::snprintf(dump.data()+at,dump.size()-static_cast<std::size_t>(at),
                            "[%zu]%08X/tag%08X/slots%u/%s/own%016llX ",i,o.key,o.tag,
                            static_cast<unsigned>(o.slots),o.root?"root":"bub",
                            static_cast<unsigned long long>(o.owners));
                    }
                    if(at>0 && static_cast<std::size_t>(at)<dump.size()) {
                        core::log::write(core::log::Channel::server,core::log::Level::error,
                            {dump.data(),static_cast<std::size_t>(at)});
                    }
                }
            }
            return RosterOutcome::noGroups;
        }
        snapshot.strike_bond=state::activity::strike_bond::snapshot(state::activity::mission_run_generation(),GetTickCount64(),state::activity::mission_seed_armed(),
            inputs.sourceMembership.currentRegion.index>=0?inputs.sourceMembership.currentRegion.index:inputs.regionIndex);
        // Freeze the exact launch identity into this authority snapshot. Body encoding never reads
        // mutable UI or global difficulty state.
        snapshot.strike_bond.enemyVariant = selection.activityIndex==813 ? 5U : 0U;
        if(snapshot.strike_bond.enabled) {
            const auto* rewardVariant=state::activity::strikes::find(selection.activityIndex);
            const bool durableReward=rewardVariant
                && rewardVariant->difficulty!=state::activity::strikes::Difficulty::standard;
            if (!state::activity::progress::observe(
                    missionCharacterSoid, name, selection.activityIndex,
                    snapshot.strike_bond.checkpointSpawnSet,
                    snapshot.strike_bond.checkpointSliceSet,
                    snapshot.strike_bond.section,
                    snapshot.strike_bond.finished && !durableReward)) {
                return RosterOutcome::noGroups;
            }
            snapshot.missionCompletion=snapshot.strike_bond.completion;
            snapshot.gameplayClockTicks=snapshot.strike_bond.gameplayClockTicks;
        }
    }
    if(strikePrepared) {
        // Keep each authored group's native bubble ownership stable across the whole run.
        strike_pact_roster::Report strikeReport{};
        if(!strike_pact_roster::admit(layout,scratch,snapshot.roster,inputs.regionIndex,
            [](std::size_t index,layouts::RosterGroup& group) noexcept {
                return state::build_data::find_roster_group(index,group);
            },strikeReport)) {
            std::array<char,256> line{};
            std::snprintf(line.data(),line.size(),"ev=strike_pact stage=roster_rejected mission=%.*s missing=%u last_key=%08X present=%u full=%u",
                static_cast<int>(name.size()),name.data(),strikeReport.missing,strikeReport.lastMissing,strikeReport.present,strikeReport.full);
            core::log::write(core::log::Channel::server,core::log::Level::warn,line.data());
            return RosterOutcome::noGroups;
        }
        const auto strikeRun=state::activity::mission_run_generation();
        static std::atomic_uint64_t lastStrikeRoster{UINT64_MAX};
        const auto stamp=(strikeRun<<20)^(static_cast<std::uint64_t>(inputs.regionIndex)<<8)
            ^strikeReport.added^(static_cast<std::uint64_t>(strikeReport.missing)<<4);
        if(lastStrikeRoster.exchange(stamp)!=stamp) {
            std::array<char,640> line{};
            int used=std::snprintf(line.data(),line.size(),
                "ev=strike_pact stage=roster run=%llu region=%d bubble=%d added=%u present=%u missing=%u full=%u last_missing=%08X groups=%zu keys=",
                static_cast<unsigned long long>(strikeRun),inputs.regionIndex,inputs.regionIndex/8,
                strikeReport.added,strikeReport.present,strikeReport.missing,strikeReport.full,
                strikeReport.lastMissing,snapshot.roster.groupCount);
            for(std::size_t i=0;i<snapshot.roster.groupCount && used>0 && static_cast<std::size_t>(used)<line.size()-12;++i) {
                used+=std::snprintf(line.data()+used,line.size()-static_cast<std::size_t>(used),"%08X,",snapshot.roster.groups[i].key);
            }
            core::log::write(core::log::Channel::server,
                strikeReport.missing?core::log::Level::warn:core::log::Level::info,line.data());
        }
        snapshot.strike_pact=state::activity::strike_pact::snapshot(strikeRun,GetTickCount64(),
            state::activity::mission_seed_armed(),inputs.sourceMembership.currentRegion.index>=0
                ?inputs.sourceMembership.currentRegion.index:inputs.regionIndex);
        snapshot.strike_pact.enemyVariant = selection.activityIndex==835 ? 5U : 0U;
        if(snapshot.strike_pact.enabled) {
            const auto* rewardVariant=state::activity::strikes::find(selection.activityIndex);
            const bool durableReward=rewardVariant
                && rewardVariant->difficulty!=state::activity::strikes::Difficulty::standard;
            if (!state::activity::progress::observe(
                    missionCharacterSoid, name, selection.activityIndex,
                    snapshot.strike_pact.checkpointSpawnSet,
                    static_cast<std::int32_t>(snapshot.strike_pact.checkpointSliceSet),
                    snapshot.strike_pact.section,
                    snapshot.strike_pact.finished && !durableReward)) {
                return RosterOutcome::noGroups;
            }
            snapshot.missionCompletion=snapshot.strike_pact.completion;
            snapshot.gameplayClockTicks=snapshot.strike_pact.activityTime;
        }
    }
    if(!session.activity.joinedForeignSession) {
        namespace tower=state::activity::newlight::launchpad::tower;
        tower::selected(state::activity::mission_run_generation(),selection.activityIndex,layout.tag,GetTickCount64());
        snapshot.launchpadTower=tower::frame(state::activity::mission_run_generation());
        if(snapshot.launchpadTower.enabled && !launchpad_roster::approach(scratch,snapshot.roster)) {return RosterOutcome::noGroups;}
        namespace intro=state::activity::gateway_intro;
        intro::selected(state::activity::mission_run_generation(),selection.activityIndex,layout.tag,GetTickCount64());
        snapshot.gatewayIntro=intro::frame(state::activity::mission_run_generation());
        if(snapshot.gatewayIntro.enabled && !launchpad_roster::gateway(scratch,snapshot.roster)) {return RosterOutcome::noGroups;}
    }
    if(launchpadPrepared) {
        std::uint32_t failedKey{};
        if(!launchpad_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& group) noexcept {return state::build_data::find_roster_group_by_key(key,tag,group);},failedKey)) {
            std::array<char,128> line{};std::snprintf(line.data(),line.size(),"ev=launchpad stage=roster result=failed registry=%08X",failedKey);
            core::log::write(core::log::Channel::server,core::log::Level::error,line.data());return RosterOutcome::noGroups;
        }
        snapshot.launchpad=state::activity::newlight::launchpad::snapshot(state::activity::mission_run_generation(),GetTickCount64(),state::activity::mission_seed_armed());
        snapshot.missionCompletion=snapshot.launchpad.completion;snapshot.gameplayClockTicks=snapshot.launchpad.gameplayClockTicks;
        launchpad_roster::movies(scratch,snapshot.roster,snapshot.launchpad.cinematic);
    }
    if(hijackedPrepared) {
        std::uint32_t failedKey{};
        const bool admitted=hijacked_roster::admit(layout,scratch,snapshot.roster,
            [](std::uint32_t key,layouts::RosterGroup& group) noexcept { return state::build_data::find_roster_group_by_key(key,group); },&failedKey);
        if(!admitted) {
            std::array<char,160> line{};std::snprintf(line.data(),line.size(),"ev=hijacked stage=roster result=failed registry=%08X",failedKey);
            core::log::write(core::log::Channel::server,core::log::Level::error,line.data());return RosterOutcome::noGroups;
        }
        snapshot.hijacked=state::activity::hijacked::snapshot(state::activity::mission_run_generation(),GetTickCount64(),state::activity::mission_seed_armed());
        if(snapshot.hijacked.enabled) {
            if (!state::activity::progress::observe(missionCharacterSoid, name,
                    selection.activityIndex, 0, -1, snapshot.hijacked.section,
                    snapshot.hijacked.finished)) return RosterOutcome::noGroups;
            snapshot.missionCompletion=snapshot.hijacked.completion;
            snapshot.gameplayClockTicks=snapshot.hijacked.gameplayClockTicks;
        }
    }
    const auto* nativeProfile =
        (name == "city_tower_social_d2"
         && state::activity::events::withheld(0x7C6DE64FU))
            ? nullptr
            : server::runtime::activity::native_activity_profile(name,selection.activityIndex);
    bool nativeTraversalRespawn{};
    native_publisher::Role nativePublisher{native_publisher::Role::invalid};
    if(nativeProfile) {
        nativePublisher=native_publisher::prepare(*nativeProfile,session.activity.instance,
            session.activity.lineage,session.activity.rosterLifetimes,scratch,snapshot.roster);
        if(nativePublisher==native_publisher::Role::invalid)return RosterOutcome::noGroups;
    }
    if(nativePublisher==native_publisher::Role::creator) {
        namespace admission=server::runtime::activity::registry;
        const auto admitProfileRegistry=[&](const auto& definition) noexcept {
            const auto result=admission::admit(layout,scratch,snapshot.roster,definition,
                [](std::uint32_t key,layouts::RosterGroup& row) noexcept {
                    return state::build_data::find_roster_group_by_key(key,row);
                });
            // Descriptor admission alone does not activate these sources. Native
            // authority is supplied separately by the owning activity service.
            // Never partially publish this set or fall back to invented slots.
            if(result!=admission::Admission::added && result!=admission::Admission::present) {
                static std::atomic<std::uint64_t> lastFailure{UINT64_MAX};
                const auto failure=(static_cast<std::uint64_t>(definition.key)<<8)|static_cast<unsigned>(result);
                if(lastFailure.exchange(failure)!=failure) {
                    std::array<char,192> line{};
                    const auto written=std::snprintf(line.data(),line.size(),
                        "ev=activity_registry activity_profile_registry key=%08X result=%u phase=admission",
                        definition.key,static_cast<unsigned>(result));
                    if(written>0 && static_cast<std::size_t>(written)<line.size()) {
                        core::log::write(core::log::Channel::server,core::log::Level::error,
                            {line.data(),static_cast<std::size_t>(written)});
                    }
                }
                return false;
            }
            return true;
        };
        for(const auto& definition:nativeProfile->registries) {
            if(!admitProfileRegistry(definition))return RosterOutcome::noGroups;
        }
        for(const auto& definition:nativeProfile->lostSectorRegistries) {
            if(!admitProfileRegistry(definition))return RosterOutcome::noGroups;
        }
        // A completion chest has no population authority and is useful only in
        // its authored region. Admit the exact current/prefetched bubble instead
        // of retaining every sector chest in the destination roster.
        const auto rewardRegion=native_publisher::runtime_region(
            inputs.regionIndex,inputs.sourceMembership.currentRegion.index);
        const auto rewardBubble=native_publisher::population_prefetch_bubble(
            rewardRegion);
        for(const auto& definition:nativeProfile->lostSectorRewardRegistries) {
            if(definition.bubble==rewardBubble && !admitProfileRegistry(definition))
                return RosterOutcome::noGroups;
        }
        server::runtime::activity::ambient_population::RegistryBatch optional{};
        if(!server::runtime::activity::native_activity::optional_registries(*nativeProfile,optional))
            return RosterOutcome::noGroups;
        // The update below binds native creation observation before this roster
        // can be published. An unscoped snapshot cannot publish the dependency.
        if(inputs.regionIndex>=0 && inputs.regionIndex%8==0) {
            for(std::size_t i=0;i<optional.count;++i)
                if(!admitProfileRegistry(*optional.entries[i]))return RosterOutcome::noGroups;
        }
    }
    bool adventureAdditive{};
    const auto nativeRuntimeRegion=native_publisher::runtime_region(
        inputs.regionIndex,inputs.sourceMembership.currentRegion.index);
    // The publication can announce a streamed-in neighbor before currentRegion
    // changes. It can prewarm incoming sources, but it never replaces the held
    // location or proves an exit, reset, or completed Lost Sector wave.
    const auto populationPrefetchBubble=native_publisher::population_prefetch_bubble(inputs.regionIndex);
    if(nativePublisher==native_publisher::Role::creator
        && nativeRuntimeRegion>=0 && nativeRuntimeRegion%8==0) {
        namespace overlay=server::runtime::activity::authored_overlay;
        server::runtime::activity::adventure_start::wire::Request selected{};
        // Only the creator publishes its persistent policy. Derived activities
        // retain their generic roster without a second copy of these sources.
        const auto& committed=inputs.sourceDestination;
        if(committed.descriptorBitLength && committed.descriptorBitLength<=committed.descriptorBits.size()*8) {
            const auto bytes=(static_cast<std::size_t>(committed.descriptorBitLength)+7)/8;
            if(!server::runtime::activity::adventure_start::wire::parse(std::span(committed.descriptorBits).first(bytes),selected)
                || selected.selection.descriptorBitLength!=committed.descriptorBitLength)selected={};
        }
        const auto frame=server::runtime::activity::native_activity::update(
            session.activity.lineage.source,static_cast<std::uint32_t>(nativeRuntimeRegion/8),
            state::activity::world_phase()==state::activity::WorldPhase::arrived,*nativeProfile,selected,
            session.activity.rosterSends>=kWarmupSends,populationPrefetchBubble,omegaExperiments.forestRewardCoffers);
        snapshot.populations=frame.populations;snapshot.placements=frame.placements;
        nativeTraversalRespawn=frame.nativeTraversalRespawn;
        snapshot.animations=frame.animations;
        snapshot.generators=frame.generators;
        snapshot.devices=frame.devices;
        snapshot.lostSectorShields=frame.lostSectorShields;
        snapshot.engagements=frame.engagements;
        snapshot.cues=frame.cues;
        snapshot.dialogues=frame.dialogues;
        snapshot.sequences=frame.sequences;
        snapshot.eventParticipants=frame.eventParticipants;
        snapshot.music=frame.music;
    snapshot.statusEffects=frame.statusEffects;
    snapshot.playerTriggers=frame.playerTriggers;
        snapshot.nativeForestSwitches=frame.nativeForestSwitches;
        if(frame.clock) {
            snapshot.activityClock=frame.clock.configuration;
            snapshot.activityElapsedTicks=frame.clock.elapsedTicks;
        }
        if (nativeProfile->rounds != nullptr) {
            layouts::RosterGroup resolvedTimerGroup{};
            const auto& roundDefinition = *nativeProfile->rounds;
            const bool resolved = !nativeProfile->registries.empty()
                && state::build_data::find_roster_group_by_key(
                    roundDefinition.completionTimerAsset.registry, resolvedTimerGroup);
            const auto projected = server::runtime::activity::native_round_projection::project(
                *nativeProfile, roundDefinition, snapshot.roster, resolvedTimerGroup,
                nativeProfile->registries.empty() ? 0U : nativeProfile->registries.front().scenario,
                static_cast<std::uint8_t>(inputs.regionIndex / 8), frame.clock,
                frame.endEpoch, frame.restricted, frame.completion);
            if (resolved && projected.controlled) {
                snapshot.nativeRound = projected;
                if (frame.completion.valid()) snapshot.missionCompletion = frame.completion;
            }
        }
        if(frame.opening.requested && frame.opening.binding) {
            const auto& binding=*frame.opening.binding->overlay;
            layouts::Definition selectedLayout{};
            // The two descriptor records are large; avoid multiplying the
            // production roster builder's stack usage on every publication.
            std::unique_ptr<overlay::Plan> plan(new(std::nothrow) overlay::Plan{});
            if(!plan || !state::build_data::find_scenario_layout(binding.selectedPackage,selectedLayout)
                || !overlay::prepare(layout,selectedLayout,binding,
                    [](std::size_t index,layouts::RosterGroup& group) noexcept {
                        return state::build_data::find_roster_group(index,group);
                    },*plan))return RosterOutcome::noGroups;
            const auto before=fold_groups(snapshot.roster);
            const auto admitted=overlay::append(*plan,scratch,snapshot.roster);
            if(admitted!=overlay::Result::added && admitted!=overlay::Result::present)return RosterOutcome::noGroups;
            namespace adventure=server::runtime::activity::adventure;
            const auto* gate=frame.opening.binding->gateway;
            std::unique_ptr<overlay::Plan> regionalPlan;
            const auto prepareRegion=[&]() noexcept {
                if(regionalPlan)return regionalPlan->prepared;
                if(!gate || !gate->region || !adventure::gateway::valid(*gate,binding))return false;
                regionalPlan.reset(new(std::nothrow) overlay::Plan{});
                return regionalPlan && overlay::prepare(layout,selectedLayout,*gate->region,
                    [](std::size_t index,layouts::RosterGroup& group) noexcept {
                        return state::build_data::find_roster_group(index,group);
                    },*regionalPlan);
            };
            auto ordinal=adventure::opening_lifetime_scenario(
                binding,*plan,snapshot.roster,inputs.regionIndex,inputs.destinationArrival);
            if(!ordinal && frame.opening.gatewayRequested && prepareRegion()) {
                const auto regionalAdmission=overlay::append_region(*regionalPlan,scratch,snapshot.roster);
                if(regionalAdmission!=overlay::Result::added && regionalAdmission!=overlay::Result::present)
                    return RosterOutcome::noGroups;
                ordinal=adventure::regional_lifetime_scenario(frame.opening,*regionalPlan,snapshot.roster,
                    inputs.regionIndex,inputs.sourceMembership.region.index,inputs.destinationArrival);
            }
            // Initial arrival remains strict. Later native movement is qualified
            // against the retained accepted lease and exact reported region index;
            // the original destination arrival and spawn fields remain intact.
            if(!ordinal)return RosterOutcome::noGroups;
            snapshot.lifetimeScenarioOrdinal=ordinal;
            if(frame.opening.gatewayRequested && !frame.opening.conflictingSelection && prepareRegion()) {
                // Preflight the authored destination before giving its source the
                // predicate. Projection retains every other service's full request.
                if(!adventure::gateway::project(*gate,static_cast<std::uint32_t>(inputs.regionIndex/8),
                    snapshot.placements,snapshot.playerPredicates))return RosterOutcome::noGroups;
            }
            adventureAdditive=admitted==overlay::Result::added && session.activity.rosterSends>=kWarmupSends
                && session.activity.rosterGroups==before;
            if(!adventure::append_opening_cue(snapshot.cues,frame.opening.cue))return RosterOutcome::noGroups;
            if(frame.opening.dialogueRequested) {
                if(snapshot.dialogues.count>=snapshot.dialogues.entries.size())return RosterOutcome::noGroups;
                for(std::size_t i=0;i<snapshot.dialogues.count;++i)
                    if(snapshot.dialogues.entries[i].registry==frame.opening.dialogue.registry
                        && snapshot.dialogues.entries[i].slot==frame.opening.dialogue.slot)return RosterOutcome::noGroups;
                snapshot.dialogues.entries[snapshot.dialogues.count++]=frame.opening.dialogue;
            }
        }
    }
    if(snapshot.omegaEndingRetire) {
        snapshot.omegaPortalEntry=false;
        snapshot.omegaForestGenerator=false;
        snapshot.omegaOpeningStage=message::kOmegaOpeningStageNone;
        snapshot.preserveMissionAuthorityState=true;
        snapshot.omegaCrownRestriction=state::activity::omega_crown_respawn::Restriction::disable;
    }
    const state::activity::defaults::FallbackPolicy& fallback =
        defaults.defaultDestination.fallback;
    snapshot.patchEpoch = session.activityPatchEpoch;
    // The character the join named wins, resolved to its authored SOID. The client binds its
    // player by matching this value against the object registry, and the short form the join
    // carries matches nothing.
    snapshot.playerKey = roster_player_key(rosterAccount, session.activity.characterSoid, true);
    // The old encoder documents this key as message 12's member record `+16` while its own code
    // sends the character SOID. That field is the membership identity, so this sends it instead.
    if (defaults.rosterKeyFromIdentity) {
        const std::uint64_t identity = inputs.sourceMembership.hasIdentity
                                           ? inputs.sourceMembership.identity.joinIdentity
                                           : 0;
        if (identity != 0) {
            snapshot.playerKey = identity;
        }
    }
    snapshot.lifetime = kLifetimeState;
    if (snapshot.strike_bond.enabled || snapshot.strike_pact.enabled) {
        const auto run = state::activity::mission_run_generation();
        snapshot.nightfallFailed = state::activity::nightfall::failed(run);
        if (snapshot.missionCompletion.valid() && !snapshot.nightfallFailed) {
            const auto* rewardVariant=state::activity::strikes::find(selection.activityIndex);
            if(rewardVariant
                && rewardVariant->difficulty!=state::activity::strikes::Difficulty::standard
                && !state::activity::nightfall::complete(
                    run,GetTickCount64(),rosterAccount.primarySoid,missionCharacterSoid,
                    state::activity::progress::mission_key(name),utc_seconds())) {
                return RosterOutcome::noGroups;
            }
        }
        if (snapshot.nightfallFailed) snapshot.missionCompletion = {};
    }
    snapshot.keyOnEveryParticipationSlot = defaults.rosterKeyOnAllSlots;
    // The participation record's `+0` latches only when the region index is known.
    snapshot.region = static_cast<std::uint32_t>(inputs.regionIndex);
    snapshot.hasRegion = true;
    if (snapshot.statusEffects.count != 0) {
        // A batch that fails the retained-authority scope is silently omitted from the auth block;
        // report the transition so a never-applied effect can be told apart from a never-sent one.
        namespace status_effect = middleware::bap::activity_message::native::status_effect;
        const bool scoped = status_effect::valid(snapshot.statusEffects, snapshot.roster, snapshot.region);
        const auto& first = snapshot.statusEffects.entries[0];
        static std::uint64_t lastKey = UINT64_MAX;
        const std::uint64_t key = (static_cast<std::uint64_t>(snapshot.region) << 32)
            | (static_cast<std::uint64_t>(snapshot.statusEffects.count) << 16)
            | (static_cast<std::uint64_t>(first.selectionRevision & 0xFFF) << 4)
            | (first.enabled ? 2U : 0U) | (scoped ? 1U : 0U);
        if (key != lastKey) {
            lastKey = key;
            core::log::writef(core::log::Channel::server,
                              scoped ? core::log::Level::info : core::log::Level::warn,
                              "ev=forest_transit stage=auth_scope region=%u effects=%zu valid=%u "
                              "first_registry=%08X first_slot=%u first_bubble=%u enabled=%u once=%u "
                              "selection_revision=%d",
                              snapshot.region, snapshot.statusEffects.count, scoped ? 1U : 0U,
                              first.registry, static_cast<unsigned>(first.slot),
                              static_cast<unsigned>(first.bubble), first.enabled ? 1U : 0U,
                              first.once ? 1U : 0U, first.selectionRevision);
        }
    }
    if (selection.activityIndex == 78 && name == "infinite_abyss") {
        const auto catalog = state::build_data::activities::entries();
        if (catalog.size() > 78U) {
            snapshot.lifetimeScenarioOrdinal =
                server::runtime::activity::haunted_forest::initial_lifetime_scenario(
                    selection, catalog[78], layout, inputs.destinationArrival);
        }
    }
    // The spawn override always names the destination's own arrival, never the player's position.
    snapshot.spawnSliceSet = inputs.destinationArrival;
    snapshot.spawnSetHash =
        state::activity::destination::attachable_spawn_set_hash(selection, fallback.spawnSetHash);
    snapshot.hasSpawnOverride =
        snapshot.spawnSetHash != 0 && snapshot.spawnSetHash != message::kAbsentSpawnSetHash;
    // A membership teleport can change the spawn within the same slice set.
    // Retaining the opening override here sends the client back to the opening
    // even though its teleport receipt names the requested destination.
    runtime::activity::native_activity_transit::Destination transitSpawn{};
    if (runtime::activity::native_activity_transit::spawn_destination(
        session.activity.lineage.source, transitSpawn)) {
        snapshot.spawnSliceSet = transitSpawn.region;
        snapshot.spawnSetHash = transitSpawn.spawn;
        snapshot.hasSpawnOverride = true;
    }
    // During generated traversal, let the native safe-position history and
    // spawn-point selection run. A retained platform override takes priority
    // over that history and sends every death back to the branch's start.
    // Entry, Terror and active membership travel keep their exact destination.
    // nativeTraversalRespawn above is only assigned inside the creator/regionIndex%8==0 block,
    // so any other roster publish for this same incarnation would publish hasSpawnOverride=true
    // with whatever the fallback or the transit override produced - including during traversal.
    // Ask the live round activity directly so every snapshot for one incarnation agrees.
    const auto respawnState=server::runtime::activity::native_activity::respawn_state(
        session.activity.lineage.source);
    if(respawnState.valid) snapshot.nativeRespawnRestricted=respawnState.restricted;
    if(respawnState.valid && respawnState.suppressed)nativeTraversalRespawn=true;
    if(nativeTraversalRespawn) {
        snapshot.hasSpawnOverride=true;
        snapshot.preferSpawnHistory=true;
        snapshot.spawnSliceSet=inputs.destinationArrival;
        snapshot.spawnSetHash=message::kAbsentSpawnSetHash;
    }
    if(respawnState.valid) {
        // Change-keyed exactly like log_transit_state: one line per distinct published state.
        // Pass criterion for a control run: no has_override=1 spawn=0x8BC697B5 line while
        // qualified=0. Nothing else in Dawn prints the override that is actually published.
        const std::uint64_t respawnKey=
            (static_cast<std::uint64_t>(static_cast<unsigned>(respawnState.phase)&0xFU)<<44)
            |(static_cast<std::uint64_t>(snapshot.hasSpawnOverride?1U:0U)<<43)
            |(static_cast<std::uint64_t>(respawnState.latched?1U:0U)<<42)
            |(static_cast<std::uint64_t>(respawnState.travelArrivalQualified?1U:0U)<<41)
            |(static_cast<std::uint64_t>(snapshot.spawnSliceSet&0xFFU)<<32)
            |static_cast<std::uint64_t>(snapshot.spawnSetHash);
        static std::atomic<std::uint64_t> lastRespawnKey{UINT64_MAX};
        if(lastRespawnKey.exchange(respawnKey)!=respawnKey) {
            core::log::writef(core::log::Channel::server,core::log::Level::debug,
                "ev=forest_respawn stage=publish phase=%u has_override=%u slice=%u spawn=0x%08X "
                "latched=%u qualified=%u",
                static_cast<unsigned>(respawnState.phase),snapshot.hasSpawnOverride?1U:0U,
                static_cast<unsigned>(snapshot.spawnSliceSet),
                static_cast<unsigned>(snapshot.spawnSetHash),
                respawnState.latched?1U:0U,respawnState.travelArrivalQualified?1U:0U);
        }
    }
    // The strike moves through four regions and each names its own respawn set. Its selected
    // checkpoint replaces the destination arrival, so a death after the Forest does not put the
    // player back at the Lighthouse. An unselected checkpoint leaves the destination's own set.
    if (snapshot.strike_bond.enabled && snapshot.strike_bond.checkpointSpawnSet != 0
        && snapshot.strike_bond.checkpointSliceSet >= 0) {
        snapshot.spawnSliceSet = snapshot.strike_bond.checkpointSliceSet;
        snapshot.spawnSetHash = snapshot.strike_bond.checkpointSpawnSet;
    }
    if (snapshot.strike_pact.enabled && snapshot.strike_pact.checkpointSpawnSet != 0
        && snapshot.strike_pact.checkpointSpawnSet != message::kAbsentSpawnSetHash) {
        snapshot.spawnSliceSet = snapshot.strike_pact.checkpointSliceSet;
        snapshot.spawnSetHash = snapshot.strike_pact.checkpointSpawnSet;
        snapshot.hasSpawnOverride = true;
    }
    if(snapshot.launchpad.enabled
        && snapshot.launchpad.cinematic.phase==state::activity::newlight::launchpad::cinematics::Phase::gameplay
        && snapshot.launchpad.section<std::size(state::activity::newlight::launchpad::kCheckpoints)) {
        const auto point=state::activity::newlight::launchpad::kCheckpoints[snapshot.launchpad.section];
        snapshot.spawnSliceSet=point.region;snapshot.spawnSetHash=point.spawnSet;snapshot.hasSpawnOverride=true;
    }
    // The archived opening packages need one registration-only packet on the primary/private
    // activity before their object state arrives. Publishing phase 1 and phase 2 together there
    // makes the client authority table report type 18 as already present before the native
    // activity-script manager has constructed it, so the manager skips its own component
    // dispatch. The joined foreign/public activity has the opposite ordering requirement: its
    // simulation registry must be fully seeded before the client swaps it to PUBLIC CURRENT and
    // creates player_broadcast. A registration-only foreign packet leaves that manager's entity-id
    // pool empty until the post-swap update, after the one native creation attempt has failed.
    const bool openingDestination =
        state::activity::forced::mission_host_reestablishment_enabled();
    snapshot.phaseOneOnly = openingDestination && !session.activity.joinedForeignSession
                            && session.activity.rosterSends == 0;
    // Keep sending the complete initialization until the client's native arrival observer proves
    // mission storage was constructed. Only that acknowledgement transfers ownership to the native
    // simulation: a server send can arrive before the authority manager is ready and be discarded.
    const bool authorityRuntimeInitialized =
        syntheticOmega && state::activity::mission_authority_runtime_initialized();
    snapshot.initializeMissionAuthorityRuntime =
        syntheticOmega && !authorityRuntimeInitialized;
    // Keep the existing delivery order for the opening runtime and its Scene/Ready commits.
    // Ikora's separate source authority now supplies one Scene-owned actor; each authored
    // wrapper binds that actor. The retained approach event above advances its orb prelude
    // independently of the still-unrecovered runtime handoff to state 2.
    // Do not spend Omega's opening clock behind the loading screen. In the measured retail intro,
    // the objective and Ghost preroll begin only after arrival. Publishing state 1 before the
    // client's in-world latch queued the Ikora Scene during transition, so both cast objects were
    // instantiated almost immediately when the world became available and the whole preroll was
    // skipped. WorldPhase::arrived is the exact activity:in_world observation; unlike the retained
    // mission_seed_armed latch it also pauses the opening during any later slice transition.
    const bool openingArrived = state::activity::world_phase() == state::activity::WorldPhase::arrived;
    const bool openingSeedArmed = state::activity::mission_seed_armed();
    const bool openingEligible =
        syntheticOmega && authorityRuntimeInitialized
        && session.activity.sensorObservation.omegaRosterReady
        && openingArrived && openingSeedArmed;
    const auto openingStageBefore = session.activity.omegaOpeningStage;
    const bool forcedOmegaSeed = omega_opening_publication::bootstrap(
        snapshot, session.activity.omegaOpeningStage, openingEligible, inputs.regionIndex,
        session.activity.sensorObservation.omegaForestEntranceTriggered);
    if (openingEligible && !forcedOmegaSeed) {
        const std::uint8_t previous = session.activity.omegaOpeningStage;
        if (previous == message::kOmegaOpeningStageReady
                   && session.activity.sensorObservation.omegaSceneHandoffArmed
                   && !session.activity.sensorObservation.omegaOpeningAuthorityPublished) {
            // Arrival started state 1; the exact type43/1 handoff is the next authored
            // boundary. Reusing the already-latched approach bit here advanced straight through
            // state 2 and skipped the Ikora vignette's natural dwell.
            snapshot.omegaOpeningStage = message::kOmegaOpeningStageTriggered;
            session.activity.omegaOpeningStage = message::kOmegaOpeningStageTriggered;
        } else if (previous == message::kOmegaOpeningStageTriggered
                   && session.activity.sensorObservation.omegaSceneCompleted) {
            // State 2 already armed the portal objects before the entrance edge could fire. Keep
            // them installed after cast retirement unless the client has now confirmed transport.
            snapshot.omegaOpeningStage =
                session.activity.sensorObservation.omegaPortalTransportConfirmed
                    ? message::kOmegaOpeningStageCompleted
                    : message::kOmegaOpeningStagePortal;
            session.activity.omegaOpeningStage = snapshot.omegaOpeningStage;
        } else if (previous == message::kOmegaOpeningStageSpawner) {
            // Compatibility with the retired direct-spawner experiment: never send type 1 again.
            snapshot.omegaOpeningStage = message::kOmegaOpeningStagePortal;
            session.activity.omegaOpeningStage = message::kOmegaOpeningStagePortal;
        } else if (previous == message::kOmegaOpeningStagePortal
                   && session.activity.sensorObservation.omegaSceneCompleted
                   && session.activity.sensorObservation.omegaForestEntranceTriggered
                   && !session.activity.sensorObservation.omegaForestEntranceAuthorityPublished) {
            // Type 30/index 24 is the authored entrance edge that requests the next mission state.
            // Publish state 4 now; type 22 or a region move can only confirm the transport after
            // the client has consumed the state that causes it.
            snapshot.omegaOpeningStage = message::kOmegaForestStageTransition;
            session.activity.omegaOpeningStage = message::kOmegaForestStageTransition;
        } else if (previous == message::kOmegaOpeningStagePortal
                   && session.activity.sensorObservation.omegaPortalTransportConfirmed) {
            // Compatibility for a client that reports transport before its entrance monitor edge.
            snapshot.omegaOpeningStage = message::kOmegaOpeningStageCompleted;
            session.activity.omegaOpeningStage = message::kOmegaOpeningStageCompleted;
        } else if (previous == message::kOmegaOpeningStagePortal) {
            snapshot.omegaOpeningStage = message::kOmegaOpeningStagePortal;
        } else if (previous == message::kOmegaOpeningStageCompleted) {
            snapshot.omegaOpeningStage = message::kOmegaOpeningStageSettled;
            session.activity.omegaOpeningStage = message::kOmegaOpeningStageSettled;
        } else if (previous == message::kOmegaOpeningStageSettled
                   && session.activity.sensorObservation.omegaForestEntranceTriggered
                   && !session.activity.sensorObservation.omegaForestEntranceAuthorityPublished) {
            snapshot.omegaOpeningStage = message::kOmegaForestStageTransition;
            session.activity.omegaOpeningStage = message::kOmegaForestStageTransition;
        } else if (previous == message::kOmegaForestStageTransition) {
            // State 4 is a persistent authored runtime state, not an edge pulse. Keep it installed
            // until a later observed mission event gives the host a proven successor.
            snapshot.omegaOpeningStage = message::kOmegaForestStageTransition;
        } else if (previous >= message::kOmegaForestStageSettled) {
            snapshot.omegaOpeningStage = message::kOmegaForestStageSettled;
        } else if (previous >= message::kOmegaOpeningStageSettled) {
            snapshot.omegaOpeningStage = message::kOmegaOpeningStageSettled;
        }
    }
    if (syntheticOmega) {
        // Emit only changes in the publication gates. An accepted approach receipt alone does
        // not prove that a scene body was sent; retain the exact blocked prerequisite in logs.
        static std::atomic_uint64_t lastOpeningTrace{UINT64_MAX};
        const std::uint64_t trace = (state::activity::mission_run_generation() << 16U)
            | (std::uint64_t{session.activity.omegaOpeningStage} << 8U)
            | (authorityRuntimeInitialized ? 1U : 0U)
            | (session.activity.sensorObservation.omegaRosterReady ? 2U : 0U)
            | (openingArrived ? 4U : 0U) | (openingSeedArmed ? 8U : 0U)
            | (session.activity.sensorObservation.omegaOpeningTriggered ? 16U : 0U);
        if (lastOpeningTrace.exchange(trace, std::memory_order_relaxed) != trace) {
            std::array<char, 352> line{};
            std::snprintf(line.data(), line.size(),
                "ev=omega_opening_publication eligible=%u runtime_ready=%u roster_ready=%u "
                "arrived=%u seed_armed=%u approach=%u stage_before=%u stage_after=%u",
                openingEligible ? 1U : 0U, authorityRuntimeInitialized ? 1U : 0U,
                session.activity.sensorObservation.omegaRosterReady ? 1U : 0U,
                openingArrived ? 1U : 0U, openingSeedArmed ? 1U : 0U,
                session.activity.sensorObservation.omegaOpeningTriggered ? 1U : 0U,
                static_cast<unsigned>(openingStageBefore),
                static_cast<unsigned>(session.activity.omegaOpeningStage));
            core::log::write(core::log::Channel::server, core::log::Level::info, line.data());
        }
    }
    snapshot.publishOmegaOpeningTransition =
        snapshot.omegaOpeningStage == message::kOmegaOpeningStageTriggered
        || snapshot.omegaOpeningStage == message::kOmegaOpeningStageCompleted
        || snapshot.omegaOpeningStage == message::kOmegaForestStageTransition;
    const bool openingBaseline =
        openingEligible
        && session.activity.omegaOpeningStage == message::kOmegaOpeningStageBaseline;
    snapshot.preserveMissionAuthorityState =
        syntheticOmega && authorityRuntimeInitialized && !openingBaseline;
    snapshot.missionDirectorActive = openingBaseline || snapshot.publishOmegaOpeningTransition;
    snapshot.missionDirectorTransition =
        openingBaseline || snapshot.publishOmegaOpeningTransition;
    snapshot.activityScriptFlag = openingBaseline || snapshot.publishOmegaOpeningTransition;
    snapshot.activityScriptState =
        snapshot.omegaOpeningStage == message::kOmegaForestStageTransition
            ? 4
            : (snapshot.omegaOpeningStage == message::kOmegaOpeningStageCompleted
                   ? 3
                   : (snapshot.omegaOpeningStage == message::kOmegaOpeningStageTriggered
                           ? 2
                           : (openingBaseline ? 1 : 0)));
    // Tower Watch uses the same root-cue/runtime transport as Omega, but the progression itself
    // is a compact manifest. Publish exactly one undelivered beat at a time so startup edges that
    // arrive in one frame cannot skip the opening objective and Ghost line.
    const bool towerWatchEligible = name == tower_watch::kPackage
                                    && session.activity.sensorObservation.towerWatchRosterReady
                                    && state::activity::world_phase()
                                           == state::activity::WorldPhase::arrived
                                    && state::activity::mission_seed_armed();
    tower_watch::Stage nextTowerWatchStage = tower_watch::Stage::none;
    if (towerWatchEligible) {
        if (!session.activity.sensorObservation.towerWatchBreachSeen
            && state::activity::tower_watch_opening_dialogue_processed()
            && session.activity.sensorObservation.towerWatchPublishedStage
                   >= tower_watch::value(tower_watch::Stage::opening)) {
            session.activity.sensorObservation.towerWatchBreachSeen = true;
            std::array<char, 224> handoffLine{};
            const int handoffWritten = std::snprintf(
                handoffLine.data(),
                handoffLine.size(),
                "ev=tower_watch_executor stage=dialogue_handoff result=accepted "
                "record=0 published=%u next=breach_cabal_beat",
                static_cast<unsigned>(
                    session.activity.sensorObservation.towerWatchPublishedStage));
            if (handoffWritten > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {handoffLine.data(),
                                  (std::min)(static_cast<std::size_t>(handoffWritten),
                                             handoffLine.size() - 1U)});
            }
        }
        const std::uint8_t published =
            session.activity.sensorObservation.towerWatchPublishedStage;
        if (published < tower_watch::value(tower_watch::Stage::opening)) {
            nextTowerWatchStage = tower_watch::Stage::opening;
        } else if (published < tower_watch::value(tower_watch::Stage::breach)
                   && session.activity.sensorObservation.towerWatchBreachSeen) {
            nextTowerWatchStage = tower_watch::Stage::breach;
        } else if (published < tower_watch::value(tower_watch::Stage::pathUnlocked)
                   && session.activity.sensorObservation.towerWatchEncounterChanged) {
            nextTowerWatchStage = tower_watch::Stage::pathUnlocked;
        }
    }
    if (const tower_watch::Beat* const beat = tower_watch::beat(nextTowerWatchStage);
        beat != nullptr) {
        snapshot.phaseOneOnly = false;
        snapshot.publishAuthoredCueTransition = true;
        snapshot.authoredCueRegistry = tower_watch::kRootCueRegistry;
        snapshot.authoredDirectiveEvent = beat->directiveEvent;
        snapshot.authoredDialogueRecord = beat->dialogueRecord;
        snapshot.authoredCueStage = tower_watch::value(beat->stage);
        snapshot.publishAuthoredSceneSelector =
            tower_watch::kPublishBreachSceneAuthority
            && beat->stage == tower_watch::Stage::breach;
        if (snapshot.publishAuthoredSceneSelector) {
            snapshot.authoredSceneRegistry = tower_watch::kTowerWatchRegistry;
            snapshot.authoredSceneType = tower_watch::kBreachSceneType;
            snapshot.authoredSceneIndex = tower_watch::kBreachSceneIndex;
            snapshot.authoredSceneSelector = tower_watch::kBreachSceneSelector;
            snapshot.authoredSceneEntryRegistry = tower_watch::kBreachSceneEntryRegistry;
            snapshot.authoredSceneEntryType = tower_watch::kBreachSceneEntryType;
            snapshot.authoredSceneEntryIndex = tower_watch::kBreachSceneEntryIndex;
        }
        snapshot.initializeMissionAuthorityRuntime =
            beat->stage == tower_watch::Stage::opening;
        snapshot.preserveMissionAuthorityState = false;
        snapshot.missionDirectorActive = true;
        snapshot.missionDirectorTransition = true;
        snapshot.activityScriptFlag = true;
        snapshot.activityScriptState = beat->scriptState;

        std::array<char, 320> cueLine{};
        const int cueWritten = std::snprintf(
            cueLine.data(),
            cueLine.size(),
            "ev=tower_watch_executor stage=publish result=staged beat=%u name=%s "
            "root=0x%08X directive=0x%08X dialogue_record=%u script_state=%d "
            "scene=%u scene_blocked_unsafe=%u scene_slot=0x%08X/%u/%u "
            "selector=0x%08X scene_entry=0x%08X/%u/%u scene_candidate_bits=129",
            static_cast<unsigned>(snapshot.authoredCueStage),
            beat->name,
            snapshot.authoredCueRegistry,
            snapshot.authoredDirectiveEvent,
            static_cast<unsigned>(snapshot.authoredDialogueRecord),
            snapshot.activityScriptState,
            snapshot.publishAuthoredSceneSelector ? 1U : 0U,
            !tower_watch::kPublishBreachSceneAuthority
                    && beat->stage == tower_watch::Stage::breach
                ? 1U
                : 0U,
            snapshot.authoredSceneRegistry,
            static_cast<unsigned>(snapshot.authoredSceneType),
            static_cast<unsigned>(snapshot.authoredSceneIndex),
            snapshot.authoredSceneSelector,
            snapshot.authoredSceneEntryRegistry,
            static_cast<unsigned>(snapshot.authoredSceneEntryType),
            static_cast<unsigned>(snapshot.authoredSceneEntryIndex));
        if (cueWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {cueLine.data(),
                              (std::min)(static_cast<std::size_t>(cueWritten),
                                         cueLine.size() - 1U)});
        }
    }
    if (name == tower_watch::kPackage
        && session.activity.sensorObservation.towerWatchPublishedStage != 0U
        && !snapshot.publishAuthoredCueTransition) {
        snapshot.preserveMissionAuthorityState = true;
    }
    const bool configuredAuthoredSeed = core::settings::get().client.seedAuthoredSensors
                                        && state::activity::mission_seed_armed();
    snapshot.seedAuthoredSensors = omegaDestination
                                       ? syntheticOmega
                                             && (forcedOmegaSeed || configuredAuthoredSeed)
                                       : configuredAuthoredSeed;
    // The state byte owns the roster object's lifetime, not its auth-body revision. Advancing it
    // for a slot-35 value change destroys slot 18 and slot 35 before the replacement auth bodies
    // can update them. Keep the object generation stable and let the changed auth body apply to
    // the existing runtime instead.
    if(snapshot.omegaEndingRetire) {
        snapshot.omegaOpeningStage=message::kOmegaOpeningStageNone;
        snapshot.publishOmegaOpeningTransition=false;
        snapshot.preserveMissionAuthorityState=true;
        snapshot.omegaPortalEntry=false;
        snapshot.omegaForestGenerator=false;
        snapshot.omegaCrownRestriction=state::activity::omega_crown_respawn::Restriction::disable;
        // Retain each prior bubble/key ordinal and publish explicit removals.
        // Native cleanup must acknowledge those removals before bookendState
        // permits the teleport. Keep the globals' generation stable throughout.
        session.activity.rosterGroups=fold_groups(snapshot.roster);
    }
    // A verified addition does not change the lifetimes of the existing base
    // objects. Only preserve generation when that exact base set is unchanged;
    // unrelated roster changes still use the normal state transition below.
    if(adventureAdditive)session.activity.rosterGroups=fold_groups(snapshot.roster);
    // The phase-one removal must be the whole preparation packet. Even an
    // empty reset block in phase two can seed a new sync record after cleanup.
    if(snapshot.launchpad.enabled && snapshot.launchpad.cinematic.retiring()) {snapshot.phaseOneOnly=true;}
    const auto folded = fold_groups(snapshot.roster);
    // Omega's region 120 -> 88 transition drops three Lighthouse groups from
    // the desired view. A shared generation bump destroys the unchanged global
    // script, participation and HUD objects after native ownership has begun.
    // Keep each existing key's ordinal and generation across streamed regions.
    const bool retainOrdinals = syntheticOmega || towerVendors
        || (nativeProfile && nativeProfile->retainRosterOrdinals);
    const bool warmup = session.activity.rosterSends < kWarmupSends;
    if (retainOrdinals && !warmup) session.activity.rosterGroups = folded;
    snapshot.stateSequence = next_state_sequence(session, folded, burst);
    if (retainOrdinals) {
        const roster_lifetime::Identity identity{session.activity.instance.sessionId,
            session.activity.instance.incarnation.value, session.activityPatchEpoch.first,
            session.activityPatchEpoch.second, layout.tag};
        const auto result = roster_lifetime::prepare(session.activity.rosterLifetimes, identity,
            static_cast<std::uint32_t>(inputs.regionIndex / 8),
            static_cast<std::uint8_t>(message::kStateByteBias + snapshot.stateSequence), warmup,
            snapshot.roster, session.activity.rosterLifetimes);
        if(towerVendors && result==roster_lifetime::Result::ready) {
            vendor_roster::retain(session.activity.rosterLifetimes,layout.tag);
            if(!warmup && session.activity.lineage.owns(session.activity.instance)) {
                (void)tower_spawn_recovery::project(session.activity.rosterLifetimes,
                    session.activity.instance,state::activity::mission_run_generation(),
                    state::activity::tower_spawn_recovery::snapshot());
            }
        }
        const bool projected = result == roster_lifetime::Result::ready
            && roster_lifetime::project(session.activity.rosterLifetimes, snapshot.roster,
                scratch.rosterSubBlocks);
        if (!projected) {
            static std::atomic_uint failures{0};
            if (failures.fetch_add(1, std::memory_order_relaxed) < 12) {
                std::array<char, 240> line{};
                const int written = std::snprintf(line.data(), line.size(),
                    "ev=roster_lifetime stage=plan result=rejected reason=%u owner=%016llX incarnation=%llu region=%d warmup=%u",
                    static_cast<unsigned>(result),
                    static_cast<unsigned long long>(identity.owner),
                    static_cast<unsigned long long>(identity.incarnation), inputs.regionIndex,
                    static_cast<unsigned>(warmup));
                if (written > 0 && static_cast<std::size_t>(written) < line.size())
                    core::log::write(core::log::Channel::server, core::log::Level::warn,
                        {line.data(), static_cast<std::size_t>(written)});
            }
            return RosterOutcome::noGroups;
        }
    } else if (session.activity.rosterLifetimes.identity.owner) {
        session.activity.rosterLifetimes = {};
    }
    // Apply Omega's persistent terminal tombstones to the retained ordering.
    // The ending owns this explicit retirement through transit; the ordinary
    // lifetime planner must not treat dormant-bubble removals as travel updates.
    if(snapshot.omegaEndingRetire
        && !omega_lair::terminal_roster(scratch,snapshot.roster,
            state::activity::omega_ending::kState)) {
        return RosterOutcome::noGroups;
    }
    if(syntheticOmega) {
        static std::atomic_uint64_t previous{UINT64_MAX};
        const auto trace=(state::activity::mission_run_generation()<<32U)
            | (std::uint64_t{static_cast<std::uint32_t>(inputs.regionIndex)}<<16U)
            | (std::uint64_t{snapshot.stateSequence}<<8U)
            | (snapshot.omegaEndingRetire?1U:0U);
        if(previous.exchange(trace,std::memory_order_relaxed)!=trace) {
            core::log::writef(core::log::Channel::server,core::log::Level::info,
                "ev=omega_roster_lifetime region=%d state=%u groups=%zu retained_globals=%zu retained_bubbles=%zu ending=%u",
                inputs.regionIndex,static_cast<unsigned>(snapshot.stateSequence),snapshot.roster.groupCount,
                snapshot.roster.topLevelKeys.size(),snapshot.roster.bubbleSubBlocks.size(),
                snapshot.omegaEndingRetire?1U:0U);
        }
    }
    return RosterOutcome::published;
}

namespace {

/** Copies the delivery fields whose candidate mutation must remain off the live Session. */
[[nodiscard]] RosterDeliveryBefore delivery_of(const ActivityBindingState& binding) noexcept {
    return {
        binding.rosterGroups,
        binding.rosterSends,
        binding.rosterState,
        binding.omegaOpeningStage,
        binding.directorSends,
        binding.missionDirectorActive,
        binding.rosterLifetimes,
        binding.towerVendorPresence,binding.vendorClockOrigin,
    };
}

/** Maps one copied membership after-image into the fixed wire schema. */
[[nodiscard]] bool make_membership_wire(
    state::activity::ActivityInstanceKey activity,bool validatedOmega,bool validatedBeyond,bool validatedGarden,bool validatedLaunchpad,bool validatedApproach,bool validatedGatewayIntro,
    const state::activity::membership::MembershipState& membership,
    const gameplay::AdvertisementSnapshot& advertisement,
    membership_message::MembershipSnapshot& wire) noexcept {
    wire = {};
    if (!membership.hasIdentity || membership.revision == 0) {
        return false;
    }
    wire.identity.memberKey = membership.identity.memberKey;
    wire.identity.field1 = membership.identity.smallOpaque;
    wire.identity.field2 = membership.identity.signedOpaque;
    wire.identity.field3 = membership.identity.joinIdentity;
    wire.identity.accountSoid = membership.identity.accountSoid;
    wire.identity.field5 = membership.identity.opaqueSoid;
    wire.identity.field6 = membership.identity.secondaryOpaque;
    wire.spawn.state = membership.spawn.state;
    wire.spawn.opaqueByte = membership.spawn.opaqueByte;
    wire.spawn.opaqueValue = membership.spawn.opaqueValue;
    state::activity::membership::SpawnState recovery{};
    if(runtime::activity::native_activity_transit::project_respawn(activity,
        membership.identity.memberKey,membership.spawn,membership.region.index,recovery))
        wire.spawn={recovery.state,recovery.opaqueByte,recovery.opaqueValue};
    wire.teleport.state = membership.teleport.state;
    wire.teleport.token = membership.teleport.token;
    wire.teleport.sliceSetIndex = membership.teleport.sliceSetIndex;
    wire.teleport.sliceSetHash = membership.teleport.sliceSetHash;
    const state::activity::omega_ending_transit::Observation nativeTransit{
        {membership.teleport.state,membership.teleport.token,membership.teleport.sliceSetIndex,
            membership.teleport.sliceSetHash},membership.region.index,membership.hasTeleportReceipt,membership.region.index>=0};
    auto terminal=state::activity::omega_ending::project_transit({activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedOmega,
        nativeTransit});
    const auto beyond=state::activity::beyond_infinity::transit::project(activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedBeyond,nativeTransit);
    if(beyond.publish) { terminal=beyond; }
    const auto launchpad=state::activity::newlight::launchpad::transit::project(activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedLaunchpad,nativeTransit,
        state::activity::newlight::launchpad::transit::dock_arrived(membership.currentLeg,membership.pendingLeg),
        state::activity::newlight::launchpad::transit::divide_retained(membership.currentLeg,membership.pendingLeg),membership.transitionToken);
    if(validatedLaunchpad) {state::activity::newlight::launchpad::transit::echo_regions(membership,wire);}
    if(launchpad.publish) {terminal=launchpad;}
    const auto approach=state::activity::newlight::launchpad::tower::project(activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedApproach,nativeTransit,GetTickCount64());
    if(approach.publish) {terminal=approach;}
    const auto briefing=state::activity::gateway_intro::project(activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedGatewayIntro,nativeTransit,GetTickCount64());
    if(briefing.publish) {terminal=briefing;}
    const auto generic = runtime::activity::native_activity_transit::project(
        activity, membership.identity.memberKey, membership.hasTeleportReceipt,
        {membership.teleport.state, membership.teleport.token, membership.teleport.sliceSetIndex,
         membership.teleport.sliceSetHash}, membership.region.index);
    if (generic.present && !terminal.publish) {
        terminal = {{generic.host.state, generic.host.token, generic.host.sliceSetIndex,
                     generic.host.sliceSetHash}, generic.present, generic.arrived,
                    generic.released};
    }
    if(terminal.publish) {
        wire.teleport={terminal.host.state,terminal.host.token,terminal.host.sliceSetIndex,
            terminal.host.sliceSetHash};
    }
    wire.revision = membership.revision;
    wire.epoch = state::activity::membership::kStableEpoch;
    wire.transitionToken = membership.hasTransitionToken
                               ? membership.transitionToken
                               : state::activity::membership::kInitialTransitionToken;
    wire.hasSynchronizationToken=membership.hasSynchronizationToken;
    wire.synchronizationToken=membership.synchronizationToken;
    if (advertisement.readiness == gameplay::AdvertisementReadiness::ready) {
        wire.citizen = advertisement.citizen;
        if(!membership_message::select_active_region(wire,wire.citizen.regionIndex)) { return false; }
    } else if(membership.region.index>=0
        && !membership_message::select_active_region(wire,membership.region.index)) {
        return false;
    }
    // During prefetch the gameplay frame can still name the old arena. The
    // real membership advertisement carries the verified destination (25).
    auto gardenNative=nativeTransit;
    if(wire.citizen.present) {gardenNative.currentRegion=wire.citizen.regionIndex;gardenNative.hasRegion=true;}
    const auto garden=state::activity::strike_bond::ending_transit(activity,
        state::activity::mission_run_generation(),membership.identity.memberKey,validatedGarden,gardenNative);
    if(garden.publish) {
        terminal=garden;wire.teleport={garden.host.state,garden.host.token,garden.host.sliceSetIndex,garden.host.sliceSetHash};
        if(!membership_message::select_active_region(wire,garden.host.sliceSetIndex)) return false;
    }
    wire.hasHostSynchronizationToken=state::activity::omega_ending_transit::host_synchronization_ready(
        terminal,garden.publish?gardenNative:nativeTransit,membership.hasSynchronizationToken,membership.synchronizationToken,
        advertisement.readiness==gameplay::AdvertisementReadiness::ready && wire.citizen.present,
        wire.citizen.regionIndex);
    if(wire.hasHostSynchronizationToken) { wire.hostSynchronizationToken=terminal.host.token; }
    return true;
}

/** Completes roster/grant/delivery fields from copied State and a candidate Session value. */
[[nodiscard]] bool finalize_roster(const Session& session,
                                   Scratch& scratch,
                                   const RosterSnapshotInputs& inputs,
                                   bool rearmMissionDirector,
                                   RegionTransitionSnapshot& snapshot) noexcept {
    if (!requires_notification(snapshot.required, RegionNotification::roster)) {
        snapshot.before = delivery_of(session.activity);
        snapshot.after = snapshot.before;
        return true;
    }
    if (!session.activityPatchEpochSeen) {
        return false;
    }
    // The retained native ordinal mirror is value-owned for atomic rollback.
    // Keep this detached Session copy off the game's callback stack.
    std::unique_ptr<Session> candidateStorage(new (std::nothrow) Session(session));
    if (!candidateStorage) return false;
    Session& candidate = *candidateStorage;
    if (rearmMissionDirector
        && snapshot.advertisement.readiness == gameplay::AdvertisementReadiness::ready) {
        candidate.activity.missionDirectorActive = false;
    }
    std::array<char, state::activity::destination::kPackageNameCapacity> destination{};
    std::size_t destinationLength = 0;
    const RosterOutcome built = build_roster_snapshot(candidate,
                                                      scratch,
                                                      inputs,
                                                      snapshot.rosterWire,
                                                      destination,
                                                      destinationLength,
                                                      snapshot.burst);
    if (built != RosterOutcome::published) {
        // Bounded rejection diagnostic: a vetoed bundle sends the client nothing at all, so a
        // destination that can never publish would otherwise starve the load invisibly.
        static std::atomic<std::uint32_t> s_lastRejection{0};
        std::uint32_t observed = kFoldBasis;
        for (std::size_t index = 0; index < destinationLength; ++index) {
            observed = (observed ^ static_cast<std::uint8_t>(destination[index])) * kFoldPrime;
        }
        observed ^= static_cast<std::uint32_t>(built) + 1U;
        if (s_lastRejection.exchange(observed) != observed) {
            const char* reason = built == RosterOutcome::noEpoch     ? "no_epoch"
                                 : built == RosterOutcome::noLayout  ? "no_layout"
                                 : built == RosterOutcome::encodeFailed ? "encode"
                                                                        : "no_groups";
            std::array<char, 160> line{};
            const int written = std::snprintf(
                line.data(), line.size(), "ev=activity stage=roster_finalize result=%s dest=%.*s",
                reason, static_cast<int>(destinationLength), destination.data());
            if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
                core::log::write(core::log::Channel::server, core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        return false;
    }
    snapshot.before = delivery_of(session.activity);
    snapshot.after = delivery_of(candidate.activity);
    if (!omega_lair::finalize_retained_roster(scratch,snapshot.rosterWire,
            snapshot.after.lifetimes)) return false;
    if (!lifecycle::stage_roster_publication_generation(session.activity,
                                                        snapshot.rosterPublication)) {
        return false;
    }
    state::activity::bubble_authority::Grant grant{};
    if (state::activity::bubble_authority::select_grant(inputs.grantBefore,
                                                        snapshot.regionIndex,
                                                        grant)) {
        snapshot.grantCandidate = grant;
        snapshot.rosterWire.hasGrant = true;
        snapshot.rosterWire.grant.bubble = grant.bubble;
        snapshot.rosterWire.grant.token = grant.token;
    }
    return true;
}

/** Finalizes a copied semantic plan after readiness and exact lineage have been proved. */
[[nodiscard]] RegionSnapshotBuildResult finalize_snapshot(
    const Session& session,
    Scratch& scratch,
    const state::activity::membership::RegionSnapshotInputs& copied,
    const state::activity::membership::MembershipState& membershipAfter,
    state::activity::HostRegionKey expectedHost,
    state::activity::HostRegionKey nextHost,
    NotificationMask required,
    bool burst,
    bool publishesHud,
    bool rearmMissionDirector,
    RegionTransitionSnapshot& output,
    RegionPublicationDebt& debt,
    gameplay::group::HostActivityLineageLease& advertisementLease) noexcept {
    output = {};
    debt = {};
    const RegionLineage& lineage = session.activity.lineage;
    const std::string_view name(
        reinterpret_cast<const char*>(copied.destination.packageName.data()),
        copied.destination.packageNameLength);
    layouts::Definition layout{};
    const bool hasLayout = state::build_data::find_scenario_layout(name, layout);
    const bool allowArrival = lineage.kind == RegionLineageKind::ownedActivity
                              && lineage.bound == lineage.source;
    const EffectiveRegion region = resolve_region(copied.defaults.defaultDestination,
                                                  copied.destination,
                                                  membershipAfter.region.index,
                                                  allowArrival,
                                                  name,
                                                  layout);
    if (!region.valid
        || (requires_notification(required, RegionNotification::roster) && !hasLayout)) {
        return RegionSnapshotBuildResult::failed;
    }

    gameplay::AdvertisementSnapshot advertisement{};
    if (requires_notification(required, RegionNotification::membership)) {
        const gameplay::AdvertisementReadiness readiness =
            gameplay::request_advertisement_host(lineage.source, region.index);
        if (readiness == gameplay::AdvertisementReadiness::stale) {
            return RegionSnapshotBuildResult::stale;
        }
        if (readiness == gameplay::AdvertisementReadiness::pending) {
            debt.binding = session.activity.key;
            debt.activity = lineage.bound;
            debt.regionSource = lineage.source;
            debt.committedHostRegion = nextHost;
            debt.membershipAfter = membershipAfter;
            debt.destination = copied.destination;
            debt.regionIndex = region.index;
            debt.required = required;
            debt.present = true;
            debt.publishesHud = publishesHud;
            return RegionSnapshotBuildResult::pending;
        }
        advertisement.readiness = readiness;
        if (readiness == gameplay::AdvertisementReadiness::ready
            && !gameplay::acquire_advertisement_snapshot(lineage.source,
                                                         region.index,
                                                         advertisement,
                                                         advertisementLease)) {
            return RegionSnapshotBuildResult::stale;
        }
    }

    output.binding = session.activity.key;
    output.activity = lineage.bound;
    output.regionSource = lineage.source;
    output.expectedHostRegion = expectedHost;
    output.nextHostRegion = nextHost;
    output.sourceHostRegion = nextHost;
    output.regionIndex = region.index;
    output.destinationArrival = region.arrival;
    output.destination = copied.destination;
    output.membershipAfter = membershipAfter;
    output.advertisement = advertisement;
    output.required = required;
    output.burst = burst;
    output.publishesHud = publishesHud;
    if (requires_notification(required, RegionNotification::membership)
        && !make_membership_wire(lineage.bound,allowArrival && name=="mission_scot"
                && hasLayout && layout.tag==0x80F47522U,
            allowArrival && name=="adventure_vod" && hasLayout && layout.tag==state::activity::beyond_infinity::kScenario,
            allowArrival && name=="mission_bond" && hasLayout && layout.tag==0x80F47445U,
            allowArrival && name=="mission_launchpad" && hasLayout && layout.tag==state::activity::newlight::launchpad::kScenario,
            allowArrival && name=="cine_110_twr" && hasLayout && layout.tag==state::activity::newlight::launchpad::tower::kApproachScenario,
            allowArrival && name==state::activity::gateway_intro::kPackage && hasLayout && layout.tag==state::activity::gateway_intro::kScenario,
            membershipAfter, advertisement, output.membershipWire)) {
        gameplay::group::release_host_activity_lineage(advertisementLease);
        return RegionSnapshotBuildResult::failed;
    }
    const RosterSnapshotInputs rosterInputs{
        copied.destination,
        copied.defaults,
        membershipAfter,
        copied.grantBefore,
        region.index,
        region.arrival,
        copied.sourceDestination,
    };
    if (!finalize_roster(
            session, scratch, rosterInputs, rearmMissionDirector, output)) {
        gameplay::group::release_host_activity_lineage(advertisementLease);
        return RegionSnapshotBuildResult::failed;
    }
    return RegionSnapshotBuildResult::ready;
}

} // namespace

/** Finalizes one prepared authoritative/refresh region plan. */
RegionSnapshotBuildResult build_region_transition_snapshot(
    const Session& session,
    Scratch& scratch,
    const activity_message::ActivityPlan& plan,
    RegionTransitionSnapshot& output,
    RegionPublicationDebt& debt,
    gameplay::group::HostActivityLineageLease& advertisementLease,
    gameplay::group::HostActivityLineageLease& boundLineageLease) noexcept {
    output = {};
    debt = {};
    gameplay::group::release_host_activity_lineage(advertisementLease);
    gameplay::group::release_host_activity_lineage(boundLineageLease);
    if (!lifecycle::activity_binding_is_current(session)
        || plan.instanceKey != session.activity.instance
        || plan.membershipMutation.instanceKey != plan.instanceKey) {
        return RegionSnapshotBuildResult::stale;
    }
    if (!acquire_region_lineage_locked(
            session, session.activity.lineage, boundLineageLease)) {
        return RegionSnapshotBuildResult::stale;
    }

    NotificationMask required = 0;
    if (plan.delivery == activity_message::Delivery::refreshNotifications) {
        required |= notification_mask(RegionNotification::globalState);
        required |= notification_mask(RegionNotification::roster);
        if (plan.membershipMutation.hasSnapshot) {
            required |= notification_mask(RegionNotification::membership);
        }
    } else if (plan.delivery == activity_message::Delivery::authoritativeNotifications) {
        if (plan.membershipMutation.hasSnapshot) {
            required |= notification_mask(RegionNotification::membership);
        }
        if (plan.regionMoved) {
            required |= notification_mask(RegionNotification::roster);
        }
    } else {
        gameplay::group::release_host_activity_lineage(boundLineageLease);
        return RegionSnapshotBuildResult::failed;
    }

    state::activity::membership::RegionSnapshotInputs copied{};
    state::activity::membership::MembershipState membershipAfter{};
    state::activity::HostRegionKey expectedHost{};
    state::activity::HostRegionKey nextHost{};
    if (plan.delivery == activity_message::Delivery::authoritativeNotifications) {
        const auto& transition = plan.membershipMutation.regionTransition;
        if (session.activity.lineage.bound != session.activity.lineage.source
            || transition.activity != session.activity.lineage.source
            || transition.activity != plan.instanceKey
            || transition.movesRegion != plan.regionMoved) {
            gameplay::group::release_host_activity_lineage(boundLineageLease);
            return RegionSnapshotBuildResult::stale;
        }
        copied.bound = transition.activity;
        copied.source = transition.activity;
        copied.sourceHostRegion = transition.nextHostRegion;
        copied.destination = transition.destination;
        copied.sourceDestination = transition.destination;
        copied.grantBefore = transition.grantBefore;
        copied.sourceMembership = transition.after;
        state::activity::defaults::snapshot(copied.defaults);
        copied.stateRevision = transition.expectedStateRevision;
        copied.boundRecordRevision = transition.expectedRecordRevision;
        copied.sourceRecordRevision = transition.expectedRecordRevision;
        membershipAfter = transition.after;
        expectedHost = transition.expectedHostRegion;
        nextHost = transition.nextHostRegion;
    } else {
        if (!state::activity::membership::snapshot_region_inputs(
                session.activity.lineage.bound,
                session.activity.lineage.source,
                {},
                copied)) {
            gameplay::group::release_host_activity_lineage(boundLineageLease);
            return RegionSnapshotBuildResult::stale;
        }
        membershipAfter = copied.sourceMembership;
        expectedHost = copied.sourceHostRegion;
        nextHost = copied.sourceHostRegion;
    }
    const RegionSnapshotBuildResult result = finalize_snapshot(
        session,
        scratch,
        copied,
        membershipAfter,
        expectedHost,
        nextHost,
        required,
        false,
        plan.delivery == activity_message::Delivery::authoritativeNotifications
            && plan.regionMoved,
        false,
        output,
        debt,
        advertisementLease);
    if (result == RegionSnapshotBuildResult::failed
        || result == RegionSnapshotBuildResult::stale) {
        gameplay::group::release_host_activity_lineage(boundLineageLease);
    }
    return result;
}

/** Finalizes one periodic bundle from the State value captured with its commit mutation. */
RegionSnapshotBuildResult build_periodic_region_snapshot(
    const Session& session,
    Scratch& scratch,
    const state::activity::membership::PeriodicRegionRefresh& refresh,
    const state::activity::membership::PendingMutation& mutation,
    bool burst,
    bool rearmMissionDirector,
    RegionTransitionSnapshot& output,
    gameplay::group::HostActivityLineageLease& advertisementLease,
    gameplay::group::HostActivityLineageLease& boundLineageLease) noexcept {
    output = {};
    gameplay::group::release_host_activity_lineage(advertisementLease);
    gameplay::group::release_host_activity_lineage(boundLineageLease);
    if (!lifecycle::activity_binding_is_current(session)
        || refresh.inputs.bound != session.activity.instance
        || refresh.inputs.source != session.activity.instance
        || mutation.instanceKey != session.activity.instance
        || (mutation.kind != state::activity::membership::MutationKind::refresh
            && mutation.kind != state::activity::membership::MutationKind::republish)
        || !acquire_region_lineage_locked(
            session, session.activity.lineage, boundLineageLease)) {
        return RegionSnapshotBuildResult::stale;
    }
    state::activity::membership::MembershipState membershipAfter =
        refresh.inputs.sourceMembership;
    if (mutation.kind == state::activity::membership::MutationKind::republish) {
        membershipAfter = mutation.regionTransition.after;
    }
    NotificationMask required = notification_mask(RegionNotification::roster);
    if (!burst) {
        required |= notification_mask(RegionNotification::globalState);
    }
    if (refresh.publishesMembership) {
        required |= notification_mask(RegionNotification::membership);
    }
    RegionPublicationDebt ignored{};
    const RegionSnapshotBuildResult result = finalize_snapshot(
        session,
        scratch,
        refresh.inputs,
        membershipAfter,
        refresh.inputs.sourceHostRegion,
        refresh.inputs.sourceHostRegion,
        required,
        burst,
        false,
        rearmMissionDirector,
        output,
        ignored,
        advertisementLease);
    if (result != RegionSnapshotBuildResult::ready
        && result != RegionSnapshotBuildResult::pending) {
        gameplay::group::release_host_activity_lineage(boundLineageLease);
    }
    return result;
}

/** Rebuilds one exact deferred debt without selecting newer/global State. */
RegionSnapshotBuildResult build_region_debt_snapshot(
    const Session& session,
    Scratch& scratch,
    const RegionPublicationDebt& debt,
    RegionTransitionSnapshot& output,
    gameplay::group::HostActivityLineageLease& advertisementLease,
    gameplay::group::HostActivityLineageLease& boundLineageLease) noexcept {
    output = {};
    gameplay::group::release_host_activity_lineage(advertisementLease);
    gameplay::group::release_host_activity_lineage(boundLineageLease);
    if (!debt.present || !lifecycle::activity_binding_is_current(session)
        || debt.binding != session.activity.key || debt.activity != session.activity.instance
        || debt.activity != session.activity.lineage.bound
        || debt.regionSource != session.activity.lineage.source
        || !acquire_region_lineage_locked(
            session, session.activity.lineage, boundLineageLease)) {
        return RegionSnapshotBuildResult::stale;
    }
    state::activity::membership::RegionSnapshotInputs copied{};
    if (!state::activity::membership::snapshot_region_inputs(debt.activity,
                                                              debt.regionSource,
                                                              debt.committedHostRegion,
                                                              copied)
        || std::memcmp(&copied.sourceMembership,
                       &debt.membershipAfter,
                       sizeof debt.membershipAfter)
               != 0
        || std::memcmp(&copied.destination, &debt.destination, sizeof debt.destination) != 0) {
        gameplay::group::release_host_activity_lineage(boundLineageLease);
        return RegionSnapshotBuildResult::stale;
    }
    RegionPublicationDebt ignored{};
    const RegionSnapshotBuildResult result = finalize_snapshot(session,
                                                               scratch,
                                                               copied,
                                                               debt.membershipAfter,
                                                               debt.committedHostRegion,
                                                               debt.committedHostRegion,
                                                               debt.required,
                                                               false,
                                                               debt.publishesHud,
                                                               false,
                                                               output,
                                                               ignored,
                                                               advertisementLease);
    if (result != RegionSnapshotBuildResult::ready
        && result != RegionSnapshotBuildResult::pending) {
        gameplay::group::release_host_activity_lineage(boundLineageLease);
    }
    return result;
}

} // namespace dawn::server::bap::encrypted::push::activity
