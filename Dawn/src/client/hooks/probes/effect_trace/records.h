#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dawn::client::hooks::probes::effect_trace {

/** Raw evidence is deliberately not assigned an ammo/element enum without a verified consumer. */
template <std::size_t Size> struct Region final {
    std::uintptr_t address{};
    std::array<std::byte, Size> bytes{};
    bool readable{};

    template <typename T> [[nodiscard]] T value(std::size_t offset) const noexcept {
        T result{};
        if (readable && offset <= Size && sizeof(T) <= Size - offset) {
            std::memcpy(&result, bytes.data() + offset, sizeof(T));
        }
        return result;
    }
};

/** Embedded components refer to a runtime RESOURCE handle, not a world-object handle. */
template <std::size_t Size>
[[nodiscard]] bool component_header_matches(const Region<Size>& header,
                                            std::uint32_t schema, std::uint32_t object) noexcept {
    return Size >= 0x30 && header.readable && object != 0xFFFFFFFFU
           && header.template value<std::uint32_t>(0x20) == schema
           && header.template value<std::uint32_t>(0x28) == object;
}

/** One firing call: the weapon controller, its content, and its magazine at that moment. */
struct FireContext final {
    std::uint64_t tick{};
    std::uint32_t thread{};
    std::uint32_t object{0xFFFFFFFFU};
    std::uint32_t resource{0xFFFFFFFFU};
    std::uintptr_t barrel{};
    std::uintptr_t controller{};
    Region<0x30> controllerHeader{};
    Region<0x380> controllerState{}; // +8C0..+C40, including combat attribute +C34.
    Region<0xA0> weaponContent{};
    Region<0x270> weaponContentDefinition{};
    Region<0x240> magazine{};
    Region<0x250> magazineDefinition{};
    bool identityValid{};
};

/** One native projectile initialization, before and after the native call. */
struct NativeLaunch final {
    std::uint64_t tick{};
    std::uint32_t thread{};
    std::uint32_t object{0xFFFFFFFFU};
    std::uint32_t resource{0xFFFFFFFFU};
    std::uintptr_t callerRva{};
    Region<0xB0> parameters{};
    Region<0x1E0> before{};
    Region<0x1E0> after{};
    Region<0xE0> definition{};
    std::array<Region<0x20>, 4> trajectories{}; // Position +40, velocity +50.
    std::size_t trajectoryCount{};
    bool succeeded{};
    bool identityValid{};
};

/** Native velocity units are not yet authenticated as metres/second. */
[[nodiscard]] inline bool finite(const std::array<float, 3>& vector) noexcept {
    return std::isfinite(vector[0]) && std::isfinite(vector[1]) && std::isfinite(vector[2]);
}

[[nodiscard]] inline double length(const std::array<float, 3>& vector) noexcept {
    return std::hypot(static_cast<double>(vector[0]), static_cast<double>(vector[1]),
                      static_cast<double>(vector[2]));
}

} // namespace dawn::client::hooks::probes::effect_trace
