#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace dawn::state::activity::omega_boss_teleport {

inline constexpr std::uint32_t kGroupHash=0x1F992208U;
inline constexpr std::uint32_t kSequenceHash=0xCBFDCA32U;
using Request=std::array<std::byte,128>;

/** Native C620F0 opcode45 -> C61660 -> B31510 ->10C6AF0.
 * The boss's authored80F4519A maps teleport/CBFDCA32 to group1/sequence0.
 * Caller validates the current character and selector ownership/admission.
 * Encoding a request does not prove it was accepted or completed. */
[[nodiscard]] inline std::optional<Request> encode_request(const std::array<float,3>& destination) noexcept {
    for(const auto value:destination) { if(!std::isfinite(value)) { return std::nullopt; } }
    Request bytes{};
    const auto put=[&](std::size_t offset,std::uint32_t value) {
        for(unsigned i=0;i<4;++i) { bytes[offset+i]=std::byte((value>>(8*i))&0xFFU); }
    };
    put(0,1); // Selector group ordinal; sequence ordinal0 and mode0 are zero.
    put(0xC,UINT32_MAX);put(0x10,UINT32_MAX); // Native unspecified range pair.
    for(std::size_t i=0;i<destination.size();++i) { put(0x14+i*4,std::bit_cast<std::uint32_t>(destination[i])); }
    bytes[0x60]=std::byte{0x45};
    return bytes;
}

struct Owner final {
    std::uint64_t run{};
    std::uint32_t actor{UINT32_MAX},character{UINT32_MAX},entity{UINT32_MAX};
    std::uint32_t generation{},revision{},biped{UINT32_MAX},selector{UINT32_MAX},interfaceHandle{UINT32_MAX};
    // Native10A8580 derives raw+38 from CC4CA0(entity)->component+60.
    // It is the movement body, distinct from the animation biped above.
    std::uint32_t movementBody{UINT32_MAX};
    std::uint8_t island{};
    std::uint32_t actionEpoch{};
    bool operator==(const Owner&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return run!=0 && actor!=UINT32_MAX && character!=UINT32_MAX && entity!=UINT32_MAX
            && generation!=0 && biped!=UINT32_MAX && selector!=UINT32_MAX && interfaceHandle!=UINT32_MAX
            && movementBody!=UINT32_MAX && island<=4;
    }
};

inline constexpr std::size_t kMotionComponentSnapshotBytes=0x710;
inline constexpr std::size_t kMotionUpdateContextBytes=0x54;

namespace detail {
[[nodiscard]] inline std::uint64_t read_unsigned(std::span<const std::byte> bytes,
    std::size_t offset,unsigned width) noexcept {
    std::uint64_t value{};
    for(unsigned i=0;i<width;++i) { value|=std::to_integer<std::uint64_t>(bytes[offset+i])<<(8*i); }
    return value;
}
[[nodiscard]] inline std::optional<std::size_t> relative_range(std::span<const std::byte> bytes,
    std::size_t field,std::size_t length) noexcept {
    const auto relative=std::bit_cast<std::int64_t>(read_unsigned(bytes,field,8));
    // Native arrays are relative to their count/type prefix, followed by 16 bytes.
    const auto origin=field+0x10;
    std::size_t start{};
    if(relative<0) {
        const auto magnitude=std::uint64_t{0}-std::bit_cast<std::uint64_t>(relative);
        if(magnitude>origin) { return std::nullopt; }
        start=origin-static_cast<std::size_t>(magnitude);
    } else {
        if(static_cast<std::uint64_t>(relative)>bytes.size()-origin) { return std::nullopt; }
        start=origin+static_cast<std::size_t>(relative);
    }
    if(start<0x90 || start>bytes.size() || length>bytes.size()-start) { return std::nullopt; }
    return start;
}
[[nodiscard]] constexpr bool overlaps(std::size_t a,std::size_t aSize,
    std::size_t b,std::size_t bSize) noexcept { return a<b+bSize && b<a+aSize; }
} // namespace detail

/** A raw motion block can move in native109BD40 without changing this ID.
 * This is not an allocation generation: caller must retain the claimed request,
 * exact Owner and observed lifecycle, and discard the ID after native cleanup. */
