#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../state/activity/omega_intro_rules.h"
#include "../../../../../state/activity/omega_enemy_crown_catalog.h"
#include "../../../../../state/activity/omega_lair_full_roster_catalog.h"
#include "../../../../../state/activity/omega_ending_rules.h"
#include "../../../../../state/build_data/scenarios/definition.h"
#include "native_roster_lifetime_projection.h"

namespace dawn::server::bap::encrypted::push::activity::omega_lair {

namespace layouts = state::build_data::scenarios;
namespace wire = middleware::bap::activity_message::sensor_auth_update;
namespace intro = state::activity::omega_presentation;
namespace crown = state::activity::omega_enemy_crown;

inline constexpr std::uint32_t kScenarioTag = 0x80F47522U;
inline constexpr std::uint32_t kRegistryTag = 0x80F47979U;
inline constexpr std::uint32_t kBubbleHash = 0x23345E71U;
inline constexpr std::size_t kBubble = 14;
inline constexpr std::size_t kIndexHint = 1076;

/** Slice121 is Lighthouse bubble15, authored state1. Its ending overlay is
 * applied to the same base services as slice120; asking the ordinary state0
 * builder for 121 omits the authored HUD root and rejects the terminal packet. */
[[nodiscard]] constexpr std::int32_t roster_region(std::int32_t region,bool retiring) noexcept {
    return retiring && region==state::activity::omega_ending::kSlice
        ? state::activity::omega_ending::kBubble*8 : region;
}

enum class Admission { unrelated, present, added, noCapacity, missingLayout, missingGroup, conflict };
enum class Content { reveal, boss, crown };

/** Complete pinned 80F476C6 descriptors, independent of extraction order.
 * Slots0..48 have client state; the remaining fifteen are authored spawn rules.
 * Every descriptor must survive admission so native group seeding is complete. */
inline constexpr std::array<std::uint8_t,49> kCrownSlotTypes{{
    1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,
    4,4,4,4,23,26,26,4,4,23,23,4,4,4,4,4,4,23,23,23,26,4,70,
    34,34,34,31,31,30,32,32,32,
}};
inline constexpr std::array<std::uint16_t,15> kCrownRuleSlots{{
    68,70,71,72,74,75,76,77,78,79,80,82,83,84,85,
}};
inline constexpr std::size_t kCrownDescriptorCount=kCrownSlotTypes.size()+kCrownRuleSlots.size();

[[nodiscard]] constexpr bool crown_descriptors(std::span<const std::uint8_t> types,
    std::span<const std::uint8_t> flags,std::span<const std::uint16_t> indices) noexcept {
    if(types.size()!=kCrownDescriptorCount || flags.size()!=types.size()
        || indices.size()!=types.size()) { return false; }
    std::array<bool,kCrownDescriptorCount> seen{};
    for(std::size_t i=0;i<types.size();++i) {
        const auto slot=indices[i];
        std::size_t ordinal=slot;
        if(slot<kCrownSlotTypes.size()) {
            const auto expectedFlags=((slot>=40 && slot<=44) || slot>=46)?2U:3U;
            if(types[i]!=kCrownSlotTypes[slot] || flags[i]!=expectedFlags) { return false; }
        } else {
            const auto rule=std::find(kCrownRuleSlots.begin(),kCrownRuleSlots.end(),slot);
            if(rule==kCrownRuleSlots.end() || types[i]!=66 || flags[i]!=0) { return false; }
            ordinal=kCrownSlotTypes.size()+static_cast<std::size_t>(rule-kCrownRuleSlots.begin());
        }
        if(seen[ordinal]) { return false; }
        seen[ordinal]=true;
    }
    return true;
}

/** The v48 catalog contains this complete group but drops the Lair's authored overlay when
 * it exceeds the four-group extraction limit. Omega already uses all four cached bubble
 * slots after adding the Forest. Extra wire-only slots hold the verified reveal,
 * boss and first Crown groups for the whole run, with their original Lair scope. Changing
 * the group set on arrival would rebuild every mission object and interrupt presentation.
 * Storage is the caller's packet scratch; none of the returned spans refer to a local. */
template<class Storage, class FindGroup>
[[nodiscard]] Admission admit_group(const layouts::Definition& layout, Storage& storage,
                                    wire::Roster& roster, FindGroup findGroup, Content content) noexcept {
    if (layout.nameLength > layout.name.size() || layout.tag != kScenarioTag
        || std::string_view(layout.name.data(), layout.nameLength) != "mission_scot") {
        return Admission::unrelated;
    }
    if (layout.bubbleCount <= kBubble || layout.bubbleHashes[kBubble] != kBubbleHash) {
        return Admission::missingLayout;
    }
    const bool boss = content == Content::boss;
    const bool firstCrown = content == Content::crown;
    const auto registry = firstCrown ? crown::kRegistry : boss ? intro::kBossRegistry : intro::kIntroRegistry;
    const auto tag = firstCrown ? crown::kRegistryDefinition : boss ? 0x80F475EEU : kRegistryTag;
    const auto hint = firstCrown ? 1072U : boss ? 1071U : kIndexHint;
    const auto type = boss ? 1U : 6U;
    const auto slotIndex = boss ? 0U : intro::kIntroSlot;
    const auto blocks = roster.bubbleSubBlocks;
    std::size_t blockIndex = blocks.size();
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (blocks[index].bubble == kBubble) { blockIndex = index; break; }
    }
    bool hasGroup = false;
    const wire::Group* existingCrown{};
    std::size_t crownGroupCount{};
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        hasGroup = hasGroup || roster.groups[index].key == registry;
        if(firstCrown && roster.groups[index].key==registry) {
            existingCrown=&roster.groups[index];
            ++crownGroupCount;
        }
    }
    const bool hasKey = blockIndex < blocks.size()
        && std::find(blocks[blockIndex].keys.begin(), blocks[blockIndex].keys.end(),
                     registry) != blocks[blockIndex].keys.end();
    if(firstCrown) {
        std::size_t crownKeyCount{};
        for(const auto& block:blocks) {
            for(const auto key:block.keys) {
                if(key!=registry) { continue; }
                if(block.bubble!=kBubble) { return Admission::conflict; }
                ++crownKeyCount;
            }
        }
        if(crownGroupCount!=0 || crownKeyCount!=0) {
            if(crownGroupCount!=1 || crownKeyCount!=1 || !hasKey || existingCrown==nullptr
                || !crown_descriptors(existingCrown->slotTypes,existingCrown->slotFlags,
                                      existingCrown->slotIndices)) { return Admission::conflict; }
            return Admission::present;
        }
    }
    if (hasGroup || hasKey) {
        return hasGroup && hasKey ? Admission::present : Admission::conflict;
    }
    const std::size_t keyCount = blockIndex < blocks.size() ? blocks[blockIndex].keys.size() : 0;
    if (roster.groupCount >= roster.groups.size() || roster.groupCount >= storage.rosterGroups.size()
        || blockIndex >= storage.rosterSubBlocks.size()
        || blockIndex >= storage.rosterSubBlockKeys.size()
        || keyCount >= storage.rosterSubBlockKeys[blockIndex].size()) {
        return Admission::noCapacity;
    }
    auto& group = storage.rosterGroups[roster.groupCount];
    const auto matches = [&group, registry, tag, type, slotIndex, boss, firstCrown]() noexcept {
        if (group.registryKey != registry || group.objectTag != tag
            || group.slotCount == 0 || group.slotCount > group.slotTypes.size()) { return false; }
        if (firstCrown) {
            return crown_descriptors(std::span(group.slotTypes).first(group.slotCount),
                std::span(group.slotFlags).first(group.slotCount),
                std::span(group.slotIndices).first(group.slotCount));
        }
        if (boss) {
            bool rule = false, member = false;
            for (std::size_t i = 0; i < group.slotCount; ++i) {
                rule = rule || (group.slotTypes[i] == 66 && group.slotIndices[i] == intro::kBossSpawnRule);
                member = member || (group.slotTypes[i] == 2 && group.slotIndices[i] == 1
                    && (group.slotFlags[i] & layouts::kSlotAuthFlag) != 0);
            }
            if (!rule || !member) { return false; }
        }
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            if (group.slotTypes[slot] == type && group.slotIndices[slot] == slotIndex
                && (group.slotFlags[slot] & layouts::kSlotAuthFlag) != 0) { return true; }
        }
        return false;
    };
    bool found = findGroup(hint, group) && matches();
    // The hint is only an optimization. Cache order can change; identity must still match.
    for (std::size_t index = 0; !found && index < layouts::kRosterGroupCapacity; ++index) {
        if (!findGroup(index, group)) { break; }
        found = matches();
    }
    if (!found) { return Admission::missingGroup; }

    auto& keys = storage.rosterSubBlockKeys[blockIndex];
    if (blockIndex < blocks.size() && blocks[blockIndex].keys.data() != keys.data()) {
        std::copy(blocks[blockIndex].keys.begin(), blocks[blockIndex].keys.end(), keys.begin());
    }
    keys[keyCount] = registry;
    storage.rosterSubBlocks[blockIndex] = {
        static_cast<std::uint32_t>(kBubble), std::span(keys).first(keyCount + 1)};
    auto& output = roster.groups[roster.groupCount++];
    output.key = group.registryKey;
    output.slotTypes = std::span(group.slotTypes).first(group.slotCount);
    output.slotFlags = std::span(group.slotFlags).first(group.slotCount);
    output.slotIndices = std::span(group.slotIndices).first(group.slotCount);
    roster.bubbleSubBlocks = std::span(storage.rosterSubBlocks).first(
        blocks.size() + (blockIndex == blocks.size() ? 1U : 0U));
    return Admission::added;
}

