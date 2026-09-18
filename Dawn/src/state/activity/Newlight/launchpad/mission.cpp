#include "mission.h"
#include <bit>

namespace dawn::state::activity::newlight::launchpad {
namespace {
using namespace coo;
CommandSpec objective(unsigned row) {return {Operation::objective,kDirectiveAsset,kObjectives[row],Wait::requested};}
CommandSpec speech(unsigned row) {return {Operation::dialogue,kDialogueAsset,row,Wait::requested};}
CommandSpec spoken(unsigned row) {return {Operation::observation,kDialogueAsset,row,Wait::observed};}
CommandSpec visit(std::uint32_t key,std::uint16_t slot) {return {Operation::observation,volume(key,slot),0,Wait::observed};}
CommandSpec spawn(Cohort cohort) {return {Operation::population,kModule,static_cast<std::uint32_t>(cohort),Wait::requested};}
CommandSpec clear(Cohort cohort) {return {Operation::observation,kModule,0x100U+static_cast<std::uint32_t>(cohort),Wait::observed};}
CommandSpec control(Mechanic what) {return {Operation::mechanic,kModule,static_cast<std::uint32_t>(what),Wait::requested};}
CommandSpec object(std::uint32_t key,std::uint16_t slot,bool on=true) {return {Operation::device,asset(key,4,slot),on?1U:0U,Wait::requested};}
CommandSpec device(std::uint32_t key,std::uint16_t slot,float value) {return {Operation::device,asset(key,23,slot),std::bit_cast<std::uint32_t>(value),Wait::requested};}
CommandSpec sequence(unsigned slot) {return {Operation::device,asset(kBreach,5,static_cast<std::uint16_t>(slot)),1,Wait::requested};}
CommandSpec arm(coo::Asset a) {return {Operation::mechanic,a,static_cast<std::uint32_t>(Mechanic::arm),Wait::requested};}
CommandSpec used(coo::Asset a) {return {Operation::observation,a,static_cast<std::uint32_t>(Event::used),Wait::observed};}
CommandSpec granted(coo::Asset a) {return {Operation::observation,a,static_cast<std::uint32_t>(Event::granted),Wait::observed};}
}
void Graph::name(std::string_view name) noexcept {definition={name,coo::Schema::otherMissions,{},{}};first_=storage_->stepCount;}
std::uint32_t Graph::add(std::string_view name,std::uint32_t dependencies,std::initializer_list<coo::CommandSpec> commands) noexcept {
    const auto count=definition.steps.size();
    if(count>=coo::Executor::kMaxSteps || !commands.size() || commands.size()>coo::Executor::kMaxCommands
        || storage_->stepCount!=first_+count || storage_->stepCount>=storage_->steps.size()
        || commands.size()>storage_->commands.size()-storage_->commandCount) {definition.schema=coo::Schema::unspecified;return 0;}
    auto out=std::span(storage_->commands).subspan(storage_->commandCount,commands.size());
    std::copy(commands.begin(),commands.end(),out.begin());storage_->commandCount+=commands.size();
    storage_->steps[storage_->stepCount++]={name,dependencies,out};definition.steps=std::span(storage_->steps).subspan(first_,count+1);return 1U<<count;
}
Mission::Mission() noexcept {
    for(auto& g:phases) {g.storage_=&storage;}
    auto& exterior=phases[0];exterior.name("New Light / exterior");
    auto a=exterior.add("resurrected",0,{objective(1),speech(0)});
    auto b=exterior.add("inside the wall",a,{visit(kExterior,11)});
    exterior.add("the Breach",b,{control(Mechanic::next)});

    auto& lights=phases[1];lights.name("New Light / find a weapon");
    // Device 75 drives both overlapping world shutters. Retain its authored
    // logical source, but keep the separate doors overlay inactive.
    a=lights.add("Breach preparation",0,{objective(2),speech(6),object(kBreach,2),object(kBreach,3,false),device(kBreach,75,0.F)});
    lights.add("Breach details",a,{object(kBreach,1),object(kBreach,7),object(kBreach,8),object(kBreach,9)});
    b=lights.add("Fallen above",a,{visit(kBreachRoute,31)});
    lights.add("overhead movement",b,{spawn(Cohort::nests),speech(7),sequence(76)});
    auto c=lights.add("dark room",a,{visit(kBreach,134)});
    auto d=lights.add("restore light",c,{object(kBreach,0),control(Mechanic::ghostLights),speech(8),speech(9)});
    auto e=lights.add("Ghost reaches the lights",d,{{Operation::observation,kModule,static_cast<std::uint32_t>(Event::ghostAtLights),Wait::observed}});
    auto lit=lights.add("lights reveal the Fallen",e,{control(Mechanic::light),
        {Operation::observation,kModule,static_cast<std::uint32_t>(Event::lightsOn),Wait::observed},spawn(Cohort::nests),speech(10)});
    auto returned=lights.add("Ghost returns to the shutter",lit,{{Operation::observation,kModule,static_cast<std::uint32_t>(Event::ghostLightsComplete),Wait::observed}});
    auto f=lights.add("open rifle shutter",returned,{control(Mechanic::shutter)});
    auto approach=lights.add("approach the rifle",f,{visit(kBreach,135)});
    lights.add("Ghost points out rifle",approach,{control(Mechanic::ghostRifle)});
    auto g=lights.add("rifle available",f,{objective(3),speech(11),object(kBreach,4),arm(kRifle)});
    auto h=lights.add("accepted rifle pickup",g,{used(kRifle)});
    auto i=lights.add("grant Khvostov",h,{control(Mechanic::rifle)});
    auto j=lights.add("inventory acknowledges rifle",i,{granted(kRifle)});
    lights.add("armed",j,{control(Mechanic::ghostDismiss),object(kBreach,0,false),object(kBreach,4,false),object(kBreach,5,false),control(Mechanic::next)});

    auto& breach=phases[2];breach.name("New Light / through the Breach");
    a=breach.add("armed corridor",0,{objective(4),speech(12),arm(kCache)});
    b=breach.add("first Vandal reveal approach",a,{visit(kBreach,136),
        {Operation::observation,kModule,static_cast<std::uint32_t>(Event::firstVandalNear),Wait::observed}});
    breach.add("first Vandal",b,{spawn(Cohort::firstVandal),speech(13)});
    c=breach.add("wall ambush",a,{visit(kBreach,138)});
    // Populate the next room around its approach corner. Only the wall/ceiling
    // performers wait for their own close crossings.
    breach.add("first fight",c,{spawn(Cohort::firstFight),spawn(Cohort::arena),speech(14)});
    auto hallDrop=breach.add("hall drop approach",a,{visit(kBreach,139)});
    breach.add("hall drop ambush",hallDrop,{spawn(Cohort::hallDrop)});
    d=breach.add("arena approach",a,{visit(kBreach,137)});
    breach.add("arena defenders",d,{spawn(Cohort::arena),spawn(Cohort::cache),spawn(Cohort::corridorDefender)});
    e=breach.add("loot cache approach",a,{visit(kBreachRoute,32)});
    breach.add("loot cache",e,{spawn(Cohort::cache),speech(15)});
    f=breach.add("accepted cache use",a,{used(kCache)});
    g=breach.add("cache reward",f,{control(Mechanic::shotgun)});
    h=breach.add("cache item delivered",g,{granted(kCache)});
    breach.add("special weapon",h,{speech(17)});
    i=breach.add("corridor approach",a,{visit(kBreach,140)});
    breach.add("corridor attackers",i,{spawn(Cohort::corridor),spawn(Cohort::corridorRear),speech(24)});
    auto melee=breach.add("second corridor ambush",a,{visit(kBreach,143)});
    breach.add("ceiling and left wall attackers",melee,{spawn(Cohort::corridorMelee)});
    auto rear=breach.add("rear corridor approach",a,{visit(kBreach,172)});
    breach.add("rear corridor defenders",rear,{spawn(Cohort::corridorRear),spawn(Cohort::garageDefender)});
    // Travelling west, final_room_battle (x461) precedes vandal_spawn (x447).
    // The old ordering admitted the ceiling cast after entering its room.
    j=breach.add("garage approach",a,{visit(kBreach,129)});
    breach.add("garage attackers",j,{spawn(Cohort::garage)});
    auto k=breach.add("final interior room",a,{visit(kBreach,129)});
    breach.add("interior exit defenders",k,{spawn(Cohort::garageExit)});
    auto garageFallback=breach.add("garage interior",a,{visit(kBreach,128)});
    breach.add("garage approach bypass",garageFallback,{spawn(Cohort::garage),spawn(Cohort::garageDefender)});
    // No locked exit: a speedrunner may leave these encounters alive.
    auto l=breach.add("leave the wall",a,{visit(kBreachRoute,33)});
    breach.add("the Divide",l,{control(Mechanic::next)});

    auto& divide=phases[3];divide.name("New Light / the Divide");
    a=divide.add("Divide approach",0,{speech(26),object(kKetch,1)});
    b=divide.add("Ketch reveal",a,{visit(kKetch,5)});
    // The outdoor crossing starts the reveal and combat together. Native
    // presentation receipts must never delay admission of the raiding party.
    divide.add("Ketch arrives",b,{object(kKetch,2),control(Mechanic::ketch),speech(27)});
    e=divide.add("repel the raiding party",b,{control(Mechanic::assault),objective(5),speech(28),spawn(Cohort::raiders)});
    f=divide.add("assault repelled",e,{{Operation::observation,kModule,static_cast<std::uint32_t>(Event::assaultComplete),Wait::observed}});
    // Walker and replacement infantry contribute to the same persistent total.
    g=divide.add("follow ship signal",f,{control(Mechanic::scanShip),objective(6),speech(32)});
    h=divide.add("Dock13 approach",g,{visit(kDivideRoute,4)});
    divide.add("enter Dock13",h,{spawn(Cohort::hangar),spawn(Cohort::backup),control(Mechanic::next)});

    auto& hangar=phases[4];hangar.name("New Light / Dock13");
    a=hangar.add("jumpship room",0,{objective(7),speech(36),object(kHangar,1),object(kHangar,2),object(kHangar,3),object(kHangar,4),object(kHangar,5),arm(kPowerCache)});
    hangar.add("ship defenders",a,{spawn(Cohort::hangar),spawn(Cohort::backup)});
    b=hangar.add("power cache accepted",a,{used(kPowerCache)});
    c=hangar.add("power reward",b,{control(Mechanic::rocket)});
    d=hangar.add("power weapon delivered",c,{granted(kPowerCache)});
    hangar.add("rocket launcher",d,{speech(38)});
    e=hangar.add("ship reveal",a,{visit(kHangar,21)});
    auto fight=hangar.add("clear the ship",e,{objective(8),speech(39)});
    f=hangar.add("ship defenders defeated",fight,{clear(Cohort::hangar),clear(Cohort::backup)});
    g=hangar.add("approach jumpship",f,{objective(0),speech(40)});
    h=hangar.add("Ghost prepares the ship",g,{spoken(40)});
    i=hangar.add("pickup point",h,{visit(kHangarRoute,9)});
    hangar.add("escape",i,{control(Mechanic::finish)});
}
bool Mission::valid() const noexcept {
    for(const auto& g:phases) {
        if(!coo::Executor::valid(g.definition)) {return false;}
        for(const auto& step:g.definition.steps) for(const auto& c:step.commands) {
            if(!c.asset.registry || (c.operation==coo::Operation::dialogue && (c.argument>=std::size(kDialogue) || !kDialogue[c.argument].durationMs))) {return false;}
        }
    }
    return true;
}
const Mission& mission() noexcept {static const Mission value;return value;}
}
