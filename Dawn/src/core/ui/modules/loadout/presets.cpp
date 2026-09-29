// SPDX-License-Identifier: GPL-3.0-only
#include "presets.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../filesystem/path.h"

namespace dawn::core::ui::modules::loadout::presets {
namespace {

/** The loadouts file, in the Dawn folder beside `hud.json` and `player.json`. */
constexpr std::wstring_view kFileName = L"loadouts.json";
/** 1 MiB holds thousands of loadouts, far past what a player keeps. A larger file is not read. */
constexpr std::size_t kFileCapacity = std::size_t{1} << 20;
/** 128 bytes hold the longest field the writer formats: one item with its slot, id and hash. */
constexpr std::size_t kFieldCapacity = 128;
/** A definition hash is 32 bits, and an ability entry 8, so a larger number is not one. */
constexpr std::uint64_t kLargestDefinition = 0xFFFFFFFFULL;
constexpr std::uint64_t kLargestEntry = 0xFFULL;
/** An item level is a signed 32-bit number, and an item carries at most this many socket lanes. */
constexpr std::uint64_t kLargestLevel = 0x7FFFFFFFULL;
constexpr std::size_t kPlugLimit = state::account::inventory::kPlugCapacity;
/** A mark some editors put before a UTF-8 file's first line, which holds nothing of the file's own. */
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
/** The key the list is written under, on the line that opens it. */
constexpr std::string_view kListKey = "\"loadouts\"";

/** Moves past spaces and tabs. */
void skip_space(std::string_view text, std::size_t& at) noexcept {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
        ++at;
    }
}

/**
 * Finds a key at or after `at` and leaves `at` on the value after its colon.
 * Searching forward from the previous value, rather than the whole line, is what keeps a name
 * that happens to spell a key from being read as one.
 * @param key The key with its quotes, such as `"name"`.
 */
[[nodiscard]] bool seek_key(std::string_view text, std::size_t& at, std::string_view key) noexcept {
    const std::size_t found = text.find(key, at);
    if (found == std::string_view::npos) {
        return false;
    }
    std::size_t colon = found + key.size();
    skip_space(text, colon);
    if (colon >= text.size() || text[colon] != ':') {
        return false;
    }
    at = colon + 1;
    skip_space(text, at);
    return true;
}

/** Reads four hex digits at `at` as one UTF-16 unit. */
[[nodiscard]] bool read_unit(std::string_view text, std::size_t at, std::uint32_t& unit) noexcept {
    if (at + 4 > text.size()) {
        return false;
    }
    unit = 0;
    for (std::size_t i = at; i < at + 4; ++i) {
        const char c = text[i];
        const int nibble = c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : -1;
        if (nibble < 0) {
            return false;
        }
        unit = (unit << 4) | static_cast<std::uint32_t>(nibble);
    }
    return true;
}

/** Appends one code point as UTF-8, which is what the interface draws names in. */
void put_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

/** Reads one JSON string at `at`, undoing its escapes, and leaves `at` past its closing quote. */
[[nodiscard]] bool read_string(std::string_view text, std::size_t& at, std::string& out) {
    out.clear();
    if (at >= text.size() || text[at] != '"') {
        return false;
    }
    for (++at; at < text.size(); ++at) {
        const char c = text[at];
        if (c == '"') {
            ++at;
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++at >= text.size()) {
            return false;
        }
        switch (text[at]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            std::uint32_t code = 0;
            if (!read_unit(text, at + 1, code)) {
                return false;
            }
            at += 4;
            // A surrogate pair spells one code point past the basic plane.
            std::uint32_t low = 0;
            if (code >= 0xD800 && code <= 0xDBFF && at + 6 < text.size() && text[at + 1] == '\\'
                && text[at + 2] == 'u' && read_unit(text, at + 3, low) && low >= 0xDC00
                && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                at += 6;
            }
            put_utf8(out, code);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

/** Reads one unsigned decimal number. */
[[nodiscard]] bool read_number(std::string_view text, std::size_t& at, std::uint64_t& out) noexcept {
    const std::size_t begin = at;
    out = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        const auto digit = static_cast<std::uint64_t>(text[at] - '0');
        if (out > (UINT64_MAX - digit) / 10) {
            return false;
        }
        out = (out * 10) + digit;
        ++at;
    }
    return at != begin;
}

/**
 * Reads one 64-bit id written as a hex string, such as "0x4000000000000001".
 * Ids are written as strings because they are 64 bits wide, which a JSON number cannot hold
 * exactly for every reader.
 */
[[nodiscard]] bool read_id(std::string_view text, std::size_t& at, std::uint64_t& out) {
    std::string value;
    if (!read_string(text, at, value)) {
        return false;
    }
    std::string_view digits(value);
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
    }
    if (digits.empty() || digits.size() > 16) {
        return false;
    }
    out = 0;
    for (const char c : digits) {
        const int nibble = c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : -1;
        if (nibble < 0) {
            return false;
        }
        out = (out << 4) | static_cast<std::uint64_t>(nibble);
    }
    return true;
}

/** Reads one list of plug hashes, such as `[123, 0, 456]`, and leaves `at` past its bracket. */
[[nodiscard]] bool read_plugs(std::string_view text, std::size_t& at, std::vector<std::uint32_t>& plugs) {
    plugs.clear();
    if (at >= text.size() || text[at] != '[') {
        return false;
    }
    ++at;
    for (;;) {
        skip_space(text, at);
        if (at < text.size() && text[at] == ']') {
            ++at;
            return true;
        }
        if (!plugs.empty()) {
            if (at >= text.size() || text[at] != ',') {
                return false;
            }
            ++at;
            skip_space(text, at);
        }
        std::uint64_t hash = 0;
        if (plugs.size() >= kPlugLimit || !read_number(text, at, hash) || hash > kLargestDefinition) {
            return false;
        }
        plugs.push_back(static_cast<std::uint32_t>(hash));
    }
}

/** @return True when text holds nothing but spaces and the braces, brackets and commas JSON sets around values. */
[[nodiscard]] bool only_punctuation(std::string_view text) noexcept {
    return text.find_first_not_of(" \t\r{}[],") == std::string_view::npos;
}

/**
 * @return True for a line that holds none of a loadout: blank, or only what the list is written
 * between, with the list's own key where it opens.
 */
[[nodiscard]] bool frames_list(std::string_view line) noexcept {
    std::size_t at = 0;
    const std::size_t key = line.find(kListKey);
    if (key != std::string_view::npos) {
        at = key;
        if (!only_punctuation(line.substr(0, key)) || !seek_key(line, at, kListKey)) {
            return false;
        }
    }
    return only_punctuation(line.substr(at));
}

/**
 * Reads the loadout one line holds. The writer puts each loadout on a line of its own with its
 * fields in a fixed order, so each field is searched for after the one before it.
 * @return False for a line that does not hold exactly one whole loadout, such as one a hand edit
 * broke or one holding a second loadout after the first, which the file could not be saved without.
 */
[[nodiscard]] bool read_loadout(std::string_view line, edit::SavedLoadout& out) {
    std::size_t at = 0;
    if (!seek_key(line, at, "\"character\"") || !read_id(line, at, out.character)
        || !seek_key(line, at, "\"name\"") || !read_string(line, at, out.name)
        || !seek_key(line, at, "\"abilities\"") || at >= line.size() || line[at] != '[') {
        return false;
    }
    ++at;
    for (std::size_t lane = 0; lane < out.abilities.size(); ++lane) {
        skip_space(line, at);
        if (lane != 0) {
            if (at >= line.size() || line[at] != ',') {
                return false;
            }
            ++at;
            skip_space(line, at);
        }
        std::uint64_t entry = 0;
        if (!read_number(line, at, entry) || entry > kLargestEntry) {
            return false;
        }
        out.abilities[lane] = static_cast<std::uint8_t>(entry);
    }
    if (!seek_key(line, at, "\"items\"") || at >= line.size() || line[at] != '[') {
        return false;
    }
    ++at;
    // An item holds numbers, one id and one list of numbers, and no object of its own, so the first
    // closing brace after it opens is its end, and every search stays inside that one item.
    for (;;) {
        skip_space(line, at);
        if (at < line.size() && line[at] == ',') {
            ++at;
            skip_space(line, at);
        }
        if (at >= line.size()) {
            return false;
        }
        // Only the closing of the loadout and the list may follow its items.
        if (line[at] == ']') {
            return only_punctuation(line.substr(at + 1));
        }
        const std::size_t close = line[at] == '{' ? line.find('}', at) : std::string_view::npos;
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view item = line.substr(0, close);
        std::size_t cursor = at;
        std::uint64_t slot = 0;
        std::uint64_t instance = 0;
        std::uint64_t definition = 0;
        if (!seek_key(item, cursor, "\"slot\"") || !read_number(item, cursor, slot)
            || !seek_key(item, cursor, "\"instance\"") || !read_id(item, cursor, instance)
            || !seek_key(item, cursor, "\"definition\"") || !read_number(item, cursor, definition)
            || slot >= out.pieces.size() || definition > kLargestDefinition) {
            return false;
        }
        edit::SavedPiece piece{instance, static_cast<std::uint32_t>(definition), 0, {}};
        // The level and the plugs came later, so a loadout saved before them reads without them.
        std::uint64_t level = 0;
        if (seek_key(item, cursor, "\"level\"")) {
            if (!read_number(item, cursor, level) || level > kLargestLevel) {
                return false;
            }
            piece.level = static_cast<std::int32_t>(level);
        }
        if (seek_key(item, cursor, "\"plugs\"") && !read_plugs(item, cursor, piece.plugs)) {
            return false;
        }
        out.pieces[slot] = std::move(piece);
        at = close + 1;
    }
}

/** Writes one string as JSON, escaping the quote, the backslash and every control character. */
void write_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            // Escaped so a name can never break the one line its loadout is written on.
            char escaped[8]{};
            (void)std::snprintf(escaped, sizeof escaped, "\\u%04X", static_cast<unsigned>(byte));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}

/**
 * @return True when a loadouts file is there with something in it. A failed read cannot tell a
 * missing file, which is the ordinary first run, from one there is no reading, which must be kept.
 */
[[nodiscard]] bool holds_content() noexcept {
    const auto file = std::make_unique<core::path::Buffer>();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return core::path::artifact_file(kFileName, *file)
           && GetFileAttributesExW(file->chars.data(), GetFileExInfoStandard, &data) != FALSE
           && (data.nFileSizeHigh != 0 || data.nFileSizeLow != 0);
}

} // namespace

