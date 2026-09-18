#include "state/activity/deadly_trial/controller.h"
#include "state/activity/deadly_trial/authority.h"
#include "state/activity/deadly_trial/ending_audio.h"
#include "client/hooks/bootflow/deadly_trial_presentation.h"
#include "state/activity/forced/prelaunch_profile.h"
#include "client/hooks/bootflow/deadly_trial_revival.h"
#include "server/bap/encrypted/push/activity/deadly_trial_roster.h"
#include "middleware/encoding/bit_writer.h"
#include "state/build_data/cache/records/codec.h"
#include <cstring>
#include "fixtures/mission_semantics.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <algorithm>
#include <map>
#include <vector>
#include <limits>
#include "client/hooks/bootflow/coo_native_player_mount.h"
#include "fixtures/deadly_trial_lifetime_fixture.h"
#include "fixtures/deadly_trial_presentation_fixture.h"
namespace t=dawn::state::activity::deadly_trial;
namespace c=dawn::state::activity::coo;
namespace r=dawn::server::bap::encrypted::push::activity::deadly_trial_roster;
namespace wire=dawn::middleware::bap::activity_message::sensor_auth_update;
namespace bits=dawn::middleware::encoding::bits;
unsigned checks{};
#define CHECK(x) do { ++checks;if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1); } } while(false)
void presentation_binding() {
    namespace binding=dawn::client::hooks::bootflow::deadly_trial_lifetime;
    using F=TrialPresentationFixture;
    F f;const auto nativeBody=f.get<std::uint32_t>(F::life+0x180);
    CHECK(binding::repair_presentation(f,f,F::roster)==binding::Result::repaired);
    CHECK(f.writes==3);CHECK(f.get<std::uint32_t>(F::root+0x1C)==F::owner);
    CHECK(f.get<std::uint32_t>(F::life+0x170)==F::sync);
    CHECK(f.get<std::uint32_t>(F::objective+0x170)==F::objectiveSync);
    CHECK(f.get<std::uint32_t>(F::life+0x180)==nativeBody);
    CHECK(f.get<std::uint32_t>(F::objective+0x180)==0xDA95AE49U);
    CHECK(binding::repair_presentation(f,f,F::roster)==binding::Result::unchanged);CHECK(f.writes==3);
    f.set(F::root+0x1C,UINT32_MAX);
    CHECK(binding::repair_presentation(f,f,F::roster)==binding::Result::repaired);CHECK(f.writes==4);
    f.set(F::objective+0x170,UINT32_MAX);
    CHECK(binding::repair_presentation(f,f,F::roster)==binding::Result::repaired);
    for(unsigned variant=0;variant<18;++variant) {
        F bad;
        switch(variant) {
        case 0:bad.set(F::activity+0x24,0x80F46DB0U);break;
        case 1:bad.set(F::root+0x1C,7U);break;
        case 2:bad.set(F::life+0x170,7U);break;
        case 3:bad.set(F::objective+0x170,7U);break;
        case 4:bad.set(F::life+4,0x80804F4DU);break;
        case 5:bad.set(F::objective+8,std::int64_t{0xB89});break;
        case 6:bad.set(F::life+0x48,F::objectiveComponent);break;
        case 7:bad.set(F::record+0x80,1U);break;
        case 8:bad.set(F::record+0x88+12,0x80804F77U);break;
        case 9:bad.active=false;break;
        case 10:bad.found=false;break;
        case 11:bad.race=true;break;
        case 12:bad.set(F::pool+0x18,4097U);break;
        case 13:bad.set(F::root+0xC,4U);break;
        case 14:bad.set(F::pool+0x18,3U);
            bad.set(F::record+2*0x88,binding::Identity{0xC9BC773AU,53,0,2});break;
        case 15:bad.set(F::root+8,2U);
            bad.set(F::root+0xC+24,bad.get<std::array<std::uint32_t,6>>(F::root+0xC));break;
        case 16:bad.set(F::pool+0x18,1U);break;
        case 17:bad.set(F::pool+0x34,0x40000393U);break;
        }
        const auto before=bad.bytes;
        CHECK(binding::repair_presentation(bad,bad,F::roster)==binding::Result::unavailable);
        CHECK(bad.writes==0);CHECK(bad.bytes==before);
    }
    F changed;changed.ownerChanged=true;
    CHECK(binding::repair_presentation(changed,changed,F::roster)==binding::Result::unavailable);
    CHECK(changed.writes==0);CHECK(changed.get<std::uint32_t>(F::root+0x1C)==7U);
}
void lifetime_binding() {
    namespace binding=dawn::client::hooks::bootflow::deadly_trial_lifetime;
    using F=LifetimeFixture;
    F f;
    CHECK(f.get<std::uint32_t>(F::life+0x24)==0U); // Captured lifetime service layout.
    CHECK(binding::repair(f,f,F::roster)==binding::Result::repaired);
    CHECK(f.get<std::uint32_t>(F::root+0x1C)==F::owner);
    CHECK(f.get<std::uint32_t>(F::life+0x170)==F::sync);
    CHECK(f.get<std::uint32_t>(F::life+0x180)==3U); // Only the real packet may end the activity.
    CHECK(f.writes==2);
    CHECK(binding::repair(f,f,F::roster)==binding::Result::unchanged);CHECK(f.writes==2);
    f.set(F::root+0x1C,UINT32_MAX); // Owner retirement, retained correct sync record.
    CHECK(binding::repair(f,f,F::roster)==binding::Result::repaired);
    F changed;changed.ownerChanged=true;
    CHECK(binding::repair(changed,changed,F::roster)==binding::Result::unavailable);
    CHECK(changed.writes==0);CHECK(changed.get<std::uint32_t>(F::root+0x1C)==7U);
    for(unsigned variant=0;variant<12;++variant) {
        F bad;
        switch(variant) {
        case 0:bad.set(F::activity+0x24,0x80F46DB0U);break; // Another mission.
        case 1:bad.set(F::root+0x1C,7U);break; // Another owner cannot be stolen.
        case 2:bad.set(F::life+0x170,7U);break; // Nor another live sync record.
        case 3:bad.set(F::life+4,0x80809917U);break; // Wrong runtime definition.
        case 4:bad.set(F::record+0x80,1U);break; // Reused sync allocation.
        case 5:bad.set(F::record+12,0x80809919U);break;
        case 6:bad.active=false;break; // Freed pool record.
        case 7:bad.found=false;break;
        case 8:bad.race=true;break; // Concurrent ownership change.
        case 9:bad.set(F::pool+0x18,4097U);break;
        case 10:bad.set(F::root+0xC,20U);break; // Unexpected global descriptor.
        case 11:bad.set(F::pool+0x18,2U);
            bad.set(F::record+0x88,binding::Identity{0x4786C0E0U,17,0,3});break; // Ambiguous slot.
        }
        const auto before=bad.bytes;
        CHECK(binding::repair(bad,bad,F::roster)==binding::Result::unavailable);
        CHECK(bad.writes==0);CHECK(bad.bytes==before);
    }
}
t::Point interior(const t::Volume& v) {
    for(unsigned y=1;y<40;++y) for(unsigned x=1;x<40;++x) {
        t::Point p{v.min.x+(v.max.x-v.min.x)*float(x)/40.F,v.min.y+(v.max.y-v.min.y)*float(y)/40.F,(v.min.z+v.max.z)/2};
        if(t::contains(v,p)) { return p; }
    }CHECK(false);return {};
}
const c::CommandSpec& authored(const c::script::Views& views,std::string_view id) {
    for(const auto& graph:views.graphs) for(const auto& command:graph.commands) {
        if(command.capability==id) { return graph.definition.steps[command.step].commands[command.command]; }
    }
    CHECK(false);return t::kCapabilities[0].spec;
}
struct Harness {
    const c::script::Views& views; t::Controller controller;std::uint64_t now{1000};std::bitset<t::kSpawns.size()> admitted,dead;
    std::array<unsigned,11> dialogueSubmissions{};bool audioCueFed{};
    explicit Harness(const c::script::Views& v):views(v) { CHECK(controller.select(v,41)); }
    t::EnemyReceipt enemy(std::size_t i) const { const auto& s=t::kSpawns[i];return {41,static_cast<std::uint32_t>(100+i*2),static_cast<std::uint32_t>(1000+i),controller.frame().spawnGeneration,s.source,s.registry}; }
    void tick(bool walker,bool use,bool finishScene,bool followRoad=true,bool clearTower=true,bool startScene=true,bool recordAudio=true) {
        const auto f=controller.update(41,now,true);now+=100;
        if(f.activeRow!=c::kNoDialogue) { CHECK(controller.submitted(41,t::kBank,f.activeRow,f.generations[f.activeRow],now));++dialogueSubmissions[f.activeRow]; }
        if(const auto* g=controller.graph()) for(const auto& b:g->commands) {
            const auto& s=g->definition.steps[b.step].commands[b.command];if(controller.step_state(b.step).phase!=c::StepPhase::active) { continue; }
            const auto travel=[&](const c::CommandSpec& leaf) {
                if(leaf.operation==c::Operation::observation && leaf.asset.type==60) for(const auto& v:t::kVolumes) {
                    if(leaf.asset.registry==v.registry && leaf.asset.slot==v.slot && (followRoad || v.registry==0x3A62993FU || v.registry==0x40ADE010U)) { controller.position(41,interior(v)); }
                }
                return true;
            };
            if(views.condition(s)) { static_cast<void>(views.evaluate(s,travel)); }else { static_cast<void>(travel(s)); }
        }
        for(std::size_t i=0;i<t::kSpawns.size();++i) {
            const auto& s=t::kSpawns[i];if(!(f.cohorts&(1U<<s.cohort))) { continue; }
            if(!admitted[i]) {
                auto receipt=enemy(i);auto stale=receipt;++stale.generation;CHECK(!controller.admitted(stale));
                CHECK(!controller.died(receipt));
                for(unsigned n=0;n<s.count;++n) { receipt.actor=enemy(i).actor+n;CHECK(controller.admitted(receipt));CHECK(!controller.admitted(receipt));
                    CHECK(controller.readiness(receipt,{true,true,true,true,1234,s.tactical.registry,s.tactical.slot,s.tactical.row}));
                }admitted.set(i);
            }
            if(!dead[i] && (s.cohort!=4 || walker) && (s.cohort!=8 || clearTower)) {
                for(unsigned n=0;n<s.count;++n) { auto receipt=enemy(i);receipt.actor+=n;CHECK(controller.died(receipt));CHECK(!controller.died(receipt)); }dead.set(i);
            }
        }
        const auto request=controller.request();
        if(request.enabled && !request.interaction.valid()) { CHECK(controller.bind({request.owner,0x123400,77,88,99})); }
        if(use && request.interaction.valid() && !f.interacted) {
            const auto& b=request.interaction;CHECK(!controller.interact(b,1,0,0,true));CHECK(!controller.interact(b,1,1,1,true));
            CHECK(!controller.interact(b,1,0,1,false));auto foreign=b;++foreign.serial;CHECK(!controller.interact(foreign,1,0,1,true));
            CHECK(controller.interact(b,1,0,1,true));CHECK(!controller.interact(b,1,0,1,true));
        }
        if(f.sceneGeneration && startScene) {
            t::SceneReceipt receipt{41,f.sceneGeneration,10,20,30};auto wrong=receipt;++wrong.generation;CHECK(!controller.scene(wrong,false));
            if(!f.sceneStarted) { CHECK(!controller.scene(receipt,true));CHECK(!controller.scene(receipt,false));CHECK(!controller.scene_audio(receipt,10.F,now));CHECK(controller.bind_scene(receipt));CHECK(!controller.scene(receipt,true));CHECK(controller.scene(receipt,false)); }
            if(recordAudio && !audioCueFed) {
                CHECK(!controller.scene_audio(wrong,10.F,now));CHECK(!controller.scene_audio(receipt,9.99F,now));
                CHECK(!controller.scene_audio(receipt,std::numeric_limits<float>::quiet_NaN(),now));
                CHECK(controller.scene_audio(receipt,10.F,now));CHECK(!controller.scene_audio(receipt,10.F,now));audioCueFed=true;
            }
            if(finishScene && !f.sceneComplete) { CHECK(controller.scene(receipt,true)); }
        }
        CHECK(controller.diagnostics().phase!=c::Phase::failed);
    }
};
struct Storage { std::array<r::layouts::RosterGroup,16> rosterGroups{};std::array<wire::BubbleSubBlock,4> rosterSubBlocks{};std::array<std::array<std::uint32_t,32>,4> rosterSubBlockKeys{}; };
void roster() {
    auto storage=std::make_unique<Storage>();auto cache=std::make_unique<std::array<r::layouts::RosterGroup,std::size(t::kGroups)>>();wire::Roster roster{};
    r::layouts::Definition layout{};layout.tag=t::kScenario;layout.nameLength=16;std::copy_n("adventure_ginger",16,layout.name.begin());layout.nameLength=16;layout.bubbleCount=52;
    for(std::size_t i=0;i<std::size(t::kGroups);++i) {
        const auto& e=t::kGroups[i];auto& a=(*cache)[i];a.registryKey=e.key;a.objectTag=e.tag;a.slotCount=static_cast<std::uint16_t>(e.slots.size());
        for(std::size_t n=0;n<e.slots.size();++n) { const auto& s=e.slots[n];a.slotTypes[n]=s.type;a.slotFlags[n]=s.flags;a.slotIndices[n]=s.index;a.descriptorTags[n]=s.tag;a.descriptorOffsets[n]=s.offset;a.componentClasses[n]=s.component;a.senseSchemas[n]=s.sense;a.authSchemas[n]=s.auth; }
        CHECK(r::matches(a,e));auto altered=a;altered.descriptorTags[0]^=1;CHECK(!r::matches(altered,e));
        if(e.topLevel) { auto& dest=storage->rosterGroups[roster.groupCount];dest=a;roster.groups[roster.groupCount++]={dest.registryKey,std::span(dest.slotTypes).first(dest.slotCount),std::span(dest.slotFlags).first(dest.slotCount),std::span(dest.slotIndices).first(dest.slotCount)}; }
    }
    roster.topLevelGroupCount=roster.groupCount;const auto lookup=[&](std::size_t i,r::layouts::RosterGroup& out) noexcept { if(i>=cache->size()) { return false; }out=(*cache)[cache->size()-1-i];return true; };
    const auto indexByKey=[&](std::uint32_t key,std::uint32_t tag,std::uint16_t& out) noexcept {
        for(std::size_t i=0;i<cache->size();++i) if((*cache)[cache->size()-1-i].registryKey==key && (*cache)[cache->size()-1-i].objectTag==tag) {out=static_cast<std::uint16_t>(i);return true;}
        return false;
    };
    const auto byKey=[&](std::uint32_t key,std::uint32_t tag,r::layouts::RosterGroup& out) noexcept {
        std::uint16_t index{};return indexByKey(key,tag,index) && lookup(index,out);
    };
    // Real cache row from the first failed run: town retains its two-entry
    // overlay, both alleys lose the root when extraction exceeds five entries.
    namespace records=dawn::state::build_data::cache::records;
    records::ScenarioRecord captured{};std::ifstream input("Dawn/unit/fixtures/deadly_trial_launch_layout.bin",std::ios::binary);
    CHECK(input.read(reinterpret_cast<char*>(&captured),sizeof captured));
    r::layouts::Definition transition{};CHECK(records::decode(captured,transition));
    CHECK(transition.authoredGroupCounts[51]==2);CHECK(transition.authoredGroupCounts[0]==0);CHECK(transition.authoredGroupCounts[1]==0);
    const auto original=transition;CHECK(r::prepare_layout(transition,indexByKey,lookup));
    for(auto bubble:{51U,0U,1U}) {
        CHECK(transition.authoredGroupCounts[bubble]==1);
        r::layouts::RosterGroup root{};CHECK(lookup(transition.authoredGroups[bubble][0],root));CHECK(root.registryKey==t::kRoot);
        CHECK(transition.authoredGroups[bubble]==transition.authoredGroups[51]);
    }
    CHECK(transition.rosterGroups==original.rosterGroups);CHECK(transition.rosterGroupCount==original.rosterGroupCount);
    CHECK(transition.bubbleGroups==original.bubbleGroups);CHECK(transition.bubbleGroupMasks==original.bubbleGroupMasks);
    CHECK(transition.bubbleStates==original.bubbleStates);CHECK(transition.bubbleHashes==original.bubbleHashes);
    CHECK(transition.bubbleMapIndices==original.bubbleMapIndices);CHECK(transition.packages==original.packages);
    const auto stable=transition;CHECK(r::prepare_layout(transition,indexByKey,lookup));CHECK(transition.authoredGroups==stable.authoredGroups);
    auto rejected=original;
    CHECK(!r::prepare_layout(rejected,indexByKey,[](std::size_t,r::layouts::RosterGroup&) noexcept { return false; }));
    CHECK(rejected.authoredGroups==original.authoredGroups);CHECK(rejected.authoredGroupCounts==original.authoredGroupCounts);
    rejected.tag^=1;CHECK(!r::prepare_layout(rejected,indexByKey,lookup));
    layout=transition;
    CHECK(r::admit(layout,*storage,roster,byKey));CHECK(roster.groupCount==std::size(t::kGroups));CHECK(roster.bubbleSubBlocks.size()==3);CHECK(r::admit(layout,*storage,roster,byKey));
    for(const auto& e:t::kGroups) if(!e.topLevel) { unsigned count{};for(const auto& block:roster.bubbleSubBlocks) for(auto key:block.keys) if(key==e.key) { CHECK(block.bubble==e.bubble);++count; }CHECK(count==1); }
    wire::Snapshot snapshot{};snapshot.roster=roster;snapshot.lifetime=3;snapshot.deadly_trial.enabled=true;snapshot.deadly_trial.spawnGeneration=7;snapshot.deadly_trial.cohorts=1022;snapshot.deadly_trial.sceneGeneration=7;snapshot.deadly_trial.pikes=2;snapshot.deadly_trial.reviveEnabled=true;
    std::array<std::byte,32768> data{};std::size_t written{};CHECK(wire::encode_sensor_auth_update(snapshot,data,written));CHECK(written<data.size());std::printf("Trial roster packet: %zu bytes, %zu groups\n",written,roster.groupCount);
    layout.tag^=1;CHECK(!r::admit(layout,*storage,roster,byKey));
}
void presentation_and_overpass(const c::script::Views& views) {
    namespace native=dawn::client::hooks::bootflow::deadly_trial_presentation;
    std::array<std::byte,0x1500> bytes{};
    const auto header=[&](bool dialogue) {
        bytes={};native::put<std::uint32_t>(bytes,0,dialogue?0x80B2E709U:0x80B2E706U);
        native::put<std::uint32_t>(bytes,4,dialogue?0x80804F4CU:0x80804F54U);
        native::put<std::int64_t>(bytes,8,dialogue?0x1408:0xB88);
        native::put<std::uint32_t>(bytes,0x48,123);
        native::put<std::uint32_t>(bytes,0x4C,dialogue?0x80804F4BU:0x80804F53U);
    };
    t::Frame f{};f.enabled=true;f.spawnGeneration=7;f.presentation.active=true;f.presentation.event=0x183F9715U;
    header(false);CHECK(native::route_point(bytes,f));CHECK(native::read<std::uint8_t>(bytes,0x484)==3);
    CHECK(native::read<std::uint8_t>(bytes,0x48C)==2);CHECK(native::read<float>(bytes,0x4A0)==945.683655F);
    for(std::uint32_t index=1;index<3;++index) {
        header(false);CHECK(native::route_point(bytes,f,index));
        CHECK(native::read<std::uint8_t>(bytes,0x484+index*0x200)==3);
        CHECK(native::read<std::uint8_t>(bytes,0x48C+index*0x200)==2);
        CHECK(native::read<float>(bytes,0x4A0+index*0x200)==945.683655F);
        CHECK(native::read<std::uint8_t>(bytes,0x484)==0);
    }
    const auto unchanged=bytes;CHECK(!native::route_point(bytes,f,3));CHECK(bytes==unchanged);
    CHECK(t::navigation::goal(f.presentation.event).target.asset.type==47);
    CHECK(t::navigation::goal(f.presentation.event).target.locator[0]==0x29930BA4U);
    f.presentation.active=false;CHECK(native::route_point(bytes,f));CHECK(native::read<std::uint8_t>(bytes,0x484)==0);
    header(false);native::put<std::uint32_t>(bytes,0,0x80F47BD4U);const auto foreign=bytes;CHECK(!native::route_point(bytes,f));CHECK(bytes==foreign);
    // Dialogue delivery is tested from production wire bodies by native_mission_dialogue_tests.
    // Captured exterior point identity plus the user-confirmed walkway height.
    header(false);f.presentation.active=true;f.presentation.event=0xE58BB2F6U;
    const auto exterior=t::navigation::goal(f.presentation.event);
    CHECK(exterior.target.asset==(c::Asset{0xC91BDFF0U,0x80B2EC47U,47,4}));
    CHECK(exterior.target.locator==(std::array<std::uint32_t,4>{0x29930BA4U,0xC6905B74U,0xC91BDFF0U,0x69AEC903U}));
    CHECK(native::route_point(bytes,f));CHECK(native::read<std::uint8_t>(bytes,0x484)==3);
    CHECK(native::read<std::uint8_t>(bytes,0x48C)==2);CHECK(native::read<std::uint32_t>(bytes,0x498)==1);
    CHECK(native::read<float>(bytes,0x4A0)==1374.614624F);CHECK(native::read<float>(bytes,0x4A4)==544.469482F);
    CHECK(native::read<float>(bytes,0x4A8)==198.662613F);
    const auto search=t::navigation::goal(0x882DD31EU),revive=t::navigation::goal(0x708B9351U);
    CHECK(search.target.asset.definition==0x80B2EC53U);CHECK(search.position.z==172.705978F);
    CHECK(revive.target.asset.definition==0x80B2EC62U);CHECK(revive.position.z==173.280655F);
    CHECK(t::navigation::goal(0).target.asset.type==0);
    // Independent first scalar in 80804F48: the live test proved 0=closed,1=open.
    for(bool open:{false,true}) {
        std::array<std::byte,32> data{};bits::Writer writer(data);f.barrierOpen=open;
        CHECK(t::write_body(writer,f,t::kAlleysA,23,46));CHECK(writer.bit_count()==147);
        CHECK(data[0]==(open?std::byte{0x3F}:std::byte{0}));CHECK(data[1]==(open?std::byte{0x80}:std::byte{0}));
    }
    f.cohorts=0;CHECK(t::body_bits(f,t::kAlleysA,1,34)==0);CHECK(t::body_bits(f,t::kAlleysA,2,35)==0);
    f.cohorts=1U<<4;CHECK(t::body_bits(f,t::kAlleysA,1,34)==641);CHECK(t::body_bits(f,t::kAlleysA,2,35)==462);
    // Independently encoded from the pinned native reflection/default programs:
    // zero loose requests, one enabled member, generation 7, and the live-accepted
    // enter_45_stop / six-second wait / exit_45 native command sequence.
    for(const auto type:{1U,2U}) {
        const auto slot=static_cast<std::uint16_t>(type==1?34:35);
        std::array<std::byte,81> actual{},expected{};bits::Writer writer(actual);
        CHECK(t::write_body(writer,f,t::kAlleysA,static_cast<std::uint8_t>(type),slot));
        std::ifstream fixture(type==1?"Dawn/unit/fixtures/deadly_trial_skiff_source.bin":"Dawn/unit/fixtures/deadly_trial_skiff_flight.bin",std::ios::binary);
        const auto bytesWritten=(writer.bit_count()+7)/8;
        CHECK(fixture.read(reinterpret_cast<char*>(expected.data()),static_cast<std::streamsize>(bytesWritten)));
        if(actual!=expected) {
            std::fprintf(stderr,"Skiff authority type %u actual/expected:\n",type);
            for(std::size_t i=0;i<bytesWritten;++i) { std::fprintf(stderr,"%02X",std::to_integer<unsigned>(actual[i])); }std::fprintf(stderr,"\n");
            for(std::size_t i=0;i<bytesWritten;++i) { std::fprintf(stderr,"%02X",std::to_integer<unsigned>(expected[i])); }std::fprintf(stderr,"\n");
        }
        CHECK(actual==expected);
        const auto before=actual;f.barrierOpen=!f.barrierOpen;actual={};bits::Writer after(actual);
        CHECK(t::write_body(after,f,t::kAlleysA,static_cast<std::uint8_t>(type),slot));CHECK(actual==before);
    }
    CHECK(t::body_bits(f,t::kAlleysB,2,35)==0);CHECK(t::body_bits(f,t::kAlleysA,2,37)==0);
    // Actual overpass position captured in the user run. Earlier narrow road
    // volumes remain unvisited; their completion must not fabricate a kill.
    t::Controller controller;CHECK(controller.select(views,51));
    for(const auto& v:t::kVolumes) if(v.registry==0x3A62993FU && v.slot==2) { controller.position(51,interior(v)); }
    controller.position(51,{843.674561F,475.404388F,135.641403F});
    for(const auto& cap:t::kCapabilities) {
        if(cap.id=="followers.entered" || cap.id=="streets.entered" || cap.id=="choke.entered" || cap.id=="overpass.entered") {
            std::string id{cap.id};id.replace(id.find("entered"),7,"arrived");
            CHECK(controller.missing(authored(views,id)).missing==c::Missing::none);
        }
    }
    CHECK(!controller.seen()[21]);auto observed=controller.update(51,1000,true);CHECK(!observed.barrierOpen);
    CHECK((observed.cohorts&(1U<<4))==0); // Square completion remains required.
}

