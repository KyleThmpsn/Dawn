// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <vector>

#include "state/editor/edit.h"

namespace dawn::core::ui::modules::loadout::presets {

namespace edit = state::editor;

/**
 * Reads every saved loadout from the Dawn folder, beside the HUD's own settings.
 * Loadouts are the editor's own data rather than game state, so they are kept out of the player
 * database, whose schema the game and Sundial both depend on. A line that does not read as one
 * whole loadout is left out and the rest are still read, so a hand edit hides only the line it
 * broke, but the file then counts as unreadable: a save writes the list whole, and would drop it.
 * @param loadouts Receives the loadouts that read, empty when there is no file yet.
 * @return False when a file is there but could not be read in full. The caller must not save over
 * it then, which would replace every loadout it holds with only the ones read this session.
 */
[[nodiscard]] bool load(std::vector<edit::SavedLoadout>& loadouts) noexcept;

/**
 * Writes every saved loadout, replacing the file.
 * @param loadouts Loadouts to write, for every character.
 * @return False when the file could not be written.
 */
[[nodiscard]] bool save(const std::vector<edit::SavedLoadout>& loadouts) noexcept;

} // namespace dawn::core::ui::modules::loadout::presets