template<class Storage, class FindGroup>
[[nodiscard]] Admission admit(const layouts::Definition& layout, Storage& storage,
                              wire::Roster& roster, FindGroup findGroup, bool boss = false) noexcept {
    return admit_group(layout,storage,roster,findGroup,boss?Content::boss:Content::reveal);
}

template<class Storage, class FindGroup>
[[nodiscard]] Admission admit_crown(const layouts::Definition& layout, Storage& storage,
                                    wire::Roster& roster, FindGroup findGroup) noexcept {
    return admit_group(layout,storage,roster,findGroup,Content::crown);
}

/** Exact complete client descriptors, including host-only rule references with zero flags. */
[[nodiscard]] inline bool full_descriptors(const wire::Group& actual,
    const state::activity::omega_lair_full_roster::Group& expected) noexcept {
    if(actual.key!=expected.key || actual.slotTypes.size()!=expected.slots.size()
        || actual.slotFlags.size()!=expected.slots.size()
        || actual.slotIndices.size()!=expected.slots.size()) { return false; }
    for(const auto& slot:expected.slots) {
        std::size_t count{};
        for(std::size_t i=0;i<actual.slotTypes.size();++i) {
            if(actual.slotIndices[i]!=slot.index) { continue; }
            if(actual.slotTypes[i]!=slot.type || actual.slotFlags[i]!=slot.flags) { return false; }
            ++count;
        }
        if(count!=1) { return false; }
    }
    return true;
}