void post_walker_route(const c::script::Views& views) {
    const auto capability=[&](std::string_view id) -> const c::CommandSpec& {
        std::string condition{id};condition.replace(condition.find("entered"),7,"arrived");return authored(views,condition);
    };
    const auto volume=[](std::uint32_t registry,std::uint16_t slot) -> const t::Volume& {
        for(const auto& v:t::kVolumes) if(v.registry==registry && v.slot==slot) { return v; }
        CHECK(false);return t::kVolumes[0];
    };
    // The rerun visited cliff, tunnel and tower while missing the post-Walker
    // dialogue filter. Later authored arrivals must release only earlier travel.
    t::Controller observed;CHECK(observed.select(views,51));
    const auto arrive=[&](std::uint32_t registry,std::uint16_t slot) { observed.position(51,interior(volume(registry,slot))); };
    arrive(0x3A62993FU,2);
    for(const auto id:{"trial.entered","cliff.entered","tunnel.entered","tower.entered"}) { CHECK(observed.missing(capability(id)).missing==c::Missing::observation); }
    observed.position(52,interior(volume(t::kAlleysB,157)));
    CHECK(observed.missing(capability("trial.entered")).missing==c::Missing::observation);
    arrive(t::kAlleysB,157);
    CHECK(observed.missing(capability("trial.entered")).missing==c::Missing::none);
    CHECK(observed.missing(capability("tunnel.entered")).missing==c::Missing::observation);
    CHECK(observed.missing(capability("tower.entered")).missing==c::Missing::observation);
    for(const auto target:{std::pair{t::kAlleysB,std::uint16_t{223}},std::pair{0xBC1C972BU,std::uint16_t{2}}}) {
        observed.reset();CHECK(observed.select(views,51));arrive(0x3A62993FU,2);arrive(target.first,target.second);
        const auto seen=observed.seen();
        for(const auto id:{"trial.entered","cliff.entered","tunnel.entered"}) { CHECK(observed.missing(capability(id)).missing==c::Missing::none); }
        CHECK(observed.seen()==seen); // No invented volume observations.
        if(target.first==t::kAlleysB) { CHECK(observed.missing(capability("tower.entered")).missing==c::Missing::observation); }
    }
    // The paired road directive and both broader tower arrivals must work
    // independently of the narrow dialogue filters, including their Z bounds.
    for(const auto target:{std::pair{0x4DDB8242U,std::uint16_t{1}},std::pair{0xC91BDFF0U,std::uint16_t{1}},std::pair{0xBC1C972BU,std::uint16_t{1}}}) {
        observed.reset();CHECK(observed.select(views,51));arrive(0x3A62993FU,2);
        auto point=interior(volume(target.first,target.second));point.z=1000;
        observed.position(51,point);CHECK(observed.missing(capability("trial.entered")).missing==c::Missing::observation);
        arrive(target.first,target.second);const auto seen=observed.seen();
        CHECK(observed.missing(capability("trial.entered")).missing==c::Missing::none);
        CHECK(observed.missing(capability("tower.entered")).missing==(target.first==0x4DDB8242U?c::Missing::observation:c::Missing::none));
        auto foreign=capability("tower.entered");foreign.asset.definition^=1;
        CHECK(observed.missing(foreign).missing==c::Missing::observation);CHECK(observed.seen()==seen);
    }
    // Real mission sequence: gate dialogue precedes cliff/tunnel travel; tower
    // dialogue fires on broad arrival, while Search still waits for real kills.
    Harness timing(views);
    for(unsigned i=0;i<400 && !(timing.controller.frame().cohorts&(1U<<4));++i) { timing.tick(false,false,false); }
    CHECK(timing.controller.frame().cohorts&(1U<<4));
    for(unsigned i=0;i<200 && !timing.controller.frame().barrierOpen;++i) { timing.tick(true,false,false,false,false); }
    CHECK(timing.controller.frame().barrierOpen);CHECK(timing.dialogueSubmissions[2]==0);CHECK(timing.dialogueSubmissions[4]==0);
    timing.controller.position(41,interior(volume(0x4DDB8242U,1)));
    for(unsigned i=0;i<200 && !timing.dialogueSubmissions[2];++i) { timing.tick(true,false,false,false,false); }
    CHECK(timing.dialogueSubmissions[2]==1);CHECK(timing.dialogueSubmissions[4]==0);
    CHECK((timing.controller.frame().cohorts&0xC0U)==0);CHECK(timing.controller.frame().objective==0xE58BB2F6U);
    timing.controller.position(41,interior(volume(0xC91BDFF0U,1)));
    for(unsigned i=0;i<200 && !timing.dialogueSubmissions[4];++i) { timing.tick(true,false,false,false,false); }
    CHECK(timing.dialogueSubmissions[4]==1);CHECK(timing.controller.frame().objective==0xE58BB2F6U);
    for(unsigned i=0;i<150;++i) { timing.controller.position(41,interior(volume(0xC91BDFF0U,1)));timing.tick(true,false,false,false,false); }
    CHECK(timing.dialogueSubmissions[2]==1);CHECK(timing.dialogueSubmissions[4]==1);
    CHECK(timing.controller.frame().objective==0xE58BB2F6U);CHECK(!timing.controller.frame().reviveEnabled);
    for(unsigned i=0;i<40 && timing.controller.frame().objective!=0x882DD31EU;++i) { timing.tick(true,false,false,false); }
    CHECK(timing.controller.frame().objective==0x882DD31EU);CHECK(!timing.controller.frame().reviveEnabled);
    CHECK(timing.controller.frame().presentation.marker==t::navigation::goal(0x882DD31EU).target);
    // Tower activation depends directly on the real Walker death, with every
    // downstream road volume and dialogue deliberately withheld.
    Harness direct(views);
    for(unsigned i=0;i<300 && !(direct.controller.frame().cohorts&(1U<<4));++i) { direct.tick(false,false,false); }
    CHECK(direct.controller.frame().cohorts&(1U<<4));
    for(unsigned i=0;i<40;++i) { direct.tick(false,false,false,false); }
    CHECK((direct.controller.frame().cohorts&(1U<<8))==0);
    for(std::size_t i=0;i<t::kSpawns.size();++i) if(t::kSpawns[i].cohort==4) {
        auto foreign=direct.enemy(i);++foreign.run;CHECK(!direct.controller.died(foreign));
        foreign=direct.enemy(i);++foreign.generation;CHECK(!direct.controller.died(foreign));
        foreign=direct.enemy(i);++foreign.owner;CHECK(!direct.controller.died(foreign));
    }
    CHECK((direct.controller.frame().cohorts&(1U<<8))==0);
    for(unsigned i=0;i<20 && !direct.controller.frame().barrierOpen;++i) { direct.tick(true,false,false,false,false); }
    CHECK(direct.controller.frame().barrierOpen);CHECK(direct.controller.frame().cohorts&(1U<<8));
    CHECK((direct.controller.frame().cohorts&0xC0U)==0);
    CHECK(direct.controller.missing(capability("trial.entered")).missing==c::Missing::observation);
    direct.tick(true,false,false,false,false);
    unsigned towerSources{};
    for(std::size_t i=0;i<t::kSpawns.size();++i) if(t::kSpawns[i].cohort==8) { ++towerSources;CHECK(direct.admitted[i]); }
    CHECK(towerSources==15);CHECK(!direct.controller.frame().reviveEnabled);
    // The last tower death places the lower-room Marauders at this callback,
    // without a drop, a lair position, or downstream dialogue submissions.
    CHECK((direct.controller.frame().cohorts&(1U<<9))==0);
    CHECK(direct.controller.missing(authored(views,"lair.entered")).missing==c::Missing::observation);
    std::vector<t::EnemyReceipt> towerActors;
    for(std::size_t i=0;i<t::kSpawns.size();++i) if(t::kSpawns[i].cohort==8) {
        for(unsigned n=0;n<t::kSpawns[i].count;++n) { auto receipt=direct.enemy(i);receipt.actor+=n;towerActors.push_back(receipt); }
    }
    for(std::size_t i=0;i<towerActors.size();++i) {
        CHECK((direct.controller.frame().cohorts&(1U<<9))==0);
        auto stale=towerActors[i];++stale.generation;CHECK(!direct.controller.died(stale));
        CHECK(direct.controller.died(towerActors[i]));CHECK(!direct.controller.died(towerActors[i]));
    }
    CHECK(direct.controller.frame().cohorts&(1U<<9));CHECK(!direct.controller.frame().reviveEnabled);
    CHECK(direct.dialogueSubmissions[4]==0);
    // A retained tower arrival before the Walker dies cannot open its barrier
    // or enable any post-Walker cohort; then a real death releases the route.
    Harness h(views);
    for(unsigned i=0;i<300 && !(h.controller.frame().cohorts&(1U<<4));++i) { h.tick(false,false,false); }
    CHECK(h.controller.frame().cohorts&(1U<<4));
    h.controller.position(41,interior(volume(0xBC1C972BU,2)));
    for(unsigned i=0;i<40;++i) { h.tick(false,false,false,false); }
    CHECK(!h.controller.frame().barrierOpen);CHECK((h.controller.frame().cohorts&0x1C0U)==0);
    for(unsigned i=0;i<300 && (h.controller.frame().cohorts&0x1C0U)!=0x1C0U;++i) { h.tick(true,false,false,false); }
    CHECK(h.controller.frame().barrierOpen);CHECK((h.controller.frame().cohorts&0x1C0U)==0x1C0U);
    CHECK(!h.controller.frame().reviveEnabled);CHECK(!h.controller.frame().interacted);CHECK(!h.controller.frame().sceneStarted);CHECK(!h.controller.frame().finished);
    h.controller.reset();CHECK(h.controller.select(views,41));
    CHECK(h.controller.missing(capability("trial.entered")).missing==c::Missing::observation);
}