struct MotionIdentity final {
    std::uint32_t component{UINT32_MAX};
    std::uint32_t logicalSlot{UINT32_MAX};
    std::size_t stateOffset{}; // Current offset in the supplied component snapshot.
    [[nodiscard]] constexpr std::uint64_t stable_id() const noexcept {
        return component==UINT32_MAX || logicalSlot>=3 ? 0
            : (static_cast<std::uint64_t>(component)<<32)|(static_cast<std::uint64_t>(logicalSlot)+1);
    }
};

/** Read-only equivalent of native109C740(arena,logicalSlot), with arena=component+30.
 * The caller freshly resolves selector+78's Ref16 (808069EF, offset0) and copies
 * this component. Never derive it from cleanup's opaque third argument.
 * Authored815B5A43 has three slots/segments and 3F0 raw bytes. Runtime array marker
 * words are deliberately not required: they are package metadata, not validity.
 * No live pointers are dereferenced here; all relative ranges stay in the snapshot. */
[[nodiscard]] inline std::optional<MotionIdentity> resolve_motion_slot(
    std::span<const std::byte> component,std::uint64_t componentAddress,
    std::uint32_t componentHandle,std::uint32_t entity,std::uint32_t logicalSlot,
    std::uint8_t expectedOpcode=0x3D,std::uint16_t expectedSize=0xF0) noexcept {
    if(component.size()<kMotionComponentSnapshotBytes || componentAddress==0
        || componentHandle==UINT32_MAX || entity==UINT32_MAX || logicalSlot>=3
        || componentAddress>std::numeric_limits<std::uint64_t>::max()-kMotionComponentSnapshotBytes) {
        return std::nullopt;
    }
    // Bound the decoder even when its caller supplies a larger allocation.
    component=component.first(kMotionComponentSnapshotBytes);
    const auto get=[&](std::size_t offset,unsigned width=4) {
        return detail::read_unsigned(component,offset,width);
    };
    constexpr std::size_t arena=0x30;
    if(get(0)!=0x815B5A43U || get(4)!=0x808069EEU || get(8,8)!=0x1010
        || get(0x24)!=componentHandle || get(0x2C)!=entity
        || get(arena)!=0x815B5A43U || get(arena+4)!=0x808069F6U || get(arena+8,8)!=0x1118
        || get(arena+0x20,8)!=3 || get(arena+0x30,8)!=3 || get(arena+0x40,8)!=0x3F0) {
        return std::nullopt;
    }
    const auto slots=detail::relative_range(component,arena+0x28,3*4);
    const auto segments=detail::relative_range(component,arena+0x38,3*6);
    const auto raw=detail::relative_range(component,arena+0x48,0x3F0);
    if(!slots || !segments || !raw || detail::overlaps(*slots,12,*segments,18)
        || detail::overlaps(*slots,12,*raw,0x3F0) || detail::overlaps(*segments,18,*raw,0x3F0)) {
        return std::nullopt;
    }
    const auto occupied=static_cast<std::uint32_t>(get(arena+0x50));
    const auto special=static_cast<std::uint32_t>(get(arena+0x54));
    const auto active=static_cast<unsigned>(std::popcount(occupied));
    if((occupied&~7U)!=0 || (occupied&(1U<<logicalSlot))==0 || (special&~occupied)!=0
        || get(arena+0x58,2)!=active || get(arena+0x5A,2)!=active
        || get(arena+0x5C,2)!=static_cast<unsigned>(std::popcount(special))) { return std::nullopt; }
    std::array<std::size_t,3> offsets{},sizes{};
    unsigned seenSegments{};
    for(unsigned slot=0;slot<3;++slot) {
        if((occupied&(1U<<slot))==0) { continue; }
        const auto slotEntry=*slots+slot*4;
        const auto segment=get(slotEntry,2);
        if(segment>=active || (seenSegments&(1U<<segment))!=0) { return std::nullopt; }
        seenSegments|=1U<<segment;
        const auto segmentEntry=*segments+static_cast<std::size_t>(segment)*6;
        const auto offset=get(segmentEntry,2),size=get(segmentEntry+2,2);
        if(offset>=0x3F0 || size==0 || size>0x3F0-offset || get(segmentEntry+4,2)!=slot
            || get(slotEntry+2,1)==0xFF) { return std::nullopt; }
        offsets[slot]=static_cast<std::size_t>(offset);sizes[slot]=static_cast<std::size_t>(size);
        for(unsigned earlier=0;earlier<slot;++earlier) {
            if(sizes[earlier]!=0 && detail::overlaps(offsets[slot],sizes[slot],offsets[earlier],sizes[earlier])) {
                return std::nullopt;
            }
        }
    }
    if(get(*slots+logicalSlot*4+2,1)!=expectedOpcode || sizes[logicalSlot]!=expectedSize) { return std::nullopt; }
    return MotionIdentity{componentHandle,logicalSlot,*raw+offsets[logicalSlot]};
}

