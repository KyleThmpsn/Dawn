#include <Windows.h>
#include "runtime.h"
#include "controller.h"
#include "../runtime.h"
#include "../../../core/logging/log.h"
#include <cstdio>
#include <mutex>
#include "../../../server/runtime/activity/mission_observation_queue.h"

namespace dawn::state::activity::deep_storage {
namespace {
std::mutex mutex;
Controller controller;
std::array<server::runtime::activity::mission_device_pose::Inbox<PlateReceipt>,std::size(kPlates)> poseInbox{};
coo::ReadinessSchedule readinessSchedule;
enum class ObservationKind {position,plateBinding,plateProgress,contest,lens,scanBinding,scanPlayback};
struct Observation {
    ObservationKind kind{};coo::Generation owner{};Point point{};PlateReceipt plate{};ScanReceipt scan{};LensReceipt lens{};
    ScanPlayback playback{};std::uint32_t revision{};float progress{};bool flag{};
};
server::runtime::activity::MissionObservationQueue<Observation> observations;
struct ContestObservation {PlateReceipt plate{};std::array<EnemyPosition,256> positions{};std::size_t count{};bool complete{};};
std::array<ContestObservation,16> contests;
std::size_t contestCount{};
void consume(const Observation& e) noexcept {
    if(e.owner!=controller.owner()) {return;}
    switch(e.kind) {
    case ObservationKind::position:controller.position(e.owner.run,e.point);break;
    case ObservationKind::plateBinding:static_cast<void>(controller.bind_plate(e.plate));break;
    case ObservationKind::plateProgress:static_cast<void>(controller.plate(e.plate,e.revision,e.progress,e.flag));break;
    case ObservationKind::contest: {
        const auto& sample=contests[e.revision];
        static_cast<void>(controller.contested_positions(sample.plate,std::span(sample.positions).first(sample.count),sample.complete));break;
    }
    case ObservationKind::lens:static_cast<void>(controller.lens(e.lens,e.flag));break;
    case ObservationKind::scanBinding:static_cast<void>(controller.bind_scan(e.scan));break;
    case ObservationKind::scanPlayback:static_cast<void>(controller.scan_playback(e.scan,e.playback,e.flag));break;
    }
}

std::unique_ptr<coo::script::MissionDocument> document;
std::uint64_t selectedRun{},nextPublication{};
coo::StallDiagnostics stalled;
std::uint32_t lastActive{UINT32_MAX},lastComplete{UINT32_MAX};
std::uint8_t lastSection{UINT8_MAX};
bool current() noexcept { return selectedRun && selectedRun==mission_run_generation() && mission_seed_armed() && world_phase()==WorldPhase::arrived; }
void log(std::string_view text) noexcept { core::log::write(core::log::Channel::server,core::log::Level::info,text); }
bool load() noexcept {
    static std::once_flag once;
    std::call_once(once,[] {
        std::string error;
        try {
            HMODULE module{};std::array<wchar_t,32768> path{};
            const bool found=GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&load),&module)!=FALSE;
            const auto size=found?GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size())):0;
            if(!size || size>=path.size()) { error="cannot resolve DLL-relative script path"; }
            else { document=coo::script::MissionDocument::read(std::filesystem::path(path.data()).parent_path()/L"Dawn"/L"scripts"/L"deep_storage.lua",kProfile,error); }
            if(document && !valid_document(document->views())) { document.reset();error="Deep Storage native profile mismatch"; }
        } catch(const std::exception& e) { error=e.what(); }
        std::array<char,768> line{};
        if(document) { std::snprintf(line.data(),line.size(),"ev=coo_script mission=deep_storage result=loaded format=lua fnv1a64=%016llX path=Dawn/scripts/deep_storage.lua reload=next_process",static_cast<unsigned long long>(document->fingerprint())); }
        else { std::snprintf(line.data(),line.size(),"ev=coo_script mission=deep_storage result=failed reason=\"%.*s\"",static_cast<int>((std::min)(error.size(),std::size_t{500})),error.data()); }
        log(line.data());
    });
    return document!=nullptr;
}
}
bool prepare(std::uint64_t run,bool selected) noexcept {
    const std::lock_guard lock(mutex);if(run!=mission_run_generation()) {return false;}
    if(!selected) {controller.reset();readinessSchedule.reset();observations.reset();poseInbox={};contestCount=0;selectedRun=nextPublication=0;stalled.reset();return false;}
    if(!load() || !controller.select(document->views(),run)) {return false;}
    if(selectedRun!=run) {readinessSchedule.reset();observations.reset();poseInbox={};contestCount=0;nextPublication=0;lastActive=lastComplete=UINT32_MAX;lastSection=UINT8_MAX;stalled.reset();}
    selectedRun=run;return true;
}
namespace {
void log_receipt(const char* stage,std::uint64_t run,std::uint32_t value) noexcept {
    std::array<char,192> line{};std::snprintf(line.data(),line.size(),"ev=deep_storage stage=%s run=%llu value=%u",stage,static_cast<unsigned long long>(run),value);log(line.data());
}

}
coo::ReadinessRequest<EnemyReceipt> readiness_request(std::uint64_t now) noexcept {
    const std::lock_guard lock(mutex);
    if(!current()) return {};
    return readinessSchedule.request<EnemyReceipt,256>(selectedRun,now,
        [&](auto visit) noexcept { controller.pending_enemies(visit); });
}
Frame snapshot(std::uint64_t run,std::uint64_t now,bool ready) noexcept {
    const std::lock_guard lock(mutex);if(!current() || run!=selectedRun) {return {};}
    if(!observations.drain(consume)) {log("ev=mission_observations result=overflow publication=stopped");return {};}
    for(auto& inbox:poseInbox) {inbox.drain([](const PlateReceipt& r,auto sample) {static_cast<void>(controller.plate_pose(r,sample));});}
    contestCount=0;
    const auto f=controller.update(run,now,ready);nextPublication=now+100;const auto d=controller.diagnostics();
    if(d.active!=lastActive || d.complete!=lastComplete || f.section!=lastSection) {
        lastActive=d.active;lastComplete=d.complete;lastSection=f.section;std::array<char,320> line{};
        std::snprintf(line.data(),line.size(),"ev=coo_executor mission=deep_storage run=%llu generation=%u section=%u phase=%u active=%08X complete=%08X failure=%u mission_finished=%u",
            static_cast<unsigned long long>(run),f.spawnGeneration,f.section,static_cast<unsigned>(d.phase),d.active,d.complete,static_cast<unsigned>(d.failure),f.finished?1U:0U);log(line.data());
    }
    if(const auto* g=controller.graph();g && d.phase==coo::Phase::running) for(const auto& b:g->commands) {
        if(controller.step_state(b.step).phase!=coo::StepPhase::active) {continue;}coo::StallReport report{};
        if(!stalled.observe({run,d.incarnation,b.step,b.command},controller.missing(g->definition.steps[b.step].commands[b.command]),now,report)) {continue;}
        std::array<char,384> line{};std::snprintf(line.data(),line.size(),"ev=coo_stall mission=deep_storage command=%.*s missing=%s registry=%08X slot=%u expected=%u actual=%u waiting_ms=%llu",
            static_cast<int>(b.id.size()),b.id.data(),coo::missing_name(report.detail.missing),report.detail.asset.registry,report.detail.asset.slot,report.detail.expected,report.detail.actual,static_cast<unsigned long long>(report.waitingMs));log(line.data());
    }return f;
}
Request request() noexcept {const std::lock_guard lock(mutex);return current()?Request{controller.owner(),controller.frame()}:Request{};}
std::uint64_t native_run() noexcept {const std::lock_guard lock(mutex);return current()?selectedRun:0;}
bool publication_due(std::uint64_t now) noexcept {
    const std::lock_guard lock(mutex);
    // The first snapshot enables the mission. Schedule it on arrival, before
    // position samples can fill the queue during the five-second idle interval.
    return current() && (nextPublication==0 || controller.frame().enabled) && now>=nextPublication;
}
void observe_position(float x,float y,float z) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::position;e.owner=controller.owner();e.point={x,y,z};observations.push(e);}}
void observe_submission(std::uint64_t run,std::uint32_t definition,std::int64_t offset,std::uint32_t bank,std::uint8_t row,std::uint32_t generation) noexcept {
    if(!dialogue_identity(definition,offset,bank,row)) {return;}const std::lock_guard lock(mutex);
    if(current() && controller.submitted(run,bank,row,generation,GetTickCount64())) {log_receipt("dialogue_submitted",run,row);}
}
void observe_prepared(coo::Generation owner,coo::Asset a) noexcept {const std::lock_guard lock(mutex);if(current()) {static_cast<void>(controller.prepared(owner,a));}}
void observe_object(const coo::ObjectReceipt& r) noexcept {const std::lock_guard lock(mutex);if(current()) {static_cast<void>(controller.object(r));}}
bool observe_admission(const EnemyReceipt& r) noexcept {const std::lock_guard lock(mutex);return current() && controller.admitted(r);}
bool observe_death(const EnemyReceipt& r) noexcept {const std::lock_guard lock(mutex);const bool ok=current() && controller.died(r);if(ok) {log_receipt("enemy_died",r.run,r.source);}return ok;}
void observe_readiness(const EnemyReceipt& r,coo::EnemyReadiness v) noexcept {const std::lock_guard lock(mutex);if(current()) {static_cast<void>(controller.readiness(r,v));}}
LivingEnemies living_enemies() noexcept {
    const std::lock_guard lock(mutex);LivingEnemies out{};if(!current() || !controller.frame().enabled) {return out;}
    out.owner=controller.owner();controller.living_enemies([&](const EnemyReceipt& r) {if(out.count<out.actors.size()) {out.actors[out.count++]=r;}});return out;
}
LensRequest lens_request() noexcept {const std::lock_guard lock(mutex);return current()?controller.lens_request():LensRequest{};}
void observe_lens(const LensReceipt& r,bool dead) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::lens;e.owner=controller.owner();e.lens=r;e.flag=dead;observations.push(e);}}
PlateRequest plate_request(std::size_t i) noexcept {const std::lock_guard lock(mutex);return current()?controller.plate_request(i):PlateRequest{};}
void observe_plate_pose(const PlateReceipt& r,server::runtime::activity::mission_device_pose::Sample sample) noexcept {
    const std::lock_guard lock(mutex);
    if(!current() || !r.valid() || controller.plate_request(r.index).plate!=r)return;
    poseInbox[r.index].submit(r,sample);
}
void observe_plate_binding(const PlateReceipt& r) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::plateBinding;e.owner=controller.owner();e.plate=r;observations.push(e);}}
void observe_plate(const PlateReceipt& r,std::uint32_t revision,float value,bool complete) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::plateProgress;e.owner=controller.owner();e.plate=r;e.revision=revision;e.progress=value;e.flag=complete;observations.push(e);}}
void observe_contested_positions(const PlateReceipt& r,std::span<const EnemyPosition> positions,bool complete) noexcept {
    const std::lock_guard lock(mutex);if(!current() || !r.valid() || positions.size()>256) {return;}
    if(contestCount==contests.size()) {observations.fail();return;}
    Observation queued{};queued.kind=ObservationKind::contest;queued.owner=controller.owner();queued.revision=static_cast<std::uint32_t>(contestCount);
    if(!observations.push(queued)) {return;}
    auto& event=contests[contestCount++];event.plate=r;event.count=positions.size();event.complete=complete;
    for(std::size_t i=0;i<positions.size();++i) {event.positions[i]=positions[i];}
}
ScanRequest scan_request(std::size_t i) noexcept {const std::lock_guard lock(mutex);return current()?controller.scan_request(i):ScanRequest{};}
void observe_scan_binding(const ScanReceipt& r) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::scanBinding;e.owner=controller.owner();e.scan=r;observations.push(e);}}
void observe_scan_playback(const ScanReceipt& r,ScanPlayback playback,bool participant) noexcept {const std::lock_guard lock(mutex);if(current()) {Observation e{};e.kind=ObservationKind::scanPlayback;e.owner=controller.owner();e.scan=r;e.playback=playback;e.flag=participant;observations.push(e);}}
}