struct MountRead {
    std::uintptr_t image{0x100000000ULL};std::map<std::uintptr_t,std::byte> bytes;
    template<class T> void put(std::uintptr_t at,T value) {
        const auto view=std::as_bytes(std::span{&value,std::size_t{1}});
        for(std::size_t i=0;i<view.size();++i) { bytes[at+i]=view[i]; }
    }
    template<class T> bool value(std::uintptr_t at,T& out) {
        auto view=std::as_writable_bytes(std::span{&out,std::size_t{1}});
        for(std::size_t i=0;i<view.size();++i) { const auto it=bytes.find(at+i);if(it==bytes.end()) { return false; }view[i]=it->second; }return true;
    }
    bool resolve(std::uint32_t handle,std::uintptr_t& base,std::uintptr_t* allocation=nullptr) {
        base=handle==7?0x200000U:handle==8?0x300000U:handle==9?0x200080U:0;
        if(allocation) { *allocation=0x400000; }return base!=0;
    }
    MountRead() {
        put(image+0x1F93428,std::uintptr_t{0x100000});put(image+0x1F93430,0xE0U);
        for(auto [entity,index]:{std::pair{0x55FAA001U,1U},std::pair{0x3DFAA002U,2U}}) {
            put(0x100000+index*0xE0+0xC,entity);put(0x100000+index*0xE0+4,0U);
        }
        put(0x100000+2*0xE0+0x4C,7U);put(0x200000,0U);put(0x200004,8U);
        put(0x300068,std::uint64_t{366});put(0x300070,std::int64_t{0x80});
        for(unsigned i=0;i<366;++i) { put(0x300100+i*24+0x14,0x80); }
        put(0x200080,0x80FDA97DU);put(0x200084,0x808071BCU);put(0x200088,std::int64_t{0xD8});
        put(0x2000A4,9U);put(0x2000AC,0x3DFAA002U);put(0x2000C8,0x55FAA001U);put(0x400018,UINT32_MAX);
    }
};
struct MountNative {
    MountRead& read;std::uint32_t player{0x55FAA001U},vehicle{0x3DFAA002U};bool dismountDuringRead{};
    void controlled(std::uint32_t& out) { out=player; }
    void parent(std::uintptr_t,std::uint32_t& out) { out=vehicle; }
    void position(std::uintptr_t,std::array<float,4>& out) { out={770.702454F,297.787933F,99.492096F,1.F};if(dismountDuringRead) { read.put(0x2000C8,UINT32_MAX); } }
};
void pike_mount(const c::script::Views& views) {
    namespace native=dawn::client::hooks::bootflow::coo_native;
    native::MountedPlayer sample{};MountRead read;MountNative source{read};
    CHECK(native::mounted_pike(read,source,sample));CHECK(sample.player==source.player);CHECK(sample.vehicle==source.vehicle);CHECK(sample.seat==9);
    CHECK(std::abs(sample.position[0]-770.702454F)<0.0001F);
    std::uintptr_t component{};CHECK(!native::component(read,7,source.vehicle,0x808071BCU,component)); // Existing enemy limit retained.
    source.vehicle=UINT32_MAX;CHECK(!native::mounted_pike(read,source,sample));source.vehicle=0x3DFAA002U;
    read.put(0x2000C8,0x55FAA002U);CHECK(!native::mounted_pike(read,source,sample));read.put(0x2000C8,source.player);
    read.put(0x100000+2*0xE0+0xC,0x3DFA8002U);CHECK(!native::mounted_pike(read,source,sample));read.put(0x100000+2*0xE0+0xC,source.vehicle);
    read.put(0x200080,0x80FDA97CU);CHECK(!native::mounted_pike(read,source,sample));read.put(0x200080,0x80FDA97DU);
    source.dismountDuringRead=true;CHECK(!native::mounted_pike(read,source,sample));source.dismountDuringRead=false;read.put(0x2000C8,source.player);
    read.put(0x300068,std::uint64_t{1025});CHECK(!native::mounted_pike(read,source,sample));
    Harness h(views);t::PikeMount receipt{{41,h.controller.frame().spawnGeneration},source.player,source.vehicle,9};
    CHECK(!h.controller.mounted(receipt));
    for(unsigned i=0;i<1000 && h.controller.frame().pikes!=1;++i) { h.tick(false,false,false,false); }
    CHECK(h.controller.frame().objective==0xDA95AE49U);const auto seen=h.controller.seen();
    auto stale=receipt;++stale.owner.value;CHECK(!h.controller.mounted(stale));stale=receipt;++stale.owner.run;CHECK(!h.controller.mounted(stale));
    for(unsigned i=0;i<20;++i) { h.tick(false,false,false,false); }CHECK(h.controller.frame().objective==0xDA95AE49U);
    CHECK(h.controller.mounted(receipt));CHECK(!h.controller.mounted(receipt));CHECK(h.controller.seen()==seen);
    for(unsigned i=0;i<20;++i) { h.tick(false,false,false,false); }
    CHECK(h.controller.frame().objective==0x183F9715U);CHECK(h.controller.frame().cohorts==(1U<<1));CHECK(!h.controller.frame().barrierOpen);
    // Real mounted position, captured after the live mount fix, releases streets.
    h.controller.position(41,{770.702454F,297.787933F,99.492096F});
    // This point lies beyond the narrow street volume; the actual live arrival
    // is retained separately when its authored boundary is crossed.
    for(const auto& v:t::kVolumes) if(v.registry==t::kAlleysA && v.slot==214) { h.controller.position(41,interior(v)); }
    for(unsigned i=0;i<1000 && !(h.controller.frame().cohorts&(1U<<2));++i) { h.tick(false,false,false,false); }CHECK(h.controller.frame().cohorts&(1U<<2));CHECK(!(h.controller.frame().cohorts&(1U<<4)));
    h.controller.reset();CHECK(h.controller.select(views,41));CHECK(!h.controller.mounted(receipt));
    for(const auto& cap:t::kCapabilities) if(cap.id=="followers.entered") { CHECK(h.controller.missing(cap.spec).missing==c::Missing::observation); }
}

