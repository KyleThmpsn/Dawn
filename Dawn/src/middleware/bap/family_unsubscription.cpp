#include "family_unsubscription.h"

#include "../encoding/byte_order.h"

namespace dawn::middleware::bap::family_unsubscription {
namespace {

/** Native F2C0D0 submits the requested family as the first byte of service 14. */
constexpr std::size_t kFamilyTypeOffset = 0;
/** The 8-byte big-endian root id follows the family selector. */
constexpr std::size_t kFamilyRootOffset = kFamilyTypeOffset + sizeof(std::uint8_t);
/** The smallest svc-14 body ends after the root id. */
constexpr std::size_t kBodySize = kFamilyRootOffset + encoding::kU64Size;

} // namespace

/** Decodes one fixed authenticated family-unsubscription request. */
bool parse(std::span<const std::byte> input, Request& request) noexcept {
    request = {};
    // This size check covers only the two reads below. It is not a rule about the client's body.
    if (input.size() < kBodySize) {
        return false;
    }
    request.familyType = std::to_integer<std::uint8_t>(input[kFamilyTypeOffset]);
    request.familyRootSoid = encoding::read_u64_be(std::span<const std::byte, encoding::kU64Size>(
        input.data() + kFamilyRootOffset, encoding::kU64Size));
    return true;
}

} // namespace dawn::middleware::bap::family_unsubscription
