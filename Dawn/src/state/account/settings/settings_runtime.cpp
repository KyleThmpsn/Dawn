#include "settings_runtime.h"

#include "../../runtime/storage/internal.h"
#include "../../persistence/persistence.h"

namespace dawn::state::account::settings {

bool save(std::uint64_t accountSoid, const AccountSettings& before,
          const AccountSettings& after, bool& changed) noexcept {
    changed = false;
    if (accountSoid == 0 || !valid(after)) return false;
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& account = runtime::storage::g_state.account;
    bool result = account.primarySoid == accountSoid && account.settings == before;
    if (result && before != after) {
        result = persistence::commit_settings(after);
        if (result) {
            account.settings = after;
            changed = true;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

} // namespace dawn::state::account::settings
