#pragma once
#include "server/runtime/activity/mercury_populations.h"
#include "server/runtime/activity/mercury_placements.h"
void population_packet_cases() {
    namespace p=dawn::server::runtime::activity::population;
    namespace m=dawn::server::runtime::activity::mercury;
    namespace n=dawn::middleware::bap::activity_message::native;
    unsigned checkIndex{};
    const auto check=[&](bool ok){++checkIndex;if(!ok){std::fprintf(stderr,"population packet failed at check %u\n",checkIndex);std::abort();}};
    std::array<std::array<std::uint8_t,16>,m::kRegistries.size()> types{},flags{};
    std::array<std::array<std::uint16_t,16>,m::kRegistries.size()> indices{};
    std::array<std::uint32_t,m::kRegistries.size()> keys{};
    wire::Snapshot snapshot{};snapshot.lifetime=3;snapshot.region=120;snapshot.hasRegion=true;
    snapshot.roster.groupCount=static_cast<std::uint32_t>(m::kRegistries.size());
    for(std::size_t i=0;i<m::kRegistries.size();++i) {
        const auto& registry=m::kRegistries[i];keys[i]=registry.key;
        for(std::size_t j=0;j<registry.slots.size();++j) {
            types[i][j]=registry.slots[j].type;flags[i][j]=registry.slots[j].flags();indices[i][j]=registry.slots[j].index;
        }
        snapshot.roster.groups[i]={registry.key,std::span(types[i]).first(registry.slots.size()),
            std::span(flags[i]).first(registry.slots.size()),std::span(indices[i]).first(registry.slots.size())};
    }
    std::array<wire::BubbleSubBlock,1> blocks{{{15,keys}}};snapshot.roster.bubbleSubBlocks=blocks;
    p::Service service;check(service.begin({42,{7}},m::kPopulations));
    check(service.request({{42,{7}},1,1,keys[0],0,1,1},15)==p::Result::accepted);
    check(service.request({{42,{7}},2,2,keys[1],0,1,1},15)==p::Result::accepted);
    std::array<std::byte,8192> packet{};std::size_t empty{},active{};
    check(wire::encode_sensor_auth_update(snapshot,packet,empty));
    snapshot.populations=service.project(15);
    check(n::population::valid(snapshot.populations,snapshot.roster,120));
    check(wire::encode_sensor_auth_update(snapshot,packet,active) && active>empty);
    namespace placement=dawn::server::runtime::activity::placement;
    check(placement::project(m::kPlacements,15,snapshot.placements) && snapshot.placements.count==9);
    n::placement::Batch outside;
    check(placement::project(m::kPlacements,14,outside) && outside.count==0);
    auto capabilities=m::kPlacements;capabilities[0].slot=10;
    check(!placement::project(capabilities,15,outside) && outside.count==0);
    capabilities=m::kPlacements;capabilities[1]=capabilities[0];
    check(!placement::project(capabilities,15,outside) && outside.count==0);
    auto oversized=snapshot;oversized.archiveOmega=true;
    oversized.roster.groupCount=wire::kGroupCapacity+1;
    packet.fill(std::byte{0xA5});active=99;
    check(!wire::encode_sensor_auth_update(oversized,packet,active) && active==0
        && packet[0]==std::byte{0xA5});
    // Both encoders support the complete roster, including more than 15 groups.
    static_assert(m::kRegistries.size()>15);
    for(const auto archive:{false,true}) {
        snapshot.archiveOmega=archive;
        check(wire::auth_body_bits(snapshot,keys[0],1,0,false)==641);
        bits::Writer measured=bits::Writer::measuring();
        check(wire::write_auth_body(measured,snapshot,keys[0],1,0,false) && measured.bit_count()==641);
        check(wire::encode_sensor_auth_update(snapshot,packet,active));
        check(wire::auth_body_bits(snapshot,keys[2],4,5,false)==253);
        auto placementMeasure=bits::Writer::measuring();
        check(wire::write_auth_body(placementMeasure,snapshot,keys[2],4,5,false) && placementMeasure.bit_count()==253);
        for(std::uint8_t slot=0;slot<3;++slot) {
            auto flagMeasure=bits::Writer::measuring();
            check(wire::auth_body_bits(snapshot,0x2749BAAE,4,slot,false)==253);
            check(wire::write_auth_body(flagMeasure,snapshot,0x2749BAAE,4,slot,false) && flagMeasure.bit_count()==253);
        }
        auto invalid=snapshot;invalid.placements.entries[0].slot=10;
        packet.fill(std::byte{0xA5});
        check(!wire::encode_sensor_auth_update(invalid,packet,active) && active==0 && packet[0]==std::byte{0xA5});
        invalid=snapshot;invalid.placements.entries[1]=invalid.placements.entries[0];
        check(!wire::encode_sensor_auth_update(invalid,packet,active));
        invalid=snapshot;invalid.placements.count=invalid.placements.entries.size()+1;
        check(!wire::encode_sensor_auth_update(invalid,packet,active));
        auto wrong=snapshot;wrong.region=112;
        packet.fill(std::byte{0xA5});
        check(!wire::encode_sensor_auth_update(wrong,packet,active) && active==0 && packet[0]==std::byte{0xA5});
        wrong=snapshot;wrong.populations.entries[0].slot=4;
        check(!wire::encode_sensor_auth_update(wrong,packet,active));
        wrong=snapshot;wrong.populations.entries[1]=wrong.populations.entries[0];
        check(!wire::encode_sensor_auth_update(wrong,packet,active));
        wrong=snapshot;wrong.populations.entries[0].source.generation=0;
        check(!wire::encode_sensor_auth_update(wrong,packet,active));
        wrong=snapshot;wrong.populations.count=wrong.populations.entries.size()+1;
        check(!wire::encode_sensor_auth_update(wrong,packet,active));
    }
}
