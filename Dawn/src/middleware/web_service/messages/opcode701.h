#pragma once

#include "../web_service_envelope.h"
#include "../../../state/account/settings/settings_state.h"

namespace dawn::middleware::web_service::messages::opcode701 {

inline constexpr std::uint16_t kOpcode = 701;

/** Decode the native account update, merging only menu settings into the supplied snapshot.
 * Absent fields retain their previous values. A malformed request leaves output unchanged.
 * Other account-update fields are validated/consumed, never imported as player progress.
 */
[[nodiscard]] bool parse_settings(const Message& message,
    const state::account::settings::AccountSettings& before,
    state::account::settings::AccountSettings& output) noexcept;

} // namespace dawn::middleware::web_service::messages::opcode701
