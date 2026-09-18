#pragma once

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"

namespace dawn::server::bap::encrypted::push::activity::omega_opening_publication {
namespace wire = middleware::bap::activity_message::sensor_auth_update;

// Eligibility includes the exact Omega destination, native runtime, admitted
// roster and in-world arrival. Appearance starts in the Lighthouse on arrival;
// the separate retained approach event advances that same scene afterward.
[[nodiscard]] inline bool bootstrap(wire::Snapshot& snapshot, std::uint8_t& stage,
                                    bool eligible, int region, bool entrance) noexcept {
    if (!eligible) return false;
    if (stage == wire::kOmegaOpeningStageNone) {
        if (region != 120 || entrance) return false;
        snapshot.initializeMissionAuthorityRuntime = true;
        stage = wire::kOmegaOpeningStageBaseline;
    } else if (stage == wire::kOmegaOpeningStageBaseline
               && snapshot.omegaSceneAuthority && wire::kOmegaSceneAuthorityBodyReady) {
        snapshot.omegaOpeningStage = stage = wire::kOmegaOpeningStageScene;
    } else if (stage == wire::kOmegaOpeningStageScene) {
        snapshot.omegaOpeningStage = stage = wire::kOmegaOpeningStageReady;
    } else {
        return false;
    }
    return true;
}
} // namespace dawn::server::bap::encrypted::push::activity::omega_opening_publication
