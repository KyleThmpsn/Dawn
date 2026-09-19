#include "ui_installed_font_reader.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <span>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../../filesystem/path.h"

namespace dawn::core::ui::fonts::installed {
namespace {

/** One KiB, the unit the fixed font size policy uses. */
constexpr std::size_t kKibibyteBytes = 1024;
/** 160 KiB holds the installed 143,564-byte face and small build drift. */
constexpr std::size_t kFontCapacityKibibytes = 160;
/** Total installed-font storage for one optional face. */
constexpr std::size_t kFontCapacityBytes = kFontCapacityKibibytes * kKibibyteBytes;
/** An OpenType tag is 4 file bytes. */
constexpr std::size_t kOpenTypeTagBytes = 4;
/** Each unsigned field in the OpenType offset table is 2 file bytes. */
constexpr std::size_t kOpenTypeFieldBytes = 2;
/** One OpenType table directory record is 16 bytes. */
constexpr std::size_t kOpenTypeTableRecordBytes = 16;
/** OpenType stores one byte as 8 bits, high to low. */
constexpr unsigned int kBitsPerByte = 8;
/** The installed CFF-based OpenType face starts with the standard OTTO tag. */
constexpr std::array<char, kOpenTypeTagBytes> kOpenTypeCffSignature{'O', 'T', 'T', 'O'};
/** The optional UI face lives in the game install's existing font directory. */
constexpr std::wstring_view kInstalledFontSuffix = L"fonts\\NeueHaasUnicaW1G-Regular.otf";
/**
 * The symbol face beside it carries the glyphs the game's own strings embed.
 * Installed strings reference these code points directly, so without the face they render
 * as replacement boxes. Neither file is bundled; both are read from the install.
 */
constexpr std::wstring_view kSymbolFontSuffix = L"fonts\\Destiny_Symbols_PC.otf";
/**
 * The heavier cuts the install ships beside the regular UI face.
 * Without them a heavier line has to be faked by striking the regular face again, which thickens
 * a glyph off one edge and eats the space beside it. The medium carries an item title. The power
 * figure takes the display bold: a display cut is drawn for sizes like that one, where a text
 * cut's looser fitting and lighter stroke read as thin.
 */
constexpr std::wstring_view kMediumFontSuffix = L"fonts\\NeueHaasUnicaW1G-Medium.otf";
constexpr std::wstring_view kDisplayBoldFontSuffix = L"fonts\\NHaasGroteskDSPro-75Bd.otf";

/** Byte arrays keep the file byte order of the OpenType offset-table fields. */
struct OpenTypeOffsetTable {
    std::array<char, kOpenTypeTagBytes> signature{};
    std::array<std::byte, kOpenTypeFieldBytes> tableCount{};
    std::array<std::byte, kOpenTypeFieldBytes> searchRange{};
    std::array<std::byte, kOpenTypeFieldBytes> entrySelector{};
    std::array<std::byte, kOpenTypeFieldBytes> rangeShift{};
};

/** The OpenType offset table is 12 bytes, before the table records. */
constexpr std::size_t kOpenTypeOffsetTableBytes = 12;
/** One offset table and one directory record are the smallest possible font file. */
constexpr std::size_t kMinimumFontBytes = kOpenTypeOffsetTableBytes + kOpenTypeTableRecordBytes;
static_assert(sizeof(OpenTypeOffsetTable) == kOpenTypeOffsetTableBytes);

alignas(std::max_align_t) std::array<std::byte, kFontCapacityBytes> g_fontBytes{};
std::size_t g_fontByteCount{};
/** The symbol face is kept separately so a missing one never costs the UI face. */
alignas(std::max_align_t) std::array<std::byte, kFontCapacityBytes> g_symbolBytes{};
std::size_t g_symbolByteCount{};
/** Each heavier cut is kept separately too, so a build without one still gets its regular face. */
alignas(std::max_align_t) std::array<std::byte, kFontCapacityBytes> g_mediumBytes{};
std::size_t g_mediumByteCount{};
alignas(std::max_align_t) std::array<std::byte, kFontCapacityBytes> g_displayBoldBytes{};
std::size_t g_displayBoldByteCount{};
core::path::Buffer g_pathScratch{};

/**
 * Finds the folder of the process image or of one loaded module.
 * @param module Null for the process image, or one loaded fallback module.
 * @return True when the whole folder and its trailing separator fit fixed storage.
 */
[[nodiscard]] bool resolve_image_directory(HMODULE module) noexcept {
    const DWORD copied = GetModuleFileNameW(
        module, g_pathScratch.chars.data(), static_cast<DWORD>(g_pathScratch.chars.size()));
    if (copied == 0 || copied >= g_pathScratch.chars.size()) {
        return false;
    }

    g_pathScratch.length = copied;
    while (g_pathScratch.length != 0 && g_pathScratch.chars[g_pathScratch.length - 1] != L'\\') {
        --g_pathScratch.length;
    }
    if (g_pathScratch.length == 0) {
        return false;
    }
    g_pathScratch.chars[g_pathScratch.length] = L'\0';
    return true;
}

/**
 * Decodes one 2-byte OpenType unsigned value.
 * @param bytes Big-endian field bytes.
 * @return The value in host order.
 */
[[nodiscard]] std::uint16_t
read_big_endian_u16(const std::array<std::byte, kOpenTypeFieldBytes>& bytes) noexcept {
    const auto high = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0]));
    const auto low = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1]));
    return static_cast<std::uint16_t>((high << kBitsPerByte) | low);
}

/**
 * Checks the fixed header before the bytes reach Dear ImGui's font parser.
 * @param bytes Complete candidate file bytes.
 * @return True for the expected CFF OpenType tag and a complete table directory.
 */
