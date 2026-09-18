#pragma once

#include "settings_state.h"

namespace dawn::state::account::settings {

/** Update the current account only if its identity and previous settings still match.
 * The database commits before process memory changes. Repeated saves do not write again.
 */
[[nodiscard]] bool save(std::uint64_t accountSoid, const AccountSettings& before,
                        const AccountSettings& after, bool& changed) noexcept;

} // namespace dawn::state::account::settings
