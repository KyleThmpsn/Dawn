#pragma once
#include <Windows.h>
namespace dawn::core::ui::modules::loadout {
bool initialize() noexcept;
void shutdown() noexcept;
void draw() noexcept;
/**
 * Writes the copy the editor queued, if any, to the Windows clipboard.
 * @param owner Game output window, called on its own thread once the presentation locks unwind.
 */
void dispatch_pending_copy(HWND owner) noexcept;
}
