#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "middleware/encoding/bit_reader.h"
#include "server/bap/encrypted/push/activity/native_roster_lifetime_projection.h"
#include "server/bap/encrypted/push/activity/omega_opening_publication.h"
#include "server/bap/encrypted/push/activity/omega_lair_roster.h"
#include <memory>

namespace omega_loading_cases {
inline void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "Omega loading: %s\n", why); std::exit(1); }
}
template<class T> T read(std::ifstream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof value);
    check(bool(input), "complete cache-derived fixture");
    return value;
}
inline std::uint64_t field(bits::Reader& reader, std::uint8_t width) {
    std::uint64_t value{};
    check(reader.read(width, value), "complete packet field");
    return value;
}
inline void roster_list(bits::Reader& reader, std::span<const std::uint32_t> keys,
                        std::uint8_t countWidth, std::size_t maskWords) {
    check(field(reader, 1) == 1 && field(reader, 1) == 1, "key array present");
    check(field(reader, countWidth) == keys.size(), "full key count");
    for (auto key : keys) check(field(reader, 32) == key, "registry order preserved");
    check(field(reader, 1) == 1, "presence array present");
    for (std::size_t word = 0; word < maskWords; ++word) {
        std::uint32_t expected{};
        for (std::size_t bit = 0; bit < 32 && word * 32 + bit < keys.size(); ++bit)
            expected |= std::uint32_t{1} << bit;
        check(field(reader, 32) == expected, "every registered group present");
    }
    check(field(reader, 1) == 1 && field(reader, countWidth) == keys.size(), "full state count");
    for ([[maybe_unused]] auto key : keys) check(field(reader, 8) == 0x81, "stable roster state");
}
inline void transition_cases(const wire::Roster& lighthouse) {
    namespace life = dawn::server::bap::encrypted::push::activity::roster_lifetime;
    namespace lair = dawn::server::bap::encrypted::push::activity::omega_lair;
    check(lair::roster_region(121,true)==120,"bookend preserves Lighthouse base service roster");
    for(int region:{88,112,120,121,128}) {
        check(lair::roster_region(region,false)==region,"ordinary region is unchanged");
        if(region!=121)check(lair::roster_region(region,true)==region,"ending only normalizes slice121");
    }
    constexpr life::Identity identity{123,1,UINT64_MAX,UINT64_MAX,lair::kScenarioTag};
    auto retained = std::make_unique<life::State>();
    check(life::prepare({},identity,15,0x83,true,lighthouse,*retained)==life::Result::ready,
        "adopt complete Lighthouse lifetime");
    const auto initial = std::make_unique<life::State>(*retained);
    auto forest=lighthouse;
    // Installed cache at region 88 omits precisely these three Lighthouse groups.
    std::size_t count{};
    for(std::size_t i=0;i<lighthouse.groupCount;++i) {
        const auto& group=lighthouse.groups[i];
        if(group.key==0xBA5F26EF || group.key==0xD00142CF || group.key==0xF7A6CE7F)continue;
        forest.groups[count++]=group;
    }
    forest.groupCount=count;
    std::array<wire::BubbleSubBlock,64> desiredBlocks{},projectedBlocks{};
    count=0;
    for(const auto& block:lighthouse.bubbleSubBlocks)
        if(block.bubble!=15)desiredBlocks[count++]=block;
    forest.bubbleSubBlocks=std::span(desiredBlocks).first(count);
    check(forest.groupCount==13 && count==4,"cache-derived Forest view");
    for(unsigned region:{88U,120U,88U,112U}) {
        auto desired=region==120?lighthouse:forest;
        check(life::prepare(*retained,identity,region/8,0x84,false,desired,*retained)==life::Result::ready,
            "ordinary Omega travel retains lifetime");
        check(life::project(*retained,desired,projectedBlocks),"project retained travel roster");
        check(retained->top.keys==initial->top.keys && retained->top.states==initial->top.states
            && retained->blockCount==initial->blockCount,"no global recreation or bubble compaction");
        for(std::size_t b=0;b<initial->blockCount;++b)
            check(retained->blocks[b].bubble==initial->blocks[b].bubble
                && retained->blocks[b].entries.keys==initial->blocks[b].entries.keys
                && retained->blocks[b].entries.states==initial->blocks[b].entries.states,
                "every old native key retains its original generation and ordinal");
    }
    struct Storage {
        std::array<dawn::state::build_data::scenarios::RosterGroup,4> rosterGroups{};
        std::array<wire::BubbleSubBlock,64> rosterSubBlocks{};
        std::array<std::array<std::uint32_t,wire::kBubbleKeyCapacity>,64> rosterSubBlockKeys{};
    };
    auto storage=std::make_unique<Storage>();
    auto ending=forest;
    check(life::project(*retained,ending,storage->rosterSubBlocks),"project before explicit ending retirement");
    check(lair::terminal_roster(*storage,ending,1),"ending still retires the retained roster");
    check(ending.groupCount==4 && ending.topLevelStates[0]==0x83,"ending preserves global generation");
    for(std::size_t b=0;b<initial->blockCount;++b) {
        check(ending.bubbleSubBlocks[b].bubble==initial->blocks[b].bubble,"ending uses retained bubble ordinal");
        const auto& block=ending.bubbleSubBlocks[b];
        for(std::size_t k=0;k<block.keys.size();++k)
            check(block.presence[k]==unsigned(block.keys[k]==0x3A6CE17A),"explicit retirement keeps only bookend");
    }
    // The production finalizer copies its lifetime value after preparing the
    // overlay. This second binding used to resurrect every retired group.
    auto detached=std::make_unique<life::State>(*retained);
    wire::Snapshot final{};final.roster=ending;final.omegaEndingRetire=true;
    for(unsigned refresh=0;refresh<3;++refresh) {
        check(lair::finalize_retained_roster(*storage,final,*detached),"final retirement publication succeeds");
        check(final.roster.groupCount==4 && final.roster.topLevelStates.data()==detached->top.states.data(),
            "copied lifetime owns final global metadata");
        unsigned movies{};
        for(const auto& block:final.roster.bubbleSubBlocks) {
            for(std::size_t k=0;k<block.keys.size();++k) {
                const bool movie=block.keys[k]==0x3A6CE17A;
                movies+=movie;
                check(block.presence[k]==unsigned(movie),"final rebind cannot restore retired presence");
                if(block.bubble==14)check(block.states[k]==0x83,"Lair removal preserves native generation");
            }
        }
        check(movies==1,"bookend appended once across refreshes");
        check(life::prepare(*detached,identity,11,0x84,false,forest,*detached)==life::Result::ready,
            "following refresh retains an admissible lifetime mirror");
    }
}
inline void ending_cases(wire::Snapshot s, std::span<std::byte> output) {
    namespace lair=dawn::server::bap::encrypted::push::activity::omega_lair;
    namespace ending=dawn::state::activity::omega_ending;
    struct Storage {
        std::array<dawn::state::build_data::scenarios::RosterGroup,4> rosterGroups{};
        std::array<wire::BubbleSubBlock,64> rosterSubBlocks{};
        std::array<std::array<std::uint32_t,wire::kBubbleKeyCapacity>,64> rosterSubBlockKeys{};
    };
    auto storage=std::make_unique<Storage>();
    check(lair::terminal_roster(*storage,s.roster,ending::kState),"bookend roster ready");
    s.phaseOneOnly=false;s.preserveMissionAuthorityState=true;
    s.omegaOpeningStage=wire::kOmegaOpeningStageNone;
    s.omegaSceneAuthority=true;s.omegaEndingRetire=true;
    s.omegaEndingState=ending::kState;s.region=ending::kSlice;
    // Return travel creates a fresh sync pool even though the global mission
    // components retain their authority. Native 4D6530 blocks the movie until
    // every root sync record is initialized; an absent auth body is sufficient.
    constexpr std::array<std::uint8_t,21> runtimeTypes{
        16,35,18,17,41,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13};
    s.omegaEndingRevision=105;
    for(bool preserved:{false,true}) for(bool seeded:{false,true})
    for(unsigned phase:{0U,1U,2U}) {
        const bool playing=phase!=2;
        const bool offered=phase==0;
        s.preserveMissionAuthorityState=preserved;
        s.seedAuthoredSensors=seeded;s.omegaEndingPlay=playing;
        s.omegaEndingSeedRuntime=wire::ending_runtime_seed_required(true,true,playing,!offered,false);
        s.omegaCrownRestriction=dawn::state::activity::omega_crown_respawn::Restriction::disable;
        std::size_t written{};
        check(wire::encode_sensor_auth_update(s,output,written),"ending packet encodes after bootstrap");
        bits::Reader reader(output.first(written));
        check(reader.skip(wire::kLatchBitWithoutGrant+1
            +wire::delta_bits(wire::top_level_key_count(s.roster),s.roster.bubbleSubBlocks)),"ending framing");
        unsigned movies{},runtimeValues{},runtimeSeeds{};
        std::array<bool,21> initialized{};
        while(field(reader,1)) {
            const auto key=field(reader,32);
            check(field(reader,32)==0,"ending registry framing");
            while(field(reader,1)) {
                check(field(reader,32)==key,"ending object registry");
                const auto type=field(reader,7)-wire::kSlotTypeBias;
                const auto slot=field(reader,16)-wire::kSlotIndexBias;
                const auto length=field(reader,32);
                const auto before=reader.remaining_bits();
                if(key==ending::kRegistry && type==6 && slot==ending::kSlot) {
                    ++movies;
                    check(length>3 && field(reader,1)==1 && field(reader,1)==1,
                        "ending play/stop body survives startup seeding disabled");
                    check(reader.skip(128) && field(reader,32)==105 && field(reader,1)==unsigned(playing),
                        "movie receives exact controller revision and play/stop state");
                }
                if(key==0x4786C0E0 && slot<runtimeTypes.size()) {
                    if(type!=runtimeTypes[slot])std::fprintf(stderr,"ending root slot=%llu type=%llu expected=%u phase=%u preserved=%u\n",
                        static_cast<unsigned long long>(slot),static_cast<unsigned long long>(type),runtimeTypes[slot],phase,preserved?1U:0U);
                    check(type==runtimeTypes[slot],"ending initializes exact native root descriptor");
                    check(!initialized[slot],"ending root initializer is not duplicated");
                    initialized[slot]=true;
                    const auto reset=field(reader,1),present=field(reader,1);
                    if(slot!=1 && slot!=3) {
                        ++runtimeSeeds;
                        runtimeValues+=static_cast<unsigned>(present);
                        check(reset==1 && present==0,"ending initializes sync without replacing retained authority");
                        check(length==(slot<4?2U:3U),"runtime initialization has no script/player values");
                        if(slot>=4)check(field(reader,1)==0,"runtime initialization has no fabricated sense receipt");
                    }
                }
                const auto consumed=before-reader.remaining_bits();
                check(consumed<=length && reader.skip(static_cast<std::size_t>(length-consumed)),"ending object complete");
            }
        }
        check(field(reader,1)==0 && reader.remaining_bits()<8 && movies==1,"exactly one complete movie command");
        check(runtimeValues==0,"ending never replaces retained mission script or participation values");
        check(runtimeSeeds==(offered?19U:0U),"ending offer initializes the complete native sync pool even with retained authority");
        if(offered)for(bool present:initialized)check(present,"all 21 native root records initialized before movie apply");
    }
    s.seedAuthoredSensors=false;s.omegaSceneAuthority=false;
    check(wire::auth_body_bits(s,ending::kRegistry,6,ending::kSlot,false)==0,"ending requires Omega authority");
    s.omegaSceneAuthority=true;s.omegaEndingRetire=false;s.omegaEndingState=0;
    check(wire::auth_body_bits(s,ending::kRegistry,6,ending::kSlot,false)==0,"ordinary Omega cannot play ending");
    s.omegaEndingState=ending::kState;
    check(wire::auth_body_bits(s,ending::kRegistry+1,6,ending::kSlot,false)==0,"ending requires exact registry");
}
inline void arrival_cases() {
    namespace opening=dawn::server::bap::encrypted::push::activity::omega_opening_publication;
    for(int region:{88,112,120}) for(bool eligible:{false,true}) for(bool entrance:{false,true}) {
        wire::Snapshot s{};s.omegaSceneAuthority=true;std::uint8_t stage{};
        const bool expected=region==120 && eligible && !entrance;
        check(opening::bootstrap(s,stage,eligible,region,entrance)==expected,"arrival scope gate");
        check(s.initializeMissionAuthorityRuntime==expected,"only admitted Lighthouse arrival starts Ikora");
        if(!expected)continue;
        check(!s.omegaIkoraPortalRequested,"appearance does not synthesize approach");
        s={};s.omegaSceneAuthority=true;
        check(opening::bootstrap(s,stage,true,120,false) && stage==wire::kOmegaOpeningStageScene,
            "arrival advances to actor scene");
        check(opening::bootstrap(s,stage,true,120,false) && stage==wire::kOmegaOpeningStageReady,
            "arrival completes scene bootstrap");
        check(!opening::bootstrap(s,stage,true,120,false),"repeated arrival cannot restart scene");
    }
}
inline void rescue_cases(wire::Snapshot s, std::span<std::byte> output) {
    namespace rescue=dawn::state::activity::omega_rescue_npc;
    // Live 2026-09-18: scene9 was requested at generation4 in Crown, but its
    // native generation stayed zero because the opening-only seed flag was off.
    s.seedAuthoredSensors=false;
    s.omegaOpeningStage=wire::kOmegaOpeningStageNone;
    s.omegaFirstLairGeneration=1;
    s.omegaCrownGeneration=1;
    s.omegaCrownCycle=1;
    for(bool preserve:{false,true}) for(bool restricted:{false,true})
    for(unsigned phase:{0U,1U,2U}) for(bool markersReady:{false,true}) {
        s.preserveMissionAuthorityState=preserve;
        s.omegaCrownRestricted=restricted;
        s.omegaRescueSourcesGeneration=phase==0?0U:1U;
        s.omegaRescueMarkerReadyMask=markersReady?0x1FFU:0U;
        s.omegaRescueScenes={};
        if(phase!=0) {
            s.omegaRescueScenes[0].generation=4; // scene9: Osiris rescue
            s.omegaRescueScenes[9].generation=5; // scene83: Echo with marker dependencies
        }
        if(phase==2) {
            s.omegaRescueScenes[0].eventCount=1;
            s.omegaRescueScenes[0].events[0]=rescue::kReleaseFirstEye;
            s.omegaRescueScenes[0].stop=true;
        }
        std::size_t written{};
        check(wire::encode_sensor_auth_update(s,output,written),"post-opening rescue packet encodes");
        bits::Reader reader(output.first(written));
        check(reader.skip(wire::kLatchBitWithoutGrant+1
            +wire::delta_bits(wire::top_level_key_count(s.roster),s.roster.bubbleSubBlocks)),"rescue framing");
        unsigned scenes{},sources{},markers{},runtimeBodies{};
        while(field(reader,1)) {
            const auto key=field(reader,32);
            check(field(reader,32)==0,"rescue group framing");
            while(field(reader,1)) {
                check(field(reader,32)==key,"rescue registry identity");
                const auto type=field(reader,7)-wire::kSlotTypeBias;
                const auto index=field(reader,16)-wire::kSlotIndexBias;
                const auto length=field(reader,32);
                const auto begin=reader.remaining_bits();
                if(key==rescue::kRegistry && length>3) {
                    if(type==43 && (index==9 || index==83)) {
                        ++scenes;
                        check(phase!=0 && field(reader,1)==1 && field(reader,1)==1,"rescue authority present");
                        if(index==9) {
                            check(sources==5,"Osiris cast precedes its scene");
                            check(field(reader,32)==0x80000004U && field(reader,1)==unsigned(phase==2)
                                && field(reader,4)==5,"requested Osiris generation, stop and cast survive bootstrap");
                            for(auto source:{0U,5U,6U,7U,8U})
                                check(field(reader,32)==rescue::kRegistry && field(reader,7)==2
                                    && field(reader,16)==32768U+source,"exact Osiris cast binding");
                            check(field(reader,31)==1 && field(reader,6)==unsigned(phase==2),"retained Osiris event count");
                            if(phase==2)check(field(reader,32)==rescue::kReleaseFirstEye,"retained eye release event");
                        } else {
                            check(field(reader,32)==(markersReady?0x80000005U:0x7FFFFFFFU),
                                "Echo still waits for its native marker entities");
                        }
                    } else if(type==1 && (index==0 || (index>=5 && index<=8))) {
                        ++sources;
                        check(phase!=0 && field(reader,1)==1 && field(reader,1)==1,"cast authority present");
                        check(reader.skip(121) && field(reader,32)==0x80000001U,"Osiris cast is requested");
                        check(reader.skip(20) && field(reader,31)==1,"cast generation stays stable across scene events");
                    } else if(type==4 && index>=101 && index<=103) {
                        ++markers;
                        check(phase!=0 && field(reader,1)==1 && field(reader,1)==1,"Echo marker authority present");
                    }
                }
                // Crown intentionally updates its one restriction director.
                if((type==13 || type==18 || type==35) && length>3
                    && !(restricted && key==0x4786C0E0 && type==35 && index==1))++runtimeBodies;
                const auto consumed=begin-reader.remaining_bits();
                check(consumed<=length && reader.skip(static_cast<std::size_t>(length-consumed)),"complete rescue object");
            }
        }
        check(field(reader,1)==0 && reader.remaining_bits()<8,"complete rescue packet");
        check(scenes==(phase==0?0U:2U) && sources==(phase==0?0U:5U)
            && markers==(phase==0?0U:3U),"post-opening packet delivers Osiris and Echo dependencies exactly once");
        check(!preserve || runtimeBodies==0,"rescue update preserves native mission state");
    }
    check(wire::auth_body_bits(s,rescue::kRegistry+1U,43,9,false)==0,"rescue authority requires exact registry");
    s.omegaSceneAuthority=false;
    check(wire::auth_body_bits(s,rescue::kRegistry,43,9,false)==0,"rescue authority requires Omega scene ownership");
}
inline void run() {
    arrival_cases();
    // Reconstructed from the failing 2026-09-18 installed cache at region 120,
    // using fill_roster and all seven production Lair admissions. No player data.
    std::ifstream input(std::filesystem::path(__FILE__).parent_path()
        / "fixtures/omega_loading_roster.bin", std::ios::binary);
    wire::Snapshot s{};
    s.roster.groupCount = read<std::uint32_t>(input);
    s.roster.topLevelGroupCount = read<std::uint32_t>(input);
    s.roster.playerKeyGroup = read<std::uint32_t>(input);
    check(s.roster.groupCount == 16 && s.roster.topLevelGroupCount == 3, "failed launch shape");
    static std::array<std::array<std::uint8_t, 1280>, 16> types{}, flags{};
    static std::array<std::array<std::uint16_t, 1280>, 16> indices{};
    for (std::size_t i = 0; i < s.roster.groupCount; ++i) {
        const auto key = read<std::uint32_t>(input);
        const auto count = read<std::uint16_t>(input);
        check(count > 0 && count <= types[i].size(), "bounded descriptors");
        for (std::size_t j = 0; j < count; ++j) {
            types[i][j] = read<std::uint8_t>(input);
            flags[i][j] = read<std::uint8_t>(input);
            indices[i][j] = read<std::uint16_t>(input);
        }
        s.roster.groups[i] = {key, std::span(types[i]).first(count),
            std::span(flags[i]).first(count), std::span(indices[i]).first(count)};
    }
    std::array<wire::BubbleSubBlock, 64> blocks{};
    std::array<std::array<std::uint32_t, wire::kBubbleKeyCapacity>, 64> keys{};
    const auto blockCount = read<std::uint32_t>(input);
    check(blockCount <= blocks.size(), "bounded bubble count");
    for (std::size_t i = 0; i < blockCount; ++i) {
        const auto bubble = read<std::uint32_t>(input);
        const auto count = read<std::uint32_t>(input);
        check(count <= keys[i].size(), "bounded bubble keys");
        for (std::size_t j = 0; j < count; ++j) keys[i][j] = read<std::uint32_t>(input);
        blocks[i] = {bubble, std::span(keys[i]).first(count)};
    }
    s.roster.bubbleSubBlocks = std::span(blocks).first(blockCount);
    transition_cases(s.roster);
    s.archiveOmega = true;
    s.lifetime = 3; s.stateSequence = 1; s.playerKey = 123;
    s.hasRegion = true; s.region = 120;
    s.hasSpawnOverride = true; s.spawnSliceSet = 120; s.spawnSetHash = 0x4AB3287A;
    s.omegaSceneAuthority = true;
    static std::array<std::byte, 256 * 1024> output{};
    std::array<std::uint32_t, 3> top{};
    for (std::size_t i = 0; i < top.size(); ++i) top[i] = s.roster.groups[i].key;
    ending_cases(s,output);
    for (unsigned mode = 0; mode < 3; ++mode) {
        s.phaseOneOnly = mode == 1;
        s.preserveMissionAuthorityState = mode == 2;
        std::size_t written{};
        check(wire::encode_sensor_auth_update(s, output, written), "complete sixteen-group Omega packet encodes");
        bits::Reader reader(std::span(output).first(written));
        check(reader.skip(wire::kLatchBitWithoutGrant), "initial header");
        check(field(reader, 1) == 1 && field(reader, 1) == 1, "roster enabled");
        roster_list(reader, top, 9, 8);
        check(field(reader, 1) == 1 && field(reader, 7) == blockCount, "all bubble scopes present");
        for (const auto& block : s.roster.bubbleSubBlocks) {
            check(field(reader, 1) == 1 && field(reader, 32) == 0x80000000ULL + block.bubble,
                "bubble ownership preserved");
            roster_list(reader, block.keys, 7, 3);
        }
        std::array<bool, 4> combatSeen{};
        while (field(reader, 1)) {
            const auto key = field(reader, 32);
            check(field(reader, 32) == 0, "group framing");
            for (std::size_t i = 0; i < combatSeen.size(); ++i)
                combatSeen[i] = combatSeen[i] || key == s.roster.groups[12 + i].key;
            while (field(reader, 1)) {
                check(field(reader, 32) == key && reader.skip(7 + 16), "object identity");
                const auto length = field(reader, 32);
                check(reader.skip(static_cast<std::size_t>(length)), "complete authority body");
            }
        }
        check(field(reader, 1) == 0 && reader.remaining_bits() < 8, "complete packet terminator");
        for (bool seen : combatSeen) check(seen == !s.phaseOneOnly, "all four combat groups retained");
        std::printf("Omega loading mode=%u groups=16 bytes=%zu\n", mode, written);
    }
    for(auto stage:{wire::kOmegaOpeningStageScene,wire::kOmegaOpeningStageReady}) {
        s.omegaOpeningStage=stage;s.seedAuthoredSensors=true;s.phaseOneOnly=false;
        s.preserveMissionAuthorityState=true;
        s.omegaIkoraPortalRequested=false;
        std::size_t written{};
        check(wire::encode_sensor_auth_update(s,output,written),"arrival scene packet encodes");
        bits::Reader reader(std::span(output).first(written));
        check(reader.skip(wire::kLatchBitWithoutGrant+1
            +wire::delta_bits(wire::top_level_key_count(s.roster),s.roster.bubbleSubBlocks)),"arrival framing");
        unsigned sources{},scenes{};
        while(field(reader,1)) {
            const auto key=field(reader,32);
            check(field(reader,32)==0,"arrival group framing");
            while(field(reader,1)) {
                check(field(reader,32)==key,"arrival object registry");
                const auto type=field(reader,7)-wire::kSlotTypeBias;
                const auto index=field(reader,16)-wire::kSlotIndexBias;
                const auto length=field(reader,32);
                if(key==0xD00142CF && type==43 && index==1 && length>3) {
                    ++scenes;
                    check(sources==1 && field(reader,1)==1 && field(reader,1)==1,"Ikora source precedes scene");
                    check(field(reader,32)==0x80EC0F96 && reader.skip(91)
                        && field(reader,6)==0,"arrival keeps generation stable with no approach event");
                    check(reader.skip(static_cast<std::size_t>(length)-131),"arrival scene remainder");
                } else {
                    if(key==0xD00142CF && type==1 && index==0 && length>3)++sources;
                    check(reader.skip(static_cast<std::size_t>(length)),"arrival object body");
                }
            }
        }
        check(field(reader,1)==0 && reader.remaining_bits()<8 && sources==1 && scenes==1,
            "arrival publishes exactly one Ikora source and scene before approach");
    }
    s.omegaOpeningStage=wire::kOmegaOpeningStageNone;
    // Live 2026-09-18 failure: native scene revision 3 released the gate after
    // bootstrap had finished, with authored seeding disabled and runtime owned
    // by the client. Decode the complete packet, not just an isolated gate body.
    s.phaseOneOnly = false;
    s.seedAuthoredSensors = false;
    s.omegaIkoraPortalRequested = true;
    s.omegaPortalEntry = true;
    s.omegaDialogueArm = true;
    s.omegaObjectiveEvent = 0x1EBF4621U;
    s.omegaForestGenerator = true;
    s.omegaForestSeed = 12345;
    for (bool preserve : {false, true}) for (bool released : {false, true}) {
        s.preserveMissionAuthorityState = preserve;
        s.omegaIkoraLatticeReleased = released;
        s.omegaPortalEntry = released;
        std::size_t written{};
        check(wire::encode_sensor_auth_update(s, output, written), "post-scene packet encodes");
        bits::Reader reader(std::span(output).first(written));
        check(reader.skip(wire::kLatchBitWithoutGrant + 1
            + wire::delta_bits(wire::top_level_key_count(s.roster), s.roster.bubbleSubBlocks)),
            "post-scene roster framing");
        unsigned gates{}, carriers{}, runtimeBodies{}, dialogues{}, directives{}, generators{}, scenes{}, sources{};
        while (field(reader, 1)) {
            const auto key = field(reader, 32);
            check(field(reader, 32) == 0, "post-scene group framing");
            while (field(reader, 1)) {
                check(field(reader, 32) == key, "post-scene object registry");
                const auto type = field(reader, 7) - wire::kSlotTypeBias;
                const auto index = field(reader, 16) - wire::kSlotIndexBias;
                const auto length = field(reader, 32);
                if (key == 0x2763EC97 && type == 37 && index == 1) {
                    ++generators;
                    check(length==1753 && field(reader,1)==1 && field(reader,1)==1
                        && field(reader,32)==12345,"full Forest activation reaches generator");
                    check(reader.skip(static_cast<std::size_t>(length)-34),"generator remainder");
                } else if (key == 0xD00142CF && type == 43 && index == 1 && length > 3) {
                    ++scenes;
                    check(field(reader,1)==1 && field(reader,1)==1,"retained approach scene authority");
                    check(reader.skip(123) && field(reader,6)==1 && field(reader,32)==0xC7ECAA77,
                        "later approach delivers one retained C7 event");
                    check(reader.skip(static_cast<std::size_t>(length)-163),"scene remainder");
                } else if (key == 0xD00142CF && type == 23 && index == 16 && length > 3) {
                    ++gates;
                    check(field(reader, 1) == 1 && field(reader, 1) == 1, "gate authority present");
                    check(field(reader, 32) == 0 && field(reader, 16) == 0x8002
                        && field(reader, 1) == 0, "native smooth release at revision two");
                    check(reader.skip(static_cast<std::size_t>(length) - 51), "remaining gate channels");
                } else if (key == 0x82FB58B7 && type == 68 && index == 0 && length > 3) {
                    ++directives;
                    check(field(reader, 1) == 1 && field(reader, 1) == 1, "objective authority present");
                    check(reader.skip(110) && field(reader, 32) == 0x1EBF4621U,
                        "Forest objective reaches native directive");
                    check(reader.skip(static_cast<std::size_t>(length) - 144), "remaining objective body");
                } else {
                    if (key == 0xBA5F26EF && type == 4 && index == 0 && length > 3) ++carriers;
                    if (key == 0x82FB58B7 && type == 53 && index == 2 && length > 3) ++dialogues;
                    if (key == 0xD00142CF && type == 1 && index == 0 && length > 3) ++sources;
                    if ((type == 18 || type == 35 || type == 13) && length > 3) ++runtimeBodies;
                    check(reader.skip(static_cast<std::size_t>(length)), "post-scene object framing");
                }
            }
        }
        check(field(reader, 1) == 0 && reader.remaining_bits() < 8, "post-scene packet complete");
        check(gates == unsigned(released), "native release publishes exactly one gate without global seeding");
        check(carriers == unsigned(released), "native release publishes exactly one contact carrier");
        check(dialogues == 1 && directives == 1, "native runtime ownership retains dialogue and objective updates");
        check(generators==1 && scenes==1 && sources==0,"stable updates deliver Forest and approach without respawning Ikora");
        check(!preserve || runtimeBodies == 0, "gate update preserves native script and player state");
    }
    rescue_cases(s,output);
    // The storage bound still rejects corrupt counts without touching the output.
    s.roster.groupCount = wire::kGroupCapacity + 1;
    output.fill(std::byte{0xA5});
    std::size_t written = 99;
    check(!wire::encode_sensor_auth_update(s, output, written) && written == 0, "oversized roster rejected");
    check(std::all_of(output.begin(), output.end(), [](auto b) { return b == std::byte{0xA5}; }),
        "rejection leaves output untouched");
}
} // namespace omega_loading_cases