[[nodiscard]] bool valid_open_type(const std::byte* bytes, std::size_t byteCount) noexcept {
    if (bytes == nullptr || byteCount < sizeof(OpenTypeOffsetTable)) {
        return false;
    }

    OpenTypeOffsetTable header{};
    std::memcpy(&header, bytes, sizeof header);
    if (header.signature != kOpenTypeCffSignature) {
        return false;
    }

    const std::size_t tableCount = read_big_endian_u16(header.tableCount);
    if (tableCount == 0) {
        return false;
    }
    const std::size_t directoryBytes = sizeof header + (tableCount * kOpenTypeTableRecordBytes);
    return directoryBytes <= byteCount;
}

/**
 * Reads one open file only when its whole size fits fixed storage.
 * @param file Readable Windows file handle with a stable share mode.
 * @return True when one exact, valid OpenType file was read.
 */
[[nodiscard]] bool read_exact_font(HANDLE file, std::span<std::byte> storage, std::size_t& byteCountOut) noexcept {
    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(file, &fileSize) == FALSE || fileSize.QuadPart < 0) {
        return false;
    }

    const auto byteCount = static_cast<unsigned long long>(fileSize.QuadPart);
    if (byteCount < kMinimumFontBytes || byteCount > storage.size()
        || byteCount > static_cast<unsigned long long>((std::numeric_limits<DWORD>::max)())
        || byteCount > static_cast<unsigned long long>((std::numeric_limits<int>::max)())) {
        return false;
    }

    const DWORD expectedBytes = static_cast<DWORD>(byteCount);
    DWORD bytesRead{};
    if (ReadFile(file, storage.data(), expectedBytes, &bytesRead, nullptr) == FALSE
        || bytesRead != expectedBytes) {
        return false;
    }

    LARGE_INTEGER finalSize{};
    if (GetFileSizeEx(file, &finalSize) == FALSE || finalSize.QuadPart != fileSize.QuadPart
        || !valid_open_type(storage.data(), bytesRead)) {
        return false;
    }
    byteCountOut = bytesRead;
    return true;
}

/** Wipes the temporary absolute path after each open attempt. */
void clear_path_scratch() noexcept {
    SecureZeroMemory(&g_pathScratch, sizeof g_pathScratch);
}

/**
 * Tries one image-relative path. Bytes are kept only after a full read and close.
 * @param module Null for the process image, or one loaded fallback module.
 * @return True when the whole optional face was loaded.
 */
[[nodiscard]] bool try_load(HMODULE module, std::wstring_view suffix, std::span<std::byte> storage,
                           std::size_t& byteCountOut) noexcept {
    if (!resolve_image_directory(module) || !core::path::append(g_pathScratch, suffix)) {
        clear_path_scratch();
        return false;
    }

    const HANDLE file = CreateFileW(g_pathScratch.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    clear_path_scratch();
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool loaded = read_exact_font(file, storage, byteCountOut);
    const bool closed = CloseHandle(file) != FALSE;
    if (!loaded || !closed) {
        SecureZeroMemory(storage.data(), storage.size());
        byteCountOut = 0;
        return false;
    }
    return true;
}

/**
 * Loads one face from the process directory, then from one module directory.
 * @return True when either location yielded the whole checked face.
 */
[[nodiscard]] bool load_face(HMODULE module, std::wstring_view suffix, std::span<std::byte> storage,
                             std::size_t& byteCountOut, DataView& output) noexcept {
    output = {};
    SecureZeroMemory(storage.data(), storage.size());
    byteCountOut = 0;
    if (!try_load(nullptr, suffix, storage, byteCountOut)
        && (module == nullptr || !try_load(module, suffix, storage, byteCountOut))) {
        SecureZeroMemory(storage.data(), storage.size());
        byteCountOut = 0;
        clear_path_scratch();
        return false;
    }
    output.bytes = storage.data();
    output.byteCount = static_cast<int>(byteCountOut);
    return true;
}

} // namespace

/** Reads the optional face from the process directory, then one module directory. */
bool load(HMODULE module, DataView& output) noexcept {
    return load_face(module, kInstalledFontSuffix, g_fontBytes, g_fontByteCount, output);
}

/** Reads the game's symbol face, which the UI face is merged with. */
bool load_symbols(HMODULE module, DataView& output) noexcept {
    return load_face(module, kSymbolFontSuffix, g_symbolBytes, g_symbolByteCount, output);
}

/** Reads one of the heavier cuts of the UI face. */
bool load_weight(HMODULE module, Weight weight, DataView& output) noexcept {
    if (weight == Weight::medium) {
        return load_face(module, kMediumFontSuffix, g_mediumBytes, g_mediumByteCount, output);
    }
    return load_face(
        module, kDisplayBoldFontSuffix, g_displayBoldBytes, g_displayBoldByteCount, output);
}

/** Wipes the installed-font bytes and all path scratch storage. */
void clear() noexcept {
    SecureZeroMemory(g_fontBytes.data(), g_fontBytes.size());
    g_fontByteCount = 0;
    SecureZeroMemory(g_symbolBytes.data(), g_symbolBytes.size());
    g_symbolByteCount = 0;
    SecureZeroMemory(g_mediumBytes.data(), g_mediumBytes.size());
    g_mediumByteCount = 0;
    SecureZeroMemory(g_displayBoldBytes.data(), g_displayBoldBytes.size());
    g_displayBoldByteCount = 0;
    clear_path_scratch();
}

/** @return Number of installed-font bytes kept for the active atlas. */
std::size_t byte_count() noexcept {
    return g_fontByteCount;
}

} // namespace dawn::core::ui::fonts::installed
