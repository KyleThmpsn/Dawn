#include "../src/state/activity/deep_storage/controller.h"
#include "../src/client/hooks/bootflow/native_hook_ownership.h"
#include "../src/state/activity/deep_storage/scan_playback.h"
#include "../src/state/activity/deep_storage/plate_presentation.h"
#include "fixtures/deep_storage/lens_damage_tests.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <limits>
namespace ds=dawn::state::activity::deep_storage;
namespace coo=dawn::state::activity::coo;
void deep_storage_runtime_startup_checks();
static void check(bool ok,const char* what) {if(!ok) {std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
// The production installer plans must not patch an entry that another owner
// already changed before mission receipt hooks can finish installing.
static void shared_hook_ownership_checks() {
    namespace hooks=dawn::client::hooks::bootflow::native_hook_ownership;
    std::array<std::uintptr_t,19> sites{};
    std::array<unsigned,19> owners{};std::size_t count{};
    const auto add=[&](const auto& targets,unsigned owner) {
        for(const auto target:targets) {
            check(target!=0 && count<sites.size(),"physical hook plan fits and has valid targets");
            check(std::find(sites.begin(),sites.begin()+count,target)==sites.begin()+count,
                  "mission and native observers have one physical owner per hook target");
            sites[count]=target;owners[count++]=owner;
        }
    };
    add(hooks::kAmbientNamedPoints,1);add(hooks::kHijackedPlacements,2);
    add(hooks::kArcCharge,3);add(hooks::kNativeCapture,4);
    add(hooks::kCleanupOwner,5);
    add(hooks::kPropertyList,6);
    add(hooks::kLocalReconnect,7);
    const auto owner_of=[&](std::uintptr_t target) {
        for(std::size_t i=0;i<count;++i)if(sites[i]==target)return owners[i];
        return 0U;
    };
    check(count==sites.size() && hooks::kNativeCapture.empty(),"capture sampler uses existing owner without a detour");
    check(owner_of(0x575690)==2,"Hijacked owns shared object construction");
    check(owner_of(0x4E25D0)==1,"ambient owns shared point interface");
    check(owner_of(0x1006F20)==3,"arc-charge owns shared mission and capture timer");
    check(owner_of(0xF9C150)==5,"cleanup owner guard has one physical detour");
}
static ds::Point point(const ds::Volume& v) {
    for(unsigned x=1;x<40;++x) for(unsigned y=1;y<40;++y) {
        ds::Point p{v.min.x+(v.max.x-v.min.x)*float(x)/40,v.min.y+(v.max.y-v.min.y)*float(y)/40,(v.min.z+v.max.z)/2};
        if(ds::contains(v,p)) {return p;}
    }std::abort();
}
static void enter(ds::Controller& c,std::uint64_t run,coo::Asset a) {
    for(const auto& v:ds::kVolumes) {if(v.asset==a) {c.position(run,point(v));return;}}std::abort();
}
static std::unique_ptr<coo::script::MissionDocument> parse(std::string_view text) {
    std::string error;auto doc=coo::script::MissionDocument::parse_lua(text,ds::kProfile,error);
    if(!doc) {std::fprintf(stderr,"Lua: %s\n",error.c_str());}return doc;
}
static std::string shipped() {
    std::ifstream file(std::filesystem::path(__FILE__).parent_path().parent_path()/"scripts"/"deep_storage.lua");
    std::ostringstream text;text<<file.rdbuf();check(file.good() || file.eof(),"read shipped Lua");return text.str();
}
static void playback_checks() {
    for(float duration:{5.F,9.F}) {
        ds::ScanPlayback p{7,2,1,.2F,duration};
        check(p.started(7,true),"effective native duration permits participant start");
        check(!p.started(7,false) && !p.started(8,true) && !p.started(0,true),"participant and current generation required");
        p.elapsed=0.F;check(!p.started(7,true),"zero native progress is not a start");
        p.active=0;p.elapsed=3.F;check(!p.finished(7,true),"base duration cannot finish source-overridden scan");
        p.elapsed=duration-.01F;check(!p.finished(7,true),"early native finish rejected");
        p.elapsed=duration;check(!p.finished(7,false) && !p.finished(8,true),"finish requires retained start and matching generation");
        check(p.finished(7,true),"effective native duration boundary completes scan");
        p.elapsed=duration+.00310754776F;check(p.finished(7,true),"native completion overshoot accepted");
        p.active=1;check(!p.finished(7,true),"active playback cannot finish");
        p.active=0;p.mode=1;check(!p.finished(7,true),"idle interaction cannot finish");
        p.mode=2;p.active=2;check(!p.valid(7),"invalid active state rejected");
    }
    const auto nan=std::numeric_limits<float>::quiet_NaN(),infinity=std::numeric_limits<float>::infinity();
    for(float invalid:{0.F,-1.F,nan,infinity,-infinity}) {
        ds::ScanPlayback p{7,2,1,.2F,invalid};
        check(!p.valid(7) && !p.started(7,true),"nonpositive or nonfinite native duration rejected");
        p.active=0;p.elapsed=100.F;check(!p.finished(7,true),"invalid duration cannot manufacture completion");
    }
    for(float invalid:{-1.F,nan,infinity,-infinity}) {
        ds::ScanPlayback p{7,2,1,invalid,5.F};
        check(!p.valid(7) && !p.started(7,true),"negative or nonfinite native elapsed rejected");
        p.active=0;check(!p.finished(7,true),"invalid elapsed cannot finish");
    }
    // Duration is native state, not an ownership selector or a whitelist.
    ds::ScanPlayback changed{7,2,1,.2F,7.25F};check(changed.started(7,true),"another positive effective duration is supported");
    changed.active=0;changed.elapsed=7.25F;check(changed.finished(7,true),"completion follows effective native duration");
}

struct PlateDevice {
    float actual{},target{};std::uint32_t revision{7};unsigned reads{},calls{};
    bool owned{true},accept{true},acceptTarget{true},acceptRevision{true};unsigned retireAfter{};
    static constexpr std::uintptr_t address=0x10000;
    template<class T> bool value(std::uintptr_t at,T& out) noexcept {
        ++reads;if(retireAfter && reads>=retireAfter) {owned=false;}
        if constexpr(std::is_same_v<T,float>) {
            if(at==address+0x370) {out=actual;return true;}
            if(at==address+0x37C) {out=target;return true;}
        } else if constexpr(std::is_same_v<T,std::uint32_t>) {
            if(at==address+0x960) {out=revision;return true;}
        }return false;
    }
    bool reconcile(ds::PlateRequest request) noexcept {
        request.capture.presentationPosition=ds::plate_presentation::position(request.state,request.plate.index);
        return ds::plate_presentation::reconcile(request,*this,address,[&] {return owned;},[&](float desired,std::uint32_t next) {
            ++calls;if(accept) {actual=desired;if(acceptTarget) {target=desired;}if(acceptRevision) {revision=next;}}
        });
    }
};
static void plate_presentation_checks() {
    ds::PlateRequest request{{7,1},{{7,2},0x20000,1,10,11,12,13},{true,false,false,false,2},true};
    request.state.armed=false;request.state.occupied=true;
    PlateDevice preload;check(preload.reconcile(request) && preload.actual==.2F,"preloaded occupied final plate stays red before arming");
    const auto preRevision=request.state.revision;request.state.armed=true;
    check(preload.reconcile(request) && preload.actual==.1F && request.state.revision==preRevision,"arming with unchanged occupancy/revision publishes charging pose");
    request.plate.index=0;request.state.armed=false;PlateDevice entryPreload;
    check(!entryPreload.reconcile(request) && entryPreload.calls==0,"entrance presentation remains disabled until armed");
    request.plate.index=1;request.state={true,false,false,false,2};
    PlateDevice d;check(d.reconcile(request) && d.actual==.2F,"final idle plate publishes red rod mode");
    request.state.occupied=true;check(d.reconcile(request) && d.actual==.1F && d.target==.1F,"final occupied plate charges in native active pose");
    request.state.contested=true;check(d.reconcile(request) && d.actual==.2F,"contested final plate returns to red pose");
    request.state.contested=false;request.state.occupied=false;check(d.reconcile(request) && d.actual==.2F,"incomplete final departure retains red rod");
    request.state.charged=true;check(d.reconcile(request) && d.actual==0.F && d.target==0.F,"completed final plate removes rod");
    const auto calls=d.calls,revision=d.revision;
    request.state.occupied=true;check(d.reconcile(request) && d.calls==calls,"completed final reentry does not restore rod");
    request.state.occupied=false;request.state.contested=true;
    check(d.reconcile(request) && d.calls==calls && d.revision==revision,"completed contest cannot restore rod");
    d.target=.1F;check(d.reconcile(request) && d.calls==calls+1 && d.target==0.F,"pending native rod reappearance is cancelled");
    d.actual=.1F;check(d.reconcile(request) && d.actual==0.F,"completed native rod drift is repaired");
    request.plate.index=2;check(d.reconcile(request) && d.actual==0.F,"right final plate has identical completion semantics");
    request.plate.index=0;request.state={true,false,false,false,2};d={};
    check(d.reconcile(request) && d.calls==0,"entrance empty plate retains original idle mode0");
    request.state.occupied=true;check(d.reconcile(request) && d.actual==.1F,"entrance occupancy keeps native mode.1");
    request.state.contested=true;check(d.reconcile(request) && d.actual==.2F,"entrance contest keeps native mode.2");
    request.state.charged=true;request.state.occupied=false;check(d.reconcile(request) && d.actual==.1F,"entrance completed plate still retains receiver");
    request.plate.index=1;
    for(unsigned retiredAt:{1U,2U,3U}) {
        PlateDevice stale;stale.actual=stale.target=.1F;stale.retireAfter=retiredAt;
        check(!stale.reconcile(request) && stale.calls==0,"source retirement during native sample prevents setter");
    }
    PlateDevice stale;stale.owned=false;check(!stale.reconcile(request) && stale.reads==0 && stale.calls==0,"stale owner rejected before native reads");
    for(unsigned bad=0;bad<3;++bad) {
        PlateDevice refused;refused.actual=refused.target=.1F;
        if(bad==0) {refused.accept=false;}if(bad==1) {refused.acceptTarget=false;}if(bad==2) {refused.acceptRevision=false;}
        check(!refused.reconcile(request),"current pose target and native revision must all acknowledge setter");
    }
    PlateDevice exhausted;exhausted.actual=exhausted.target=.1F;exhausted.revision=UINT32_MAX-1U;
    check(!exhausted.reconcile(request) && exhausted.calls==0,"visual revision exhaustion cannot wrap");
    PlateDevice first;first.actual=first.target=.1F;first.revision=UINT32_MAX;
    check(first.reconcile(request) && first.revision==0,"native unset revision starts at zero");
    for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        PlateDevice bad;bad.actual=invalid;check(!bad.reconcile(request) && bad.calls==0,"invalid current native pose rejected");
        bad.actual=0.F;bad.target=invalid;check(!bad.reconcile(request) && bad.calls==0,"invalid target native pose rejected");
    }
    request.enabled=false;PlateDevice ended;
    check(!ended.reconcile(request) && ended.calls==0,"disabled mission relinquishes visual ownership");
    request.enabled=true;request.plate.owner.value=0;
    check(!ended.reconcile(request) && ended.calls==0,"invalid plate generation cannot affect visual ownership");
}

static constexpr std::uint16_t firstWaves[2][5]{{99,100,101,105,106},{102,103,104,107,108}};
static constexpr std::uint16_t secondWaves[2][5]{{109,111,113,114,117},{110,112,115,116,118}};
static int first_wave_side(std::uint16_t slot) {
    for(int side=0;side<2;++side) {for(auto source:firstWaves[side]) {if(slot==source) {return side;}}}return -1;
}
static bool death_gate(std::uint16_t slot) {return slot==35 || slot==52 || (slot>=54 && slot<=71) || first_wave_side(slot)>=0;}
struct Replay {
    ds::Controller c;const coo::script::Views& views;std::uint64_t now{1000};static constexpr std::uint64_t run=700;
    std::array<std::vector<ds::EnemyReceipt>,std::size(ds::kSpawns)> enemies{};
    std::array<bool,3> plateChecked{};std::array<bool,2> scanChecked{};std::array<bool,7> phases{};
    std::vector<unsigned> dialogue;bool readinessChecked{},deathChecked{},endingHeld{},restricted{};unsigned hologramWaitTicks{};
    bool preloadBeforePit{},hydraSpawnWithSurvivors{},portalWithSurvivors{},finalScanWithSurvivors{};unsigned hydraDelay{};
    bool bossLast{},pitBossOnly{},pitWavesOnly{},pitClosed{},pitReleased{};unsigned bossDelay{};
    std::bitset<19> pitDeaths{};bool lensBound{},lensShot{},lensProtected{},lensExposedBeforeClear{};unsigned lensWaitTicks{};std::bitset<4> initialBeams{},removedBeams{};
    bool rightFirst{},skipWaves{},descentRequestedEarly{},descentDeferred{},earlyVolumes{};
    std::array<unsigned,2> firstWaveTicks{};std::array<bool,2> firstWaveBeforeCharge{},secondWaveBeforeCharge{},sideVisited{};
    explicit Replay(const coo::script::Views& v,bool last=false,bool right=false,bool skip=false):views(v),bossLast(last),rightFirst(right),skipWaves(skip) {
        check(c.select(v,run),"select recovered mission");check(!c.update(run,now,false).enabled,"loading cannot start the mission");
        c.position(run+1,point(ds::kVolumes[0]));check(!c.update(run,++now,false).enabled,"foreign position cannot establish arrival");
        check(c.update(run,++now,true).enabled,"confirmed arrival starts without a position sample");
    }
    unsigned living(std::uint16_t first,std::uint16_t last) {
        unsigned count{};c.living_enemies([&](const auto& r) {if(r.registry==0x59700FA7U && r.source>=first && r.source<=last) {++count;}});return count;
    }
    bool active_source(std::uint16_t slot) const {
        return c.frame().native[ds::asset_index(ds::find(0x59700FA7U,1,slot)->asset)].active;
    }
    bool wave_active(unsigned side,bool second) const {
        for(auto slot:std::span<const std::uint16_t>{second?secondWaves[side]:firstWaves[side],5}) {if(!active_source(slot)) {return false;}}return true;
    }
    unsigned wave_living(unsigned side) {
        unsigned n{};for(auto slot:firstWaves[side]) {n+=living(slot,slot);}return n;
    }
    void trigger_state() {
        const auto& f=c.frame();
        unsigned descent{};for(std::uint16_t slot=1;slot<=17;++slot) {if(active_source(slot)) {++descent;}}
        if(f.section==0 && descent) {
            check(f.native[ds::asset_index(ds::find(0xA13D8A45U,23,0)->asset)].active,"descent requests follow opening the Pyramidion door");
            check(living(1,17)==0,"opening does not depend on any descent actor admission");
            if(descent==17) {
                if(!earlyVolumes) {for(std::size_t i=0;i<std::size(ds::kVolumes);++i) {if(ds::kVolumes[i].asset.registry==0x59700FA7U) {check(!c.seen()[i],"all17 descent requests precede any descent volume crossing");}}}
                descentRequestedEarly=true;
            }
        }
        for(unsigned side=0;side<2;++side) {
            if(wave_active(side,false) && wave_living(side)>0) {
                ++firstWaveTicks[side];
                if(!firstWaveBeforeCharge[side]) {check(!f.plates[side+1].charged && sideVisited[side],"own occupancy starts first wave before plate charge");firstWaveBeforeCharge[side]=true;}
            }
            if(wave_active(side,true)) {
                check(!skipWaves && firstWaveTicks[side]>=4 && wave_living(side)==0,"second wave requires genuine deaths of all own first-wave actors");
                if(!secondWaveBeforeCharge[side]) {check(!f.plates[side+1].charged,"second wave starts before charge without a charge prerequisite");secondWaveBeforeCharge[side]=true;}
            }
            if(!sideVisited[side]) {for(auto slot:firstWaves[side]) {check(!active_source(slot),"unvisited plate has no first-wave requests");}for(auto slot:secondWaves[side]) {check(!active_source(slot),"unvisited plate has no second-wave requests");}}
            if(skipWaves) {for(auto slot:secondWaves[side]) {check(!active_source(slot),"skipped first-wave deaths never spawn second wave");}}
        }
        const auto center=ds::find(0x59700FA7U,23,88)->asset;
        if(f.lensDestroyed) {check(!f.native[ds::asset_index(center)].active && ds::device_position(f,center)==0.F,"center beam remains removed after box death through ending");}
        const auto wall=ds::find(0xE6402111U,23,0)->asset;const auto state=f.native[ds::asset_index(wall)];
        if(state.managed) {check(!state.desired && !state.active && ds::device_position(f,wall)==.2F,"unused descent wall stays natively removed in every phase");}
        if(f.native[ds::asset_index(ds::kPlates[0].source)].managed) {check(state.managed,"unused wall removal begins with opening setup before descent");}const auto portal=ds::find(0x59700FA7U,23,38)->asset;
        if(f.native[ds::asset_index(portal)].active) {
            check(hydraDelay>=4,"portal cannot open before the authentic Hydra death");
            check(living(0,34)>0,"portal opens with descent and arena adds still alive");portalWithSurvivors=true;
        }
        if(f.section==4 && !preloadBeforePit && initialBeams.all() && c.plate_request(1).plate.valid() && c.plate_request(2).plate.valid()) {
            check(!pitDeaths.all() && f.native[ds::asset_index(ds::kLens)].acknowledged,"final puzzle loads while prior pit encounter is still active");
            for(std::uint8_t i=1;i<3;++i) {
                enter(c,run,ds::kPlates[i].volume);const auto plate=c.plate_request(i);
                check(plate.enabled && !plate.state.armed && plate.state.occupied,"preloaded plate can present before gameplay arming");
                check(ds::plate_presentation::position(plate.state,i)==.2F,"preloaded occupied plate remains red");
                check(!c.plate(plate.plate,plate.state.revision,.5F,false) && !c.plate(plate.plate,plate.state.revision,1.F,true),"early occupancy cannot charge an unarmed final plate");
            }
            c.position(run,{0,0,0});preloadBeforePit=true;
        }
        if(f.scanArmed[1]) {check(living(98,118)>0,"final scan becomes available with final-room Vex still alive");finalScanWithSurvivors=true;}
    }
    void pit_state(const ds::Frame& f) {
        const coo::Asset pit{0x59700FA7U,0x80B566FDU,23,50};const auto state=f.native[ds::asset_index(pit)];
        if(!state.managed) {return;}
        check(state.desired==state.active,"pit logical request and published active state remain aligned");
        if(!pitDeaths.all()) {
            check(state.active && ds::device_position(pit,state.active)==0.F,"pit remains physically closed before boss AND all three wave deaths");
            pitClosed=true;
            if(pitDeaths[0] && pitDeaths.count()<19) {pitBossOnly=true;}
            if(!pitDeaths[0] && pitDeaths.count()==18) {pitWavesOnly=true;}
        } else if(!state.active) {
            check(ds::device_position(pit,state.active)==1.F,"pit removal publishes native fade/collision-removal mode1");pitReleased=true;
        }
    }
    void natives() {
        const auto frame=c.frame();
        if(frame.section==1 && !descentDeferred) {
            check(descentRequestedEarly && living(1,17)==0,"opening reaches descent with all17 requested and zero descent admissions");
            for(std::uint16_t slot=1;slot<=17;++slot) {check(active_source(slot),"all17 early requests survive phase transition");}descentDeferred=true;
        }
        for(const auto& b:ds::kAssets) {
            const auto a=b.asset;const auto state=frame.native[ds::asset_index(a)];
            if(a.type!=4 || !state.managed || !state.desired) {continue;}
            if(!state.prepared) {
                check(!c.prepared({run+1,c.owner().value},a),"foreign object preparation rejected");
                check(c.prepared(c.owner(),a),"native inactive state permits fresh creation");continue;
            }
            if(!state.acknowledged) {
                if(a==ds::find(0x59700FA7U,4,82)->asset && hologramWaitTicks++<3) {
                    check(std::find(dialogue.begin(),dialogue.end(),14U)==dialogue.end(),"coordinates wait for owned hologram creation");
                    const auto device=ds::find(0x59700FA7U,23,92)->asset;
                    check(!c.frame().native[ds::asset_index(device)].active,"hologram remains hidden while creating its source");
                    check(!c.frame().finished,"ending cannot complete before hologram source exists");continue;
                }
                const auto index=static_cast<std::uint32_t>(ds::asset_index(a));
                coo::ObjectReceipt receipt{{run,state.generation},a,10000+index,20000+index};
                auto bad=receipt;++bad.owner.value;check(!c.object(bad),"wrong object generation rejected");
                check(c.object(receipt),"owned native entity acknowledges source");
            }
        }
        const auto lensSource=c.frame().native[ds::asset_index(ds::kLens)];
        if(lensSource.acknowledged && !lensBound) {
            const ds::LensReceipt receipt{{run,lensSource.generation},0x40000,1500,1600,1700};
            check(!c.lens(receipt,true),"already-dead candidate cannot establish live lens owner");
            auto bad=receipt;++bad.owner.value;check(!c.lens(bad,false),"foreign generation cannot bind lens");
            check(c.lens(receipt,false),"exact live lens health binds source owner");lensBound=true;
            check(!c.lens(receipt,true),"initial shield rejects native death before both final plates");lensProtected=true;
        }
        for(std::uint8_t i=0;i<3;++i) {
            const auto s=c.frame().native[ds::asset_index(ds::kPlates[i].source)];if(!s.acknowledged || c.plate_request(i).plate.valid()) {continue;}
            ds::PlateReceipt r{{run,s.generation},0x10000U+std::uintptr_t(i)*0x1000,i,100U+i,200U+i,300U+i,400U+i};
            auto stale=r;++stale.owner.value;check(!c.bind_plate(stale),"plate bound to current created source only");check(c.bind_plate(r),"native plate owner accepted");
            {auto wrongPoseOwner=r;++wrongPoseOwner.owner.value;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects retired source generation");
             wrongPoseOwner=r;++wrongPoseOwner.serial;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects recycled entity identity");
             wrongPoseOwner=r;++wrongPoseOwner.device;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another device component");
             wrongPoseOwner=r;++wrongPoseOwner.owner.run;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects retired mission run");
             wrongPoseOwner=r;++wrongPoseOwner.source;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another source address");
             wrongPoseOwner=r;++wrongPoseOwner.timer;
             check(!c.plate_pose(wrongPoseOwner,{0.F,0.F,INT32_MAX,INT32_MAX}),"pose rejects another native timer");
             check(c.plate_pose(r,{0.F,0.F,700,900}),"server accepts exact native pose receipt");}

        }
        for(std::uint8_t i=0;i<2;++i) {
            const auto s=c.frame().native[ds::asset_index(ds::kScans[i].source)];if(!s.acknowledged || c.scan_request(i).scan.valid()) {continue;}
            ds::ScanReceipt r{{run,s.generation},0x20000U+std::uintptr_t(i)*0x1000,i,500U+i,600U+i,700U+i};
            const coo::CommandSpec waiting{coo::Operation::observation,ds::kScans[i].source,11,coo::Wait::observed};
            check(c.missing(waiting).missing==coo::Missing::controller,"created scan awaiting binding reports controller");
            check(c.bind_scan(r),"native scan owner accepted");check(!c.scan(r,false,true),"scan cannot complete without start");
            const auto missing=c.missing(waiting);
            check(missing.missing==coo::Missing::observation && missing.expected==1 && missing.actual==0 && missing.detail==11,
                "bound scan awaiting native start reports observation rather than missing object");
        }
        for(std::size_t i=0;i<std::size(ds::kSpawns);++i) {
            const auto& spawn=ds::kSpawns[i];const auto a=ds::find(spawn.registry,1,spawn.source)->asset;
            if(!c.frame().native[ds::asset_index(a)].active || !enemies[i].empty()) {continue;}
            if(frame.section==0 && a.registry==0x59700FA7U && a.slot>=1 && a.slot<=17) {continue;}
            check(c.missing({coo::Operation::population,a,1,coo::Wait::nativeReady}).missing==coo::Missing::admission,"empty source never counts as ready");
            for(std::uint8_t n=0;n<spawn.count;++n) {
                ds::EnemyReceipt r{run,static_cast<std::uint32_t>(1000+i*16+n),static_cast<std::uint32_t>(3000+i),c.frame().spawnGeneration,spawn.source,spawn.registry};
                auto stale=r;++stale.generation;check(!c.admitted(stale),"stale source generation rejected");
                check(!c.died(r),"death before admission cannot clear a cohort");check(c.admitted(r),"native actor admitted to its requested source");
                check(!c.admitted(r),"duplicate actor admission ignored");enemies[i].push_back(r);
                if(!readinessChecked) {
                    const auto miss=c.missing({coo::Operation::population,a,1,coo::Wait::nativeReady}).missing;
                    check(miss==coo::Missing::admission || miss==coo::Missing::health,"created actor with no health is not ready");
                    readinessChecked=true;
                }
                const auto t=spawn.tactical;
                check(c.readiness(r,{true,true,true,true,r.actor+9000,t.registry,t.slot,t.row}),"native health AI and tactical ownership acknowledged");
            }
        }
    }
    void fulfill(const coo::CommandSpec& s) {
        if(const auto* condition=views.condition(s)) {
            static_cast<void>(condition->evaluate([&](const coo::CommandSpec& child) {fulfill(child);return true;}));return;
        }
        if(s.asset==ds::kLens) {
            const auto request=c.lens_request();check(request.enabled && request.lens.valid() && request.vulnerable,"both final plates expose a bound box");
            check(ds::device_position(c.frame(),ds::kLensDevice)==.75F,"exposed native lens uses .75");
            unsigned living{};c.living_enemies([&](const auto&) {++living;});lensExposedBeforeClear|=living!=0;
            if(lensWaitTicks++<3) {
                check(!c.frame().native[ds::asset_index(ds::kScans[1].source)].active,"plates alone cannot reveal conflux without real box destruction");return;
            }
            auto stale=request.lens;++stale.health;check(!c.lens(stale,true),"foreign health cannot destroy puzzle box");
            stale=request.lens;++stale.serial;check(!c.lens(stale,true),"recycled box entity cannot report destruction");
            const auto exposed=c.frame().native[ds::asset_index(ds::kLensDevice)];
            check(c.device({run,exposed.generation},ds::kLensDevice,static_cast<std::int16_t>(exposed.generation),.75F),"native exposed position acknowledges its revision");
            check(c.lens(request.lens,true),"real death of exposed owned box unlocks reveal");lensShot=true;
            const auto removed=c.frame().native[ds::asset_index(ds::kLensDevice)];
            check(removed.generation==exposed.generation+1 && !removed.acknowledged && removed.active,"native death reserves new removal revision before Lua retirement");
            check(ds::device_position(c.frame(),ds::kLensDevice)==0.F,"native death publishes removal mode0 immediately");
            check(!c.device({run,exposed.generation},ds::kLensDevice,static_cast<std::int16_t>(exposed.generation),.75F),"old exposed acknowledgement cannot consume removal");
            check(!c.device({run,exposed.generation},ds::kLensDevice,static_cast<std::int16_t>(exposed.generation),0.F),"new removal value with old revision is rejected");
            check(c.device({run,removed.generation},ds::kLensDevice,static_cast<std::int16_t>(removed.generation),0.F),"new native removal acknowledgement accepted");
            check(!c.lens(request.lens,true) && c.frame().native[ds::asset_index(ds::kLensDevice)].generation==removed.generation,"duplicate box death cannot advance revision");return;
        }
        if(s.asset==ds::kDialogueAsset) {return;}
        if(s.asset.type==60) {enter(c,run,s.asset);return;}
        if(s.asset.type==1) {
            check(s.asset.registry==0x59700FA7U && death_gate(s.asset.slot),"only portal Hydra pit and ten first-wave sources request deaths");
            const auto waveSide=first_wave_side(s.asset.slot);
            if(waveSide>=0 && (skipWaves || firstWaveTicks[waveSide]<4)) {return;}
            if(s.asset.slot==35) {
                check(living(0,34)>0,"Hydra spawns without clearing descent or earlier gate enemies");hydraSpawnWithSurvivors=true;
                if(hydraDelay++<3) {return;}
            }
            if((s.asset.slot==52 || (s.asset.slot>=54 && s.asset.slot<=71)) && !preloadBeforePit) {return;}
            const auto i=ds::spawn_index(s.asset);check(i<enemies.size() && !enemies[i].empty(),"clear wait has an admitted roster");
            const bool pitBoss=s.asset.registry==0x59700FA7U && s.asset.slot==52;
            const bool pitWave=s.asset.registry==0x59700FA7U && s.asset.slot>=54 && s.asset.slot<=71;
            if(pitWave && !bossLast && !pitDeaths[0]) {return;}
            if(pitBoss && bossLast && (pitDeaths.count()<18 || bossDelay++<3)) {return;}
            for(const auto& r:enemies[i]) {
                if(!deathChecked) {
                    auto wrong=r;++wrong.owner;check(!c.died(wrong),"foreign source owner cannot report death");
                    auto stale=r;stale.actor^=0x40000000U;check(!c.died(stale),"unadmitted actor cannot report death");deathChecked=true;
                }
                static_cast<void>(c.died(r));
            }
            if(pitBoss) {pitDeaths.set(0);}if(pitWave) {pitDeaths.set(s.asset.slot-53);}
            return;
        }
        for(std::uint8_t i=0;i<3;++i) {if(s.asset!=ds::kPlates[i].source) {continue;}
            const auto request=c.plate_request(i);if(!request.enabled || !request.plate.valid()) {return;}
            if(i) {
                const unsigned first=rightFirst?2U:1U;
                if(i!=first && !c.frame().plates[first].charged) {return;}
                sideVisited[i-1]=true;
            }
            enter(c,run,ds::kPlates[i].volume);if(s.argument==11 || c.frame().plates[i].charged) {return;}
            if(i && (skipWaves?firstWaveTicks[i-1]<4:!secondWaveBeforeCharge[i-1])) {return;}
            auto state=c.frame().plates[i];const auto receipt=request.plate;
            if(!plateChecked[i]) {
                check(!c.plate(receipt,state.revision,1.F,true),"no timer progress cannot complete a plate");
                static_cast<void>(c.plate(receipt,state.revision,.4F,false));
                c.position(run,{0,0,0});check(!c.plate(receipt,state.revision,1.F,true),"leaving plate invalidates active charge");
                enter(c,run,ds::kPlates[i].volume);state=c.frame().plates[i];
                static_cast<void>(c.plate(receipt,state.revision,.3F,false));
                check(c.contested(receipt,true),"native enemy occupying volume contests plate");
                check(!c.plate(receipt,state.revision,1.F,true),"enemy contest invalidates prior completion");
                check(!c.contested_positions(receipt,{},false) && c.frame().plates[i].contested,"missing enemy positions cannot clear contest");
                std::array<ds::EnemyPosition,256> outside{};std::size_t outsideCount{};
                c.living_enemies([&](const auto& enemy) {outside[outsideCount++]={enemy,{100000.F,100000.F,100000.F}};});
                check(c.contested_positions(receipt,std::span(outside).first(outsideCount),true),"server clears contest only after all current living actors are outside");state=c.frame().plates[i];
                auto bad=receipt;++bad.serial;check(!c.plate(bad,state.revision,.5F,false),"recycled entity cannot charge plate");plateChecked[i]=true;
            }
            state=c.frame().plates[i];static_cast<void>(c.plate(receipt,state.revision,.1F,false));
            check(c.plate(receipt,state.revision,1.F,true),"native timer progress then completion accepted");
            c.position(run,{0,0,0});enter(c,run,ds::kPlates[i].volume);
            check(c.frame().plates[i].charged && c.frame().plates[i].revision==state.revision,"completed charge survives departure and reentry");
            if(i) {
                const bool both=c.frame().plates[1].charged && c.frame().plates[2].charged;
                check(c.frame().lensExposed==both,"only both final plates expose box");
                if(!both) {check(!c.lens(c.lens_request().lens,true),"one plate cannot release box destruction");}
                else {unsigned living{};c.living_enemies([&](const auto&) {++living;});lensExposedBeforeClear|=living!=0;}
            }return;
        }
        for(std::uint8_t i=0;i<2;++i) {if(s.asset!=ds::kScans[i].source) {continue;}
            const auto request=c.scan_request(i);if(!request.enabled || !request.scan.valid()) {return;}
            auto bad=request.scan;++bad.controller;check(!c.scan(bad,true,false),"foreign Ghost controller cannot start scan");
            ds::ScanPlayback playback{request.scan.owner.value,2,1,.25F,i==0?5.F:9.F};
            if(s.argument==11 && !request.started) {
                check(!c.scan_playback(request.scan,playback,false),"missing native participant cannot start route scan");
                auto stalePlayback=playback;++stalePlayback.revision;check(!c.scan_playback(request.scan,stalePlayback,true),"stale playback generation cannot start route scan");
                check(c.scan_playback(request.scan,playback,true),"5/9-second native playback starts held interaction");scanChecked[i]=true;
            }
            if(s.argument==12 && request.started && !request.complete) {
                const auto missing=c.missing(s);
                check(missing.missing==coo::Missing::observation && missing.detail==12,"bound started scan reports pending completion observation");
                playback.active=0;playback.elapsed=3.F;
                check(!c.scan_playback(request.scan,playback,false),"default3 cannot release overridden route scan");
                playback.elapsed=playback.duration+.00310754776F;
                check(c.scan_playback(request.scan,playback,false),"5/9-second native completion advances mission");
            }return;
        }
    }
    void execute() {
        for(unsigned tick=0;tick<1600 && !c.frame().finished;++tick) {
            const auto frame=c.update(run,now,true);now+=250;check(frame.enabled,"executor stays enabled");phases[frame.section]=true;restricted|=frame.restricted;
            pit_state(frame);
            if(frame.activeRow!=coo::kNoDialogue) {
                check(!c.submitted(run+1,ds::kBank,frame.activeRow,frame.generations[frame.activeRow],now),"foreign dialogue callback rejected");
                if(c.submitted(run,ds::kBank,frame.activeRow,frame.generations[frame.activeRow],now)) {dialogue.push_back(frame.activeRow);}
            }
            if(frame.section==6 && !dialogue.empty() && dialogue.back()==14 && !frame.finished) {endingHeld=true;}
            natives();const auto* graph=c.graph();
            if(frame.section==4 || frame.section==5) {
                for(const auto& b:graph->commands) {
                    const auto step=c.step_state(b.step);
                    if(!step.commands[b.command].requested) {continue;}
                    const auto& spec=graph->definition.steps[b.step].commands[b.command];
                    for(const char* side:{"left","right"}) {
                        const std::string name=std::string("d_pyramidion_whisk_map_room_laser_")+side;
                        const std::string catchName=std::string("d_pyramidion_whisk_map_room_laser_catch_")+side;
                        const auto* asset=ds::find(spec.asset.registry,static_cast<std::uint8_t>(spec.asset.type),spec.asset.slot);
                        if(!asset || (asset->name!=name && asset->name!=catchName)) {continue;}
                        const auto i=std::string_view(side)=="left"?1U:2U;
                        const auto beam=(i-1)*2+(asset->name==catchName?1U:0U);
                        if(spec.argument==1 && !c.frame().plates[i].charged) {
                            check(c.frame().native[ds::asset_index(spec.asset)].active,"initial uncharged plate beam and catch are enabled");initialBeams.set(beam);
                        }
                        if(spec.argument==0) {
                            check(c.frame().plates[i].charged && !c.frame().native[ds::asset_index(spec.asset)].active,"completed plate publishes its beam and catch removal");removedBeams.set(beam);
                        }
                    }
                }
            }
            trigger_state();
            for(const auto& b:graph->commands) {
                const auto state=c.step_state(b.step);if(state.phase!=coo::StepPhase::active || !state.commands[b.command].requested) {continue;}
                const auto& s=graph->definition.steps[b.step].commands[b.command];if(coo::is_observation(s.operation)) {fulfill(s);}
            }
        }
        if(!c.frame().finished) {
            const auto* graph=c.graph();std::fprintf(stderr,"Stalled section=%u\n",c.frame().section);
            for(const auto& b:graph->commands) {if(c.step_state(b.step).phase==coo::StepPhase::active) {std::fprintf(stderr," %.*s\n",static_cast<int>(b.id.size()),b.id.data());}}
        }
        check(c.frame().finished && c.frame().completion.valid(),"complete route publishes owned mission success");
        check(descentRequestedEarly && descentDeferred,"all descent requests precede entry despite withheld native admission");
        for(unsigned side=0;side<2;++side) {
            check(firstWaveBeforeCharge[side] && firstWaveTicks[side]>=4,"both own first waves remain alive across multiple ticks before charge");
            if(skipWaves) {check(wave_living(side)==5 && !wave_active(side,true),"scan completes encounter with both first waves alive and both second waves unrequested");}
            else {check(secondWaveBeforeCharge[side] && wave_living(side)==0,"each independent second wave follows actual first-wave deaths before charge");}
        }
        check(preloadBeforePit && hydraSpawnWithSurvivors && portalWithSurvivors && finalScanWithSurvivors,"early puzzle preload and trigger-driven path succeed without unrelated enemy deaths");
        check(initialBeams.all() && removedBeams.all(),"both initial beams/catches and independent removal are exercised");
        check(lensBound && lensProtected && lensShot && lensWaitTicks>=4 && lensExposedBeforeClear,"box is protected then exposed before combat clear and awaits actual native death");
        check(pitClosed && pitReleased && pitDeaths.all(),"pit closes for encounter and removes only after complete deaths");
        check(bossLast?pitWavesOnly:pitBossOnly,"pit gate independently waits for remaining boss or waves");
        check(endingHeld && restricted && !c.frame().restricted,"closing speech precedes completion and releases final death restriction");
        check(hologramWaitTicks>=4,"ending exercises delayed native hologram creation");
        const auto hologramSource=ds::find(0x59700FA7U,4,82)->asset,hologramDevice=ds::find(0x59700FA7U,23,92)->asset;
        const auto sourceBefore=c.frame().native[ds::asset_index(hologramSource)];
        const auto deviceBefore=c.frame().native[ds::asset_index(hologramDevice)];
        check(sourceBefore.active && sourceBefore.acknowledged && deviceBefore.active,"completed mission retains its hologram source and display");
        check(ds::device_position(hologramDevice,true)==.5F,"hologram uses the retained middle display range");
        const auto held=c.update(run,now+60000,true);
        const auto center=ds::find(0x59700FA7U,23,88)->asset;
        check(!held.native[ds::asset_index(center)].active && ds::device_position(held,center)==0.F,"central beam remains natively removed60 seconds after ending");
        check(held.finished && held.native[ds::asset_index(hologramSource)].active
            && held.native[ds::asset_index(hologramSource)].generation==sourceBefore.generation
            && held.native[ds::asset_index(hologramDevice)].active
            && held.native[ds::asset_index(hologramDevice)].generation==deviceBefore.generation,"hologram persists after closing dialogue without source recreation");
        for(bool seen:phases) {check(seen,"every mission phase exercised");}
        for(bool seen:plateChecked) {check(seen,"each plate interruption and completion exercised");}
        for(bool seen:scanChecked) {check(seen,"both held Ghost interactions exercised");}
        check(dialogue==std::vector<unsigned>({0,1,2,3,4,5,6,7,9,10,12,11,13,14}),"full transcript plays once in intended order");
        unsigned living{};c.living_enemies([&](const auto&) {++living;});check(living>0,"optional encounter survivors do not block mission completion");
        for(std::size_t i=0;i<3;++i) {
            const auto retained=c.plate_request(i);
            check(retained.enabled && retained.state.charged,"charged native presentation stays owned through mission completion");
        }
        const auto old=c.owner();const auto scan=c.scan_request(1).scan;c.reset();
        for(std::size_t i=0;i<3;++i) {check(!c.plate_request(i).enabled && !c.plate_request(i).plate.valid(),"reset releases completed plate presentation ownership");}
check(c.select(views,run),"same activity can restart after reset");
        check(c.owner().value>old.value,"new attempt receives a fresh lifecycle lease");check(!c.scan(scan,true,false),"previous attempt scan cannot affect reset");
        check(c.seen().none() && !c.frame().restricted,"reset clears geometry and death restriction");
    }
};
int main() {
    deep_storage_runtime_startup_checks();
    shared_hook_ownership_checks();playback_checks();plate_presentation_checks();deep_lens_damage_fixture::run(check);const auto text=shipped();auto doc=parse(text);check(doc!=nullptr,"shipped mission compiles");
    check(doc->views().phases.size()==7,"seven authored mission sections");
    for(const auto* graph:doc->views().phases) for(const auto& step:graph->definition.steps) for(const auto& command:step.commands) {
        if(coo::is_observation(command.operation) && command.asset.type==1) {
            check(command.asset.registry==0x59700FA7U && death_gate(command.asset.slot),"shipped Lua kill waits are limited to portal pit and ten first-wave sources");
        }
    }
    std::bitset<17> openingRequests;std::bitset<10> plateDeathSources;unsigned encounterFinishes{};
    for(std::size_t phase=0;phase<doc->views().phases.size();++phase) {
        for(const auto& step:doc->views().phases[phase]->definition.steps) {for(const auto& command:step.commands) {
            if(command.operation==coo::Operation::population && command.asset.registry==0x59700FA7U && command.asset.type==1 && command.asset.slot>=1 && command.asset.slot<=17) {
                check(phase==0 && command.wait==coo::Wait::requested,"all descent requests belong to opening and never await native admission");openingRequests.set(command.asset.slot-1);
            }
            if(coo::is_observation(command.operation) && command.asset.type==1) {
                for(unsigned side=0;side<2;++side) {for(unsigned n=0;n<5;++n) {if(command.asset.slot==firstWaves[side][n]) {plateDeathSources.set(side*5+n);}}}
            }
            if(command.operation==coo::Operation::mechanic && command.asset==ds::kModule && command.argument==12) {
                check(phase==5 && command.wait==coo::Wait::requested,"trusted encounter completion belongs only to map phase");++encounterFinishes;
            }
        }}
    }
    check(openingRequests.all() && plateDeathSources.all() && encounterFinishes==1,"shipped route contains17 early requests ten plate-wave death gates and one encounter completion");
    auto foreign=text;const auto index=foreign.find("id=\"deep_storage\"");check(index!=std::string::npos,"mission id present");foreign.replace(index,17,"id=\"gateway\"");
    auto other=parse(foreign);ds::Controller rejected;check(!other || !rejected.select(other->views(),1),"foreign mission cannot select this controller");
    Replay replay(doc->views());replay.execute();
    Replay fast(doc->views(),true,true);fast.earlyVolumes=true;
    // Early crossings survive queued dialogue and inactive later phases; current
    // receiving occupancy still must be established by the route driver.
    for(const auto& v:ds::kVolumes) {fast.c.position(Replay::run,point(v));}
    fast.c.position(Replay::run,{0,0,0});check(fast.c.seen().any(),"valid early traversal is retained");fast.execute();
    Replay skipped(doc->views(),false,false,true);skipped.execute();
    std::puts("PASS: Deep Storage full Lua route, authentic receipt guards, plate contest/interruption, scan playback, dialogue ending and reset (offline simulation)");
}