/** Native10D35D0 stops the owned intro selector first; native10397D0 later
 * removes its incompatible opcode39 allocation. A selector stop alone is not
 * sufficient to admit teleport. Stale raw bytes after free are immaterial. */
[[nodiscard]] inline bool empty_motion_arena(std::span<const std::byte> component,
    std::uint32_t componentHandle,std::uint32_t entity) noexcept {
    if(component.size()<kMotionComponentSnapshotBytes || componentHandle==UINT32_MAX || entity==UINT32_MAX) {
        return false;
    }
    const auto get=[&](std::size_t offset,unsigned width=4) { return detail::read_unsigned(component,offset,width); };
    return get(0)==0x815B5A43U && get(4)==0x808069EEU && get(8,8)==0x1010
        && get(0x24)==componentHandle && get(0x2C)==entity
        && get(0x30)==0x815B5A43U && get(0x34)==0x808069F6U && get(0x38,8)==0x1118
        && get(0x50,8)==3 && get(0x60,8)==3 && get(0x70,8)==0x3F0
        && get(0x80)==0 && get(0x84)==0 && get(0x88,2)==0 && get(0x8A,2)==0 && get(0x8C,2)==0;
}

/** Native scheduling can replace the retired intro with default locomotion6C
 * before the next member tick. Original F103F0 with these authored policies
 * cancels that motion and admits teleport3D when no prepass protects it. Do not
 * require an empty arena indefinitely or cancel locomotion in the bridge.
 * Native10A9B90 initializes the full world owner at raw+304; earlier raw bytes
 * can retain data from the freed intro and are not ownership evidence. */
[[nodiscard]] inline bool default_locomotion_motion_arena(std::span<const std::byte> component,
    std::uint64_t componentAddress,std::uint32_t componentHandle,std::uint32_t entity) noexcept {
    if(component.size()<kMotionComponentSnapshotBytes) { return false; }
    const auto get=[&](std::size_t offset,unsigned width=4) { return detail::read_unsigned(component,offset,width); };
    if(std::popcount(static_cast<std::uint32_t>(get(0x80)))!=1 || get(0x84)!=0
        || get(0x1E4)!=0x80F83641U || get(0x1E8)!=0x80F83881U
        || get(0x2A0,1)!=0 || get(0x2A2,1)!=1) { return false; }
    for(const std::size_t base:{0x90U,0xF0U}) {
        const auto definition=base==0x90U?0x1138U:0x1150U;
        if(get(base)!=0x815B5A43U || get(base+4)!=0x808069FFU || get(base+8,8)!=definition
            || get(base+0x20,8)!=0x2F0 || get(base+0x50,2)!=0 || get(base+0x52,2)!=0) { return false; }
    }
    for(std::uint32_t slot=0;slot<3;++slot) {
        const auto id=resolve_motion_slot(component,componentAddress,componentHandle,entity,slot,0x6C,0x330);
        if(id) { return get(id->stateOffset+0x304)==entity; }
    }
    return false;
}

/** Resolve a callback's current raw argument to a stable logical identity. */
[[nodiscard]] inline std::optional<MotionIdentity> resolve_motion_identity(
    std::span<const std::byte> component,std::uint64_t componentAddress,std::uint64_t stateAddress,
    std::uint32_t componentHandle,std::uint32_t entity) noexcept {
    for(unsigned slot=0;slot<3;++slot) {
        const auto identity=resolve_motion_slot(component,componentAddress,componentHandle,entity,slot);
        if(identity && componentAddress+identity->stateOffset==stateAddress) { return identity; }
    }
    return std::nullopt;
}

/** F44C40 constructs these fields; F49DA0 sets the active opcode.
 * Applies only to the update ABI (handler,updateContext,rawState). */
[[nodiscard]] inline bool validate_update_context(std::span<const std::byte> context,
    std::uint64_t componentAddress,std::uint32_t componentHandle) noexcept {
    return context.size()>=kMotionUpdateContextBytes && componentAddress!=0 && componentHandle!=UINT32_MAX
        && componentAddress<=std::numeric_limits<std::uint64_t>::max()-0x150
        && detail::read_unsigned(context,0x38,8)==componentAddress+0x150
        && detail::read_unsigned(context,0x40,4)==componentHandle
        && detail::read_unsigned(context,0x50,4)==0x3D;
}

