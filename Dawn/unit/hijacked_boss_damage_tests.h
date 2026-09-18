#pragma once
#include "../src/client/hooks/bootflow/hijacked_boss_damage.h"
#include <cstdio>
#include <limits>
#include <map>

namespace hijacked_damage_fixture {
namespace native=dawn::client::hooks::bootflow::hijacked_boss_damage;
namespace mission=dawn::state::activity::hijacked;
struct Memory {
    std::map<std::uintptr_t,std::byte> bytes;
    std::map<std::uint32_t,std::uintptr_t> handles;
    template<class T> void put(std::uintptr_t address,T value) {
        const auto* data=reinterpret_cast<const std::byte*>(&value);
        for(std::size_t i=0;i<sizeof(T);++i) {bytes[address+i]=data[i];}
    }
    void zero(std::uintptr_t address,std::size_t count) {for(std::size_t i=0;i<count;++i) {bytes[address+i]=std::byte{};}}
    bool copy(std::uintptr_t address,std::span<std::byte> output) {
        for(std::size_t i=0;i<output.size();++i) {const auto it=bytes.find(address+i);if(it==bytes.end()) {return false;}output[i]=it->second;}return true;
    }
    template<class T> bool value(std::uintptr_t address,T& out) {return copy(address,std::as_writable_bytes(std::span{&out,1}));}
    bool resolve(std::uint32_t handle,std::uintptr_t& out,std::uintptr_t* allocation=nullptr) {
        const auto it=handles.find(handle);if(it==handles.end()) {return false;}out=it->second;if(allocation) {*allocation=out;}return true;
    }
};
struct Fixture : Memory {
    static constexpr std::uintptr_t image=0x100000,table=0x200000,source=0x300000,entities=0x500000,
        bundle=0x600000,metadata=0x700000,health=0x800000,sourceBase=0x900000,definitionBase=0xA00000,context=0xB00000;
    static constexpr auto actor=table+3*0x100,entity=entities+5*0x100,character=bundle+0x100,
        definition=definitionBase+0x1338,region=definitionBase+0x1A30;
    mission::BossRequest request{{1,7},{1,3,10,7,mission::kBoss.slot,mission::kBoss.registry},0,1,true};
    Fixture() {
        put<std::uintptr_t>(image+0x1F9D7F8,table);put<std::uint32_t>(image+0x1F9D800,0x100);
        put<std::uint32_t>(actor+0x48,3);put<std::uint32_t>(actor+0x4C,5);put<std::uint32_t>(actor+0x38,10);
        put<std::int64_t>(actor+0x40,0);put<std::uint32_t>(actor+0x50,UINT32_MAX);
        handles[10]=source;handles[12]=bundle;handles[13]=metadata;handles[14]=character;handles[15]=health;
        handles[17]=sourceBase;handles[0x815B5AA2U]=definitionBase;
        put<std::uint32_t>(source,17);put<std::uint32_t>(source+4,0x8080948F);put<std::int64_t>(source+8,0x728);
        put<std::uint32_t>(sourceBase+0x758,mission::kBoss.registry);put<std::uint8_t>(sourceBase+0x75C,1);
        put<std::uint16_t>(sourceBase+0x75E,mission::kBoss.slot);
        put<std::uint32_t>(source+0x1FC,7);put<std::uint32_t>(source+0x244,7);
        put<std::uintptr_t>(image+0x1F93428,entities);put<std::uint32_t>(image+0x1F93430,0x100);
        put<std::uint32_t>(entity+4,0);put<std::uint32_t>(entity+0x4C,12);
        put<std::uint32_t>(bundle,0);put<std::uint32_t>(bundle+4,13);put<std::uint32_t>(bundle+0x18,UINT32_MAX);
        put<std::uint64_t>(metadata+0x68,1);put<std::int64_t>(metadata+0x70,0x80);put<std::int32_t>(metadata+0x114,0x100);
        put<std::uint32_t>(character+4,0x80806832);put<std::uint32_t>(character+0x24,14);put<std::uint32_t>(character+0x2C,5);
        put<std::uint32_t>(character+0xC0,3);put<std::uint32_t>(character+0x2E8,15);
        put<std::uint32_t>(character+0x2EC,0x80804BEE);put<std::int64_t>(character+0x2F0,0);
        zero(health,0x30);put<std::uint32_t>(health,0x815B5AA2U);put<std::uint32_t>(health+4,0x80804B8A);
        put<std::int64_t>(health+8,0x1338);put<std::uint32_t>(health+0x24,15);put<std::uint32_t>(health+0x2C,5);
        put<std::int16_t>(health+0x6F8,7);put<std::uintptr_t>(context,definition);put<std::uintptr_t>(context+8,health);
        put<std::int32_t>(definition+0x1B0,1);put<std::int64_t>(definition+0x1B8,0x530);
        zero(region,0xD4);put<std::uint32_t>(region,0x815B5AA2U);put<std::uint32_t>(region+4,0x80804BAB);
        put<std::int64_t>(region+8,0x10F0);put<std::uint32_t>(region+0x10,0x6DFE676D);put<std::uint32_t>(region+0xD0,0);
        constexpr std::array<std::uint8_t,16> prefix{0x48,0x83,0xEC,0x68,0x44,0x8B,0x09,0x4C,0x8B,0xD1,0x48,0x89,0x4C,0x24,0x28,0x41};
        for(std::size_t i=0;i<prefix.size();++i) {put<std::uint8_t>(image+0xCD6C20+i,prefix[i]);}
    }
    bool accepted() {native::Sample sample{};return native::sample(*this,image,context,request,sample);}
};
template<class T,std::size_t N> void field(std::array<std::byte,N>& packet,std::size_t offset,T value) {std::memcpy(packet.data()+offset,&value,sizeof value);}
}

