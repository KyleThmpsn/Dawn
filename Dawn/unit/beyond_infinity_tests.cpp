#include "../src/state/activity/beyond_infinity/controller.h"
#include "../src/state/activity/beyond_infinity/authority.h"
#include "../src/middleware/encoding/bit_writer.h"
#include "../src/server/bap/encrypted/push/activity/beyond_infinity_roster.h"
#include "../src/client/hooks/bootflow/beyond_infinity_native_receipts.h"
#include "fixtures/beyond_infinity/lens_damage_tests.h"
#include "fixtures/beyond_infinity/plate_timer_tests.h"
#include "fixtures/beyond_infinity/lease_tests.h"
#include "fixtures/beyond_infinity/clock_state_tests.h"
#include "fixtures/beyond_infinity/forest_tests.h"
#include "fixtures/beyond_infinity/transit_tests.h"
#include "fixtures/beyond_infinity/native_past_arrival_tests.h"
#include "fixtures/beyond_infinity/native_future_arrival_tests.h"
#include "fixtures/beyond_infinity/forest_entry_latch_tests.h"
#include "fixtures/beyond_infinity/native_past_return_tests.h"
#include "fixtures/beyond_infinity/future_native_audio_tests.h"
#include "fixtures/beyond_infinity/shield_tests.h"
#include "fixtures/beyond_infinity/ai_tests.h"
#include "fixtures/beyond_infinity/well_dialogue_queue_tests.h"
#include "fixtures/beyond_infinity/future_cast_fallback_tests.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace bi=dawn::state::activity::beyond_infinity;
namespace coo=dawn::state::activity::coo;
using Writer=dawn::middleware::encoding::bits::Writer;
static void check(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1); } }
static const coo::script::Capability& cap(std::string_view id) {
    for(const auto& c:bi::kCapabilities) { if(c.id==id) { return c; } }std::abort();
}
static bi::Point center(const bi::Volume& v) {
    for(unsigned x=1;x<20;++x) for(unsigned y=1;y<20;++y) {
        const bi::Point p{v.min.x+(v.max.x-v.min.x)*float(x)/20,v.min.y+(v.max.y-v.min.y)*float(y)/20,(v.min.z+v.max.z)/2};
        if(bi::contains(v,p)) { return p; }
    }
    return v.vertices.front();
}
static void enter(bi::Controller& c,std::uint64_t run,coo::Asset a) {
    for(const auto& v:bi::kVolumes) { if(v.asset==a) { c.position(run,center(v));return; } }std::abort();
}
static std::string script() {
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"scripts"/"beyond_infinity.lua";
    std::ifstream file(path);std::ostringstream text;text<<file.rdbuf();check(file.good() || file.eof(),"read shipped Lua");return text.str();
}
static auto parse(std::string_view text) {
    std::string error;auto doc=coo::script::MissionDocument::parse_lua(text,bi::kProfile,error);
    if(!doc) { std::fprintf(stderr,"Lua: %s\n",error.c_str()); }return doc;
}
static void wires(const bi::Frame& frame) {
    for(const auto& binding:bi::kAssets) {
        const auto a=binding.asset;const auto bits=bi::body_bits(frame,a.registry,static_cast<std::uint8_t>(a.type),a.slot);
        if(!bits) { continue; }
        auto w=Writer::measuring();
        check(bi::write_body(w,frame,a.registry,static_cast<std::uint8_t>(a.type),a.slot),"write native authority");
        check(w.bit_count()==bits,"native body length parity");
    }
    check(bi::body_bits(frame,0x986985D0U,53,2)==0,"Gateway bank receives no Beyond authority");
    check(bi::body_bits(frame,0xC9BC773AU,68,0)==0,"Deadly Trial receives no Beyond authority");
}
static void roster_checks() {
    namespace r=dawn::server::bap::encrypted::push::activity::beyond_infinity_roster;
    struct Storage { std::array<r::layouts::RosterGroup,20> rosterGroups{};std::array<r::wire::BubbleSubBlock,64> rosterSubBlocks{};std::array<std::array<std::uint32_t,20>,64> rosterSubBlockKeys{}; };
    auto storage=std::make_unique<Storage>();auto cache=std::make_unique<std::array<r::layouts::RosterGroup,std::size(bi::kGroups)>>();r::wire::Roster roster{};
    r::layouts::Definition layout{};layout.tag=bi::kScenario;layout.nameLength=13;std::copy_n("adventure_vod",13,layout.name.begin());layout.bubbleCount=20;
    for(std::size_t i=0;i<std::size(bi::kGroups);++i) {
        const auto& g=bi::kGroups[i];auto& a=(*cache)[i];a.registryKey=g.key;a.objectTag=g.tag;a.slotCount=static_cast<std::uint16_t>(g.slots.size());
        for(std::size_t j=0;j<g.slots.size();++j) { const auto& t=g.slots[j];a.slotTypes[j]=t.type;a.slotFlags[j]=t.flags;a.slotIndices[j]=t.index;a.descriptorTags[j]=t.tag;a.descriptorOffsets[j]=t.offset;a.componentClasses[j]=t.component;a.senseSchemas[j]=t.sense;a.authSchemas[j]=t.auth; }
        check(r::matches(a,g),"exact roster descriptors match");auto corrupt=a;corrupt.authSchemas[0]^=1;check(!r::matches(corrupt,g),"wrong authority schema rejected");
        if(g.topLevel) { auto& d=storage->rosterGroups[roster.groupCount];d=a;roster.groups[roster.groupCount++]={d.registryKey,std::span(d.slotTypes).first(d.slotCount),std::span(d.slotFlags).first(d.slotCount),std::span(d.slotIndices).first(d.slotCount)}; }
    }
    // Leave room for the two ordinary top-level package roots omitted by the
    // authored-script extraction. Their wire identities remain untouched.
    roster.groups[roster.groupCount++].key=0x29D7B029U;
    roster.groups[roster.groupCount++].key=0x96E0CBE5U;
    roster.topLevelGroupCount=roster.groupCount;
    const auto find=[&](std::size_t i,r::layouts::RosterGroup& out) noexcept { if(i>=cache->size()) { return false; }out=(*cache)[cache->size()-1-i];return true; };
    const auto indexByKey=[&](std::uint32_t key,std::uint32_t tag,std::uint16_t& out) noexcept {
        for(std::size_t i=0;i<cache->size();++i) if((*cache)[cache->size()-1-i].registryKey==key && (*cache)[cache->size()-1-i].objectTag==tag) {out=static_cast<std::uint16_t>(i);return true;}
        return false;
    };
    const auto byKey=[&](std::uint32_t key,std::uint32_t tag,r::layouts::RosterGroup& out) noexcept {
        std::uint16_t index{};return indexByKey(key,tag,index) && find(index,out);
    };
    check(r::prepare_layout(layout,indexByKey,find),"mission-local root overlay resolves independently of cache order");
    check(r::admit(layout,*storage,roster,byKey),"full required roster fits unchanged capacity");
    check(roster.groupCount==r::kRequiredGroups+2 && roster.groupCount<=20,"no shared roster capacity change");
    const auto count=roster.groupCount;check(r::admit(layout,*storage,roster,byKey) && roster.groupCount==count,"stable roster across bubble changes");
    layout.tag=0x80F46DB0U;check(!r::prepare_layout(layout,indexByKey,find) && !r::admit(layout,*storage,roster,byKey),"Gateway layout cannot enter Beyond roster adapter");
}
static void well_channel_checks() {
    // The captured native callback prefix uses 0x1408. 0x1460 is its bank
    // field, not the definition offset; rejecting 0x1408 stalled Well creation.
    check(bi::dialogue_identity(0x80F4622BU,0x1408,0x80F1FDF7U,0),"captured Beyond dialogue callback accepted");
    check(!bi::dialogue_identity(0x80F4622BU,0x1460,0x80F1FDF7U,0),"bank-field offset cannot impersonate callback definition");
    check(!bi::dialogue_identity(0x80F4622BU,0x1408,0x80F1FDF7U,49),"out-of-bank row rejected");
    check(!bi::dialogue_identity(0x80F4622BU,0x1408,0x80F48000U,0),"foreign bank rejected");
    bi::Frame frame{};
    for(bool occupied:{false,true,false}) {
        frame.plateOccupied=occupied;
        check(bi::well_channel(38,frame) && bi::well_channel(40,frame),"both beams exist before and during plate entry");
        check(bi::lens_position(frame)==1.F,"occupancy alone cannot remove the box shield");
    }
    frame.lensExposed=true;
    check(bi::lens_position(frame)==.75F,"completed plate effect uses Gateway exposed box position");
    check(bi::well_channel(38,frame) && bi::well_channel(40,frame),"exposure preserves both beam links");
    frame.lensDestroyed=true;
    check(bi::lens_position(frame)==0.F && !bi::well_channel(38,frame),"destroyed box and lower link retire together");
    check(bi::well_channel(40,frame),"upper link continues into Well opening");
    check(bi::lens_position(frame,false)==0.F,"unrequested box channel remains absent");
}
static void native_receipt_checks() {
    namespace n=dawn::client::hooks::bootflow::beyond_infinity_native;
    bi::Frame frame{};frame.enabled=true;frame.spawnGeneration=9;
    const auto& scene=bi::kScenes[bi::scene_index(cap("reveal.scene_if_reveal").spec.asset)];
    frame.native[bi::asset_index(scene.asset)]={9,true,true,true,true};
    n::SceneSample sample{{scene.asset.definition,0x80806266U,0x368},scene.asset,10,11,9,{scene.selectorGraph,0x80806384U,scene.selectorOffset},12,13,12,14,0};
    bi::SceneReceipt receipt{};
    check(n::scene_sample(sample,frame,{17,9},receipt),"native Scene sample authenticates definition, scope, generation, selector and weak owner");
    for(unsigned i=0;i<11;++i) {
        auto bad=sample;
        switch(i) { case 0:++bad.definition.tag;break;case 1:++bad.definition.kind;break;case 2:++bad.definition.offset;break;
        case 3:++bad.scope.registry;break;case 4:++bad.generation;break;case 5:++bad.selector.tag;break;case 6:++bad.selector.kind;break;
        case 7:++bad.self;break;case 8:bad.serial=UINT32_MAX;break;case 9:bad.complete=2;break;case 10:++bad.selector.offset;break; }
        check(!n::scene_sample(bad,frame,{17,9},receipt),"corrupt native Scene identity rejected");
    }
    auto container=sample;container.selector.tag=scene.selector;
    check(!n::scene_sample(container,frame,{17,9},receipt),"Scene container is not its selected graph");
    const auto& speech=scene.speech[0];
    const n::Definition node{scene.selectorGraph,0x808062F6U,speech.definition};
    check(n::speech_node(scene,speech,node,1) && n::speech_node(scene,speech,node,2),"authored speech node running and finished accepted");
    auto wrongNode=node;++wrongNode.offset;
    check(!n::speech_node(scene,speech,wrongNode,2) && !n::speech_node(scene,speech,node,0),"foreign node and idle state cannot finish speech");
    const auto& pastScene=bi::kScenes[bi::scene_index(cap("past.scene_past_echo").spec.asset)];
    check(!pastScene.cues.empty(),"Past exposes authenticated action cues");
    for(const auto& cue:pastScene.cues) {
        const n::Definition action{pastScene.selectorGraph,cue.kind,cue.definition};
        const n::Definition input{pastScene.selectorGraph,cue.signalKind,cue.signalDefinition};
        check(n::cue_node(pastScene,cue,action,input,12,2,1),"instant finished node needs its native start counter");
        check(!n::cue_node(pastScene,cue,action,input,12,2,0),"idle input cannot manufacture a finished cue");
        auto foreign=action;++foreign.tag;
        check(!n::cue_node(pastScene,cue,foreign,input,12,2,1),"foreign action graph cannot emit a cue");
        auto recycled=input;++recycled.offset;
        check(!n::cue_node(pastScene,cue,action,recycled,12,2,1),"wrong input definition cannot authenticate a cue");
        check(!n::cue_node(pastScene,cue,action,input,UINT32_MAX,2,1),"disposed selector cannot emit a cue");
    }
    const auto lower=cap("reflections.scene_echo_intro_six.silent").spec.asset;
    n::SceneSample lowerSample{};lowerSample.scope=lower;lowerSample.selector={0x80EC0966U,0x80806384U,0x1C78};
    frame.sceneRequests[bi::scene_index(lower)].silent=true;
    check(n::silent_voice(lowerSample,frame),"requested lower route voice may be muted");
    auto upperSample=lowerSample;upperSample.scope=cap("reflections.scene_echo_intro_six_right").spec.asset;
    check(!n::silent_voice(upperSample,frame),"higher route voice cannot be muted by lower policy");
    std::array<std::byte,24> callback{};const std::array<std::uint32_t,4> callbackWords{0x80806342U,0,12,0x808062F5U};const std::int64_t callbackOffset=0xA30;
    std::memcpy(callback.data(),callbackWords.data(),16);std::memcpy(callback.data()+16,&callbackOffset,8);
    check(n::silent_callback(callback,12) && !n::silent_callback(callback,13),"mute callback must name the current selector owner");
    for(const auto& selected:bi::kScenes) {
        n::SceneSample current{};current.scope=selected.asset;
        current.selector={selected.selectorGraph,0x80806384U,selected.selectorOffset};
        frame.sceneRequests[bi::scene_index(selected.asset)].silent=true;
        const bool supported=selected.asset.registry==0x1194F70FU && (selected.asset.slot==14
            || selected.asset.slot==19 || selected.asset.slot==22 || selected.asset.slot==24
            || selected.asset.slot==25 || selected.asset.slot==28);
        check(n::silent_voice(current,frame)==supported,"silent Well policy cannot mute another native scene");
        if(!supported) { continue; }
        auto wrongGraph=current;++wrongGraph.selector.offset;
        check(!n::silent_voice(wrongGraph,frame),"wrong selected graph definition cannot be muted");
        for(const auto& spoken:selected.speech) {
            const std::int64_t offset=spoken.offset;std::memcpy(callback.data()+16,&offset,8);
            check(n::silent_callback(callback,12,spoken.offset),"each selected Well callback targets its exact speech node");
            check(!n::silent_callback(callback,12,spoken.offset+1),"another callback node cannot be detached");
        }
    }
    auto after=sample;after.complete=1;check(n::same_scene(sample,after),"same owner may report native completion");++after.serial;check(!n::same_scene(sample,after),"selector recycling across original call rejected");
}
int main() {
    beyond_lens_damage_fixture::run(check);
    beyond_plate_timer_fixture::run(check);
    beyond_lease_fixture::run(check);
    beyond_clock_state_fixture::run(check);
    beyond_forest_fixture::run(check);
    beyond_transit_fixture::run(check);
    beyond_shield_fixture::run(check);
    beyond_ai_fixture::run(check);
    beyond_future_cast_fixture::run(check);
    well_channel_checks();
    native_receipt_checks();
    roster_checks();
    const auto text=script();auto doc=parse(text);check(doc && bi::valid_document(doc->views()),"shipped mission accepted");
    check(doc->views().phases.size()==8,"full route has eight authored phases");
    beyond_native_past_arrival_fixture::run(check,doc->views());
    beyond_native_future_arrival_fixture::run(check,doc->views());
    beyond_forest_entry_latch_fixture::run(check,doc->views());
    beyond_native_past_return_fixture::run(check,doc->views());
    beyond_future_native_audio_fixture::run(check,doc->views());
    beyond_well_dialogue_queue_fixture::run(check,doc->views());
    {
        bi::Controller clockController;
        check(clockController.select(doc->views(),71),"clock owner selected");
        check(!clockController.update(71,90000,false).enabled,"loading cannot start the gameplay clock");
        check(clockController.update(71,100000,true).gameplayClockTicks==0,"arrival starts clock at zero");
        check(clockController.update(71,107000,true).gameplayClockTicks==4712400,"seven seconds match native timer units");
        check(!clockController.update(72,200000,true).enabled,"foreign run cannot publish clock");
        check(clockController.update(71,106000,true).gameplayClockTicks==4712400,"late sample cannot rewind native time");
        check(clockController.update(71,108000,true).gameplayClockTicks==5385600,"clock continues after a stale sample");
        clockController.reset();check(clockController.select(doc->views(),72),"clock replay selects new owner");
        check(clockController.update(72,300000,true).gameplayClockTicks==0,"new run cannot inherit old native epoch");
        check(coo::native_activity_ticks(1)==673 && coo::native_activity_ticks(10)==6732,"native tick conversion keeps subsecond precision");
        check(coo::native_activity_ticks(UINT64_MAX)==UINT64_MAX,"native tick conversion cannot wrap on overflow");
    }
    bi::Frame all{};all.enabled=true;all.spawnGeneration=7;
    for(const bool active:{false,true}) {
        for(auto& n:all.native) { n={7,true,active,true,active}; }
        wires(all);
    }
    auto foreign=text;const auto identity=foreign.find("id=\"beyond_infinity\"");check(identity!=std::string::npos,"mission identity present");
    foreign.replace(identity,20,"id=\"gateway\"");auto other=parse(foreign);
    check(!other || !bi::valid_document(other->views()),"foreign mission rejected");
    bi::Controller controller;
    check(controller.select(doc->views(),17),"select run");
    check(!controller.update(17,1,false).enabled,"no progression during loading");
    controller.position(99,{269,250,87});check(!controller.update(17,2,false).enabled,"foreign position cannot establish arrival");
    auto frame=controller.update(17,3,true);check(frame.enabled,"confirmed arrival starts without a position sample");
    controller.update(17,4,true);frame=controller.update(17,5,true);
    for(unsigned i=0;i<8 && frame.activeRow==coo::kNoDialogue;++i) { frame=controller.update(17,6+i,true); }
    check(frame.activeRow==0,"opening row offered");
    check(!controller.submitted(17,bi::kBank,0,frame.generations[0]+1,30),"wrong playback generation rejected");
    check(!controller.submitted(18,bi::kBank,0,frame.generations[0],30),"wrong run rejected");
    check(controller.submitted(17,bi::kBank,0,frame.generations[0],30),"native playback accepted");
    check(!controller.submitted(17,bi::kBank,0,frame.generations[0],31),"duplicate playback rejected");
    wires(frame);
    // Drive only active authored observations. These are simulated hook inputs,
    // not evidence of a live playthrough or actual native scene-event timing.
    std::uint64_t now=100000;
    bool checkedLens=false,completed=false,beholdFinished=false,beholdHeld=false,checkedSpeech=false;std::uint64_t beholdStarted{};std::array<bool,8> visited{};
    std::uint64_t overlookAt{},futureCueAt{};
    bool jumpHeld{},futureCueHeld{};std::uint8_t previousForestPass{};std::array<bool,3> forestPassChecked{};
    for(unsigned iteration=0;iteration<1500 && !completed;++iteration) {
        frame=controller.update(17,now,true);now+=1000;visited[frame.section]=true;wires(frame);
        if(frame.forestPass!=previousForestPass) {
            check(!frame.forestReady,"changing Forest pass cannot inherit prior native readiness");
            previousForestPass=frame.forestPass;
        }
        if(frame.forestPass>=1 && frame.forestPass<=2 && !forestPassChecked[frame.forestPass]) {
            const auto owner=controller.owner();const auto pass=frame.forestPass;
            check(!controller.forest_ready({owner.run+1,owner.value},pass,true),"Forest rejects foreign run");
            check(!controller.forest_ready({owner.run,owner.value+1},pass,true),"Forest rejects stale incarnation");
            check(!controller.forest_ready(owner,pass==1?2:1,true),"Forest rejects the other traversal");
            check(!controller.forest_ready(owner,0,true) && !controller.forest_ready(owner,3,true),"Forest rejects unsupported pass");
            check(controller.forest_ready(owner,pass,true) && controller.frame().forestReady,"exact native switch readiness enables Forest");
            const auto seed=controller.frame().forestSeed;check(seed!=0,"Forest gets stable positive native seed");
            check(controller.forest_ready(owner,pass,false) && !controller.frame().forestReady,"readiness loss parks native request");
            check(controller.forest_ready(owner,pass,true) && controller.frame().forestSeed==seed,"recovery retains native seed");
            forestPassChecked[pass]=true;
        }

        if(frame.activeRow!=coo::kNoDialogue) {
            check(controller.submitted(17,bi::kBank,frame.activeRow,frame.generations[frame.activeRow],now),"accept current offered row");
        }
        for(std::size_t i=0;i<std::size(bi::kAssets);++i) {
            const auto a=bi::kAssets[i].asset;const auto s=frame.native[i];
            if(a.type==4 && s.managed && !s.prepared) {
                check(!controller.prepared({18,s.generation},a),"foreign preparation rejected");
                check(controller.prepared({17,s.generation},a),"inactive native preparation accepted");
            }
        }
        for(const auto& scene:bi::kScenes) {
            const auto s=frame.native[bi::asset_index(scene.asset)];if(!s.active) { continue; }
            const bi::SceneReceipt receipt{{17,s.generation},scene.asset,10,20,30,40};
            static_cast<void>(controller.scene(receipt,false));static_cast<void>(controller.scene(receipt,true));
            for(const auto& cue:scene.cues) {
                if(scene.asset==cap("past.scene_past_echo").spec.asset && cue.id==17) { continue; }
                if(scene.asset==cap("future.scene_future_echo").spec.asset && cue.id==21) {
                    const auto events=frame.sceneRequests[bi::scene_index(scene.asset)].inputs();
                    if(std::find(events.begin(),events.end(),0x43E472AFU)==events.end()) { continue; }
                    if(!futureCueAt) { futureCueAt=now; }
                    if(now<futureCueAt+35000) {
                        check(!frame.native[bi::asset_index(cap("ambush.lighthouse_teleport_object.on").spec.asset)].desired,
                            "future escape portal waits for native reveal event");
                        futureCueHeld=true;continue;
                    }
                }
                check(!controller.scene_cue(receipt,cue.id,2,0),"zero native activation count cannot finish a cue");
                auto stale=receipt;++stale.serial;
                check(!controller.scene_cue(stale,cue.id,2,1),"recycled selector cannot publish old action completion");
                static_cast<void>(controller.scene_cue(receipt,cue.id,2,1));
            }
            for(const auto& speech:scene.speech) {
                const auto sceneIndex=bi::scene_index(scene.asset);
                if(frame.sceneRequests[sceneIndex].silent) {
                    check(!controller.scene_speech(receipt,speech.row,1),"detached Well speech cannot claim a native spoken milestone");continue;
                }
                if(!checkedSpeech) {
                    check(!controller.scene_speech(receipt,speech.row,2),"finished value without observed native playback is rejected");
                    auto stale=receipt;++stale.owner.value;
                    check(!controller.scene_speech(stale,speech.row,1),"stale scene generation cannot start speech milestone");
                    auto foreignReceipt=receipt;++foreignReceipt.owner.run;
                    check(!controller.scene_speech(foreignReceipt,speech.row,1),"foreign scene run cannot start speech milestone");checkedSpeech=true;
                }
                if(scene.asset==cap("reveal.scene_if_reveal").spec.asset && speech.row==23) {
                    const auto events=frame.sceneRequests[sceneIndex].inputs();
                    if(std::find(events.begin(),events.end(),0xC7ECAA77U)==events.end()) {
                        check(frame.forestPass==0,"Forest stays unreleased while final input is missing");continue;
                    }
                    if(!beholdStarted) { beholdStarted=now; }
                    static_cast<void>(controller.scene_speech(receipt,speech.row,1));
                    if(now<beholdStarted+15000) {
                        check(frame.forestPass==0,"Scene root completion cannot release Forest during Behold");beholdHeld=true;continue;
                    }
                    static_cast<void>(controller.scene_speech(receipt,speech.row,2));beholdFinished=true;continue;
                }
                static_cast<void>(controller.scene_speech(receipt,speech.row,1));
                static_cast<void>(controller.scene_speech(receipt,speech.row,2));
            }
        }
        check(frame.transitRoute==0,"shipped route leaves all portal travel to native transitions");
        const auto* graph=controller.graph();check(graph!=nullptr,"active graph exists");
        for(const auto& b:graph->commands) {
            if(controller.step_state(b.step).phase!=coo::StepPhase::active) { continue; }
            const auto& spec=graph->definition.steps[b.step].commands[b.command];
            if(spec.asset==bi::kModule && spec.argument>=31 && spec.argument<=35) {
                constexpr bi::Point contacts[]{{-973.218506F,1072.88257F,-68.017967F},{748.928650F,709.899658F,2.931518F},
                    {-228.F,1073.F,-60.F},{262.085175F,752.081970F,312.565247F},{41.F,1412.F,5.F}};
                controller.position(17,contacts[spec.argument-31]);
            }
            else if(spec.asset.type==60) {
                if(spec.asset==cap("reveal.tv_if_entered").spec.asset && frame.section==3) {
                    if(!overlookAt) { overlookAt=now; }
                    if(now<overlookAt+15000) {
                        check(frame.generations[30]==0 && frame.activeRow!=30,"overlook presence cannot start Daemon tutorial");
                        jumpHeld=true;continue;
                    }
                }
                enter(controller,17,spec.asset);
            }
            else if(const auto* condition=doc->views().condition(spec)) {
                for(const auto& node:condition->nodes) { if(node.native.asset.type==60) { enter(controller,17,node.native.asset); } }
            } else if(spec.asset==bi::kLens) {
                const auto s=controller.frame().native[bi::asset_index(bi::kLens)];
                if(!s.active) { continue; }
                const bi::LensReceipt receipt{{17,s.generation},0x100000,1,2,3};
                if(!checkedLens) {
                    check(!controller.lens(receipt,true),"death before live lens owner rejected");
                    check(controller.lens(receipt,false),"live lens owner accepted");
                    controller.position(17,{0,0,0});check(!controller.lens(receipt,true),"box remains protected before charge");
                    enter(controller,17,cap("well.plate_occupied").spec.asset);
                    check(!controller.lens(receipt,true),"occupancy does not expose box immediately");
                    const auto plateState=controller.frame().native[bi::asset_index(bi::kPlate)];
                    const bi::PlateReceipt plate{{17,plateState.generation},0x200000,4,5,6,7};
                    const auto revision=controller.frame().plateRevision;
                    check(!controller.plate(plate,revision,1.F,true),"unbound timer completion rejected");
                    auto wrongPlate=plate;++wrongPlate.owner.value;
                    check(!controller.bind_plate(wrongPlate),"foreign plate generation rejected");
                    check(controller.bind_plate(plate),"native plate source and component binding accepted");
            {auto wrongPoseOwner=plate;++wrongPoseOwner.owner.value;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects retired source generation");
             wrongPoseOwner=plate;++wrongPoseOwner.serial;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects recycled entity identity");
             wrongPoseOwner=plate;++wrongPoseOwner.device;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another device component");
             wrongPoseOwner=plate;++wrongPoseOwner.owner.run;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects retired mission run");
             wrongPoseOwner=plate;++wrongPoseOwner.source;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another source address");
             wrongPoseOwner=plate;++wrongPoseOwner.timer;
             check(!controller.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another native timer");
             check(controller.plate_pose(plate,{0.F,0.F,700,900}),"server accepts exact native pose receipt");}

                    check(!controller.plate(plate,revision,1.F,true),"old full value before a running receipt rejected");
                    check(!controller.plate(plate,revision,.3F,false) && !controller.frame().lensExposed,"partial native charge keeps shield");
                    check(!controller.plate(wrongPlate,revision,1.F,true),"foreign plate completion rejected");
                    check(!controller.plate(plate,revision,1.F,false),"full scalar without native latch rejected");
                    check(!controller.plate(plate,revision,std::numeric_limits<float>::quiet_NaN(),true),"NaN native output rejected");
                    controller.position(17,{0,0,0});
                    check(!controller.plate(plate,revision,1.F,true),"leaving plate invalidates in-flight charge");
                    enter(controller,17,cap("well.plate_occupied").spec.asset);
                    const auto retry=controller.frame().plateRevision;
                    check(retry!=revision && !controller.plate(plate,revision,1.F,true),"previous occupancy completion cannot finish re-entry");
                    check(!controller.plate(plate,retry,1.F,true),"re-entry needs its own running timer receipt");
                    static_cast<void>(controller.plate(plate,retry,0.F,false));
                    for(unsigned cycle=0;cycle<80;++cycle) {
                        const auto interrupted=controller.frame().plateRevision;
                        controller.position(17,{0,0,0});
                        check(!controller.frame().lensExposed,"leaving a partial charge keeps box protected");
                        enter(controller,17,cap("well.plate_occupied").spec.asset);
                        const auto chargeRevision=controller.frame().plateRevision;
                        check(chargeRevision!=interrupted,"partial retry receives a fresh native timer revision");
                        check(!controller.plate(plate,interrupted,1.F,true),"interrupted completion cannot unlock box");
                        check(!controller.plate(plate,chargeRevision,.3F,false),"partial native progress is not completion");
                    }
                    const auto completedRevision=controller.frame().plateRevision;
                    check(controller.plate(plate,completedRevision,1.F,true),"native timer endpoint exposes box after running receipt");
                    check(!controller.plate(plate,completedRevision,1.F,true),"duplicate charge completion rejected");
                    const auto completedChannel=controller.frame().native[bi::asset_index(bi::find(0x233E7149U,23,41)->asset)];
                    for(unsigned cycle=0;cycle<80;++cycle) {
                        controller.position(17,{0,0,0});
                        check(controller.frame().lensExposed && bi::lens_position(controller.frame())==.75F,"leaving completed plate cannot restore box barrier");
                        check(controller.frame().plateRevision==completedRevision,"completed native pose is not reset by exit");
                        enter(controller,17,cap("well.plate_occupied").spec.asset);
                        const auto channel=controller.frame().native[bi::asset_index(bi::find(0x233E7149U,23,41)->asset)];
                        check(channel.active && channel.generation==completedChannel.generation,"solved box retains its existing exposed channel on reentry");
                        check(controller.frame().plateRevision==completedRevision,"reentry cannot restart completed charge");
                    }
                    controller.position(17,{0,0,0});
                    auto stale=receipt;++stale.owner.value;check(!controller.lens(stale,true),"stale lens owner rejected");
                    check(controller.lens(receipt,true),"native owned destruction after completed charge accepted");checkedLens=true;
                }
            }
        }
        completed=controller.frame().finished;
    }
    check(beholdHeld && beholdFinished && checkedSpeech,"route waits for authenticated Behold playback before Forest request");
    check(jumpHeld && futureCueHeld,"Forest jump and native future reveal cue gate progression");
    check(checkedLens,"route passes actual lens receipt gate");check(completed,"all authored phases reach completion with required receipts");
    for(bool seen:visited) { check(seen,"every phase exercised"); }
    check(forestPassChecked[1] && forestPassChecked[2],"both native Forest readiness lifecycles covered");
    check(controller.frame().completion.valid(),"completion has current lifecycle owner");
    const auto previous=controller.owner();controller.reset();check(controller.select(doc->views(),17),"same run may be reselected after reset");
    check(controller.owner().value>previous.value,"reset preserves generation high-water mark");
    check(!controller.forest_ready(previous,1,true),"reset rejects previous Forest readiness");
    check(!controller.prepared(previous,bi::kLens),"retired generation rejected on replay");
    const auto& reveal=bi::kScenes[bi::scene_index(cap("reveal.scene_if_reveal").spec.asset)];
    check(reveal.cast.size()==5,"reveal has all five authored cast members");
    auto w=Writer::measuring();check(coo::native_scene::cast_scene(w,3,reveal.cast),"multi-cast scene encodes");
    check(w.bit_count()==349,"five-member Scene width");
    const std::array<std::uint32_t,1> start{0xC7ECAA77U};auto eventsWriter=Writer::measuring();
    check(coo::native_scene::cast_scene(eventsWriter,3,reveal.cast,start) && eventsWriter.bit_count()==381,"five-member Scene includes the native 32-bit start event");
    const std::array<std::uint32_t,2> duplicate{start[0],start[0]};auto invalidWriter=Writer::measuring();
    check(!coo::native_scene::cast_scene(invalidWriter,3,reveal.cast,duplicate),"duplicate native input is rejected");
    check(!coo::native_scene::cast_scene(invalidWriter,0,{},start),"dormant Scene cannot carry active input");
    const auto& v=bi::kVolumes[0];auto p=center(v);p.x=std::numeric_limits<float>::quiet_NaN();check(!bi::contains(v,p),"NaN cannot enter a trigger");
    std::puts("PASS: Beyond Infinity Lua route, authentic receipt guards, reset isolation, Scene cast and authority widths (offline simulation)");
}
