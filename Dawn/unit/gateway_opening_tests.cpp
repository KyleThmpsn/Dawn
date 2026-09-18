#include "state/activity/gateway/controller.h"
#include "state/activity/gateway/authority.h"
#include "state/activity/gateway/ending_cadence.h"
#include "state/activity/omega/omega_ikora_authority.h"
#include "state/activity/gateway/preparation.h"
#include "client/hooks/bootflow/gateway_module_native_path.h"
#include "client/hooks/bootflow/gateway_vance_native_path.h"
#include "client/hooks/bootflow/gateway_module_damage.h"
#include "client/hooks/bootflow/gateway_patrol_native.h"
#include "state/activity/omega_rescue_scene_authority.h"
#include "fixtures/gateway_traversal_wire.h"
#include "fixtures/gateway_mainland_wire.h"
#include "fixtures/gateway_ending_wire.h"
#include "fixtures/reinforcement_counts.h"
#include "server/bap/encrypted/push/activity/gateway_roster.h"
#include "middleware/bap/activity_message/sensor_auth_update.h"
#include "middleware/encoding/bit_reader.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <map>
namespace g=dawn::state::activity::gateway;
namespace c=dawn::state::activity::coo;
namespace r=dawn::server::bap::encrypted::push::activity::gateway_roster;
namespace wire=dawn::middleware::bap::activity_message::sensor_auth_update;
namespace bits=dawn::middleware::encoding::bits;
unsigned checks{};
#define CHECK(x) do { ++checks;if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1); } } while(false)
struct Storage {
    std::array<r::layouts::RosterGroup,16> rosterGroups{};
    std::array<wire::BubbleSubBlock,4> rosterSubBlocks{};
    std::array<std::array<std::uint32_t,32>,4> rosterSubBlockKeys{};
};
r::layouts::RosterGroup group(const g::Group& expected) {
    r::layouts::RosterGroup result{};result.registryKey=expected.key;result.objectTag=expected.tag;
    result.slotCount=static_cast<std::uint16_t>(expected.slots.size());
    for(std::size_t i=0;i<expected.slots.size();++i) {
        const auto& slot=expected.slots[i];result.slotTypes[i]=slot.type;result.slotFlags[i]=slot.flags;result.slotIndices[i]=slot.index;
        result.descriptorTags[i]=slot.tag;result.descriptorOffsets[i]=slot.offset;result.componentClasses[i]=slot.component;
        result.senseSchemas[i]=slot.sense;result.authSchemas[i]=slot.auth;
    }
    return result;
}
void roster_tests() {
    auto storage=std::make_unique<Storage>();wire::Roster roster{};
    r::layouts::Definition layout{};layout.tag=g::kScenario;layout.nameLength=11;
    std::copy_n("mission_abs",11,layout.name.begin());layout.bubbleCount=20;layout.bubbleHashes[15]=g::kBubbleHash;
    auto cache=std::make_unique<std::array<r::layouts::RosterGroup,std::size(g::kGroups)>>();
    for(std::size_t i=0;i<std::size(g::kGroups);++i) {
        (*cache)[i]=group(g::kGroups[i]);CHECK(r::matches((*cache)[i],g::kGroups[i]));
        auto altered=(*cache)[i];altered.descriptorOffsets[0]^=8;CHECK(!r::matches(altered,g::kGroups[i]));
        if(!g::kGroups[i].topLevel) { continue; }
        auto& dest=storage->rosterGroups[roster.groupCount];dest=(*cache)[i];
        roster.groups[roster.groupCount++]={dest.registryKey,std::span(dest.slotTypes).first(dest.slotCount),
            std::span(dest.slotFlags).first(dest.slotCount),std::span(dest.slotIndices).first(dest.slotCount)};
    }
    roster.topLevelGroupCount=roster.groupCount;
    const auto lookup=[&](std::uint32_t key,std::uint32_t tag,r::layouts::RosterGroup& out) noexcept {
        for(const auto& row:*cache) if(row.registryKey==key && row.objectTag==tag) {out=row;return true;}
        return false;
    };
    CHECK(r::admit(layout,*storage,roster,lookup));CHECK(roster.groupCount==std::size(g::kGroups));
    const auto count=roster.groupCount,keys=roster.bubbleSubBlocks[0].keys.size();
    CHECK(r::admit(layout,*storage,roster,lookup));CHECK(roster.groupCount==count);CHECK(roster.bubbleSubBlocks[0].keys.size()==keys);
    CHECK(roster.bubbleSubBlocks[0].bubble==15);
    wire::Snapshot snapshot{};snapshot.roster=roster;snapshot.lifetime=3;
    snapshot.gateway.enabled=true;snapshot.gateway.objective=g::kObjectives[0];snapshot.gateway.activeRow=1;snapshot.gateway.generations[1]=1;
    std::array<std::byte,16384> packet{};std::size_t written{};
    CHECK(wire::encode_sensor_auth_update(snapshot,packet,written));CHECK(written>3000 && written<packet.size());
    std::printf("Gateway full roster packet: %zu bytes, %zu groups\n",written,roster.groupCount);
    snapshot.gateway.spawnGeneration=7;snapshot.gateway.cohorts=32767;snapshot.gateway.section=1;snapshot.gateway.preparedMask=127;snapshot.gateway.sceneGeneration=7;snapshot.gateway.marchers=true;
    std::array<std::byte,32768> traversalPacket{};
    CHECK(wire::encode_sensor_auth_update(snapshot,traversalPacket,written));
    std::printf("Gateway traversal roster packet: %zu bytes\n",written);
    layout.bubbleHashes[15]^=1;CHECK(!r::admit(layout,*storage,roster,lookup));layout.bubbleHashes[15]^=1;
    (*cache)[3].authSchemas[0]^=1;CHECK(!r::admit(layout,*storage,roster,lookup));
}
void codec_tests() {
    wire::Snapshot gateway{},omega{};gateway.gateway.enabled=true;gateway.gateway.objective=g::kObjectives[0];
    gateway.gateway.generations[1]=8;gateway.gateway.generations[2]=7;gateway.gateway.activeRow=1;
    omega.archiveOmega=true;omega.omegaSceneAuthority=true;omega.omegaDialogueArm=true;omega.omegaObjectiveEvent=g::kObjectives[0];
    omega.omegaDialogueGenerations[1]=8;omega.omegaDialogueGenerations[2]=7;omega.omegaActiveDialogueRow=1;
    for(const auto type:{53,68}) {
        std::array<std::byte,4096> a{},b{};bits::Writer left(a),right(b);
        const auto slot=static_cast<std::uint16_t>(type==53?2:0);
        CHECK(wire::write_auth_body(left,gateway,g::kRoot,static_cast<std::uint8_t>(type),slot,false));
        CHECK(wire::write_auth_body(right,omega,0x82FB58B7U,static_cast<std::uint8_t>(type),slot,false));
        CHECK(left.bit_count()==right.bit_count());
        if(a!=b) { for(std::size_t i=0;i<a.size();++i) { if(a[i]!=b[i]) { std::fprintf(stderr,"type=%d byte=%zu actual=%02X reference=%02X\n",type,i,std::to_integer<unsigned>(a[i]),std::to_integer<unsigned>(b[i]));break; } } }CHECK(a==b);
        CHECK(left.bit_count()==(type==53?19895U:4802U));
    }
    // Replay successive publications into retained decoded optional times. An
    // absent field leaves its previous value; retired rows must explicitly zero it.
    std::array<std::uint64_t,128> times{};
    gateway.gateway.generations={};gateway.gateway.generations[1]=1;
    for(const auto row:std::array<std::uint8_t,4>{1,2,255,1}) {
        gateway.gateway.activeRow=row;
        if(row!=255)++gateway.gateway.generations[row];
        std::array<std::byte,4096> bytes{};bits::Writer out(bytes);
        CHECK(g::write_body(out,gateway.gateway,g::kRoot,53,2));
        CHECK(out.bit_count()==g::body_bits(gateway.gateway,g::kRoot,53,2));
        bits::Reader in(bytes);CHECK(in.skip(55));
        const auto field=[&](std::uint8_t width) {std::uint64_t value{};CHECK(in.read(width,value));return value;};
        for(std::size_t i=0;i<128;++i) {
            CHECK(field(64)==UINT64_MAX);
            if(field(1))times[i]=field(64);
            CHECK(in.skip(55));
            const auto generation=i<gateway.gateway.generations.size()?gateway.gateway.generations[i]:0;
            CHECK(field(32)==0x80000000ULL+generation);
            CHECK(field(2)==(i==row?3U:1U));
            CHECK(times[i]==(i==row?1U:0U));
        }
        CHECK(bytes.size()*8-in.remaining_bits()==out.bit_count());
    }
    gateway.gateway.generations[2]=0x80000000U;
    auto rejected=bits::Writer::measuring();
    CHECK(!g::write_body(rejected,gateway.gateway,g::kRoot,53,2));
    CHECK(!rejected.bit_count() && !g::body_bits(gateway.gateway,g::kRoot,53,2));
    gateway.gateway.generations[2]=1;
    gateway.gateway.activeRow=128;
    auto badSentinel=bits::Writer::measuring();
    CHECK(!g::write_body(badSentinel,gateway.gateway,g::kRoot,53,2) && !badSentinel.bit_count());
    auto writer=bits::Writer::measuring();gateway.gateway.activeRow=16;
    CHECK(!g::write_body(writer,gateway.gateway,g::kRoot,53,2));CHECK(writer.bit_count()==0);
    gateway.gateway.enabled=false;CHECK(g::body_bits(gateway.gateway,g::kRoot,53,2)==0);
    gateway.gateway.enabled=true;CHECK(g::body_bits(gateway.gateway,0x82FB58B7U,53,2)==0);
}