bool load(std::vector<edit::SavedLoadout>& loadouts) noexcept {
    loadouts.clear();
    std::vector<char> text(kFileCapacity);
    if (!core::path::read_artifact_text(kFileName, text)) {
        return !holds_content();
    }
    // The buffer starts zeroed, so anything but zeros after the first one is a NUL inside the file,
    // which would end the text there and leave the rest of the file out of every later save.
    const auto stop = std::find(text.begin(), text.end(), '\0');
    if (std::any_of(stop, text.end(), [](char c) { return c != '\0'; })) {
        return false;
    }
    std::string_view document(text.data(), static_cast<std::size_t>(stop - text.begin()));
    if (document.starts_with(kByteOrderMark)) {
        document.remove_prefix(kByteOrderMark.size());
    }
    // Every line is read, so the sheet still offers whatever reads, but a line that is neither one
    // whole loadout nor the list around them leaves the file unwritable: saving writes the list
    // whole, so the next save would drop that line for good.
    bool whole = true;
    for (std::size_t begin = 0; begin < document.size();) {
        std::size_t end = document.find('\n', begin);
        if (end == std::string_view::npos) {
            end = document.size();
        }
        const std::string_view line = document.substr(begin, end - begin);
        edit::SavedLoadout loadout;
        if (read_loadout(line, loadout)) {
            loadouts.push_back(std::move(loadout));
        } else if (!frames_list(line)) {
            whole = false;
        }
        begin = end + 1;
    }
    return whole;
}

