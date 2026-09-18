#pragma once
#include "../src/state/activity/Newlight/launchpad/shutter_native.h"

namespace launchpad_shutter_tests {
namespace sh=lp::shutter;
namespace gn=sh::gn;
struct Fixture {
    static constexpr std::uintptr_t source=0x1000,physical=0x2000,logical=0x3000,row=0x4000,logicalRow=0x5000,image=0x100000;
    std::array<std::byte,0x8000> memory{};
    std::uintptr_t physicalAddress{physical};
    gn::Weak entity{71,5},device{72,6},logicalEntity{73,7},logicalDevice{74,8};
    unsigned exchanges{},enables{};std::uint32_t authority{0x80000000U};bool race{};
    template<class T> void put(std::uintptr_t a,T v) {std::memcpy(memory.data()+a,&v,sizeof v);}
    template<class T> bool value(std::uintptr_t a,T& v) {
        if(a>memory.size() || sizeof v>memory.size()-a) {return false;}
        std::memcpy(&v,memory.data()+a,sizeof v);return true;
    }
    bool copy(std::uintptr_t a,std::span<std::byte> out) {
        if(a>memory.size() || out.size()>memory.size()-a) {return false;}
        std::memcpy(out.data(),memory.data()+a,out.size());return true;
    }
    bool weak(gn::Weak v) {return v==entity || v==device || v==logicalEntity || v==logicalDevice;}
    bool make_weak(std::uint32_t h,gn::Weak& out) {
        for(const auto v:{entity,device,logicalEntity,logicalDevice}) {if(v.handle==h) {out=v;return true;}}return false;
    }
    bool entity_row(gn::Weak v,std::uintptr_t& out) {
        if(v==entity) {out=row;return true;}if(v==logicalEntity) {out=logicalRow;return true;}return false;
    }
    bool resolve(std::uint32_t h,std::uintptr_t& out) {
        if(h==device.handle) {out=physicalAddress;return true;}if(h==logicalDevice.handle) {out=logical;return true;}return false;
    }
    std::uint64_t compare_exchange(std::uintptr_t a,std::uint64_t expected,std::uint64_t desired) {
        CHECK(a==source+0x1F0);++exchanges;std::uint64_t current{};CHECK(value(a,current));
        if(race) {return ~expected;}if(current==expected) {put(a,desired);}return current;
    }
    void enable(std::uintptr_t a,std::uint32_t mask) {
        CHECK(a==image+0x26BE0E0);CHECK(mask==(1U<<entity.handle) || mask==(1U<<logicalEntity.handle));++enables;authority|=mask;
    }
    sh::Physical target() {return {entity,device,physical};}
    Fixture() {
        put(source,gn::Ref{sh::kGate,0x80804F46U,0x278});put(source+0x1F0,logicalDevice);
        put(physical,gn::Ref{sh::kDevice,0x80803910U,0xA78});put(physical+0x24,device.handle);put(physical+0x2C,entity.handle);
        put(logical,gn::Ref{sh::kLogicalDevice,0x80803910U,0xA78});put(logical+0x24,logicalDevice.handle);put(logical+0x2C,logicalEntity.handle);
        put(row+0xC,entity.handle);put(logicalRow+0xC,logicalEntity.handle);
        const std::array<std::uint32_t,4> origin{1,2,3,4};put(row+0xD0,origin);put(logicalRow+0xD0,origin);
    }
    sh::Bound bind() {return sh::bind(*this,*this,image,source,target());}
};
inline void verify() {
    Fixture f;CHECK(f.bind()==sh::Bound::changed);CHECK(f.exchanges==1 && f.enables==1);
    CHECK(f.authority==(0x80000000U|(1U<<5)));gn::Weak actual{};CHECK(f.value(f.source+0x1F0,actual));CHECK(actual==f.device);
    CHECK(f.bind()==sh::Bound::ready && f.exchanges==1);
    // Streaming compacts component storage without changing either weak handle.
    // The cached address can now contain unrelated data; only resolve the handle.
    Fixture moved;const auto captured=moved.target();CHECK(moved.bind()==sh::Bound::changed);
    moved.physicalAddress=0x6000;
    std::memcpy(moved.memory.data()+moved.physicalAddress,moved.memory.data()+moved.physical,0xA78);
    moved.put(moved.physical,0xDEADBEEFU);
    CHECK(sh::bind(moved,moved,moved.image,moved.source,captured)==sh::Bound::ready);
    CHECK(moved.enables==2 && moved.exchanges==1);
    moved.put(moved.physicalAddress+0x2C,999U);
    CHECK(sh::bind(moved,moved,moved.image,moved.source,captured)==sh::Bound::invalid);
    Fixture stale;stale.put(stale.source+0x1F0,gn::Weak{999,stale.logicalDevice.handle});CHECK(stale.bind()==sh::Bound::changed);
    Fixture absent;absent.put(absent.source+0x1F0,gn::Weak{});CHECK(absent.bind()==sh::Bound::changed);
    Fixture foreign;foreign.put(foreign.logical,0x12345678U);CHECK(foreign.bind()==sh::Bound::foreign);CHECK(!foreign.exchanges && !foreign.enables);
    Fixture displaced;displaced.put(displaced.logicalRow+0xD0,5U);CHECK(displaced.bind()==sh::Bound::foreign);CHECK(!displaced.exchanges);
    Fixture recycled;++recycled.device.serial;CHECK(sh::bind(recycled,recycled,recycled.image,recycled.source,f.target())==sh::Bound::invalid);CHECK(!recycled.exchanges);
    Fixture wrongOwner;wrongOwner.put(wrongOwner.physical+0x2C,999U);CHECK(wrongOwner.bind()==sh::Bound::invalid);
    Fixture retired;retired.put(retired.row+4,4U);CHECK(retired.bind()==sh::Bound::invalid && !retired.enables);
    Fixture wrongSource;wrongSource.put(wrongSource.source,0x8153C14BU);CHECK(wrongSource.bind()==sh::Bound::invalid);
    Fixture raced;raced.race=true;CHECK(raced.bind()==sh::Bound::raced && !raced.enables);

    Fixture pair;
    pair.put(pair.logical,gn::Ref{sh::kDevice,0x80803910U,0xA78});
    const sh::Physical second{pair.logicalEntity,pair.logicalDevice,pair.logical};
    const std::array candidates{pair.target(),second};sh::Physical selected{};
    pair.put(pair.source+0x1F0,gn::Weak{});
    pair.put(pair.source+0x1C0,0.F);pair.put(pair.source+0x1C4,std::uint16_t{1});
    pair.put(pair.physical+0x960,-1);pair.put(pair.logical+0x960,-1);
    const auto next=[&] {return sh::next(pair,pair.source,pair.target(),candidates,selected);};
    CHECK(next() && selected.device==pair.device);
    CHECK(sh::bind(pair,pair,pair.image,pair.source,selected)==sh::Bound::changed);
    CHECK(next() && selected.device==pair.device); // Wait for the native tick, not a frame count.
    pair.put(pair.physical+0x960,1);
    CHECK(next() && selected.device==pair.logicalDevice);
    CHECK(sh::bind(pair,pair,pair.image,pair.source,selected)==sh::Bound::changed);
    pair.put(pair.logical+0x960,1);CHECK(!next());
    CHECK(pair.authority==(0x80000000U|(1U<<5)|(1U<<7)));
    // The new open command is independently delivered to both physical devices.
    pair.put(pair.source+0x1C0,1.F);pair.put(pair.source+0x1C4,std::uint16_t{2});
    CHECK(next() && selected.device==pair.device);
    CHECK(sh::bind(pair,pair,pair.image,pair.source,selected)==sh::Bound::changed);
    pair.put(pair.physical+0x960,2);
    CHECK(next() && selected.device==pair.logicalDevice);
    CHECK(sh::bind(pair,pair,pair.image,pair.source,selected)==sh::Bound::changed);
    pair.put(pair.logical+0x960,2);CHECK(!next());
    // A late prefab starts at local zero. Do not drive it until its final world
    // transform matches the rifle doorway, even if the command already arrived.
    pair.put(pair.logical+0x960,-1);pair.put(pair.logicalRow+0xD0,0U);CHECK(!next());
    pair.put(pair.logicalRow+0xD0,1U);CHECK(next() && selected.device==pair.logicalDevice);
    pair.put(pair.logicalRow+4,4U);CHECK(!next());pair.put(pair.logicalRow+4,0U);
    ++pair.logicalDevice.serial;CHECK(!next());--pair.logicalDevice.serial;
    pair.put(pair.source+0x1C4,UINT16_MAX);CHECK(!next());
    pair.put(pair.source+0x1C4,std::uint16_t{2});pair.put(pair.source+0x1C0,std::numeric_limits<float>::quiet_NaN());CHECK(!next());
}
}
