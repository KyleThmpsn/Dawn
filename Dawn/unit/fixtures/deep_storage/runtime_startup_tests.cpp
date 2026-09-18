// Exercise the production runtime with isolated world state and the shipped
// Lua script copied beside the test executable. No game process is touched.
#include "../../../src/state/activity/deep_storage/runtime.h"
#include "../../../src/state/activity/runtime.h"
#include "../../../src/core/logging/log.h"
#include <cstdio>
#include <cstdlib>

namespace {
namespace ds=dawn::state::activity::deep_storage;
namespace activity=dawn::state::activity;
std::uint64_t run=100;
activity::WorldPhase phase=activity::WorldPhase::transitioning;
bool armed{};
unsigned overflows{};
void check(bool ok,const char* message) {
    if(!ok) {std::fprintf(stderr,"FAIL: runtime startup: %s\n",message);std::exit(1);}
}
}

namespace dawn::state::activity {
std::uint64_t mission_run_generation() noexcept {return run;}
bool mission_seed_armed() noexcept {return armed;}
WorldPhase world_phase() noexcept {return phase;}
}
namespace dawn::core::log {
void write(Channel,Level,std::string_view event) noexcept {
    if(event.find("ev=mission_observations result=overflow")!=std::string_view::npos) ++overflows;
}
}

void deep_storage_runtime_startup_checks() {
    constexpr std::uint64_t arrival=74'609;
    check(!ds::publication_due(arrival),"unselected mission stays dormant");
    check(ds::prepare(run,true),"load the shipped Lua through the real runtime");
    check(!ds::publication_due(arrival),"loading cannot start mission authority");
    check(!ds::snapshot(run,arrival-1,true).enabled,"loading snapshot stays disabled");
    phase=activity::WorldPhase::arrived;
    check(!ds::publication_due(arrival),"arrival still requires the mission seed");
    armed=true;
    check(!ds::request().frame.enabled,"mission has not run before its first publication");
    check(ds::publication_due(arrival),"arrival schedules the first update before the idle keepalive");
    check(!ds::snapshot(run+1,arrival,true).enabled && ds::publication_due(arrival),
        "foreign snapshot cannot consume the pending startup");
    auto frame=ds::snapshot(run,arrival,true);
    check(frame.enabled && frame.presentation.active && frame.presentation.event==ds::kObjectives[0].event,
        "first update publishes the opening objective even before a position sample");
    const auto plateIndex=ds::asset_index(ds::kPlates[0].source);
    check(frame.native[plateIndex].managed && frame.native[plateIndex].desired,
        "first update requests the entrance plate");
    check(!ds::publication_due(arrival+99) && ds::publication_due(arrival+100),
        "startup enters the normal 100 ms schedule");
    ds::observe_prepared(ds::request().owner,ds::kPlates[0].source);

    // More than a full queue of physics observations arrives before the next
    // five-second idle keepalive. Only the real runtime's due signal drains it.
    unsigned publications{};
    for(std::uint64_t elapsed=5;elapsed<=5'000;elapsed+=5) {
        ds::observe_position(0.F,0.F,0.F);
        if(ds::publication_due(arrival+elapsed)) {
            frame=ds::snapshot(run,arrival+elapsed,true);++publications;
            check(frame.enabled && frame.presentation.active,"opening survives sustained position input");
        }
    }
    check(publications==50 && overflows==0,"scheduled updates drain all 1,000 samples without overflow");
    check(frame.native[plateIndex].active,"prepared entrance plate is published");
    check(ds::prepare(run,true) && !ds::publication_due(arrival+5'001),
        "repeated preparation preserves the current deadline");
    phase=activity::WorldPhase::transitioning;
    check(!ds::publication_due(arrival+6'000),"transition suspends mission publication");
    phase=activity::WorldPhase::arrived;
    ++run;
    check(!ds::publication_due(arrival+6'000),"old owner cannot publish into a new run");
    check(ds::prepare(run,true) && ds::publication_due(arrival+5'001),
        "new run starts immediately instead of inheriting the old deadline");
    frame=ds::snapshot(run,arrival+5'001,true);
    check(frame.enabled && frame.presentation.active,"new run publishes a fresh opening");

    // Keep the existing fail-closed receipt policy: scheduling must not hide
    // a genuine overflow or resurrect its dropped observations.
    for(unsigned i=0;i<257;++i) ds::observe_position(0.F,0.F,0.F);
    check(!ds::snapshot(run,arrival+5'101,true).enabled && overflows==1,
        "a genuine observation overflow still stops publication");
    check(!ds::snapshot(run,arrival+5'201,true).enabled && overflows==2,
        "the failed queue stays failed for the same owner");
    ++run;
    check(ds::prepare(run,true) && ds::publication_due(arrival+5'202),
        "new owner clears the failed queue and schedules startup");
    check(ds::snapshot(run,arrival+5'202,true).enabled,"new owner recovers from the previous overflow");
    check(!ds::prepare(run,false) && !ds::publication_due(arrival+6'000),
        "deselection stops scheduling");
    std::puts("PASS: Deep Storage runtime arrival, objective/plate startup, queue draining and owner reset");
}