g::Point interior(std::uint16_t slot,std::uint32_t registry=g::kTraversalRegistry) {
    for(const auto& v:g::kVolumes) { if(v.registry!=registry || v.slot!=slot) { continue; }
        for(int x=1;x<20;++x) for(int y=1;y<20;++y) {
            g::Point p{v.min.x+(v.max.x-v.min.x)*static_cast<float>(x)/20.F,v.min.y+(v.max.y-v.min.y)*static_cast<float>(y)/20.F,(v.min.z+v.max.z)/2.F};
            if(g::contains(v,p)) { return p; }
        }
    }
    CHECK(false);return {};
}
void traversal_tests(const c::script::Views& views,bool retainShelfActor,bool skipShelfReinforcement,std::uint32_t returnDelay=g::kReturnCueMs) {
    g::Controller controller;constexpr std::uint64_t run=90;CHECK(controller.select(views,run));
    std::uint64_t now=100;g::Frame frame{};
    const auto tick=[&] { for(unsigned i=0;i<4;++i) { frame=controller.update(run,++now,true); } };
    controller.position(run,interior(359));tick();CHECK(frame.activeRow==1);
    CHECK(frame.marchers);CHECK(frame.cohorts==1);
    CHECK(g::body_bits(frame,g::kTraversalRegistry,23,8)==147);
    CHECK(controller.submitted(run,g::kBank,1,frame.generations[1],now));tick();
    controller.position(run,interior(361));tick();CHECK(frame.cohorts==3);CHECK(frame.marchers);CHECK(!frame.cannons);
    const auto generation=frame.spawnGeneration;
    CHECK(frame.sceneGeneration==generation && g::vance_event_count(frame)==0);
    g::SceneReceipt scene{run,generation,0xDAAA0001U,0xDBBB0002U,0xDCCC0003U};
    CHECK(!controller.scene(scene,true));auto badScene=scene;badScene.generation++;
    CHECK(!controller.scene(badScene,false));CHECK(controller.scene(scene,false));
    CHECK(!controller.scene(scene,false));CHECK(!controller.scene(scene,true));
    // The cast may exist from arrival, but proximity cannot begin his closing dialogue early.
    CHECK(!frame.vanceEntered && !frame.vanceConversation);
    g::ModuleReceipt module{run,generation+1U,0x1234A2340123ULL,0xB3450456U,9,0xC4560789U};
    // Arrival binds a live module. Early shots cannot retire it or open the barrier.
    CHECK(!controller.module(module,false));CHECK(controller.prepared(run,generation,5));tick();
    CHECK(!controller.module(module,true));auto badModule=module;badModule.run++;
    CHECK(!controller.module(badModule,false));badModule=module;badModule.generation--;
    CHECK(!controller.module(badModule,false));CHECK(controller.module(module,false));
    CHECK(!controller.module(module,false));CHECK(!controller.module(module,true));
    CHECK(controller.ending_request().owner==module);CHECK(!frame.moduleDestroyed && !frame.lighthouseOpen);
    // Each linked device waits for its own native entity/controller and revision receipt.
    std::array<c::ObjectReceipt,3> objectReceipts{};
    for(std::size_t i=0;i<3;++i) {
        if(i!=1) { CHECK(controller.prepared(run,generation,g::kObjectPreparation[i])); }tick();
        auto& receipt=objectReceipts[i];receipt={{run,generation+1U},g::kEndingObjects[i].source,100+static_cast<unsigned>(i),50,UINT32_MAX};
        CHECK(frame.objects[i].phase==c::ObjectPhase::create && !frame.objects[i].apply);
        CHECK(controller.object(i,receipt,false,0,-1));tick();CHECK(frame.objects[i].phase==c::ObjectPhase::bind);
        receipt.controller=200+static_cast<unsigned>(i);CHECK(controller.object(i,receipt,false,0,-1));tick();
        CHECK(frame.objects[i].apply && frame.objects[i].position==1.F);
        CHECK(controller.object(i,receipt,true,1.F,frame.objects[i].revision));tick();CHECK(frame.objects[i].phase==c::ObjectPhase::ready);
    }
    CHECK(!frame.pendingServices);
    std::uint32_t nextActor=0x12340000U;
    g::EnemyReceipt shelfStraggler{};
    const auto clear=[&](std::uint8_t cohort) {
        for(const auto& source:g::kSpawns) { if(source.cohort!=cohort) { continue; }
            for(unsigned n=0;n<source.count;++n) {
                g::EnemyReceipt receipt{run,++nextActor,0x23450123U,generation,source.source,source.registry};
                CHECK(!controller.died(receipt));
                auto wrong=receipt;wrong.generation++;CHECK(!controller.admitted(wrong));
                wrong=receipt;wrong.run++;CHECK(!controller.admitted(wrong));
                CHECK(controller.admitted(receipt));CHECK(!controller.admitted(receipt));
                wrong=receipt;wrong.owner++;CHECK(!controller.died(wrong));
                if(retainShelfActor && source.registry==g::kTraversalRegistry && source.source==231) { shelfStraggler=receipt;continue; }
                CHECK(controller.died(receipt));CHECK(!controller.died(receipt));
            }
        }
        tick();
    };
    // No timer may open a cannon or synthesize an enemy death.
    now+=60000;tick();CHECK(frame.cohorts==3 && !frame.cannons);
    // Backline play: the first clear starts reinforcements without entering their volume.
    clear(1);CHECK(frame.cohorts==7);clear(2);CHECK(!frame.cannons);
    CHECK(frame.cohorts==47); // Both later platforms are placed before either arrival.
    CHECK(!controller.prepared(run,generation+1,0));CHECK(!controller.prepared(run+1,generation,0));
    CHECK(controller.prepared(run,generation,0));tick();CHECK(!frame.cannons);
    CHECK(controller.prepared(run,generation,1));tick();if(!frame.cannons) { const auto d=controller.diagnostics();std::fprintf(stderr,"cannon blocked phase=%u active=%08X complete=%08X cohorts=%X fault=%u\n",unsigned(d.phase),d.active,d.complete,frame.cohorts,unsigned(frame.populationFault)); } CHECK(frame.cannons);
    CHECK(!controller.prepared(run,generation,1));
    controller.position(run,interior(369));tick();CHECK(frame.cohorts==47);
    // Forward play can request reinforcements before the first cohort dies.
    if(!skipShelfReinforcement) { controller.position(run,interior(372));tick();CHECK(frame.cohorts==63);clear(4); }
    CHECK(!frame.finalCannon);clear(3);
    // Captured run: all shelf actors except source 231 have died. Time cannot resolve it.
    CHECK(shelfStraggler.valid()==retainShelfActor);now+=60000;tick();CHECK(frame.cohorts==(skipShelfReinforcement?47U:63U));CHECK(!frame.finalCannon);
    controller.position(run,interior(375));tick();CHECK(frame.cohorts==63);
    if(skipShelfReinforcement) { clear(4); }
    // Reaching the final ledge starts its real encounter despite the surviving shelf actor.
    CHECK(!frame.finalCannon);clear(5);CHECK(frame.cohorts==127);CHECK(!frame.finalCannon);
    clear(6);tick();CHECK(frame.finalCannon);
    CHECK(frame.cohorts==511); // Mainland placed before the final cannon is taken.
    // Shelf passage did not fabricate that actor's death or discard its native identity.
    if(retainShelfActor) { CHECK(controller.died(shelfStraggler));CHECK(!controller.died(shelfStraggler)); }
    CHECK(frame.activeRow==2);CHECK(!frame.checked);
    // A receipt for opening row1 cannot acknowledge Vance row2.
    CHECK(!controller.submitted(run,g::kBank,1,frame.generations[1],now));tick();CHECK(!frame.checked);
    CHECK(controller.submitted(run,g::kBank,2,frame.generations[2],now));tick();CHECK(!frame.checked);
    controller.position(run,interior(378));tick();CHECK(!frame.checked);
    CHECK(frame.cohorts==511);CHECK(frame.objective==g::kObjectives[1]);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,1,49)==641);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,1,47)==673);
    // Mainland actors use scoped source identities; a matching slot in the other registry is not proof.
    auto foreign=g::EnemyReceipt{run,++nextActor,0x23450123U,generation,49,g::kTraversalRegistry};
    CHECK(!controller.admitted(foreign));
    controller.position(run,interior(439,g::kMainlandRegistry));tick();
    const auto speak=[&](std::uint8_t row) {
        if(frame.activeRow==c::kNoDialogue) { now+=25000;tick(); }
        CHECK(frame.activeRow==row);
        CHECK(!controller.submitted(run,g::kBank,static_cast<std::uint8_t>(row-1),frame.generations[row],now));
        CHECK(controller.submitted(run,g::kBank,row,frame.generations[row],now));tick();
    };
    speak(3);
    // The ready line starts during the Hydra fight, without clearing cohort 8.
    controller.position(run,interior(441,g::kMainlandRegistry));tick();
    speak(4);CHECK(!frame.checked);CHECK(!frame.generations[5]);
    // First barrier contact queues the exchange even with defenders alive.
    controller.position(run,interior(448,g::kMainlandRegistry));now+=60000;tick();
    CHECK(!frame.checked);CHECK(frame.objective==g::kObjectives[1]);CHECK(frame.activeRow==5);
    const auto blockedGeneration=frame.generations[5];
    controller.position(run,interior(448,g::kMainlandRegistry));tick();
    CHECK(frame.generations[5]==blockedGeneration); // Repeated contact cannot replay it.
    // Only the Hydra is a kill gate. Admit its defenders and leave them alive.
    for(const auto& source:g::kSpawns) if(source.cohort==8) {
        for(unsigned n=0;n<source.count;++n) {
            g::EnemyReceipt receipt{run,++nextActor,0x23450123U,generation,source.source,source.registry};
            CHECK(controller.admitted(receipt));
            if(source.source==49) { CHECK(controller.died(receipt)); }
        }
    }
    tick();CHECK(!frame.checked);
    const c::CommandSpec returnCue{c::Operation::eventAfter,g::kBlockedDialogueClock,returnDelay,c::Wait::observed};
    CHECK(frame.returnCuePending && !frame.openingChecked);
    CHECK(controller.missing(returnCue).missing==c::Missing::eventOrigin);
    // Queueing, elapsed queue time, wrong receipts, and delayed native dispatch
    // must not start return enemies or move the objective.
    now+=1000;tick();CHECK(frame.activeRow==5);
    now+=g::kReturnCueMs+100;frame=controller.update(run,now,true);
    CHECK(!frame.openingChecked && frame.cohorts==511 && frame.objective==g::kObjectives[1]);
    CHECK(!controller.submitted(run+1,g::kBank,5,frame.generations[5],now));
    CHECK(!controller.submitted(run,g::kBank,5,frame.generations[5]+1,now));
    CHECK(!controller.submitted(run,g::kBank+1,5,frame.generations[5],now));
    CHECK(controller.missing(returnCue).missing==c::Missing::eventOrigin);
    const auto exchangeAt=now;const auto exchangeGeneration=frame.generations[5];
    CHECK(controller.submitted(run,g::kBank,5,exchangeGeneration,exchangeAt));
    CHECK(!controller.submitted(run,g::kBank,5,exchangeGeneration,exchangeAt+100));
    frame=controller.update(run,exchangeAt,true);CHECK(controller.missing(returnCue).missing==c::Missing::timer);
    now=exchangeAt+returnDelay-1;frame=controller.update(run,now,true);
    CHECK(!frame.openingChecked && frame.cohorts==511 && frame.objective==g::kObjectives[1]);
    CHECK(frame.returnCuePending);
    now=exchangeAt+returnDelay;frame=controller.update(run,now,true);
    CHECK(frame.openingChecked && !frame.checked && frame.section==1 && !frame.returnCuePending);
    CHECK(controller.missing(returnCue).missing==c::Missing::none);
    // Both cohorts and the final Lighthouse objective are present in ONE publication.
    // The remaining audio still owns the voice channel after the cue.
    CHECK(frame.activeRow==c::kNoDialogue);
    CHECK(frame.objective==g::kObjectives[3]);
    CHECK(frame.presentation.active && frame.presentation.marker.asset==g::kMarkers[1].target.asset);
    CHECK((frame.cohorts&(1U<<9)) && (frame.cohorts&(1U<<10)));
    // Hydra death plus the exchange cue starts wave one without return kills or travel.
    CHECK(frame.cohorts&(1U<<11));
    clear(9);CHECK(frame.activeRow==c::kNoDialogue);
    now=exchangeAt+views.dialogue.rows[5].durationMs+views.dialogue.rows[5].delayMs+views.dialogue.spacingMs-1;
    frame=controller.update(run,now,true);CHECK(frame.activeRow==c::kNoDialogue);
    ++now;frame=controller.update(run,now,true);CHECK(frame.activeRow==6);speak(6);
    tick();CHECK(frame.cohorts&(1U<<11)); // No additional proximity trigger.
    now+=60000;tick();CHECK(!(frame.cohorts&(1U<<12)));CHECK(!frame.moduleVulnerable);
    clear(11);CHECK(frame.cohorts&(1U<<12));clear(12);CHECK(frame.cohorts&(1U<<13));
    CHECK(frame.cohorts&(1U<<14));CHECK(!frame.moduleVulnerable);clear(13);
    CHECK(frame.moduleVulnerable);CHECK(!frame.lighthouseOpen);speak(7);
    CHECK(frame.objects[1].position==.75F && frame.objects[1].phase==c::ObjectPhase::apply);
    CHECK(frame.objects[0].position==1.F && frame.objects[2].position==1.F);
    CHECK(controller.object(1,objectReceipts[1],true,.75F,frame.objects[1].revision));tick();

    CHECK(controller.ending_request().owner==module); // Boss exposure preserves the live entity.
    now+=60000;tick();CHECK(!frame.lighthouseOpen);CHECK(!frame.moduleDestroyed);
    // Boss death and player overlap do not substitute for module destruction.
    controller.position(run,interior(11,0xBA0B27A0U));tick();CHECK(!frame.lighthouseOpen);
    badModule=module;badModule.entity^=0x2000;CHECK(!controller.module(badModule,true));
    badModule=module;badModule.serial++;CHECK(!controller.module(badModule,true));
    badModule=module;badModule.health++;CHECK(!controller.module(badModule,true));
    CHECK(controller.module(module,true));CHECK(!controller.module(module,true));
    for(const auto& object:controller.frame().objects) { CHECK(object.position==0.F && !object.create && object.phase==c::ObjectPhase::retired); }

    frame=controller.update(run,++now,true);
    // First authority update after the real death clears BOTH beam device and source.
    std::array<std::byte,32> beamDevice{},beamSource{};bits::Writer bd(beamDevice),bs(beamSource);
    CHECK(g::write_body(bd,frame,g::kMainlandRegistry,23,3));
    CHECK(std::all_of(beamDevice.begin(),beamDevice.begin()+4,[](auto b) { return b==std::byte{}; }));
    CHECK(g::write_body(bs,frame,g::kMainlandRegistry,4,33));CHECK((std::to_integer<unsigned>(beamSource[8])&0x80U)==0);
    tick();CHECK(frame.moduleDestroyed && frame.lighthouseOpen);speak(8);tick();speak(10);
    CHECK(frame.objective==g::kObjectives[6]);CHECK(frame.sceneGeneration==generation);
    CHECK(frame.vanceEntered && !frame.vanceConversation && g::vance_event_count(frame)==1);
    CHECK(frame.generations[10]==generation);CHECK(frame.generations[9]==0); // Invitation at entry, before approach.
    const bool approachLate=retainShelfActor && !skipShelfReinforcement;
    if(!approachLate) { controller.position(run,interior(13,0xBA0B27A0U));tick(); } // One approach, during the invitation.
    now+=1000;tick();speak(9);CHECK(frame.sceneGeneration==generation);
    CHECK(frame.generations[11]==0 && frame.generations[15]==0); // Native Scene owns closing dialogue.
    CHECK(!frame.vanceConversation);CHECK(!frame.conversationStarted);
    CHECK(frame.sceneStarted);CHECK(!controller.scene(scene,false));
    const auto rejectVance=[&](const auto& receipt) {
        CHECK(!controller.vance(receipt,g::VanceMilestone::turned,now));
        CHECK(!controller.vance(receipt,g::VanceMilestone::conversationStarted,now));
    };
    badScene=scene;badScene.run++;rejectVance(badScene);
    badScene=scene;badScene.generation++;rejectVance(badScene);
    badScene=scene;badScene.group++;rejectVance(badScene);
    badScene=scene;badScene.sensor++;rejectVance(badScene);
    badScene=scene;badScene.selector^=0x2000;rejectVance(badScene);
    CHECK(!controller.vance(scene,g::VanceMilestone::conversationStarted,now));
    CHECK(controller.vance(scene,g::VanceMilestone::turned,now));
    CHECK(!controller.vance(scene,g::VanceMilestone::turned,now));tick();
    CHECK(frame.vanceTurned && !frame.vanceConversation); // Ghost's entry line is still speaking.
    now+=4580;tick();
    if(approachLate) {
        CHECK(!frame.vanceConversation);controller.position(run,interior(13,0xBA0B27A0U));tick();
    }
    CHECK(frame.vanceConversation && g::vance_event_count(frame)==2); // No exit/re-entry in either order.
    CHECK(!controller.vance(scene,static_cast<g::VanceMilestone>(255),now));
    now+=120000;tick();CHECK(!frame.checked && !frame.finished && frame.lighthouseChannels==0);
    CHECK(!controller.scene(badScene,true)); // Stale selector cannot complete this owner.
    const auto conversationAt=now;
    CHECK(controller.vance(scene,g::VanceMilestone::conversationStarted,conversationAt));tick();
    CHECK(frame.conversationStarted && !frame.sceneComplete);
    CHECK(!controller.vance(scene,g::VanceMilestone::conversationStarted,now+50000));
    // A missing native child cannot be replaced by elapsed wall time. Once it starts,
    // exact cue boundaries control presentation without fabricating scene completion.
    now=conversationAt+g::kVanceAscentMs-1;frame=controller.update(run,now,true);
    CHECK(frame.lighthouseChannels==0 && !frame.finished);
    now=conversationAt+g::kVanceAscentMs;tick();CHECK(frame.lighthouseChannels==3 && !frame.finished);
    now=conversationAt+g::kVanceFinishMs-1;frame=controller.update(run,now,true);
    CHECK(!frame.finished && !frame.checked && !frame.sceneComplete);
    now=conversationAt+g::kVanceFinishMs;tick();CHECK(frame.checked && frame.finished);
    CHECK(!frame.sceneComplete); // The actor's native idle timeline deliberately stays alive.
    CHECK(frame.completion.valid() && frame.completion.owner.run==run);CHECK(!frame.presentation.active && !frame.presentation.marker.valid());
    CHECK(controller.diagnostics().phase==c::Phase::complete);
    // The outskirts are a route fight; no distant optional actor is a gate-clear requirement.
    // Closed lattice persists after the blocked-entry dialogue; no Forest transition.
    CHECK(g::body_bits(frame,g::kMainlandRegistry,23,2)==147);
    // No marcher admission or death was required to reach the physical landing.
    const auto prior=generation;controller.reset();CHECK(controller.select(views,run));
    controller.position(run,interior(359));tick();CHECK(frame.spawnGeneration>module.generation);
    CHECK(frame.sceneGeneration==frame.spawnGeneration && g::vance_event_count(frame)==0);
    CHECK(!frame.returnCuePending && controller.missing(returnCue).missing==c::Missing::eventOrigin);
    CHECK(!controller.submitted(run,g::kBank,5,exchangeGeneration,now));
    CHECK(!frame.completion.valid());for(std::size_t i=0;i<3;++i) { CHECK(!controller.object(i,objectReceipts[i],true,1.F,1)); }
    CHECK(!frame.vanceTurned && !frame.conversationStarted && frame.lighthouseChannels==0);
    CHECK(!controller.vance(scene,g::VanceMilestone::conversationStarted,now));
    CHECK(!controller.prepared(run,prior,0));CHECK(!controller.module(module,true));CHECK(!controller.scene(scene,true));
    CHECK(!controller.admitted({run,0x12341234U,0x23450123U,prior,214,g::kTraversalRegistry}));
    CHECK(controller.submitted(run,g::kBank,1,frame.generations[1],now));tick();controller.position(run,interior(361));tick();
    const auto gen=frame.spawnGeneration;
    CHECK(controller.admitted({run,0x12341234U,0x23450123U,gen,214,g::kTraversalRegistry}));
    CHECK(!controller.admitted({run,0x12341235U,0x23450123U,gen,214,g::kTraversalRegistry}));
    tick();CHECK(!frame.enabled);CHECK(controller.diagnostics().phase==c::Phase::failed);
}
void ai_lattice_tests() {
    // Decode the beginning of each real source body: a scoped type3 reference,
    // never a marcher group from the other registry with the same slot number.
    for(const auto& source:g::kSpawns) {
        const auto task=g::tactical_group(source);
        CHECK(task.registry==source.registry);CHECK(task.row>=0 && task.row<24);
        bool found=false;
        for(const auto& group:g::kGroups) if(group.key==task.registry) {
            for(const auto& slot:group.slots) if(slot.type==3 && slot.index==task.slot) { found=true; }
        }
        CHECK(found);
        if(source.cohort>0) { CHECK(task.slot==(source.cohort<=2?6:source.cohort<=4?7:source.cohort<=6?5:source.cohort==7?17:source.cohort==8?16:source.cohort==9?22:source.cohort==10?21:source.cohort==11?18:source.cohort==12?19:20)); }
        g::Frame frame{};frame.enabled=true;frame.spawnGeneration=7;frame.cohorts=1U<<source.cohort;
        std::array<std::byte,128> actual{},expected{};bits::Writer a(actual),e(expected);
        CHECK(g::write_body(a,frame,source.registry,1,source.source));
        CHECK(e.write(1,1) && e.write(task.registry,32) && e.write(4,7) && e.write(32768U+task.slot,16));
        CHECK(std::equal(expected.begin(),expected.begin()+7,actual.begin()));
        frame.cohorts=0;CHECK(g::body_bits(frame,source.registry,1,source.source)==0);
    }
    g::Frame frame{};frame.enabled=true;
    // This device is present even before the first spawn generation exists.
    std::array<std::byte,32> actual{},accepted{};bits::Writer a(actual),b(accepted);
    CHECK(g::write_body(a,frame,g::kMainlandRegistry,23,2));CHECK(a.bit_count()==147);
    CHECK(dawn::state::activity::omega::ikora::write_gate(b,false));CHECK(actual==accepted);
    // Boundary checks: neither other device slots nor another registry receive it.
    CHECK(g::body_bits(frame,g::kMainlandRegistry,23,0)==0);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,23,1)==0);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,4,2)==0);
    CHECK(g::body_bits(frame,g::kTraversalRegistry,23,2)==0);
    frame.spawnGeneration=7;frame.finalCannon=true;frame.checked=true;
    std::array<std::byte,32> final{};bits::Writer end(final);
    CHECK(g::write_body(end,frame,g::kMainlandRegistry,23,2));CHECK(final==accepted);
    frame.enabled=false;CHECK(g::body_bits(frame,g::kMainlandRegistry,23,2)==0);
}
void traversal_wire_tests() {
    g::Frame frame{};frame.enabled=true;frame.spawnGeneration=7;frame.cohorts=511;frame.marchers=true;
    for(const auto& group:g::kGroups) for(const auto& slot:group.slots) {
        const auto expected=g::body_bits(frame,group.key,slot.type,slot.index);
        if(!expected) { continue; }
        auto writer=bits::Writer::measuring();CHECK(g::write_body(writer,frame,group.key,slot.type,slot.index));CHECK(writer.bit_count()==expected);
    }
    // Preserve the frozen packet except the explicitly changed 32-bit request count.
    const auto expectedSource=[](auto expected,const g::Spawn* source) {
        if(source && source->count>source->categories) {
            constexpr unsigned offset=121; // Two native references, then source category header.
            for(unsigned i=0;i<32;++i) {
                const auto bit=offset+i;const auto mask=1U<<(7-bit%8);
                expected[bit/8]=static_cast<std::uint8_t>((expected[bit/8]&~mask)
                    | (((0x80000002U>>(31-i))&1U)?mask:0U));
            }
        }
        return expected;
    };
    const auto compare=[&](std::uint8_t type,std::uint16_t slot,const auto& frozen,unsigned count) {
        const auto expected=expectedSource(frozen,type==1?g::spawn(g::kTraversalRegistry,slot):nullptr);
        std::array<std::byte,128> actual{};bits::Writer writer(actual);
        CHECK(g::write_body(writer,frame,g::kTraversalRegistry,type,slot));CHECK(writer.bit_count()==count);
        for(std::size_t i=0;i<expected.size();++i) { CHECK(std::to_integer<unsigned>(actual[i])==expected[i]); }
    };
    CHECK(g::spawn(g::kTraversalRegistry,58)!=nullptr);
    CHECK(g::spawn(g::kMainlandRegistry,58)!=nullptr);
    CHECK(g::spawn(g::kTraversalRegistry,58)->definition!=g::spawn(g::kMainlandRegistry,58)->definition);
    CHECK(g::spawn(0xDEADBEEFU,58)==nullptr);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,26,25)==0);
    CHECK(g::body_bits(frame,g::kMainlandRegistry,23,8)==0);
    namespace f=gateway_wire_fixture;
    compare(1,16,f::source16,f::source16Bits);compare(1,216,f::source216,f::source216Bits);compare(1,230,f::source230,f::source230Bits);
    compare(26,25,f::effect,f::effectBits);compare(34,268,f::collection,f::collectionBits);
    const auto mainlandCompare=[&](std::uint16_t slot,const auto& frozen,unsigned count) {
        const auto expected=expectedSource(frozen,g::spawn(g::kMainlandRegistry,slot));
        std::array<std::byte,128> actual{};bits::Writer writer(actual);
        CHECK(g::write_body(writer,frame,g::kMainlandRegistry,1,slot));CHECK(writer.bit_count()==count);
        for(std::size_t i=0;i<expected.size();++i) { CHECK(std::to_integer<unsigned>(actual[i])==expected[i]); }
    };
    namespace mf=gateway_mainland_wire_fixture;
    mainlandCompare(47,mf::source47,mf::source47Bits);mainlandCompare(49,mf::source49,mf::source49Bits);
    mainlandCompare(63,mf::source63,mf::source63Bits);
    frame.cohorts=32767;
    namespace ef=gateway_ending_wire_fixture;
    mainlandCompare(65,ef::source65,ef::source65Bits);mainlandCompare(69,ef::source69,ef::source69Bits);
    mainlandCompare(89,ef::source89,ef::source89Bits);mainlandCompare(98,ef::source98,ef::source98Bits);
    mainlandCompare(99,ef::source99,ef::source99Bits);mainlandCompare(101,ef::source101,ef::source101Bits);
    const auto sceneCompare=[&](bool active,const auto& expected,unsigned count) {
        frame.sceneGeneration=active?7:0;std::array<std::byte,32> actual{};bits::Writer writer(actual);
        CHECK(g::write_body(writer,frame,0xBA0B27A0,43,5));CHECK(writer.bit_count()==count);
        for(std::size_t i=0;i<expected.size();++i) { CHECK(std::to_integer<unsigned>(actual[i])==expected[i]); }
    };
    sceneCompare(false,ef::sceneDormant,ef::sceneDormantBits);sceneCompare(true,ef::sceneActive,ef::sceneActiveBits);
    // Retained native events preserve the same cast and Scene generation on every retry.
    for(unsigned stage=0;stage<3;++stage) {
        frame.vanceEntered=stage>=1;frame.vanceConversation=stage==2;
        std::array<std::byte,32> body{},retry{};bits::Writer w(body),rw(retry);
        CHECK(g::write_body(w,frame,0xBA0B27A0U,43,5));CHECK(w.bit_count()==129U+stage*32U);
        CHECK(g::body_bits(frame,0xBA0B27A0U,43,5)==w.bit_count());
        const auto field=[&](unsigned start,unsigned count) {
            std::uint32_t value{};
            for(unsigned i=0;i<count;++i) { const auto bit=start+i;value=(value<<1)|((std::to_integer<unsigned>(body[bit/8])>>(7-bit%8))&1U); }
            return value;
        };
        CHECK(field(0,32)==0x80000007U);CHECK(field(123,6)==stage);
        if(stage>=1) { CHECK(field(129,32)==0x3A5C256CU); }
        if(stage==2) { CHECK(field(161,32)==0xC2656F80U); }
        CHECK(g::write_body(rw,frame,0xBA0B27A0U,43,5));CHECK(body==retry);
    }
    frame.vanceEntered=false;frame.vanceConversation=false;
    for(const auto events:std::array{std::array<std::uint32_t,2>{0,1},std::array<std::uint32_t,2>{1,1},std::array<std::uint32_t,2>{1,UINT32_MAX}}) {
        std::array<std::byte,32> body{};bits::Writer w(body);CHECK(!c::native_scene::scene(w,0xBA0B27A0U,4,7,events));CHECK(w.bit_count()==0);
    }
    // Exact scoped selectors and non-player effects, independently reflected widths.
    std::array<std::byte,64> collection{},effect{};bits::Writer cw(collection),ew(effect);
    CHECK(g::write_body(cw,frame,g::kTraversalRegistry,34,268));CHECK(cw.bit_count()==364);
    CHECK(g::write_body(ew,frame,g::kTraversalRegistry,26,25));CHECK(ew.bit_count()==186);
    // 80809579 add operation, source type1, slots16/18/20/22.
    CHECK(collection[0]==std::byte{0x4C});
    std::array<std::byte,16> prefix{};std::array<std::byte,0x44> state{};
    const auto put=[](auto& array,std::size_t offset,auto value) { std::memcpy(array.data()+offset,&value,sizeof value); };
    put(prefix,0,std::uint32_t{0x80F470F1});put(prefix,4,std::uint32_t{0x80809928});put(prefix,8,std::uint64_t{0x4C8});
    put(state,0,std::uint32_t{7});put(state,0xC,UINT32_MAX);put(state,0x10,std::uint64_t{0xFFFF00FF811C9DC5ULL});put(state,0x2C,std::uint32_t{0x3F800000});
    std::uint32_t generation{};std::uint8_t index{};
    CHECK(g::preparation(prefix,state,generation,index));CHECK(generation==7 && index==0);
    state[8]=std::byte{1};CHECK(!g::preparation(prefix,state,generation,index));state[8]={};
    for(std::uint8_t i=0;i<g::kPreparedDefinitions.size();++i) {
        put(prefix,0,g::kPreparedDefinitions[i]);CHECK(g::preparation(prefix,state,generation,index));CHECK(index==i && generation==7);
    }
    put(prefix,0,std::uint32_t{0x80F47588});CHECK(!g::preparation(prefix,state,generation,index));
}
struct ModuleMemory {
    std::array<std::byte,0x10000> bytes{};
    std::uintptr_t healthAddress{0x12500};
    bool copy(std::uintptr_t address,std::span<std::byte> out) noexcept {
        if(address<0x10000 || address>0x20000 || out.size()>0x20000-address) { return false; }
        std::copy_n(bytes.data()+address-0x10000,out.size(),out.data());return true;
    }
    template<class T> bool value(std::uintptr_t address,T& out) noexcept { return copy(address,std::as_writable_bytes(std::span{&out,std::size_t{1}})); }
    template<class T> void put(std::uintptr_t address,T v) { std::memcpy(bytes.data()+address-0x10000,&v,sizeof v); }
    bool resolve(std::uint32_t handle,std::uintptr_t& base,std::uintptr_t* allocation=nullptr) noexcept {
        if(handle==11) { base=0x11000;if(allocation) { *allocation=0x10800; }return true; }
        if(handle==12) { base=0x12000;if(allocation) { *allocation=0x10840; }return true; }
        if(handle==13) { base=0x18000;return true; }
        if(handle==0x12340123) { base=healthAddress;return true; }return false;
    }
};
void vance_native_path_tests() {
    namespace path=dawn::client::hooks::bootflow::gateway_vance_native_path;
    auto memory=std::make_unique<ModuleMemory>();auto& m=*memory;
    constexpr std::uintptr_t root=0x11000,child=0x18000;
    constexpr std::uint32_t parent=0x09F9F578U,childHandle=0x12345678U;
    const path::Weak weak{0xAABBCCDDU,childHandle};
    // Identity values independently checked against selector-full.bin and child-latest.bin.
    m.put(root,path::Ref{0x80EC0ABCU,0x80806384U,0x2548});m.put(root+0x24,parent);
    m.put(root+0x2C,std::uint32_t{0x23450123});m.put(root+0x38,std::uint64_t{12});
    for(const auto offset:{0x8B0,0x1240,0x14A0}) {
        const auto kind=offset==0x8B0?0x808062FEU:0x80806307U;
        const auto definition=offset==0x8B0?0x2998U:offset==0x1240?0x2D28U:0x2DE8U;
        m.put(root+offset,path::Ref{0x80EC0ABCU,kind,definition});m.put(root+offset+0x28,parent);
        m.put(root+offset+0x2C,offset==0x8B0?0x808062FDU:0x80806306U);
        m.put(root+offset+0x30,static_cast<std::uint64_t>(offset));
    }
    m.put(root+0xA60,weak);m.put(child,path::Ref{0x80EC0AC5U,0x808084E9U,0x27E8});
    m.put(child+0x24,childHandle);m.put(child+0x2C,std::uint32_t{0x34561234});
    bool resolveOk=true,recycle=false;unsigned resolves{};
    const auto resolve=[&](const path::Weak& value,std::uintptr_t& out) {
        ++resolves;if(!resolveOk || value!=weak) { return false; }
        out=child;if(recycle) { m.put(root+0xA60,path::Weak{weak.serial+1U,weak.handle}); }return true;
    };
    const auto probe=[&] { return path::probe(m,root,parent,resolve); };
    auto stage=probe();CHECK(stage.valid && !stage.turned && !stage.conversation);CHECK(resolves==0);
    m.put(root+0x12D8,std::uint8_t{1});stage=probe();CHECK(stage.valid && !stage.turned);
    m.put(root+0x12D8,std::uint8_t{2});m.put(root+0x1538,std::uint8_t{1});
    stage=probe();CHECK(stage.valid && stage.turned && !stage.conversation);
    m.put(root+0x1538,std::uint8_t{2});m.put(root+0x948,std::uint8_t{1});
    stage=probe();CHECK(stage.valid && !stage.turned && stage.conversation);CHECK(resolves==1);
    resolveOk=false;CHECK(!probe().valid);resolveOk=true;
    recycle=true;CHECK(!probe().valid);recycle=false;m.put(root+0xA60,weak);
    for(const auto address:{root,root+0x24,root+0x38,root+0x1244,root+0x1268,
        root+0x126C,root+0x1270,child,child+0x24,child+8}) {
        std::uint32_t old{};CHECK(m.value(address,old));m.put(address,old^0x10U);
        CHECK(!probe().valid);m.put(address,old);
    }
    m.put(root+0x12D8,std::uint8_t{3});CHECK(!probe().valid);m.put(root+0x12D8,std::uint8_t{2});
    m.put(root+0xA50,std::int32_t{1});CHECK(!probe().valid);m.put(root+0xA50,std::int32_t{0});
    m.put(root+0xA60,path::Weak{weak.serial+1U,weak.handle});CHECK(!probe().valid);m.put(root+0xA60,weak);
    CHECK(probe().conversation);CHECK(!path::probe(m,UINTPTR_MAX,parent,resolve).valid);
}
void ending_cadence_tests() {
    g::EndingCadence cadence;g::Frame frame{};
    CHECK(!cadence.due(1,100));frame.enabled=true;cadence.snapshot(1,100,frame);
    CHECK(!cadence.due(1,10000)); // Opening keeps its accepted cadence except the authored cue.
    frame.returnCuePending=true;cadence.snapshot(1,100,frame);
    CHECK(!cadence.due(1,199));CHECK(cadence.due(1,200));
    frame.returnCuePending=false;cadence.snapshot(1,200,frame);CHECK(!cadence.due(1,10000));
    frame.moduleVulnerable=true;cadence.snapshot(1,10000,frame);
    CHECK(!cadence.due(2,10100));CHECK(!cadence.due(0,10100));CHECK(!cadence.due(1,10099));
    CHECK(cadence.due(1,10100));CHECK(!cadence.due(1,10100));CHECK(!cadence.due(1,10199));
    CHECK(cadence.due(1,10200)); // Failed staging retries without a new entry/death event.
    frame.moduleDestroyed=true;cadence.snapshot(1,10200,frame);CHECK(cadence.due(1,10300));
    frame.vanceEntered=true;cadence.snapshot(1,10300,frame);CHECK(cadence.due(1,10400));
    frame.finished=true;cadence.snapshot(1,10400,frame);CHECK(!cadence.due(1,100000));
    frame.finished=false;frame.enabled=false;cadence.snapshot(1,10500,frame);CHECK(!cadence.due(1,10600));
    cadence.reset();CHECK(!cadence.due(1,100000));frame.enabled=true;cadence.snapshot(2,200,frame);
    CHECK(!cadence.due(1,300));CHECK(cadence.due(2,300));cadence.reset();CHECK(!cadence.due(2,400));
}
void lighthouse_wire_tests() {
    // Decode the native position body independently: time target, value, revision, snap.
    for(const std::uint32_t generation:{1U,3U,32765U}) for(unsigned channels=0;channels<4;++channels) {
        g::Frame frame{};frame.enabled=true;frame.spawnGeneration=generation;frame.lighthouseChannels=static_cast<std::uint8_t>(channels);
        for(std::uint16_t slot=0;slot<2;++slot) {
            std::array<std::byte,32> bytes{};bits::Writer writer(bytes);
            CHECK(g::write_body(writer,frame,0xBA0B27A0U,23,slot));CHECK(writer.bit_count()==147);
            const auto field=[&](unsigned start,unsigned count) {
                std::uint32_t value{};for(unsigned i=0;i<count;++i) { const auto bit=start+i;value=(value<<1)|((std::to_integer<unsigned>(bytes[bit/8])>>(7-bit%8))&1U); }return value;
            };
            const bool raised=(channels&(1U<<slot))!=0;
            CHECK(field(0,32)==(raised?0U:0x3F800000U));
            CHECK(field(32,16)==0x8000U+generation+(raised?1U:0U));
            CHECK(field(48,1)==(raised?0U:1U));
            CHECK(field(49,32)==0x3F800000U && field(81,16)==0x7FFFU); // Preserve native power.
            CHECK(field(98,32)==0 && field(130,16)==0x7FFFU); // Preserve native lock.
        }
        CHECK(g::body_bits(frame,0xBA0B27A0U,23,2)==0);
    }
}
void module_native_path_tests() {
    namespace path=dawn::client::hooks::bootflow::gateway_module_native_path;
    auto memory=std::make_unique<ModuleMemory>();auto& m=*memory;path::Probe probe{};
    m.put(0x11000,std::uint32_t{2});m.put(0x10818,std::uint32_t{12});m.put(0x10858,UINT32_MAX);
    m.put(0x12004,std::uint32_t{13});m.put(0x18068,std::uint64_t{2});m.put(0x18070,std::int64_t{0x100});
    m.put(0x18194,std::int32_t{0x80});m.put(0x181AC,std::int32_t{0x500});
    m.put(0x12500,std::uint32_t{0x80F48026});m.put(0x12504,std::uint32_t{0x80804B8A});m.put(0x12508,std::uint64_t{0xB08});
    m.put(0x12524,std::uint32_t{0x12340123});m.put(0x1252C,std::uint32_t{0x23450234});
    CHECK(path::find(m,11,0x23450234,probe));CHECK(probe.resources==2 && probe.component==0x12500 && !probe.dead);
    namespace damage=dawn::client::hooks::bootflow::gateway_module_damage;
    m.put(0x19008,std::uintptr_t{0x12500});damage::Sample sample{};
    CHECK(damage::sample(m,0x19000,sample));CHECK(!sample.dead);
    g::EndingRequest request{true,false,90,2,0,{90,2,0x15000,0x23450234U,9,0x12340123U}};
    m.put(0x15000,std::uint32_t{0x80F46F23});m.put(0x15004,std::uint32_t{0x80809928});m.put(0x15008,std::uint64_t{0x4C8});
    m.put(0x15180,std::uint32_t{2});m.put(0x152F0,std::uint32_t{2});m.put(0x15188,std::uint8_t{1});
    m.put(0x15440,std::uint32_t{9});m.put(0x15444,std::uint32_t{0x23450234});
    CHECK(damage::current(m,request,sample));
    for(bool native:{false,true}) {
        CHECK(damage::blocked(request));CHECK(!damage::allowed(request,true,native));CHECK(!damage::allowed(request,false,native));
        request.moduleVulnerable=true;CHECK(!damage::blocked(request));CHECK(damage::allowed(request,true,native));CHECK(damage::allowed(request,false,native)==native);
        request.moduleDestroyed=true;CHECK(damage::allowed(request,true,native)==native);request.moduleDestroyed=false;
        request.enabled=false;CHECK(!damage::blocked(request));CHECK(damage::allowed(request,true,native)==native);request.enabled=true;request.moduleVulnerable=false;
    }
    for(const auto address:{0x15000U,0x15004U,0x15008U,0x15180U,0x152F0U,0x15188U,0x15440U,0x15444U}) {
        std::uint8_t saved{};CHECK(m.value(address,saved));m.put(address,static_cast<std::uint8_t>(saved^1U));
        CHECK(!damage::current(m,request,sample));m.put(address,saved);
    }
    auto stale=request;stale.run++;CHECK(!damage::current(m,stale,sample));stale=request;stale.generation++;
    CHECK(!damage::current(m,stale,sample));stale=request;stale.owner.health++;CHECK(!damage::current(m,stale,sample));
    m.put(0x12500,std::uint32_t{0x815B5A40});CHECK(!damage::sample(m,0x19000,sample));m.put(0x12500,std::uint32_t{0x80F48026});
    m.healthAddress=0x12510;CHECK(!damage::sample(m,0x19000,sample));m.healthAddress=0x12500;
    m.put(0x12838,std::uint8_t{1});CHECK(damage::sample(m,0x19000,sample) && sample.dead);
    m.put(0x12838,std::uint8_t{1});probe={};CHECK(path::find(m,11,0x23450234,probe) && probe.dead);
    probe={};CHECK(!path::find(m,11,0x23452234,probe));
    m.healthAddress=0x12510;probe={};CHECK(!path::find(m,11,0x23450234,probe));m.healthAddress=0x12500;
    m.put(0x18068,std::uint64_t{257});probe={};CHECK(!path::find(m,11,0x23450234,probe));m.put(0x18068,std::uint64_t{2});
    m.put(0x181AC,std::int32_t{-1});probe={};CHECK(!path::find(m,11,0x23450234,probe));m.put(0x181AC,std::int32_t{0x500});
    m.put(0x18070,std::int64_t{-0x100});m.put(0x17F94,std::int32_t{0x80});m.put(0x17FAC,std::int32_t{0x500});
    probe={};CHECK(path::find(m,11,0x23450234,probe));
    m.put(0x12000,std::uint32_t{2});m.put(0x10858,std::uint32_t{11});probe={};CHECK(!path::find(m,11,0x23450234,probe));CHECK(probe.resources==2);
    probe={};CHECK(!path::find(m,UINT32_MAX,0x23450234,probe));
}
void ending_identity_and_codec_tests() {
    namespace identity=dawn::client::hooks::bootflow::gateway_module_identity;
    std::array<std::byte,0x340> health{};
    const auto put=[&](std::size_t offset,auto value) { std::memcpy(health.data()+offset,&value,sizeof value); };
    put(0,std::uint32_t{0x80F48026});put(4,std::uint32_t{0x80804B8A});put(8,std::uint64_t{0xB08});
    put(0x24,std::uint32_t{0x12340123});put(0x2C,std::uint32_t{0x23450234});
    CHECK(identity::health(health,0x12340123U,0x23450234U));CHECK(!identity::dead(health,0x12340123U,0x23450234U));
    put(0x338,std::uint8_t{1});CHECK(identity::dead(health,0x12340123U,0x23450234U));
    CHECK(!identity::dead(health,0x12342123U,0x23450234U));CHECK(!identity::dead(health,0x12340123U,0x23452234U));
    for(const auto off:{0U,4U,8U,0x24U,0x2CU}) { health[off]^=std::byte{1};CHECK(!identity::health(health,0x12340123U,0x23450234U));health[off]^=std::byte{1}; }
    CHECK(!identity::health(std::span(health).first(0x338),0x12340123U,0x23450234U));
    for(bool active:{false,true}) {
        std::array<std::byte,128> actual{},accepted{};bits::Writer a(actual),b(accepted);
        CHECK(c::native_scene::write_source(a,7,active));
        CHECK(dawn::state::activity::omega_rescue_npc::write_source(b,7,active));
        CHECK(a.bit_count()==641 && a.bit_count()==b.bit_count() && actual==accepted);
    }
    g::Frame frame{};frame.enabled=true;frame.spawnGeneration=7;
    for(const auto slot:std::array<std::uint16_t,5>{26,27,28,32,33}) {
        std::array<std::byte,64> before{},after{},expected{};bits::Writer a(before),b(after),e(expected);
        CHECK(g::write_body(a,frame,g::kMainlandRegistry,4,slot));CHECK(a.bit_count()==252);
        CHECK(c::native_device::object(e,7,false));CHECK(before==expected);
        frame.preparedMask=127;frame.moduleVulnerable=true;frame.section=1;
        CHECK(g::write_body(b,frame,g::kMainlandRegistry,4,slot));CHECK(before!=after);
        frame.preparedMask=0;frame.moduleVulnerable=false;frame.section=0;
    }
    // Decode the real type4 body, including the native generation retry condition.
    const auto objectState=[&](std::uint16_t slot) {
        std::array<std::byte,64> body{};bits::Writer writer(body);CHECK(g::write_body(writer,frame,g::kMainlandRegistry,4,slot));
        const auto word=[&](std::size_t offset) { std::uint32_t v{};for(unsigned i=0;i<4;++i) { v=(v<<8)|std::to_integer<unsigned>(body[offset+i]); }return v; };
        return std::pair{word(0)^0x80000000U,(std::to_integer<unsigned>(body[8])&0x80U)!=0};
    };
    CHECK(objectState(32)==std::make_pair(7U,false));frame.preparedMask=127;
    const auto live=objectState(32);CHECK(live==std::make_pair(8U,true));CHECK(live.first>7U);
    CHECK(objectState(27)==std::make_pair(8U,true)); // Fresh generation recreates the entrance barrier.
    CHECK(objectState(33)==std::make_pair(8U,true)); // Beam exists from arrival, before return combat.
    frame.moduleVulnerable=true;CHECK(objectState(32)==live);CHECK(objectState(27).second);
    frame.moduleDestroyed=true;CHECK(objectState(32)==std::make_pair(8U,false));CHECK(objectState(27).second);
    frame.lighthouseOpen=true;CHECK(!objectState(27).second); // Only after confirmed module destruction.
    // The target's display activation must not wait for the boss damage gate.
    const auto targetPosition=[&](std::uint16_t slot) {
        std::array<std::byte,32> body{};bits::Writer writer(body);
        CHECK(g::write_body(writer,frame,g::kMainlandRegistry,23,slot));CHECK(writer.bit_count()==147);
        std::uint32_t value{};for(unsigned i=0;i<4;++i) { value=(value<<8)|std::to_integer<unsigned>(body[i]); }
        return std::bit_cast<float>(value);
    };
    frame.moduleVulnerable=false;frame.moduleDestroyed=false;frame.lighthouseOpen=false;
    CHECK(targetPosition(4)==1.F);CHECK(targetPosition(0)==1.F);
    frame.moduleVulnerable=true;CHECK(targetPosition(4)==.75F);CHECK(targetPosition(0)==1.F);
    frame.moduleDestroyed=true;CHECK(targetPosition(4)==0.F);CHECK(targetPosition(0)==1.F);
    frame.lighthouseOpen=true;CHECK(targetPosition(0)==0.F);
    frame.preparedMask=0;frame.moduleVulnerable=false;frame.moduleDestroyed=false;frame.lighthouseOpen=false;
    for(bool finished:{false,true}) {
        wire::Snapshot snapshot{};snapshot.lifetime=3;snapshot.gateway=frame;snapshot.gateway.finished=finished;snapshot.missionCompletion={{71,1},finished};
        std::array<std::byte,128> packet{};bits::Writer writer(packet);
        CHECK(wire::write_auth_body(writer,snapshot,g::kGroups[0].key,17,3,false));
        const auto head=std::to_integer<unsigned>(packet[0]);auto phase=head>>4,result=(head>>1)&7U;
        CHECK(phase==(finished?7U:4U));CHECK(result==(finished?2U:1U));
        snapshot.archiveOmega=true;std::array<std::byte,128> archive{};bits::Writer aw(archive);
        CHECK(wire::write_auth_body(aw,snapshot,0x82FB58B7U,17,3,false));
        CHECK((std::to_integer<unsigned>(archive[0])>>4)==(finished?7U:4U));
        CHECK(((std::to_integer<unsigned>(archive[0])>>1)&7U)==(finished?2U:1U));snapshot.archiveOmega=false;
        snapshot.gateway.enabled=false;snapshot.missionCompletion={};std::array<std::byte,128> foreign{};bits::Writer fw(foreign);
        CHECK(wire::write_auth_body(fw,snapshot,g::kGroups[0].key,17,3,false));phase=std::to_integer<unsigned>(foreign[0])>>4;result=(std::to_integer<unsigned>(foreign[0])>>1)&7U;
        CHECK(phase==4 && result==1);
    }
}
void changed_lua_flow_tests() {
    // The authored route uses an authentic enemy death AND Vance's native turn.
    // Its early unlock/lift omit the original module, travel, dialogue and timer gates.
    const std::string script=R"lua(
local ready=condition("custom.ready",all_of("population.contact.9","vance.turned"))
local root=graph("composition","custom Gateway",sequence(
    step("run",parallel("opening.module","opening.checked"))))
local finish=graph("custom_finish","finish after native conversation starts",sequence(
    step("complete","mission.finish")))
local encounter=graph("custom_encounter","new native gate",sequence(
    step("begin",parallel("return.center","vance.greeting","objective.enter")),
    step("ready","custom.ready"),
    step("release",parallel("module.expose","lighthouse.unlock","lighthouse.raise","lighthouse.light","vance.scene"))))
return mission{id="gateway",graphs={root,finish,encounter},roles={mission="composition"},
    phases={"custom_encounter","custom_finish"},conditions={ready},
    entry="composition",modules={"opening"},observations={"opening.checked"}}
)lua";
    std::string error;auto document=c::script::MissionDocument::parse_lua(script,g::kProfile,error);
    if(!document) { std::fprintf(stderr,"custom Gateway: %s\n",error.c_str()); }CHECK(document);CHECK(g::valid_document(document->views()));
    g::Controller controller;constexpr std::uint64_t run=191;CHECK(controller.select(document->views(),run));
    std::uint64_t now=1000;g::Frame frame{};
    const auto tick=[&] { for(unsigned i=0;i<4;++i) { frame=controller.update(run,++now,true); } };
    tick();CHECK(controller.graph()->id=="custom_encounter");CHECK(frame.cohorts==(1U<<9));
    CHECK(frame.objective==0xB10D6455U);CHECK(frame.activeRow==c::kNoDialogue);
    CHECK(!frame.lighthouseOpen && !frame.moduleVulnerable && !frame.lighthouseChannels);
    g::SceneReceipt scene{run,frame.sceneGeneration,0xDAAA0101U,0xDBBB0102U,0xDCCC0103U};
    CHECK(controller.scene(scene,false));tick();CHECK(frame.vanceEntered);CHECK(!frame.vanceConversation);
    const g::Spawn* source{};for(const auto& candidate:g::kSpawns) if(candidate.cohort==9) { source=&candidate;break; }CHECK(source);
    g::EnemyReceipt enemy{run,0x12345000U,0x12346000U,frame.spawnGeneration,source->source,source->registry};
    CHECK(!controller.died(enemy));CHECK(controller.admitted(enemy));auto stale=enemy;++stale.generation;CHECK(!controller.died(stale));
    CHECK(controller.died(enemy));CHECK(!controller.died(enemy));tick();
    CHECK(!frame.moduleVulnerable && !frame.lighthouseOpen && !frame.lighthouseChannels); // all_of still waits for turn.
    auto foreign=scene;++foreign.sensor;CHECK(!controller.vance(foreign,g::VanceMilestone::turned,now));
    CHECK(controller.vance(scene,g::VanceMilestone::turned,now));tick();
    CHECK(frame.moduleVulnerable && frame.lighthouseOpen && frame.lighthouseChannels==3);
    CHECK(!frame.moduleDestroyed);CHECK(frame.vanceConversation);CHECK(!frame.finished);
    CHECK(!frame.generations[9] && !frame.generations[10]);CHECK(!controller.seen().any());
    CHECK(!controller.vance(foreign,g::VanceMilestone::conversationStarted,now));tick();CHECK(!frame.finished);
    CHECK(controller.vance(scene,g::VanceMilestone::conversationStarted,now));tick();
    CHECK(controller.graph()->id=="custom_finish");CHECK(frame.finished && frame.checked && frame.completion.valid());
    CHECK(frame.completion.owner.run==run);CHECK(!frame.sceneComplete);CHECK(!frame.moduleDestroyed);
    CHECK(controller.diagnostics().phase==c::Phase::complete);
}

