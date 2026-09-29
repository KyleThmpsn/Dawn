#pragma once

#include "records.h"
#include "../../../content/handles/handle_resolver.h"

namespace dawn::client::hooks::probes::effect_trace {

struct ResourceIdentity final {
    std::uintptr_t address{};
    std::uint32_t schema{};
    Region<0x30> header{};
};

/** Profiled runtime-resource pool (975), NOT the world-object or package-content pool. */
inline bool resolve_resource(const content::handles::Source& source, std::uint32_t handle,
                             ResourceIdentity& output) noexcept {
    output = {};
    if (source.read == nullptr || handle == 0xFFFFFFFFU || (handle & 0x80000000U) != 0
        || ((handle >> 13) & 0x3FFU) != 975) { return false; }
    const auto read = [&]<std::size_t Size>(std::uintptr_t address, Region<Size>& region) {
        region.address = address;
        region.readable = address >= 0x10000 && address < 0x0000800000000000ULL - Size
            && source.read(source.context, address, region.bytes);
        return region.readable;
    };
    Region<8> slot{}, tables{};
    Region<64> descriptor{};
    if (!read(source.tablesSlot, slot) || !read(slot.value<std::uintptr_t>(0), tables)
        || !read(tables.value<std::uintptr_t>(0) + 975 * 64, descriptor)
        || descriptor.value<std::uint32_t>(0x30) != 32
        || descriptor.value<std::int32_t>(0x34) != -1) { return false; }
    Region<32> record{}, after{};
    const auto recordAddress = descriptor.value<std::uintptr_t>(8) + (handle & 0x1FFFU) * 32;
    if (!read(recordAddress, record)
        || (record.value<std::uint32_t>(0) & 0xFFFF0000U) == 0xFEFE0000U) { return false; }
    const auto address = recordAddress - record.value<std::uintptr_t>(8);
    Region<0x30> header{};
    if (!read(address, header) || header.value<std::uint32_t>(0x24) != handle
        || !read(recordAddress, after) || record.bytes != after.bytes) { return false; }
    output = {address, record.value<std::uint32_t>(0x18), header};
    return true;
}

} // namespace dawn::client::hooks::probes::effect_trace
