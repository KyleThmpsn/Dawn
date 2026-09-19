#pragma once

#include <Windows.h>

#include <cstddef>

namespace dawn::core::ui::fonts::installed {

/** Borrowed view over the fixed Dawn-owned installed-font bytes. */
struct DataView {
    void* bytes{};
    int byteCount{};
};

/**
 * Reads the optional face from the process directory, then one module directory.
 * @param module Loaded fallback module, or null to use only the process image.
 * @param output Receives bytes that stay valid until clear is called.
 * @return True when the whole checked OpenType face fits fixed storage.
 */
[[nodiscard]] bool load(HMODULE module, DataView& output) noexcept;

/**
 * Reads the game's symbol face, whose glyphs the installed strings embed by code point.
 * @param module Loaded fallback module, or null to use only the process image.
 * @param output Receives bytes that stay valid until clear is called.
 * @return True when the whole checked OpenType face fits fixed storage.
 */
[[nodiscard]] bool load_symbols(HMODULE module, DataView& output) noexcept;

/** The heavier cuts the install ships beside the regular UI face. */
enum class Weight {
    /** The medium cut of the same family, which an item title is set in. */
    medium,
    /** The display bold, a face drawn for figures set large rather than for running text. */
    displayBold,
};

/**
 * Reads one of the heavier cuts, each kept in storage of its own.
 * @param module Loaded fallback module, or null to use only the process image.
 * @param weight Cut to read.
 * @param output Receives bytes that stay valid until clear is called.
 * @return True when the whole checked OpenType face fits fixed storage.
 */
[[nodiscard]] bool load_weight(HMODULE module, Weight weight, DataView& output) noexcept;

/** Wipes the installed-font bytes and all path scratch storage. */
void clear() noexcept;

/** @return Number of installed-font bytes kept for the active atlas. */
[[nodiscard]] std::size_t byte_count() noexcept;

} // namespace dawn::core::ui::fonts::installed