/** Admit all later combat content from the start, so a wave never changes the roster epoch. */
template<class Storage,class FindGroup>
[[nodiscard]] Admission admit_full(const layouts::Definition& layout,Storage& storage,
    wire::Roster& roster,FindGroup findGroup,
    const state::activity::omega_lair_full_roster::Group& expected) noexcept {
    if(layout.nameLength>layout.name.size() || layout.tag!=kScenarioTag
        || std::string_view(layout.name.data(),layout.nameLength)!="mission_scot") {
        return Admission::unrelated;
    }
    if(expected.bubble!=kBubble || layout.bubbleCount<=kBubble
        || layout.bubbleHashes[kBubble]!=kBubbleHash) { return Admission::missingLayout; }
    const auto blocks=roster.bubbleSubBlocks;
    std::size_t blockIndex=blocks.size(),keyCount{},groupCount{};
    const wire::Group* existing{};
    for(std::size_t i=0;i<blocks.size();++i) {
        if(blocks[i].bubble==expected.bubble) {
            if(blockIndex!=blocks.size()) { return Admission::conflict; }
            blockIndex=i;
        }
        for(const auto key:blocks[i].keys) {
            if(key!=expected.key) { continue; }
            if(blocks[i].bubble!=expected.bubble) { return Admission::conflict; }
            ++keyCount;
        }
    }
    for(std::size_t i=0;i<roster.groupCount;++i) {
        if(roster.groups[i].key==expected.key) { ++groupCount;existing=&roster.groups[i]; }
    }
    if(groupCount || keyCount) {
        return groupCount==1 && keyCount==1 && existing && full_descriptors(*existing,expected)
            ? Admission::present : Admission::conflict;
    }
    const auto keysBefore=blockIndex<blocks.size()?blocks[blockIndex].keys.size():0U;
    if(roster.groupCount>=roster.groups.size() || roster.groupCount>=storage.rosterGroups.size()
        || blockIndex>=storage.rosterSubBlocks.size()
        || blockIndex>=storage.rosterSubBlockKeys.size()
        || keysBefore>=storage.rosterSubBlockKeys[blockIndex].size()) { return Admission::noCapacity; }
    auto& group=storage.rosterGroups[roster.groupCount];
    const auto matches=[&]() noexcept {
        if(group.registryKey!=expected.key || group.objectTag!=expected.tag
            || group.slotCount>group.slotTypes.size()) { return false; }
        return full_descriptors({group.registryKey,
            std::span(group.slotTypes).first(group.slotCount),
            std::span(group.slotFlags).first(group.slotCount),
            std::span(group.slotIndices).first(group.slotCount)},expected);
    };
    bool found=findGroup(expected.hint,group) && matches();
    for(std::size_t i=0;!found && i<layouts::kRosterGroupCapacity;++i) {
        if(!findGroup(i,group)) { break; }
        found=matches();
    }
    if(!found) { return Admission::missingGroup; }
    auto& keys=storage.rosterSubBlockKeys[blockIndex];
    if(blockIndex<blocks.size() && blocks[blockIndex].keys.data()!=keys.data()) {
        std::copy(blocks[blockIndex].keys.begin(),blocks[blockIndex].keys.end(),keys.begin());
    }
    keys[keysBefore]=expected.key;
    storage.rosterSubBlocks[blockIndex]={expected.bubble,std::span(keys).first(keysBefore+1)};
    roster.groups[roster.groupCount++]={group.registryKey,
        std::span(group.slotTypes).first(group.slotCount),
        std::span(group.slotFlags).first(group.slotCount),
        std::span(group.slotIndices).first(group.slotCount)};
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(
        blocks.size()+(blockIndex==blocks.size()?1U:0U));
    return Admission::added;
}