void changed_lua_flow() {
    // Reorder phases, omit all original objectives, and replace the barrier's
    // Walker gate with raw square arrival using only registered native bindings.
    const std::string script=R"lua(
local gate=condition("alternate.ready",any_of("square.entered","pike.mounted"))
local composition=graph("composition","alternate mission",sequence(
    step("run",parallel("opening.module","opening.checked"))))
local first=graph("reordered","reordered actions",sequence(
    step("objective","objective.search"),
    step("gate","alternate.ready"),
    step("actions",parallel("barrier.open","revive.enable","tower.enable"))))
local second=graph("custom_end","custom ending",sequence(
    step("finish","mission.finish")))
return mission{id="deadly_trial",graphs={composition,second,first},
    roles={mission="composition"},phases={"reordered","custom_end"},
    conditions={gate},entry="composition",modules={"opening"},observations={"opening.checked"}}
)lua";
    std::string error;auto doc=c::script::MissionDocument::parse_lua(script,t::kProfile,error);
    if(!doc) { std::fprintf(stderr,"changed flow: %s\n",error.c_str()); }CHECK(doc);CHECK(t::valid_document(doc->views()));
    t::Controller controller;CHECK(controller.select(doc->views(),61));
    auto frame=controller.update(61,1000,true);CHECK(frame.objective==0x882DD31EU);CHECK(!frame.barrierOpen);
    CHECK(controller.graph()->id=="reordered");CHECK(!frame.cohorts);CHECK(!frame.sceneGeneration);
    for(const auto& volume:t::kVolumes) if(volume.registry==0x40ADE010U && volume.slot==3) { controller.position(61,interior(volume)); }
    frame=controller.update(61,1100,true);CHECK(frame.barrierOpen);CHECK(frame.reviveEnabled);CHECK(frame.cohorts==(1U<<8));
    for(unsigned i=0;i<4 && !frame.finished;++i) { frame=controller.update(61,1200+i*100,true); }
    CHECK(frame.finished);CHECK(frame.completion.valid());CHECK(!frame.interacted);CHECK(!frame.sceneComplete);
    frame=controller.update(61,1700,true);CHECK(frame.finished);CHECK(controller.diagnostics().phase==c::Phase::complete);
    CHECK(controller.graph()->id=="custom_end");
    // Native admission identities remain mandatory in changed flows.
    t::EnemyReceipt foreign{62,100,1000,frame.spawnGeneration,t::kSpawns[0].source,t::kSpawns[0].registry};
    CHECK(!controller.died(foreign));
}