enum class Stage : std::uint8_t { departure=1,relocation=2,arrival=3 };
struct MotionReceipt final {
    bool valid{};
    Stage stage{Stage::departure};
    bool nativeContinues{};
};

/** The state is a raw 0xF0 motion allocation, with no component prefix.
 * Caller must verify the handler's vtable is native1C40050, plus the current
 * character/selector ownership. This decoder resolves no process pointers. */
[[nodiscard]] inline MotionReceipt parse_motion(std::span<const std::byte> state,
    const Owner& owner,bool nativeContinues) noexcept {
    if(state.size()<0xF0 || !owner.valid()) { return {}; }
    const auto u32=[&](std::size_t offset) {
        std::uint32_t value{};
        for(unsigned i=0;i<4;++i) { value|=std::to_integer<std::uint32_t>(state[offset+i])<<(8*i); }
        return value;
    };
    if(u32(0x30)!=owner.entity || u32(0x38)!=owner.movementBody || u32(0x50)!=owner.interfaceHandle
        || u32(0x58)!=owner.selector || u32(0x60)!=0 || u32(0x64)!=0
        || u32(0x6C)!=0x3D || u32(0xA0)!=0xB6622307U) { return {}; }
    const auto stage=std::to_integer<unsigned>(state[0xA4]);
    if(stage<1 || stage>3) { return {}; }
    return {true,static_cast<Stage>(stage),nativeContinues};
}

enum class CyclePhase : std::uint8_t { idle,claimed,departing,relocating,arriving,finished,completed,interrupted };
enum class CycleEvent : std::uint8_t { none,started,relocated,arriving,finished,completed,interrupted };

/** Records one native movement lifecycle. A terminal update must precede
 * cleanup: selector inactive alone also occurs on cancellation. The final-arena
 * selector can finish at stage2 after placement; earlier moves require stage3.
 * Native placement must be bound separately: selector requested/placed points
 * identify the authored request and the resulting raw motion target. The actual
 * entity position is independently supplied at cleanup. All serials belong to
 * this request and increase across update and cleanup observations. motionId is
 * MotionIdentity::stable_id(), never the raw allocation's movable address. */