bool save(const std::vector<edit::SavedLoadout>& loadouts) noexcept {
    std::string document = "{\n  \"loadouts\": [\n";
    char field[kFieldCapacity]{};
    for (std::size_t index = 0; index < loadouts.size(); ++index) {
        const edit::SavedLoadout& loadout = loadouts[index];
        (void)std::snprintf(field,
                            sizeof field,
                            "    {\"character\": \"0x%016llX\", \"name\": ",
                            static_cast<unsigned long long>(loadout.character));
        document += field;
        write_string(document, loadout.name);
        document += ", \"abilities\": [";
        for (std::size_t lane = 0; lane < loadout.abilities.size(); ++lane) {
            (void)std::snprintf(field,
                                sizeof field,
                                "%s%u",
                                lane == 0 ? "" : ", ",
                                static_cast<unsigned>(loadout.abilities[lane]));
            document += field;
        }
        document += "], \"items\": [";
        bool first = true;
        for (std::size_t slot = 0; slot < loadout.pieces.size(); ++slot) {
            const edit::SavedPiece& piece = loadout.pieces[slot];
            if (piece.instance == 0) {
                continue;
            }
            (void)std::snprintf(field,
                                sizeof field,
                                "%s{\"slot\": %zu, \"instance\": \"0x%016llX\", \"definition\": %lu, "
                                "\"level\": %ld, \"plugs\": [",
                                first ? "" : ", ",
                                slot,
                                static_cast<unsigned long long>(piece.instance),
                                static_cast<unsigned long>(piece.definition),
                                static_cast<long>((std::max)(piece.level, std::int32_t{0})));
            document += field;
            for (std::size_t lane = 0; lane < piece.plugs.size(); ++lane) {
                (void)std::snprintf(field,
                                    sizeof field,
                                    "%s%lu",
                                    lane == 0 ? "" : ", ",
                                    static_cast<unsigned long>(piece.plugs[lane]));
                document += field;
            }
            document += "]}";
            first = false;
        }
        document += index + 1 == loadouts.size() ? "]}\n" : "]},\n";
    }
    document += "  ]\n}\n";
    return core::path::write_artifact_text(kFileName, document);
}

} // namespace dawn::core::ui::modules::loadout::presets