void immediate_authored_death_branch() {
    const std::string script=R"lua(
local composition=graph("composition","death branch mission",sequence(
    step("run",parallel("opening.module","opening.checked"))))
local encounter=graph("encounter","independent tower",{
    step("roadblock",parallel("walker.enable","dialogue.0")),
    step("death","walker.cleared"),
    step("tower","tower.enable",{after={"death"}})})
return mission{id="deadly_trial",graphs={composition,encounter},
    roles={mission="composition"},phases={"encounter"},entry="composition",
    modules={"opening"},observations={"opening.checked"}}
)lua";
    std::string error;auto doc=c::script::MissionDocument::parse_lua(script,t::kProfile,error);CHECK(doc);
    t::Controller controller;CHECK(controller.select(doc->views(),71));
    const auto frame=controller.update(71,1000,true);CHECK(frame.cohorts==(1U<<4));
    CHECK(frame.activeRow==0);CHECK(controller.step_state(0).phase==c::StepPhase::active);
    for(std::size_t i=0;i<t::kSpawns.size();++i) if(t::kSpawns[i].cohort==4) {
        const auto& spawn=t::kSpawns[i];
        for(unsigned n=0;n<spawn.count;++n) {
            t::EnemyReceipt receipt{71,static_cast<std::uint32_t>(100+i*2+n),static_cast<std::uint32_t>(1000+i),frame.spawnGeneration,spawn.source,spawn.registry};
            CHECK(controller.admitted(receipt));
            CHECK(controller.readiness(receipt,{true,true,true,true,1234,spawn.tactical.registry,spawn.tactical.slot,spawn.tactical.row}));
            auto foreign=receipt;++foreign.run;CHECK(!controller.died(foreign));
            CHECK(!(controller.frame().cohorts&(1U<<8)));CHECK(controller.died(receipt));
        }
    }
    // No update, travel, or dialogue submission occurs between death and intent.
    CHECK(controller.frame().cohorts&(1U<<8));CHECK(controller.frame().activeRow==0);
    CHECK(controller.step_state(0).phase==c::StepPhase::active);CHECK(!controller.frame().barrierOpen);
}

