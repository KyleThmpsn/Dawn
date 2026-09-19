// SPDX-License-Identifier: GPL-3.0-only
#include <imgui.h>

#include "../internal.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

/** Shown when the roll reached every target exactly. */
constexpr const char* kExactResult = "Armor stat targets reached.";
/** Shown when the closest supported roll fell short of one or more targets. */
constexpr const char* kClosestResult = "Closest supported roll applied.";
/** Shown when the item has no stat plug the editor may move. */
constexpr const char* kNoAdjustableResult = "This armor has no adjustable stat plugs.";

} // namespace

edit::Stats& stat_targets_for(const edit::Item& item) noexcept {
    Model& state = model();
    // Targets follow whichever item the inspector is showing, so a new item starts from its roll.
    if (state.targets.owner != item.instanceSoid) {
        state.targets.values = edit::item_stats(item, state.catalog);
        state.targets.owner = item.instanceSoid;
    }
    return state.targets.values;
}

void settle_stat_targets(edit::Item& item, bool released) noexcept {
    Model& state = model();
    const edit::Stats current = edit::item_stats(item, state.catalog);
    if (!released || state.targets.values == current) {
        return;
    }
    // Letting go of a bar is the ask. The closest roll the sockets support is applied at once,
    // and the bars settle on whatever it reached, so a stat the plugs cannot give is never left
    // standing as though it had been.
    edit::Stats achieved{};
    if (!edit::adjust_stats(item, state.catalog, state.targets.values, achieved)) {
        state.status = kNoAdjustableResult;
        state.targets.values = current;
        return;
    }
    state.status = achieved == state.targets.values ? kExactResult : kClosestResult;
    // `mark_changed` drops the target ownership, so both are put back once it has run.
    mark_changed();
    state.targets.values = achieved;
    state.targets.owner = item.instanceSoid;
}

} // namespace dawn::core::ui::modules::loadout::internal
