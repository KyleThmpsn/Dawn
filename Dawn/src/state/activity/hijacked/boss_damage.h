#pragma once
#include "frame.h"
#include "../coo/native_damage_floor.h"
#include <cmath>
#include <cstring>
#include <span>
namespace dawn::state::activity::hijacked::boss_damage {
inline constexpr float floor(std::uint8_t stage) noexcept { return stage==0?2.F/3.F:stage==1?1.F/3.F:0.F; }
inline bool blocked(const BossRequest& request,float fraction) noexcept {
    return request.owner.valid() && request.enemy.valid() && request.stage<3 && std::isfinite(fraction)
        && ((request.stage>0 && request.requested) || (request.stage<2 && fraction<=floor(request.stage)));
}
using coo::native_damage_floor::Packet;
using coo::native_damage_floor::get;
inline constexpr std::size_t kLethalFlagsOffset=0x60;
inline Packet clamp(std::span<std::byte> packet,std::int32_t bodyRegion,float minimum) noexcept {
    const auto result=coo::native_damage_floor::clamp(packet,bodyRegion,minimum);
    // B804E0 reads this flag at B80D23 and skips the post-damage health calculation.
    // A positive body floor invalidates that lethal prediction for this hit.
    if(result==Packet::clamped) {packet[kLethalFlagsOffset]&=std::byte{0xFE};}
    return result;
}
}