void scene_owned_dialogue() {
    std::array<std::byte,200> fixture{};std::ifstream file("Dawn/unit/fixtures/deadly_trial_revival_dialogue.bin",std::ios::binary);
    CHECK(file.read(reinterpret_cast<char*>(fixture.data()),fixture.size()));
    namespace native=dawn::client::hooks::bootflow::deadly_trial_presentation;
    for(unsigned row=0;row<2;++row) {
        const auto offset=row*104U;
        CHECK(native::read<std::uint32_t>(fixture,offset)==0x80F275D9U);
        CHECK(native::read<std::uint32_t>(fixture,offset+4)==0x8080658DU);
        CHECK(native::read<std::uint32_t>(fixture,offset+16)==t::kDialogueRows[9+row].selector);
        CHECK(native::read<float>(fixture,offset+28)==(row?10.F:0.F));
        CHECK(native::read<float>(fixture,offset+32)==(row?10.F:0.F));
        CHECK(native::read<std::uint32_t>(fixture,offset+76)==t::kBank);
        CHECK(t::kDialogueRows[9+row].sceneOwned);
    }
    c::DialogueService<11> service;t::Frame frame{};frame.enabled=true;frame.spawnGeneration=1;
    service.enqueue(t::kDialogue,9,1000,0,0,frame.revision);service.enqueue(t::kDialogue,10,1000,0,0,frame.revision);
    service.advance(t::kDialogue,0,1000,false,frame,frame.revision);
    CHECK(frame.activeRow==c::kNoDialogue);CHECK(frame.generations[9]==0);CHECK(frame.generations[10]==0);
    CHECK(t::revival_audio_remaining(10.F)==38925);CHECK(t::revival_audio_remaining(10.5F)==38425);
    CHECK(!t::valid_revival_audio_cue(9.999F));CHECK(!t::valid_revival_audio_cue(37.75F));
    CHECK(t::valid_revival_audio_cue(10.F));
}
int main(int argc,char** argv) {
    scene_owned_dialogue();changed_lua_flow();immediate_authored_death_branch();
    static_cast<void>(argc);static_cast<void>(argv);
    std::string error;auto doc=c::script::MissionDocument::read("Dawn/scripts/deadly_trial.lua",t::kProfile,error);
    if(!doc) { std::fprintf(stderr,"script: %s\n",error.c_str()); }CHECK(doc);CHECK(t::valid_document(doc->views()));pike_mount(doc->views());
    // Independent 80804D3F schema fixture: i32 (biased), bool, FNV selector.
    // Native capture is revision 2 / active 1 / 811C9DC5. MSB-first 65-bit body.
    const std::array<std::byte,9> activeBody{std::byte{0x80},std::byte{0},std::byte{0},std::byte{2},std::byte{0xC0},std::byte{0x8E},std::byte{0x4E},std::byte{0xE2},std::byte{0x80}};
    t::Frame sceneFrame{};sceneFrame.enabled=true;sceneFrame.spawnGeneration=2;sceneFrame.sceneGeneration=2;
    std::array<std::byte,9> encoded{};bits::Writer pending(encoded);
    CHECK(t::write_body(pending,sceneFrame,0x27660927U,65,0));CHECK(pending.bit_count()==65);
    CHECK(encoded!=activeBody);CHECK(encoded[0]==std::byte{0x7F});
    sceneFrame.sceneBound=true;bits::Writer active(encoded);CHECK(t::write_body(active,sceneFrame,0x27660927U,65,0));CHECK(encoded==activeBody);
    CHECK(t::body_bits(sceneFrame,t::kAlleysB,43,74)==0); // No duplicate native scan.
    CHECK(t::body_bits(sceneFrame,0x27660927U,65,1)==0);
    sceneFrame.sceneComplete=true;bits::Writer ended(encoded);CHECK(t::write_body(ended,sceneFrame,0x27660927U,65,0));CHECK(encoded[4]==std::byte{0x40});
    namespace revival=dawn::client::hooks::bootflow::deadly_trial_revival;
    revival::Playback playback{2,2,1,0.F,37.75F};CHECK(!playback.started(2,true));
    playback.elapsed=1.F;CHECK(!playback.started(2,false));CHECK(!playback.started(3,true));CHECK(playback.started(2,true));
    CHECK(!playback.finished(2,true));playback.active=0;CHECK(!playback.finished(2,true));
    // Accepted capture advanced one frame beyond duration before retiring Ghost.
    playback.elapsed=37.762062072753906F;CHECK(playback.finished(2,true));CHECK(!playback.finished(2,false));CHECK(!playback.finished(3,true));
    playback.mode=1;CHECK(!playback.finished(2,true));playback.mode=2;
    playback.elapsed=std::numeric_limits<float>::quiet_NaN();CHECK(!playback.finished(2,true));
    namespace forced=dawn::state::activity::forced;
    CHECK(forced::prelaunch::configured(forced::profiles::kDeadlyTrialOpening)==&forced::prelaunch::kDeadlyTrial);
    CHECK(forced::prelaunch::kDeadlyTrial.activity==293);CHECK(forced::profiles::kDeadlyTrialOpening.sliceSet==408);
    CHECK(forced::prelaunch::kGateway.activity==292);CHECK(forced::profiles::kGatewayOpening.sliceSet==120);
    // Confirmed arrival publishes the opening without any position sample.
    Harness delayed(doc->views());CHECK(!delayed.controller.update(41,1000,false).enabled);
    auto opening=delayed.controller.update(41,1000,true);
    CHECK(opening.objective==3961616249U);CHECK(opening.activeRow==0);
    const auto openingGeneration=opening.generations[0];
    for(const auto now:{17000ULL,33000ULL,65000ULL}) {
        opening=delayed.controller.update(41,now,true);
        CHECK(opening.activeRow==0 && opening.generations[0]==openingGeneration);
        CHECK(opening.cohorts==0 && !opening.barrierOpen);
    }
    CHECK(!delayed.controller.submitted(42,t::kBank,0,openingGeneration,65001));
    CHECK(delayed.controller.submitted(41,t::kBank,0,openingGeneration,65001));
    CHECK(!delayed.controller.submitted(41,t::kBank,0,openingGeneration,65001));
    // A far-away position cannot synthesize encounter entry or enemy deaths.
    Harness h(doc->views());h.controller.position(41,{1400,585,175});auto initial=h.controller.update(41,1000,true);CHECK(initial.cohorts==0);CHECK(!initial.barrierOpen);
    for(unsigned i=0;i<1600 && !(h.controller.frame().cohorts&(1U<<4));++i) { h.tick(false,false,false); }
    CHECK(h.controller.frame().cohorts&(1U<<4));for(unsigned i=0;i<30;++i) { h.tick(false,false,false); }CHECK(!h.controller.frame().barrierOpen);
    for(unsigned i=0;i<2000 && !h.controller.frame().reviveEnabled;++i) { h.tick(true,false,false); }
    CHECK(h.controller.frame().barrierOpen);CHECK(h.controller.frame().reviveEnabled);CHECK(h.controller.frame().section==1);
    for(unsigned i=0;i<500;++i) { h.tick(true,false,false); }CHECK(!h.controller.frame().sceneGeneration);CHECK(!h.controller.frame().finished);
    CHECK(!h.controller.request().preparing);
    for(unsigned i=0;i<400;++i) { h.tick(true,true,false,true,true,false); }
    CHECK(h.controller.request().preparing);CHECK(h.controller.frame().sceneGeneration);
    CHECK(h.dialogueSubmissions[9]==0);CHECK(h.dialogueSubmissions[10]==0);CHECK(!h.controller.frame().finished);
    for(unsigned i=0;i<600;++i) { h.tick(true,true,true,true,true,true,false); }
    CHECK(h.controller.frame().sceneComplete);CHECK(!h.controller.frame().finished);
    for(unsigned i=0;i<380;++i) { h.tick(true,true,false); }CHECK(h.controller.frame().sceneStarted);CHECK(!h.controller.frame().finished);
    for(unsigned i=0;i<100 && !h.controller.frame().finished;++i) { h.tick(true,true,true); }CHECK(h.controller.frame().finished);CHECK(h.controller.frame().completion.valid());CHECK(h.dialogueSubmissions[9]==0);CHECK(h.dialogueSubmissions[10]==0);CHECK(!h.controller.request().enabled);
    // Completion must remain on every outgoing snapshot after the script releases
    // its module lease, or a later keepalive can restore the running state.
    const auto terminal=h.controller.frame().completion;
    for(unsigned i=0;i<12;++i) {
        h.now+=5000;
        const auto sent=h.controller.update(41,h.now,true);
        CHECK(sent.enabled && sent.finished && sent.completion.valid());
        CHECK(sent.completion.owner==terminal.owner);
        wire::Snapshot update{};update.lifetime=3;update.deadly_trial=sent;update.missionCompletion=sent.completion;
        std::array<std::byte,128> body{};bits::Writer writer(body);
        CHECK(wire::write_auth_body(writer,update,0x4786C0E0U,17,3,false));
        CHECK((std::to_integer<unsigned>(body[0])>>4)==7U); // Biased state 6.
        CHECK(((std::to_integer<unsigned>(body[0])>>1)&7U)==2U); // Success 1.
    }
    CHECK(!h.controller.update(42,h.now,true).completion.valid());
    const auto stale=h.controller.request().interaction;const auto old=h.controller.frame().spawnGeneration;
    h.controller.reset();CHECK(h.controller.select(doc->views(),41));CHECK(h.controller.frame().spawnGeneration>old);CHECK(!h.controller.bind(stale));CHECK(!h.controller.died(h.enemy(0)));
    for(const auto& g:t::kGroups) for(const auto& s:g.slots) {
        t::Frame f{};f.enabled=true;f.spawnGeneration=7;f.sceneGeneration=7;f.cohorts=1022;f.pikes=2;f.reviveEnabled=true;
        const auto expected=t::body_bits(f,g.key,s.type,s.index);if(!expected) { continue; }
        auto writer=bits::Writer::measuring();CHECK(t::write_body(writer,f,g.key,s.type,s.index));CHECK(writer.bit_count()==expected);
        f.enabled=false;CHECK(t::body_bits(f,g.key,s.type,s.index)==0);
    }
    presentation_and_overpass(doc->views());post_walker_route(doc->views());roster();lifetime_binding();presentation_binding();std::printf("A Deadly Trial: %u checks passed\n",checks);
}