class CycleTracker final {
public:
    void begin_run(std::uint64_t run) noexcept {
        if(run==run_) { return; }
        *this={};run_=run;
    }
    [[nodiscard]] bool claim(const Owner& owner,std::uint64_t requestId,bool selectorInactive,
        const std::array<float,3>& destination,float positionTolerance) noexcept {
        if(!owner.valid() || owner.run!=run_ || requestId==0 || requestId<=requestId_
            || !selectorInactive || !finite(destination) || !std::isfinite(positionTolerance)
            || positionTolerance<0 || active()) { return false; }
        owner_=owner;requestId_=requestId;destination_=destination;tolerance_=positionTolerance;
        phase_=CyclePhase::claimed;motionId_=0;lastSerial_=0;destinationBound_=false;placedDestination_={};return true;
    }
    /** Native10D0020 may adjust the authored point. Bind that result only when
     * the owned selector still identifies our exact request and its placed XYZ
     * agrees with the raw motion transform consumed by F4AAB0. Caller separately
     * validates selector ownership and W=1 on all native point records.
     * Assignment is claim-only; identical repeats during the same active cycle
     * are idempotent so repeated stage1 updates need no second policy path. */
    [[nodiscard]] bool bind_destination(const Owner& owner,std::uint64_t requestId,
        std::uint64_t motionId,const std::array<float,3>& requested,
        const std::array<float,3>& placed,const std::array<float,3>& rawPlaced) noexcept {
        if(owner!=owner_ || owner.run!=run_ || requestId!=requestId_ || motionId==0
            || !active() || !finite(requested) || requested!=destination_
            || !finite(placed) || !finite(rawPlaced) || placed!=rawPlaced) { return false; }
        if(destinationBound_) { return motionId==motionId_ && placed==placedDestination_; }
        if(phase_!=CyclePhase::claimed) { return false; }
        motionId_=motionId;placedDestination_=placed;destinationBound_=true;return true;
    }
    [[nodiscard]] CycleEvent observe(const Owner& owner,std::uint64_t requestId,
        std::uint64_t serial,std::uint64_t motionId,const MotionReceipt& receipt) noexcept {
        if(!destinationBound_ || motionId!=motionId_ || !accept(owner,requestId,serial)
            || motionId==0 || !receipt.valid) { return CycleEvent::none; }
        if(phase_==CyclePhase::claimed) {
            if(receipt.stage!=Stage::departure || !receipt.nativeContinues) { return interrupt(); }
            phase_=CyclePhase::departing;return CycleEvent::started;
        }
        if(motionId!=motionId_) { return CycleEvent::none; }
        if(phase_==CyclePhase::departing) {
            if(receipt.stage==Stage::departure && receipt.nativeContinues) { return CycleEvent::none; }
            // Native10B0030 places through F4AAB0 before its stage2 selector
            // callback. The final-arena callback may end the motion there.
            // This is only a terminal-update receipt: cleanup must still prove
            // the exact owned selector is inactive and the boss is at the
            // independently bound native placement before releasing traversal.
            if(owner_.island==4 && receipt.stage==Stage::relocation && !receipt.nativeContinues) {
                phase_=CyclePhase::finished;return CycleEvent::finished;
            }
            if(receipt.stage==Stage::relocation && receipt.nativeContinues) {
                phase_=CyclePhase::relocating;return CycleEvent::relocated;
            }
            return interrupt();
        }
        if(phase_==CyclePhase::relocating) {
            if(receipt.stage==Stage::relocation && receipt.nativeContinues) { return CycleEvent::none; }
            if(receipt.stage!=Stage::arrival) { return interrupt(); }
            phase_=receipt.nativeContinues ? CyclePhase::arriving : CyclePhase::finished;
            return receipt.nativeContinues ? CycleEvent::arriving : CycleEvent::finished;
        }
        if(phase_==CyclePhase::arriving) {
            if(receipt.stage!=Stage::arrival) { return interrupt(); }
            if(!receipt.nativeContinues) { phase_=CyclePhase::finished;return CycleEvent::finished; }
        }
        return CycleEvent::none;
    }
    [[nodiscard]] CycleEvent cleanup(const Owner& owner,std::uint64_t requestId,
        std::uint64_t serial,std::uint64_t motionId,bool selectorInactive,
        const std::array<float,3>& actualPosition) noexcept {
        if(!destinationBound_ || !accept(owner,requestId,serial) || motionId==0 || motionId!=motionId_) {
            return CycleEvent::none;
        }
        if(phase_!=CyclePhase::finished || !selectorInactive || !finite(actualPosition)) { return interrupt(); }
        double distanceSquared{};
        for(std::size_t i=0;i<3;++i) {
            const double difference=static_cast<double>(actualPosition[i])-placedDestination_[i];
            distanceSquared+=difference*difference;
        }
        if(distanceSquared>static_cast<double>(tolerance_)*tolerance_) { return interrupt(); }
        phase_=CyclePhase::completed;return CycleEvent::completed;
    }
    [[nodiscard]] CyclePhase phase() const noexcept { return phase_; }
    [[nodiscard]] const Owner& owner() const noexcept { return owner_; }
    [[nodiscard]] std::uint64_t request_id() const noexcept { return requestId_; }
    [[nodiscard]] std::uint64_t motion_id() const noexcept { return motionId_; }
private:
    [[nodiscard]] static bool finite(const std::array<float,3>& values) noexcept {
        for(const auto value:values) { if(!std::isfinite(value)) { return false; } }return true;
    }
    [[nodiscard]] bool active() const noexcept {
        return phase_==CyclePhase::claimed || phase_==CyclePhase::departing
            || phase_==CyclePhase::relocating || phase_==CyclePhase::arriving || phase_==CyclePhase::finished;
    }
    [[nodiscard]] bool accept(const Owner& owner,std::uint64_t requestId,std::uint64_t serial) noexcept {
        if(owner!=owner_ || owner.run!=run_ || requestId!=requestId_ || serial==0 || serial<=lastSerial_
            || !active()) { return false; }
        lastSerial_=serial;return true;
    }
    [[nodiscard]] CycleEvent interrupt() noexcept { phase_=CyclePhase::interrupted;return CycleEvent::interrupted; }
    Owner owner_{};
    std::array<float,3> destination_{},placedDestination_{};
    std::uint64_t run_{},requestId_{},lastSerial_{},motionId_{};
    float tolerance_{};
    bool destinationBound_{};
    CyclePhase phase_{CyclePhase::idle};
};

} // namespace dawn::state::activity::omega_boss_teleport