inline bool hijacked_boss_damage_contracts() {
    using namespace hijacked_damage_fixture;
    namespace policy=mission::boss_damage;
#define HIJACKED_DAMAGE_CHECK(expression) do {if(!(expression)) {std::fprintf(stderr,"FAIL Hijacked damage line %d: %s\n",__LINE__,#expression);return false;}} while(false)
    Fixture good;native::Sample sampled{};
    HIJACKED_DAMAGE_CHECK(native::sample(good,Fixture::image,Fixture::context,good.request,sampled));
    HIJACKED_DAMAGE_CHECK(sampled.health==Fixture::health && sampled.handle==15 && sampled.bodyRegion==7);
    // The source generation belongs to the lifecycle. Health identity uses its
    // salted self handle instead of incorrectly comparing that salt to generation.
    for(const auto offset:{0x1FCU,0x244U}) {auto bad=good;bad.put<std::uint32_t>(Fixture::source+offset,8);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;++bad.request.owner.value;HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;++bad.request.enemy.run;HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;++bad.request.enemy.source;HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uint32_t>(Fixture::actor+0x48,3U^0x2000U);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uint32_t>(Fixture::health+0x24,15U^0x2000U);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uint32_t>(Fixture::health+0x2C,6);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.handles[15]=Fixture::health+0x100;HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uintptr_t>(Fixture::context,Fixture::definition+8);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    for(const auto offset:{0U,4U,8U}) {auto bad=good;bad.put<std::uint32_t>(Fixture::health+offset,0);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::int64_t>(Fixture::definition+0x1B8,0x538);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::int32_t>(Fixture::definition+0x1B0,0);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    for(const auto offset:{0U,4U,8U,0x10U}) {auto bad=good;bad.put<std::uint32_t>(Fixture::region+offset,0);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uint32_t>(Fixture::region+0xD0,1);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::int16_t>(Fixture::health+0x6F8,-1);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    {auto bad=good;bad.put<std::uint8_t>(Fixture::image+0xCD6C20,0xE9);HIJACKED_DAMAGE_CHECK(!bad.accepted());}
    auto request=good.request;
    HIJACKED_DAMAGE_CHECK(policy::floor(0)==2.F/3.F && policy::floor(1)==1.F/3.F && policy::floor(2)==0);
    HIJACKED_DAMAGE_CHECK(!policy::blocked(request,1.F) && policy::blocked(request,2.F/3.F));
    request.stage=1;request.requested=true;HIJACKED_DAMAGE_CHECK(policy::blocked(request,.6F));
    request.requested=false;HIJACKED_DAMAGE_CHECK(!policy::blocked(request,.6F) && policy::blocked(request,1.F/3.F));
    request.stage=2;request.requested=true;HIJACKED_DAMAGE_CHECK(policy::blocked(request,.2F));
    request.requested=false;HIJACKED_DAMAGE_CHECK(!policy::blocked(request,0.F));
    request.owner={};HIJACKED_DAMAGE_CHECK(!policy::blocked(request,0.F));
    // A native row stores a resulting fraction. A protected body clamp must also
    // clear the packet's lethal prediction, preserving all other header/row bits.
    std::array<std::byte,0x68+4*12> packet{};packet.fill(std::byte{0xA5});field<std::int32_t>(packet,0x64,4);
    const std::array<std::int32_t,4> regions{7,0,7,7};const std::array<float,4> targets{0.F,.1F,.9F,-.5F};
    for(std::size_t i=0;i<4;++i) {field<std::uint32_t>(packet,0x68+i*12,0xABCDEF01U+static_cast<std::uint32_t>(i));field(packet,0x6C+i*12,regions[i]);field(packet,0x70+i*12,targets[i]);}
    const auto original=packet;auto expected=original;expected[0x60]=std::byte{0xA4};field(expected,0x70,2.F/3.F);field(expected,0x70+3*12,2.F/3.F);
    HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,policy::floor(0))==policy::Packet::clamped && packet==expected);
    HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,policy::floor(0))==policy::Packet::unchanged && packet==expected);
    packet=original;expected=original;expected[0x60]=std::byte{0xA4};field(expected,0x70,1.F/3.F);field(expected,0x70+3*12,1.F/3.F);
    HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,policy::floor(1))==policy::Packet::clamped && packet==expected);
    // Nonlethal threshold hits keep their flags. Unrelated regions and hits
    // above the threshold must not have a lethal prediction rewritten.
    packet=original;packet[0x60]=std::byte{0xA4};
    HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,policy::floor(1))==policy::Packet::clamped && packet==expected);
    packet=original;HIJACKED_DAMAGE_CHECK(policy::clamp(packet,99,policy::floor(1))==policy::Packet::unchanged && packet==original);
    packet=original;field(packet,0x70,.8F);field(packet,0x70+3*12,.8F);
    {const auto before=packet;HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,policy::floor(0))==policy::Packet::unchanged && packet==before);}
    // The final phase retains the real lethal flag and zero-health result.
    packet=original;HIJACKED_DAMAGE_CHECK(policy::clamp(packet,7,0)==policy::Packet::unchanged && packet==original);
    for(const auto count:{-1,33}) {auto bad=original;field<std::int32_t>(bad,0x64,count);const auto before=bad;HIJACKED_DAMAGE_CHECK(policy::clamp(bad,7,2.F/3.F)==policy::Packet::invalid && bad==before);}
    {auto bad=original;const auto before=bad;HIJACKED_DAMAGE_CHECK(policy::clamp(std::span(bad).first(0x67),7,.5F)==policy::Packet::invalid && bad==before);HIJACKED_DAMAGE_CHECK(policy::clamp(std::span(bad).first(bad.size()-1),7,.5F)==policy::Packet::invalid && bad==before);}
    {auto bad=original;field(bad,0x70+3*12,std::numeric_limits<float>::quiet_NaN());const auto before=bad;HIJACKED_DAMAGE_CHECK(policy::clamp(bad,7,.5F)==policy::Packet::invalid && bad==before);}
    {auto bad=original;const auto before=bad;HIJACKED_DAMAGE_CHECK(policy::clamp(bad,7,std::numeric_limits<float>::infinity())==policy::Packet::invalid && bad==before);}
#undef HIJACKED_DAMAGE_CHECK
    return true;
}
