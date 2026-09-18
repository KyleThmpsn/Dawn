#include "server/bap/encrypted/push/activity/deadly_trial_roster.h"
#include "server/bap/encrypted/push/activity/beyond_infinity_roster.h"
#include "server/bap/encrypted/push/activity/deep_storage_roster.h"
#include "server/bap/encrypted/push/activity/gateway_roster.h"
#include "state/build_data/scenarios/scenario_catalog.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace layouts=dawn::state::build_data::scenarios;
namespace wire=dawn::middleware::bap::activity_message::sensor_auth_update;
namespace trial=dawn::server::bap::encrypted::push::activity::deadly_trial_roster;
namespace beyond=dawn::server::bap::encrypted::push::activity::beyond_infinity_roster;
namespace deep=dawn::server::bap::encrypted::push::activity::deep_storage_roster;
namespace gateway=dawn::server::bap::encrypted::push::activity::gateway_roster;
unsigned checks{};
#define CHECK(value) do {++checks;if(!(value)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value);std::exit(1);}} while(false)
struct Storage {
    std::array<layouts::RosterGroup,wire::kGroupCapacity> rosterGroups{};
    std::array<wire::BubbleSubBlock,64> rosterSubBlocks{};
    std::array<std::array<std::uint32_t,96>,64> rosterSubBlockKeys{};
};
template<class Groups,class Prepare,class Admit,class Required>
void run(const char* label,std::uint32_t scenario,std::string_view package,std::uint8_t bubbles,
    std::uint32_t rootKey,const Groups& groups,Prepare prepare,Admit admit,Required required,
    bool hasPrepare=true,std::uint32_t bubbleHash=0) {
    auto rows=std::make_unique<std::vector<layouts::RosterGroup>>(std::size(groups));
    std::vector<std::uint16_t> indices;
    layouts::Definition layout{};layout.tag=scenario;layout.nameLength=static_cast<std::uint8_t>(package.size());
    std::copy(package.begin(),package.end(),layout.name.begin());layout.bubbleCount=bubbles;layout.bubbleHashes[15]=bubbleHash;
    std::size_t root{},requiredCount{};
    for(std::size_t i=0;i<rows->size();++i) {
        const auto& e=groups[i];auto& row=(*rows)[i];row.registryKey=e.key;row.objectTag=e.tag;
        row.slotCount=static_cast<std::uint16_t>(e.slots.size());
        for(std::size_t j=0;j<e.slots.size();++j) {
            const auto& slot=e.slots[j];row.slotTypes[j]=slot.type;row.slotFlags[j]=slot.flags;row.slotIndices[j]=slot.index;
            row.descriptorTags[j]=slot.tag;row.descriptorOffsets[j]=slot.offset;row.componentClasses[j]=slot.component;
            row.senseSchemas[j]=slot.sense;row.authSchemas[j]=slot.auth;
        }
        // Trial reproduces the live root mismatch: hint 649, actual index 743.
        indices.push_back(static_cast<std::uint16_t>(rootKey==trial::native::kRoot?743+i:1700+(rows->size()-1-i)*3));
        CHECK(indices.back()!=e.hint);
        if(e.key==rootKey) root=i;
        if(required(e)) ++requiredCount;
    }
    unsigned copies{},queries{};
    // Reproduce the live Beyond collision before its correct record: same key,
    // different object tags. Other missions exercise this for their root too.
    std::size_t shared=root;
    for(std::size_t i=0;i<rows->size();++i) if((*rows)[i].registryKey==0x34D23982U) shared=i;
    auto decoys=std::make_unique<std::array<layouts::RosterGroup,2>>();
    for(auto& decoy:*decoys) decoy=(*rows)[shared];
    (*decoys)[0].objectTag=(*rows)[shared].objectTag^1U;
    (*decoys)[1].objectTag=(*rows)[shared].objectTag^2U;
    if((*rows)[shared].registryKey==0x34D23982U) {
        (*decoys)[0].objectTag=0x81550188U;(*decoys)[1].objectTag=0x8155213EU;
        CHECK((*rows)[shared].objectTag==0x80F5B493U);
    }
    auto findIndex=[&](std::uint32_t key,std::uint32_t tag,std::uint16_t& out) {
        ++queries;
        for(std::size_t i=0;i<decoys->size();++i) if((*decoys)[i].registryKey==key && (*decoys)[i].objectTag==tag) {out=static_cast<std::uint16_t>(1000+i);return true;}
        for(std::size_t i=0;i<rows->size();++i) if((*rows)[i].registryKey==key && (*rows)[i].objectTag==tag) {out=indices[i];return true;}
        return false;
    };
    auto find=[&](std::size_t index,layouts::RosterGroup& out) {
        ++copies;
        if(index>=1000 && index<1000+decoys->size()) {out=(*decoys)[index-1000];return true;}
        for(std::size_t i=0;i<rows->size();++i) if(indices[i]==index) {out=(*rows)[i];return true;}
        return false;
    };
    auto byKey=[&](std::uint32_t key,std::uint32_t tag,layouts::RosterGroup& out) {
        ++copies;
        for(const auto& row:*decoys) if(row.registryKey==key && row.objectTag==tag) {out=row;return true;}
        for(const auto& row:*rows) if(row.registryKey==key && row.objectTag==tag) {out=row;return true;}
        return false;
    };
    if(hasPrepare) {
        const auto original=layout;
        CHECK(prepare(layout,findIndex,find));CHECK(copies==1);CHECK(queries==1);
        bool published{};for(std::size_t b=0;b<bubbles;++b) if(layout.authoredGroupCounts[b]) {
            CHECK(layout.authoredGroupCounts[b]==1);CHECK(layout.authoredGroups[b][0]==indices[root]);published=true;
        }
        CHECK(published);
        const auto stable=layout;
        for(unsigned repeat=0;repeat<100;++repeat) {
            copies=queries=0;CHECK(prepare(layout,findIndex,find));CHECK(copies==1 && queries==1);
            CHECK(layout.authoredGroups==stable.authoredGroups);CHECK(layout.authoredGroupCounts==stable.authoredGroupCounts);
        }
        auto rejected=original;copies=0;
        CHECK(!prepare(rejected,[](std::uint32_t,std::uint32_t,std::uint16_t&){return false;},find));CHECK(copies==0);
        CHECK(rejected.authoredGroups==original.authoredGroups && rejected.authoredGroupCounts==original.authoredGroupCounts);
        CHECK(!prepare(rejected,[](std::uint32_t,std::uint32_t,std::uint16_t& index){index=layouts::kRosterGroupCapacity;return true;},find));
        CHECK(copies==0);
        (*rows)[root].authSchemas[0]^=1;
        CHECK(!prepare(rejected,findIndex,find));CHECK(rejected.authoredGroups==original.authoredGroups);
        (*rows)[root].authSchemas[0]^=1;
        const auto other=(root+1)%rows->size();
        CHECK(!prepare(rejected,[&](std::uint32_t,std::uint32_t,std::uint16_t& index){index=indices[other];return true;},find));
    }
    auto storage=std::make_unique<Storage>();wire::Roster roster{};
    for(std::size_t i=0;i<rows->size();++i) if(required(groups[i]) && groups[i].topLevel) {
        auto& row=storage->rosterGroups[roster.groupCount];row=(*rows)[i];
        roster.groups[roster.groupCount++]={row.registryKey,std::span(row.slotTypes).first(row.slotCount),
            std::span(row.slotFlags).first(row.slotCount),std::span(row.slotIndices).first(row.slotCount)};
    }
    roster.topLevelGroupCount=roster.groupCount;
    std::uint32_t failed{};copies=0;
    CHECK(admit(layout,*storage,roster,byKey,&failed));CHECK(failed==0);CHECK(copies==requiredCount);
    CHECK(roster.groupCount==requiredCount);
    const auto finalCount=roster.groupCount;
    for(unsigned repeat=0;repeat<25;++repeat) {
        copies=0;CHECK(admit(layout,*storage,roster,byKey,&failed));CHECK(copies==requiredCount);
        CHECK(roster.groupCount==finalCount);
    }
    (*rows)[root].authSchemas[0]^=1;
    CHECK(!admit(layout,*storage,roster,byKey,&failed));CHECK(failed==rootKey);
    (*rows)[root].authSchemas[0]^=1;
    CHECK(!admit(layout,*storage,roster,[](std::uint32_t,std::uint32_t,layouts::RosterGroup&){return false;},&failed));
    layout.tag^=1;copies=0;CHECK(!admit(layout,*storage,roster,byKey,&failed));CHECK(copies==0);
    std::printf("%s: reordered-cache lookup and validation passed; one record copy per requested group\n",label);
}
void catalog_identity_checks() {
    const auto* shared=static_cast<const beyond::native::Group*>(nullptr);
    for(const auto& group:beyond::native::kGroups) if(group.key==0x34D23982U) shared=&group;
    CHECK(shared!=nullptr);CHECK(shared->tag==0x80F5B493U);
    auto rows=std::make_unique<std::vector<layouts::RosterGroup>>(3);
    auto& correct=(*rows)[2];correct.registryKey=shared->key;correct.objectTag=shared->tag;
    correct.slotCount=static_cast<std::uint16_t>(shared->slots.size());
    for(std::size_t j=0;j<shared->slots.size();++j) {
        const auto& slot=shared->slots[j];correct.slotTypes[j]=slot.type;correct.slotFlags[j]=slot.flags;correct.slotIndices[j]=slot.index;
        correct.descriptorTags[j]=slot.tag;correct.descriptorOffsets[j]=slot.offset;correct.componentClasses[j]=slot.component;
        correct.senseSchemas[j]=slot.sense;correct.authSchemas[j]=slot.auth;
    }
    (*rows)[0]=correct;(*rows)[0].objectTag=0x81550188U;
    (*rows)[1]=correct;(*rows)[1].objectTag=0x8155213EU;
    CHECK(layouts::replace({},*rows));
    auto output=std::make_unique<layouts::RosterGroup>();std::uint16_t index{};
    CHECK(layouts::group_by_key(shared->key,*output));CHECK(!beyond::matches(*output,*shared));
    CHECK(layouts::find_group_index(shared->key,shared->tag,index));CHECK(index==2);
    CHECK(layouts::group_by_key(shared->key,shared->tag,*output));CHECK(beyond::matches(*output,*shared));
    CHECK(!layouts::find_group_index(shared->key,0,index));CHECK(index==0);
    CHECK(!layouts::group_by_key(shared->key,0,*output));CHECK(output->registryKey==0);
    std::swap((*rows)[0],(*rows)[2]);CHECK(layouts::replace({},*rows));
    CHECK(layouts::find_group_index(shared->key,shared->tag,index));CHECK(index==0);
    CHECK(layouts::group_by_key(shared->key,shared->tag,*output));CHECK(beyond::matches(*output,*shared));
    rows->push_back((*rows)[0]);CHECK(layouts::replace({},*rows));
    CHECK(!layouts::find_group_index(shared->key,shared->tag,index));CHECK(index==0);
    CHECK(!layouts::group_by_key(shared->key,shared->tag,*output));CHECK(output->registryKey==0);
    layouts::clear();
    std::printf("Production catalog: shared-key identity selection, reordered rows and ambiguity rejection passed\n");
}
int main() {
    catalog_identity_checks();
    run("Deadly Trial",trial::native::kScenario,"adventure_ginger",64,trial::native::kRoot,trial::native::kGroups,
        [](auto& layout,auto index,auto find){return trial::prepare_layout(layout,index,find);},
        [](auto& layout,auto& storage,auto& roster,auto find,auto failed){return trial::admit(layout,storage,roster,find,failed);},
        [](const auto&){return true;});
    run("Beyond Infinity",beyond::native::kScenario,"adventure_vod",20,beyond::native::kRoot,beyond::native::kGroups,
        [](auto& layout,auto index,auto find){return beyond::prepare_layout(layout,index,find);},
        [](auto& layout,auto& storage,auto& roster,auto find,auto failed){return beyond::admit(layout,storage,roster,find,failed);},
        [](const auto& group){return beyond::required(group);});
    run("Deep Storage",deep::native::kScenario,"adventure_whisk",22,deep::native::kRoot,deep::native::kGroups,
        [](auto& layout,auto index,auto find){return deep::prepare_layout(layout,index,find);},
        [](auto& layout,auto& storage,auto& roster,auto find,auto failed){return deep::admit(layout,storage,roster,find,failed);},
        [](const auto& group){return deep::required(group);});
    run("Gateway",gateway::native::kScenario,"mission_abs",20,gateway::native::kRoot,gateway::native::kGroups,
        [](auto&,auto,auto){return true;},
        [](auto& layout,auto& storage,auto& roster,auto find,auto failed){return gateway::admit(layout,storage,roster,find,failed);},
        [](const auto&){return true;},false,gateway::native::kBubbleHash);
    std::printf("Mission roster lookup: %u checks passed\n",checks);
}
