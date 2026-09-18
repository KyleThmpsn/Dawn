#pragma once
#include "shutter.h"
#include "../../../../client/hooks/bootflow/coo_native_components.h"
#include "../../../../client/hooks/bootflow/gateway_native_read.h"

namespace dawn::state::activity::newlight::launchpad::shutter {
namespace gn=client::hooks::bootflow::gateway_native;
inline constexpr std::uint32_t kDevice=0x80F3D672U,kLogicalDevice=0x80C230ABU,kGate=0x8153C14AU;
struct Physical {
    // address records discovery only: native component storage can relocate.
    // The weak handles, resource header and owner establish current identity.
    gn::Weak entity{},device{};std::uintptr_t address{};
    friend bool operator==(const Physical&,const Physical&)=default;
};
template<class Read> bool identity(Read& read,const Physical& p,std::uintptr_t& row) noexcept {
    gn::Ref header{};std::uintptr_t resolved{};std::uint32_t self{},owner{},entity{},flags{};
    return p.address && read.entity_row(p.entity,row) && read.value(row+0xC,entity) && entity==p.entity.handle
        && read.value(row+4,flags) && !(flags&5U) && read.weak(p.device)
        && read.resolve(p.device.handle,resolved)
        && read.value(resolved,header) && header.handle==kDevice && header.kind==0x80803910U && header.offset==0xA78
        && read.value(resolved+0x24,self) && self==p.device.handle
        && read.value(resolved+0x2C,owner) && owner==p.entity.handle;
}
template<class Read> bool capture(Read& read,std::uint32_t entity,Physical& out) noexcept {
    Physical p{};std::uintptr_t row{};std::uint32_t actual{},bundle{},self{};
    if(!read.make_weak(entity,p.entity) || !read.entity_row(p.entity,row)
        || !read.value(row+0xC,actual) || actual!=entity || !read.value(row+0x4C,bundle)
        || !client::hooks::bootflow::coo_native::component<Read,1024>(read,bundle,entity,0x80803910U,p.address)
        || !read.value(p.address+0x24,self) || !read.make_weak(self,p.device) || !identity(read,p,row)) {return false;}
    out=p;return true;
}
template<class Read> bool gate(Read& read,std::uintptr_t source) noexcept {
    gn::Ref header{};
    return read.value(source,header) && header.handle==kGate && header.kind==0x80804F46U && header.offset==0x278;
}
inline std::uint64_t packed(gn::Weak value) noexcept {
    std::uint64_t bits{};std::memcpy(&bits,&value,sizeof bits);return bits;
}
// Replace only this mission's co-located logical controller or physical shutter,
// or a stale binding. Both physical surfaces must consume each position revision.
// A live foreign device is never displaced. Native 106AD60 continues to own the
// position setter, interpolation, rendering and collision on its normal tick.
template<class Read> bool previous_allowed(Read& read,gn::Weak previous,const Physical& p,std::uintptr_t row) noexcept {
    if(previous==p.device || previous.handle==UINT32_MAX || !read.weak(previous)) {return true;}
    std::uintptr_t logical{},logicalRow{};gn::Ref header{};gn::Weak entity{};std::uint32_t self{},owner{},actual{};
    std::array<std::byte,16> physicalOrigin{},logicalOrigin{};
    return read.resolve(previous.handle,logical) && read.value(logical,header)
        && (header.handle==kLogicalDevice || header.handle==kDevice) && header.kind==0x80803910U && header.offset==0xA78
        && read.value(logical+0x24,self) && self==previous.handle
        && read.value(logical+0x2C,owner) && read.make_weak(owner,entity) && read.entity_row(entity,logicalRow)
        && read.value(logicalRow+0xC,actual) && actual==owner
        && read.copy(row+0xD0,physicalOrigin) && read.copy(logicalRow+0xD0,logicalOrigin)
        && physicalOrigin==logicalOrigin;
}

// A prefab shutter is constructed at local origin zero; only its finalized
// world row can prove that it overlaps the placed rifle doorway. Never infer
// that a local-zero descriptor belongs here or affect the shutter behind us.
template<class Read> bool overlaps(Read& read,const Physical& anchor,const Physical& candidate) noexcept {
    std::uintptr_t anchorRow{},row{};std::array<std::byte,16> origin{},other{};
    return identity(read,anchor,anchorRow) && identity(read,candidate,row)
        && read.copy(anchorRow+0xD0,origin) && read.copy(row+0xD0,other) && origin==other;
}

// Leave a binding in place until the normal native tick acknowledges it. Then
// select the next overlapping surface that has not consumed this revision.
// Native interpolation continues independently after a surface is unbound.
template<class Read> bool next(Read& read,std::uintptr_t source,const Physical& anchor,
    std::span<const Physical> candidates,Physical& out) noexcept {
    float position{};std::uint16_t revision{};
    if(!gate(read,source) || !read.value(source+0x1C0,position) || (position!=0.F && position!=1.F)
        || !read.value(source+0x1C4,revision) || revision>=32766U) {return false;}
    for(const auto& candidate:candidates) {
        std::uintptr_t address{};std::int32_t consumed{};
        if(overlaps(read,anchor,candidate) && read.resolve(candidate.device.handle,address)
            && read.value(address+0x960,consumed) && consumed!=revision) {out=candidate;return true;}
    }
    return false;
}
enum class Bound {invalid,foreign,raced,ready,changed};
template<class Read,class Exchange> Bound bind(Read& read,Exchange& exchange,std::uintptr_t image,
    std::uintptr_t source,const Physical& p) noexcept {
    std::uintptr_t row{},stableRow{};gn::Weak previous{},again{};
    if(!gate(read,source) || !identity(read,p,row) || !read.value(source+0x1F0,previous)) {return Bound::invalid;}
    if(!previous_allowed(read,previous,p,row)) {return Bound::foreign;}
    if(!gate(read,source) || !identity(read,p,stableRow) || row!=stableRow
        || !read.value(source+0x1F0,again) || again!=previous) {return Bound::invalid;}
    const auto before=packed(previous),desired=packed(p.device);
    if(previous!=p.device && exchange.compare_exchange(source+0x1F0,before,desired)!=before) {return Bound::raced;}
    if(!gate(read,source) || !identity(read,p,stableRow) || row!=stableRow
        || !read.value(source+0x1F0,again) || again!=p.device) {
        if(previous!=p.device) {static_cast<void>(exchange.compare_exchange(source+0x1F0,desired,before));}
        return Bound::invalid;
    }
    // The placed world entity is not owned by a type-4 source. Grant just its
    // authority bit so the normal type-23 tick accepts the position command.
    exchange.enable(image+0x26BE0E0+4U*((p.entity.handle&0x1FFFU)/32U),1U<<(p.entity.handle&31U));
    return previous==p.device?Bound::ready:Bound::changed;
}
}