inline constexpr std::array<std::uint8_t,wire::kBubbleKeyCapacity> kRetiredKeyPresence{};
inline constexpr auto kEndingKeyPresence=[] {
    std::array<std::uint8_t,wire::kBubbleKeyCapacity> values{};
    values.back()=1;
    return values;
}();

/** Native 3CCE50 compares presence at the same bubble/key ordinal and only walks
 * the incoming count. Retain retired keys with zero presence rather than dropping
 * their blocks, then append the bookend to its existing bubble. Only the global
 * services and bookend receive phase-2 bodies. The removal must apply in the Lair
 * before its native bubble changes; this function does not acknowledge that. */
template<class Storage>
[[nodiscard]] bool terminal_roster(Storage& storage,wire::Roster& roster,
    std::uint8_t authoredState) noexcept {
    namespace ending=state::activity::omega_ending;
    if(authoredState!=ending::kState || roster.topLevelGroupCount!=3 || roster.groupCount<3
        || roster.groups[0].key!=0x4786C0E0U || roster.groups[1].key!=0x29D7B029U
        || roster.groups[2].key!=0x82FB58B7U) { return false; }
    const auto blocks=roster.bubbleSubBlocks;
    std::size_t endingBlock=blocks.size();
    bool hasEnding{};
    for(std::size_t i=0;i<blocks.size();++i) {
        const auto& block=blocks[i];
        if(i>=storage.rosterSubBlocks.size() || i>=storage.rosterSubBlockKeys.size()
            || block.keys.empty() || block.keys.size()>wire::kBubbleKeyCapacity
            || block.keys.size()>storage.rosterSubBlockKeys[i].size()) { return false; }
        for(std::size_t earlier=0;earlier<i;++earlier) {
            if(blocks[earlier].bubble==block.bubble) { return false; }
        }
        if(block.bubble==ending::kBubble) { endingBlock=i; }
        for(std::size_t key=0;key<block.keys.size();++key) {
            if(block.keys[key]!=ending::kRegistry) { continue; }
            // Our prior publication always appends this key exactly once.
            if(hasEnding || block.bubble!=ending::kBubble || key+1!=block.keys.size()) {
                return false;
            }
            hasEnding=true;
        }
    }
    if(endingBlock>=storage.rosterSubBlocks.size()
        || endingBlock>=storage.rosterSubBlockKeys.size()) { return false; }
    const auto endingCount=endingBlock<blocks.size()?blocks[endingBlock].keys.size():0U;
    const auto nextEndingCount=endingCount+(hasEnding?0U:1U);
    if(nextEndingCount>wire::kBubbleKeyCapacity
        || nextEndingCount>storage.rosterSubBlockKeys[endingBlock].size()) { return false; }
    for(std::size_t i=0;i<blocks.size();++i) {
        const auto block=blocks[i];
        auto& keys=storage.rosterSubBlockKeys[i];
        if(block.keys.data()!=keys.data()) {
            std::copy(block.keys.begin(),block.keys.end(),keys.begin());
        }
        storage.rosterSubBlocks[i]={block.bubble,std::span(keys).first(block.keys.size()),
            std::span(kRetiredKeyPresence).first(block.keys.size()),block.states};
    }
    auto& endingKeys=storage.rosterSubBlockKeys[endingBlock];
    if(!hasEnding) { endingKeys[endingCount]=ending::kRegistry; }
    storage.rosterSubBlocks[endingBlock]={ending::kBubble,
        std::span(endingKeys).first(nextEndingCount),
        std::span(kEndingKeyPresence).last(nextEndingCount)};
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(
        blocks.size()+(endingBlock==blocks.size()?1U:0U));
    auto& group=storage.rosterGroups[3];
    group={};group.registryKey=ending::kRegistry;group.objectTag=ending::kRegistryTag;
    group.slotCount=1;group.slotTypes[0]=6;group.slotFlags[0]=2;group.slotIndices[0]=0;
    roster.groups[3]={group.registryKey,std::span(group.slotTypes).first(1),
        std::span(group.slotFlags).first(1),std::span(group.slotIndices).first(1)};
    roster.groupCount=4;
    return true;
}

/** Rebind the copied lifetime owner before applying the ending overlay. A
 * later ordinary projection would restore presence and silently cancel native
 * cleanup. The retained lifetime value stays unchanged so refresh planning can
 * still use its ordinary travel rules; retirement is reapplied to every packet. */
template<class Storage>
[[nodiscard]] bool finalize_retained_roster(Storage& storage,wire::Snapshot& snapshot,
    const roster_lifetime::State& lifetimes) noexcept {
    if(lifetimes.identity.owner
        && !roster_lifetime::project(lifetimes,snapshot.roster,storage.rosterSubBlocks))return false;
    return !snapshot.omegaEndingRetire || terminal_roster(storage,snapshot.roster,
        state::activity::omega_ending::kState);
}

} // namespace dawn::server::bap::encrypted::push::activity::omega_lair
