#pragma once
#include <algorithm>
#include "../../../../../state/activity/beyond_infinity/native_catalog.h"
#include "../../../../../state/build_data/scenarios/definition.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
namespace dawn::server::bap::encrypted::push::activity::beyond_infinity_roster {
namespace native=state::activity::beyond_infinity;
namespace layouts=state::build_data::scenarios;
namespace wire=middleware::bap::activity_message::sensor_auth_update;
[[nodiscard]] inline bool matches(const layouts::RosterGroup& actual,const native::Group& expected) noexcept {
    if(actual.registryKey!=expected.key || actual.objectTag!=expected.tag
        || actual.slotCount!=expected.slots.size() || actual.slotCount>actual.slotTypes.size()) { return false; }
    for(const auto& s:expected.slots) {
        unsigned count{};
        for(std::size_t i=0;i<actual.slotCount;++i) {
            if(actual.slotIndices[i]!=s.index) { continue; }
            if(actual.slotTypes[i]!=s.type || actual.slotFlags[i]!=s.flags || actual.descriptorTags[i]!=s.tag
                || actual.descriptorOffsets[i]!=s.offset || actual.componentClasses[i]!=s.component
                || actual.senseSchemas[i]!=s.sense || actual.authSchemas[i]!=s.auth) { return false; }
            ++count;
        }
        if(count!=1) { return false; }
    }
    return true;
}
// Preserve the existing wire capacity. Lua owns directive triggers; groups
// containing only type-31 trigger callbacks have no authority to publish.
// Native Scene cast, objects, generators and shared services remain registered.
inline constexpr bool required(const native::Group& group) noexcept {
    for(const auto& slot:group.slots) { if(slot.type!=31) { return true; } }return false;
}
inline constexpr std::size_t kRequiredGroups=[] { std::size_t n{};for(const auto& g:native::kGroups) { if(required(g)) { ++n; } }return n; }();
static_assert(kRequiredGroups+2<=wire::kGroupCapacity); // two ordinary package roots
inline constexpr std::uint64_t kRootBubbles=0xFFFFFULL;
template<class FindIndex,class FindGroup>
[[nodiscard]] bool prepare_layout(layouts::Definition& layout,FindIndex findIndex,FindGroup find) noexcept {
    if(layout.tag!=native::kScenario || layout.nameLength!=13
        || std::string_view(layout.name.data(),layout.nameLength)!="adventure_vod"
        || layout.bubbleCount!=20) { return false; }
    const native::Group* root{};
    for(const auto& group:native::kGroups) { if(group.key==native::kRoot) { if(root) { return false; }root=&group; } }
    if(!root) { return false; }
    // Cache extraction order is not a stable mission identity. Resolve the
    // key first, then copy and validate only its matching descriptor record.
    layouts::RosterGroup verified{};std::uint16_t index{};
    if(!findIndex(root->key,root->tag,index) || index>=layouts::kRosterGroupCapacity
        || !find(index,verified) || !matches(verified,*root)) { return false; }
    layout.authoredGroupCounts={};layout.authoredGroups={};
    for(std::size_t bubble=0;bubble<layout.bubbleCount;++bubble) {
        if((kRootBubbles&(1ULL<<bubble))==0) { continue; }
        layout.authoredGroupCounts[bubble]=1;
        layout.authoredGroups[bubble][0]=static_cast<std::uint16_t>(index);
    }
    return true;
}
[[nodiscard]] inline bool wire_matches(const wire::Group& actual,const native::Group& expected) noexcept {
    if(actual.key!=expected.key || actual.slotTypes.size()!=expected.slots.size()
        || actual.slotFlags.size()!=expected.slots.size() || actual.slotIndices.size()!=expected.slots.size()) { return false; }
    for(const auto& s:expected.slots) {
        unsigned count{};
        for(std::size_t i=0;i<actual.slotTypes.size();++i) {
            if(actual.slotIndices[i]==s.index) {
                if(actual.slotTypes[i]!=s.type || actual.slotFlags[i]!=s.flags) { return false; }
                ++count;
            }
        }
        if(count!=1) { return false; }
    }
    return true;
}
// The caller discards the entire scratch snapshot on failure. No cache mutation,
// no pointers to temporary storage, and no group changes at encounter boundaries.
template<class Storage,class FindGroupByKey>
[[nodiscard]] bool admit(const layouts::Definition& layout,Storage& storage,wire::Roster& roster,FindGroupByKey findByKey,std::uint32_t* failedKey=nullptr) noexcept {
    if(failedKey) { *failedKey=0; }
    if(layout.tag!=native::kScenario || layout.nameLength>layout.name.size()
        || std::string_view(layout.name.data(),layout.nameLength)!="adventure_vod"
        || layout.bubbleCount!=20
        || roster.groupCount>roster.groups.size() || roster.topLevelGroupCount>roster.groupCount) { return false; }
    for(const auto& expected:native::kGroups) {
        if(!required(expected)) { continue; }
        if(failedKey) { *failedKey=expected.key; }
        std::size_t foundIndex=roster.groupCount;unsigned count{};
        for(std::size_t i=0;i<roster.groupCount;++i) {
            if(roster.groups[i].key==expected.key) { foundIndex=i;++count; }
        }
        if(count>1) { return false; }
        // All top-level services must already come from the normal native layout.
        if(expected.topLevel && (count!=1 || foundIndex>=roster.topLevelGroupCount)) { return false; }
        if(!expected.topLevel && count && foundIndex<roster.topLevelGroupCount) { return false; }
        if(roster.groupCount>=storage.rosterGroups.size()) { return false; }
        // For an existing group use a temporary; don't overwrite scratch backing
        // another published group's spans. The temporary never escapes.
        layouts::RosterGroup check{};
        auto& group=count?check:storage.rosterGroups[roster.groupCount];
        if(!findByKey(expected.key,expected.tag,group) || !matches(group,expected)) { return false; }
        std::size_t block=roster.bubbleSubBlocks.size();unsigned keys{},blocks{};
        for(std::size_t i=0;i<roster.bubbleSubBlocks.size();++i) {
            const auto& b=roster.bubbleSubBlocks[i];
            if(b.bubble==expected.bubble) { block=i;++blocks; }
            for(const auto key:b.keys) {
                if(key==expected.key) { if(expected.topLevel || b.bubble!=expected.bubble) { return false; } ++keys; }
            }
        }
        if(blocks>1 || keys>1 || (count && keys!=(expected.topLevel?0U:1U)) || (!count && keys)) { return false; }
        if(count) { if(!wire_matches(roster.groups[foundIndex],expected)) { return false; }continue; }
        const auto before=block<roster.bubbleSubBlocks.size()?roster.bubbleSubBlocks[block].keys.size():0;
        if(roster.groupCount>=roster.groups.size() || block>=storage.rosterSubBlocks.size()
            || block>=storage.rosterSubBlockKeys.size() || before>=storage.rosterSubBlockKeys[block].size()) { return false; }
        auto& keyStorage=storage.rosterSubBlockKeys[block];
        if(before && roster.bubbleSubBlocks[block].keys.data()!=keyStorage.data()) {
            std::copy(roster.bubbleSubBlocks[block].keys.begin(),roster.bubbleSubBlocks[block].keys.end(),keyStorage.begin());
        }
        const auto blockCount=roster.bubbleSubBlocks.size()+(block==roster.bubbleSubBlocks.size()?1:0);
        keyStorage[before]=expected.key;storage.rosterSubBlocks[block]={expected.bubble,std::span(keyStorage).first(before+1)};
        roster.groups[roster.groupCount++]={group.registryKey,std::span(group.slotTypes).first(group.slotCount),
            std::span(group.slotFlags).first(group.slotCount),std::span(group.slotIndices).first(group.slotCount)};
        roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(blockCount);
    }
    if(failedKey) { *failedKey=0; }
    return true;
}
}