struct PatrolCommandRead {
    bool fail{};
    template<class T> bool value(std::uintptr_t address,T& out) {
        if(fail || !address) return false;
        std::memcpy(&out,reinterpret_cast<const void*>(address),sizeof(out));return true;
    }
};
struct PatrolIdentityRead {
    std::map<std::uintptr_t,std::byte> bytes;
    std::map<std::uint32_t,std::uintptr_t> handles;
    template<class T> void put(std::uintptr_t address,T value) {
        const auto* p=reinterpret_cast<const std::byte*>(&value);
        for(std::size_t i=0;i<sizeof(T);++i) bytes[address+i]=p[i];
    }
    template<class T> bool value(std::uintptr_t address,T& out) {
        auto* p=reinterpret_cast<std::byte*>(&out);
        for(std::size_t i=0;i<sizeof(T);++i) {
            const auto it=bytes.find(address+i);if(it==bytes.end()) return false;p[i]=it->second;
        }
        return true;
    }
    bool resolve(std::uint32_t handle,std::uintptr_t& out) {
        const auto it=handles.find(handle);if(it==handles.end()) return false;out=it->second;return true;
    }
};
void patrol_identity_tests(const c::script::Views& views) {
    namespace p=dawn::client::hooks::bootflow::gateway_patrol;
    PatrolIdentityRead m;constexpr std::uintptr_t image=0x10000000,actor=0x20000000,
        source=0x30000000,definition=0x40000000,parent=0x50000000,entity=0x60000000;
    g::EnemyReceipt owner{91,0x12340000,0x34560000,7,16,g::kTraversalRegistry};
    m.put(image+0x1F9D7F8,actor);m.put(image+0x1F9D800,std::uint32_t{0xB000});
    m.put(actor+0x48,owner.actor);m.put(actor+0x38,owner.owner);m.put(actor+0x40,std::int64_t{0});
    m.handles[owner.owner]=source;m.handles[0x45670000]=definition;m.handles[0x56780000]=parent;
    m.put(source+4,std::uint32_t{0x8080948F});m.put(source+0x1FC,owner.generation);m.put(source+0x244,owner.generation);
    m.put(source,std::uint32_t{0x45670000});m.put(source+8,std::int64_t{0});
    m.put(definition+0x30,owner.registry);m.put(definition+0x34,std::uint8_t{1});m.put(definition+0x36,owner.source);
    m.put(actor+0x4C,std::uint32_t{0x67880000});m.put(actor+0x50,std::uint32_t{0x56780000});
    m.put(parent+4,std::uint32_t{0x808082EC});m.put(parent+0x24,std::uint32_t{0x56780000});
    m.put(parent+0x2C,std::uint32_t{0x67880000});m.put(parent+0x1470,owner.actor);
    m.put(image+0x1F93428,entity);m.put(image+0x1F93430,std::uint32_t{0x200});
    m.put(entity+0xC,std::uint32_t{0x67880000});m.put(entity+4,std::uint32_t{0});
    const auto owned=[&] {return p::owns(m,image,actor,owner);};CHECK(owned());
    // Every source identity must survive re-resolution. Recycling any one
    // native row excludes it from both continuous patrol and no-aggro behavior.
    for(const auto address:{actor+0x48,actor+0x38,source+4,source+0x1FC,source+0x244,
        definition+0x30,definition+0x34,definition+0x36,parent+4,parent+0x24,parent+0x2C,parent+0x1470,entity+0xC}) {
        m.bytes[address]^=std::byte{1};CHECK(!owned());m.bytes[address]^=std::byte{1};CHECK(owned());
    }
    m.put(entity+4,std::uint32_t{4});CHECK(!owned());m.put(entity+4,std::uint32_t{0});
    for(const auto& spawn:g::kSpawns) {
        owner.registry=spawn.registry;owner.source=spawn.source;
        m.put(definition+0x30,owner.registry);m.put(definition+0x36,owner.source);
        CHECK(owned()==(spawn.cohort==0));
    }
    g::Controller controller;CHECK(controller.select(views,91));
    auto frame=controller.update(91,100,true);CHECK(frame.marchers);
    std::uint32_t actorHandle=0x12340000;
    for(const auto& spawn:g::kSpawns) if(spawn.cohort==0) {
        g::EnemyReceipt live{91,++actorHandle,0x34560000,frame.spawnGeneration,spawn.source,spawn.registry};
        CHECK(!controller.marcher(live.actor).valid());CHECK(controller.admitted(live));CHECK(controller.marcher(live.actor)==live);
        CHECK(controller.died(live));CHECK(!controller.marcher(live.actor).valid());
    }
    CHECK(controller.submitted(91,g::kBank,1,frame.generations[1],101));
    controller.position(91,interior(361));for(unsigned i=0;i<4;++i) frame=controller.update(91,102+i,true);
    for(const auto& spawn:g::kSpawns) if(spawn.cohort==1) {
        g::EnemyReceipt live{91,++actorHandle,0x34560000,frame.spawnGeneration,spawn.source,spawn.registry};
        CHECK(controller.admitted(live));CHECK(!controller.marcher(live.actor).valid());
    }
    CHECK(controller.select(views,92));CHECK(!controller.marcher(actorHandle).valid());
}
void patrol_command_tests() {
    namespace p=dawn::client::hooks::bootflow::gateway_patrol;
    for(const bool nativeChanged:{false,true}) for(const bool redirected:{false,true}) {
        alignas(16) std::array<std::byte,0xB0> retained{},output{};
        retained.fill(std::byte{0x23});output.fill(std::byte{0xCC});
        if(nativeChanged) output=retained;
        const auto original=output;
        const std::array<std::uintptr_t,4> context{0,0x12340001,0,reinterpret_cast<std::uintptr_t>(retained.data())};
        PatrolCommandRead read;
        unsigned calls{};
        const auto change=[&](std::uintptr_t actor,std::uintptr_t candidate) {
            ++calls;CHECK(actor==context[1]);
            auto& bytes=*reinterpret_cast<std::array<std::byte,0xB0>*>(candidate);CHECK(bytes==retained);
            if(redirected) bytes[0x10]=std::byte{0x54};
            return redirected;
        };
        CHECK(p::publish_point(read,reinterpret_cast<std::uintptr_t>(context.data()),output.data(),nativeChanged,change)==(nativeChanged || redirected));
        CHECK(calls==1);
        auto expected=original;if(redirected) {expected=retained;expected[0x10]=std::byte{0x54};}CHECK(output==expected);
        read.fail=true;calls=0;
        CHECK(p::publish_point(read,reinterpret_cast<std::uintptr_t>(context.data()),output.data(),nativeChanged,change)==nativeChanged);
        CHECK(calls==0 && output==expected);
    }
    alignas(16) std::array<std::byte,0xB0> output{};output.fill(std::byte{0xCC});const auto original=output;
    const std::array<std::uintptr_t,4> context{0,0x12340001,0,0};PatrolCommandRead read;
    CHECK(!p::publish_point(read,reinterpret_cast<std::uintptr_t>(context.data()),output.data(),false,
        [](auto,auto) { CHECK(false);return true; }));CHECK(output==original);
}
void patrol_loop_tests() {
    g::patrol::Loop loop;
    const auto same=[](g::Point a,g::Point b) { return a.x==b.x && a.y==b.y && a.z==b.z; };
    for(std::size_t i=0;i<44;++i) {
        const auto slot=static_cast<std::uint16_t>(16+(i/4)*18+(i%4)*2);
        CHECK(g::patrol::index(g::kTraversalRegistry,slot)==i);
        g::EnemyReceipt receipt{7,static_cast<std::uint32_t>(100+i),static_cast<std::uint32_t>(200+i),3,slot,g::kTraversalRegistry};
        const auto& lane=g::patrol::kLanes[i/4];const auto nativeGoal=g::patrol::center(lane);
        auto target=loop.update(receipt,lane.start,nativeGoal);CHECK(target.valid && !target.turned && target.turns==0);
        const auto away=target.position;
        auto next=loop.update(receipt,away,nativeGoal);CHECK(next.valid && next.turned && next.turns==1);
        const auto home=next.position;CHECK(!same(home,away));
        // Repeated observations at one end cannot flap the direction. There is
        // no timeout, finite command list, lap limit, respawn or generation bump.
        for(unsigned turn=2;turn<=2000;++turn) {
            target=loop.update(receipt,next.position,nativeGoal);
            CHECK(target.valid && target.turned && target.turns==turn);
            CHECK(same(target.position,turn%2?home:away));
            next=loop.update(receipt,next.position,nativeGoal);
            CHECK(next.valid && !next.turned && next.turns==turn && same(next.position,target.position));
        }
        auto invalid=receipt;invalid.registry=g::kMainlandRegistry;
        CHECK(!loop.update(invalid,home,nativeGoal).valid);
        invalid=receipt;invalid.source=206;CHECK(!loop.update(invalid,home,nativeGoal).valid);
        CHECK(!loop.update(receipt,{home.x,home.y,home.z+20.F},nativeGoal).valid);
        CHECK(!loop.update(receipt,{std::numeric_limits<float>::quiet_NaN(),home.y,home.z},nativeGoal).valid);
        ++receipt.run;target=loop.update(receipt,lane.start,nativeGoal);
        CHECK(target.valid && !target.turned && target.turns==0);
        ++receipt.actor;target=loop.update(receipt,nativeGoal,nativeGoal);
        CHECK(target.valid && target.turns==0 && same(target.position,home));
    }
    for(const auto& source:g::kSpawns) CHECK((g::patrol::index(source.registry,source.source)<44)==(source.cohort==0));
}
void forest_threshold_tests(const c::script::Views& views) {
    const c::CommandSpec threshold{c::Operation::observation,g::kModule,1024,c::Wait::observed};
    const c::CommandSpec hydra{c::Operation::observation,g::kModule,1025,c::Wait::observed};
    for(const auto point:std::array<g::Point,3>{{{266.F,245.5F,88.8F},{266.F,-9000.F,7000.F},{1000.F,9000.F,-7000.F}}}) {
        g::Controller controller;CHECK(controller.select(views,987));
        CHECK(controller.update(987,1,true).enabled);
        controller.position(988,point);CHECK(controller.missing(threshold).missing!=c::Missing::none);
        controller.position(987,{std::numeric_limits<float>::quiet_NaN(),0,0});
        controller.position(987,{265.999F,point.y,point.z});
        CHECK(controller.missing(threshold).missing!=c::Missing::none);
        controller.position(987,point);CHECK(controller.missing(threshold).missing==c::Missing::none);
        auto frame=controller.update(987,2,true);
        CHECK(frame.activeRow==1);
        CHECK(controller.submitted(987,g::kBank,1,frame.generations[1],3));
        for(unsigned n=25000;n<25008;++n) frame=controller.update(987,n,true);
        CHECK(frame.activeRow==4);CHECK(frame.generations[4]!=0);CHECK(frame.generations[5]==0);
        CHECK(controller.missing(hydra).missing!=c::Missing::none);
        controller.position(987,{-1000.F,0,0});
        CHECK(controller.missing(threshold).missing==c::Missing::none);
        CHECK(controller.select(views,989));
        CHECK(controller.missing(threshold).missing!=c::Missing::none);
        CHECK(controller.missing(hydra).missing!=c::Missing::none);
    }
}
int main() {
    patrol_loop_tests();patrol_command_tests();
    changed_lua_flow_tests();
    std::string error;auto document=c::script::MissionDocument::read("Dawn/scripts/gateway.lua",g::kProfile,error);
    if(!document) { std::fprintf(stderr,"%s\n",error.c_str()); }CHECK(document);CHECK(g::valid_document(document->views()));
    forest_threshold_tests(document->views());
    CHECK(document->views().dialogue.rows[0].sceneOwned);CHECK(document->views().dialogue.rows[11].sceneOwned);
    std::ifstream cueFile("Dawn/scripts/gateway.lua");
    std::string cueText((std::istreambuf_iterator<char>(cueFile)),std::istreambuf_iterator<char>());
    const std::string originalCue="argument=8960";const auto cueAt=cueText.find(originalCue);CHECK(cueAt!=std::string::npos);
    cueText.replace(cueAt,originalCue.size(),"argument=6000");
    auto customCue=c::script::MissionDocument::parse_lua(cueText,g::kProfile,error);CHECK(customCue && g::valid_document(customCue->views()));
    traversal_tests(customCue->views(),false,false,6000); // Timing belongs to the document.
    patrol_identity_tests(document->views());
    auto altered=document->views();altered.missionId="omega";CHECK(!g::valid_document(altered));
    g::Controller controller;CHECK(controller.select(document->views(),71));
    CHECK(!controller.update(71,100,false).enabled);
    auto frame=controller.update(71,100,true);CHECK(frame.objective==0xC8DC7CFDU);CHECK(frame.activeRow==1);
    controller.position(71,{350,250,100});frame=controller.update(71,101,true);CHECK(frame.objective==0xC8DC7CFDU);
    controller.position(70,{-772.25F,-130.75F,-7.5F});frame=controller.update(71,102,true);CHECK(frame.objective==0xC8DC7CFDU);
    controller.position(71,{-772.25F,-130.75F,-7.5F});frame=controller.update(71,103,true);
    CHECK(frame.objective==0xC8DC7CFDU);CHECK(frame.activeRow==1);const auto generation=frame.generations[1];
    CHECK(!controller.submitted(70,g::kBank,1,generation,104));CHECK(!controller.submitted(71,0x80F1FC9FU,1,generation,104));
    CHECK(!controller.submitted(71,g::kBank,1,generation+1,104));CHECK(!controller.submitted(71,g::kBank,0,generation,104));
    // Time cannot acknowledge dispatch or finish the foundation.
    frame=controller.update(71,20000,true);CHECK(!frame.checked);CHECK(controller.timeouts()==1);
    CHECK(!controller.submitted(71,g::kBank,1,generation,20001));
    CHECK(controller.select(document->views(),72));CHECK(!controller.update(71,20002,true).enabled);
    controller.position(72,{-772.25F,-130.75F,-7.5F});frame=controller.update(72,20003,true);CHECK(frame.activeRow==1);
    CHECK(!controller.submitted(72,g::kBank,1,generation,20004));
    CHECK(controller.submitted(72,g::kBank,1,frame.generations[1],20004));
    CHECK(!controller.submitted(72,g::kBank,1,frame.generations[1],20005));
    frame=controller.update(72,20005,true);CHECK(!frame.checked);CHECK(frame.activeRow==c::kNoDialogue);
    // A real point in the authored recess polygon, not just its AABB.
    const g::Volume* recess{};
    for(const auto& v:g::kVolumes) { if(v.registry==g::kRecess.registry && v.slot==g::kRecess.slot) { recess=&v; } }
    CHECK(recess);g::Point inside{};
    for(const auto& point:recess->vertices) { inside.x+=point.x;inside.y+=point.y; }
    inside.x/=static_cast<float>(recess->vertices.size());inside.y/=static_cast<float>(recess->vertices.size());inside.z=(recess->min.z+recess->max.z)/2;
    CHECK(g::contains(*recess,inside));CHECK(!g::contains(*recess,{inside.x,inside.y,recess->max.z+1}));
    CHECK(!g::contains(*recess,{std::numeric_limits<float>::quiet_NaN(),inside.y,inside.z}));
    controller.position(72,inside);frame=controller.update(72,20006,true);CHECK(!frame.checked);CHECK(frame.marchers);
    const auto priorGeneration=frame.generations[1];controller.reset();CHECK(!controller.update(72,20007,true).enabled);
    CHECK(controller.select(document->views(),72));frame=controller.update(72,20008,true);CHECK(frame.objective==0xC8DC7CFDU);CHECK(!frame.checked);
    controller.position(72,{-772.25F,-130.75F,-7.5F});frame=controller.update(72,20009,true);
    CHECK(frame.generations[1]!=priorGeneration);CHECK(!controller.submitted(72,g::kBank,1,priorGeneration,20010));
    constexpr g::Point triangle[]{{0,0,0},{2,0,0},{0,2,0},{0,0,0}};
    const g::Volume concave{0,0,"closed polygon",{0,0,0},{2,2,1},triangle};
    CHECK(!g::contains(concave,{1.8F,1.8F,0.5F}));CHECK(g::contains(concave,{0.1F,0.1F,0.5F}));
    traversal_tests(document->views(),false,false);traversal_tests(document->views(),true,false);traversal_tests(document->views(),true,true);traversal_wire_tests();ending_identity_and_codec_tests();module_native_path_tests();vance_native_path_tests();ending_cadence_tests();lighthouse_wire_tests();ai_lattice_tests();roster_tests();codec_tests();std::printf("Gateway opening: %u checks passed\n",checks);
}
