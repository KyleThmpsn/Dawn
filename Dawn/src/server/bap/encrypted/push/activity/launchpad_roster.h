#pragma once
#include "../../../../../state/activity/Newlight/launchpad/native_catalog.h"
#include "../../../../../state/activity/Newlight/launchpad/cinematics.h"
#include "../../../../../state/build_data/scenarios/definition.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"

namespace dawn::server::bap::encrypted::push::activity::launchpad_roster {
namespace native=state::activity::newlight::launchpad;
namespace layouts=state::build_data::scenarios;
namespace wire=middleware::bap::activity_message::sensor_auth_update;
static_assert(std::size(native::kGroups)<=wire::kGroupCapacity);
// Only the private mission publishes its scripts, encounters and bookends.
// The derived public session retains shared navigation and player registries.
inline constexpr bool owns_key(std::uint32_t key) noexcept {
    for(const auto& group:native::kGroups) if(group.key==key) {
        if(group.topLevel) {return key==native::kRoot;}
        for(const auto& slot:group.slots) {if(slot.asset.type!=13) {return true;}}
    }
    return false;
}
inline bool matches(const layouts::RosterGroup& actual,const native::Group& expected) noexcept {
    if(actual.registryKey!=expected.key || actual.objectTag!=expected.tag || actual.slotCount!=expected.slots.size()) {return false;}
    for(std::size_t i=0;i<expected.slots.size();++i) {
        const auto& s=expected.slots[i];
        if(actual.slotTypes[i]!=s.asset.type || actual.slotIndices[i]!=s.asset.slot || actual.slotFlags[i]!=native::slot_flags(s)
            || actual.descriptorTags[i]!=s.asset.definition || actual.descriptorOffsets[i]!=s.offset
            || actual.componentClasses[i]!=s.component || actual.senseSchemas[i]!=s.sense || actual.authSchemas[i]!=s.authority) {return false;}
    }return true;
}
inline void recovered(layouts::RosterGroup& out,const native::Group& group) noexcept {
    out={};out.registryKey=group.key;out.objectTag=group.tag;out.slotCount=static_cast<std::uint16_t>(group.slots.size());
    for(std::size_t i=0;i<group.slots.size();++i) {
        const auto& s=group.slots[i];out.slotTypes[i]=static_cast<std::uint8_t>(s.asset.type);out.slotIndices[i]=s.asset.slot;out.slotFlags[i]=native::slot_flags(s);
        out.descriptorTags[i]=s.asset.definition;out.descriptorOffsets[i]=s.offset;out.componentClasses[i]=s.component;out.senseSchemas[i]=s.sense;out.authSchemas[i]=s.authority;
    }
}
// Cached ordinary and root groups authenticate the activity. The small native
// ambient and cinematic overlays omitted by the cache retain their package schema.
template<class Storage,class Find>
bool admit(const layouts::Definition& layout,Storage& storage,wire::Roster& roster,Find find,std::uint32_t& failedKey) noexcept {
    if(layout.tag!=native::kScenario || layout.bubbleCount!=4 || layout.nameLength!=native::kPackage.size()
        || std::string_view(layout.name.data(),layout.nameLength)!=native::kPackage || !roster.playerKeyGroup) {return false;}
    const auto player=roster.playerKeyGroup;roster={};roster.playerKeyGroup=player;
    for(bool top:{true,false}) for(const auto& expected:native::kGroups) {
        if(expected.topLevel!=top) {continue;}failedKey=expected.key;
        if(roster.groupCount>=storage.rosterGroups.size() || roster.groupCount>=roster.groups.size()) {return false;}
        auto& group=storage.rosterGroups[roster.groupCount];
        if(expected.hint==UINT32_MAX) {recovered(group,expected);}
        else {
            if(!find(expected.key,expected.tag,group) || !matches(group,expected)) {return false;}
        }
        roster.groups[roster.groupCount++]={group.registryKey,std::span(group.slotTypes).first(group.slotCount),std::span(group.slotFlags).first(group.slotCount),std::span(group.slotIndices).first(group.slotCount)};
        if(top) {++roster.topLevelGroupCount;}
    }
    for(std::size_t b=0;b<4;++b) {
        auto& keys=storage.rosterSubBlockKeys[b];std::size_t count{};
        for(const auto& g:native::kGroups) if(!g.topLevel && g.bubble==b) {if(count==keys.size()) {return false;}keys[count++]=g.key;}
        storage.rosterSubBlocks[b]={static_cast<std::uint32_t>(b),std::span(keys).first(count)};
    }
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(4);failedKey=0;return true;
}
inline constexpr auto kPresence=[] {
    std::array<std::array<std::array<std::uint8_t,wire::kBubbleKeyCapacity>,4>,8> masks{};
    for(unsigned phase=0;phase<8;++phase) for(unsigned bubble=0;bubble<4;++bubble) {
        std::size_t i{};
        for(const auto& group:native::kGroups) if(!group.topLevel && group.bubble==bubble) {
            int movie=-1;for(unsigned n=0;n<3;++n) {if(native::cinematics::kMovies[n].registry==group.key) {movie=static_cast<int>(n);}}
            masks[phase][bubble][i++]=static_cast<std::uint8_t>(movie>=0?(phase%4)==static_cast<unsigned>(movie)+1:phase<4);
        }
    }return masks;
}();
template<class Storage> void movies(Storage& storage,wire::Roster& roster,const native::cinematics::State& state) noexcept {
    namespace cine=native::cinematics;
    const bool selected=state.phase==cine::Phase::preparing || state.phase==cine::Phase::offered || state.phase==cine::Phase::playing || state.phase==cine::Phase::stopping;
    // Like 1AU, retire gameplay before C9 de-instantiates its slice set. Waiting
    // for playback leaves the old simulation alive during the cinematic swap.
    // Keep every key/slot ordinal: native cleanup still walks the old entries.
    const unsigned phase=state.retiring()?4U:(state.ending()?4U:0U)+(selected?state.movie+1U:0U);
    // C9 reactivates the activity and clears its sync-record pool (3CDB80 ->
    // 4DA230). Retire the globals as well, before that clear, so their registry
    // links cannot retain freed records. Arrival re-admits the same ordinals.
    static constexpr std::array<std::uint8_t,wire::kGroupCapacity> retired{};
    static constexpr auto globals=[] {
        std::array<std::uint32_t,3> keys{};std::size_t i{};
        for(const auto& group:native::kGroups) if(group.topLevel) {keys[i++]=group.key;}
        return keys;
    }();
    roster.topLevelKeys=globals;
    roster.topLevelPresence=state.retiring()?std::span(retired).first(roster.topLevelGroupCount):std::span<const std::uint8_t>{};
    for(std::size_t b=0;b<4;++b) {auto& block=storage.rosterSubBlocks[b];block.presence=std::span(kPresence[phase][b]).first(block.keys.size());}
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(4);
}
// Gateway's briefing is an authored region overlay omitted by the cache.
template<class Storage> bool gateway(Storage& storage,wire::Roster& roster) noexcept {
    if(!roster.playerKeyGroup || !roster.topLevelGroupCount || roster.topLevelGroupCount>=roster.groups.size()) return false;
    const auto top=roster.topLevelGroupCount;
    auto& group=storage.rosterGroups.back();group={};
    group.registryKey=0x15F0F5F9U;group.objectTag=0x80B4A077U;group.slotCount=1;
    group.slotTypes[0]=6;group.slotFlags[0]=2;group.slotIndices[0]=0;
    group.descriptorTags[0]=0x80B4A11DU;group.descriptorOffsets[0]=0x70;
    group.componentClasses[0]=0x80804F06U;group.senseSchemas[0]=UINT32_MAX;group.authSchemas[0]=0x80804F08U;
    roster.groups[top]={group.registryKey,std::span(group.slotTypes).first(1),std::span(group.slotFlags).first(1),std::span(group.slotIndices).first(1)};
    roster.groupCount=top+1;
    storage.rosterSubBlockKeys[0][0]=group.registryKey;
    storage.rosterSubBlocks[0]={1,std::span(storage.rosterSubBlockKeys[0]).first(1)};
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(1);return true;
}
// Tower Approach's bookend is an authored region overlay omitted by the cache.
// Keep the existing globals/player group and own this one movie until its native stop.
template<class Storage> bool approach(Storage& storage,wire::Roster& roster) noexcept {
    if(!roster.playerKeyGroup || !roster.topLevelGroupCount || roster.topLevelGroupCount>=roster.groups.size()) {return false;}
    const auto top=roster.topLevelGroupCount;
    auto& group=storage.rosterGroups.back();group={};
    group.registryKey=0x32DDAD77U;group.objectTag=0x80B4A0F9U;group.slotCount=1;
    group.slotTypes[0]=6;group.slotFlags[0]=2;group.slotIndices[0]=0;
    group.descriptorTags[0]=0x80B4A0F6U;group.descriptorOffsets[0]=0x2E8;
    group.componentClasses[0]=0x80804F06U;group.senseSchemas[0]=UINT32_MAX;group.authSchemas[0]=0x80804F08U;
    roster.groups[top]={group.registryKey,std::span(group.slotTypes).first(1),std::span(group.slotFlags).first(1),std::span(group.slotIndices).first(1)};
    roster.groupCount=top+1;
    storage.rosterSubBlockKeys[0][0]=group.registryKey;
    storage.rosterSubBlocks[0]={3,std::span(storage.rosterSubBlockKeys[0]).first(1)};
    roster.bubbleSubBlocks=std::span(storage.rosterSubBlocks).first(1);return true;
}

}
