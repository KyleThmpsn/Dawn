// SPDX-License-Identifier: GPL-3.0-only
// Selection scopes follow Sundial by KyleThmpsn. See vendor/sundial/NOTICE.md.
#include "catalog.h"
#include "../../../vendor/sundial/dummy_items.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dawn::state::editor {
/** @return The curve for one stat row inside a group, or null when the group does not scale it. */
const ScaledStat* scaled_stat(const Catalog& catalog, std::uint16_t groupIndex, std::uint16_t statRow) noexcept {
    if (groupIndex >= catalog.statGroups.size()) return nullptr;
    for (const auto& scaled : catalog.statGroups[groupIndex].scaled) {
        if (scaled.definitionIndex == statRow) return &scaled;
    }
    return nullptr;
}

std::int32_t display_stat(const Catalog& catalog, std::uint16_t groupIndex, std::uint16_t statRow,
                          std::int32_t investment) noexcept {
    const auto* scaled = scaled_stat(catalog, groupIndex, statRow);
    if (!scaled || scaled->curve.empty()) return investment;
    const auto& curve = scaled->curve;
    // An authored point wins outright; the curve is a lookup before it is an interpolation.
    for (const auto& point : curve) if (point.investment == investment) return point.display;
    // A linear stat keeps whatever the curve does not name, rather than clamping to an endpoint.
    if (scaled->linear) return investment;
    if (investment < curve.front().investment) return curve.front().display;
    if (investment > curve.back().investment) return curve.back().display;
    for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
        const auto& left = curve[i];
        const auto& right = curve[i + 1];
        if (investment < left.investment || investment > right.investment) continue;
        const std::int64_t span = std::int64_t(right.investment) - left.investment;
        if (span == 0) return left.display;
        const std::int64_t rise = std::int64_t(right.display) - left.display;
        const std::int64_t run = std::int64_t(investment) - left.investment;
        return static_cast<std::int32_t>(left.display + ((rise * run) / span));
    }
    return investment;
}

bool numeric_stat(const Catalog& catalog, std::uint16_t groupIndex, std::uint16_t statRow) noexcept {
    const auto* scaled = scaled_stat(catalog, groupIndex, statRow);
    return scaled && scaled->numeric;
}

/**
 * @return The element one item deals, or none when it carries no damage marker.
 * The installed build marks a weapon's element with a sandbox perk rather than a field. Six
 * indices name the fixed markers: an older trio and the one the modern sandbox uses. A weapon
 * carrying markers for more than one element switches at runtime, so it reports none.
 * @param detail Item detail carrying the sandbox perk list.
 */
Element element_of(const build_data::items::details::Definition& detail) noexcept {
    constexpr std::uint16_t kLegacyArc = 83, kLegacySolar = 84, kLegacyVoid = 85;
    constexpr std::uint16_t kModernArc = 449, kModernSolar = 450, kModernVoid = 451;
    Element found = Element::none;
    const std::size_t count = (std::min)(static_cast<std::size_t>(detail.sandboxPerkCount),
                                         detail.sandboxPerks.size());
    for (std::size_t i = 0; i < count; ++i) {
        Element marker = Element::none;
        switch (detail.sandboxPerks[i]) {
        case kLegacyArc: case kModernArc: marker = Element::arc; break;
        case kLegacySolar: case kModernSolar: marker = Element::solar; break;
        case kLegacyVoid: case kModernVoid: marker = Element::void_; break;
        default: continue;
        }
        // Two markers naming different elements mean the weapon chooses at runtime.
        if (found != Element::none && found != marker) return Element::none;
        found = marker;
    }
    return found;
}

std::string searchable(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
bool matches(const CatalogItem& item, const std::string& query) {
    std::size_t begin = 0;
    while (begin < query.size()) {
        const auto end = query.find(' ', begin);
        const auto word = query.substr(begin, end == std::string::npos ? end : end - begin);
        if (!word.empty() && item.search.find(word) == std::string::npos) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}
bool fits_class(const CatalogItem& item, CharacterClass characterClass) noexcept {
    return item.characterClass == 3 || item.characterClass == static_cast<std::uint8_t>(characterClass);
}
const CatalogItem* Catalog::find(std::uint32_t hash) const noexcept {
    const auto it = hashes.find(hash);
    return it == hashes.end() ? nullptr : &items[it->second];
}
const CatalogItem* Catalog::index(std::uint16_t id) const noexcept {
    const auto it = indices.find(id);
    return it == indices.end() ? nullptr : &items[it->second];
}
namespace {
void unique(std::vector<std::uint16_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}
}
void Catalog::finish() {
    hashes.clear(); indices.clear(); socketPools.clear(); plugs.clear();
    for (auto& pool : gearPools) pool.clear();
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        hashes[item.definition.definitionHash] = i;
        indices[item.definition.definitionIndex] = i;
        char hash[40]{};
        std::snprintf(hash, sizeof hash, "0x%08X %u", item.definition.definitionHash, item.definition.definitionHash);
        item.internal = item.internal || item.name.empty()
            || ((item.kind == GearKind::weapon || item.kind == GearKind::armor) && item.type.empty())
            || dummies::contains(item.definition.definitionHash);
        if (item.name.empty()) { item.unnamed = true; item.name = std::string(item.plug ? "Unnamed perk " : "Unnamed item ") + hash; }
        item.search = searchable(item.name + " " + item.type + " " + item.description + " " + hash);
        if (item.plug) plugs.push_back(item.definition.definitionIndex);
        for (std::size_t lane = 0; lane < item.detail.ordinarySocketCount; ++lane) {
            auto& pool = item.compatible[lane];
            unique(pool);
            const auto type = item.detail.socketTypes[lane];
            if (type != build_data::items::details::kUnavailableSocketType) {
                auto& socket = socketPools[type];
                socket.insert(socket.end(), pool.begin(), pool.end());
                auto& combined = socketPools[0x10000U + (static_cast<std::uint32_t>(item.kind) << 16U) + type];
                combined.insert(combined.end(), pool.begin(), pool.end());
            }
            auto& gear = gearPools[static_cast<std::size_t>(item.kind)];
            gear.insert(gear.end(), pool.begin(), pool.end());
        }
    }
    for (auto& [key, values] : socketPools) { (void)key; unique(values); }
    for (auto& pool : gearPools) unique(pool);
    // Pools also expose unnamed/internal plugs that declare no category of their own.
    for (const auto& pool : gearPools) plugs.insert(plugs.end(), pool.begin(), pool.end());
    unique(plugs);
    for (auto id : plugs) if (auto it = indices.find(id); it != indices.end()) items[it->second].plug = true;
}
std::vector<std::uint16_t> Catalog::candidates(const CatalogItem& item, std::size_t lane, PlugScope scope) const {
    if (lane >= item.detail.ordinarySocketCount || lane >= item.compatible.size()) return {};
    if (scope == PlugScope::all) return plugs;
    if (scope == PlugScope::compatible) return item.compatible[lane];
    if (scope == PlugScope::gear) return gearPools[static_cast<std::size_t>(item.kind)];
    const auto type = item.detail.socketTypes[lane];
    if (type == build_data::items::details::kUnavailableSocketType) return item.compatible[lane];
    const auto key = scope == PlugScope::socket ? type
        : 0x10000U + (static_cast<std::uint32_t>(item.kind) << 16U) + type;
    const auto it = socketPools.find(key);
    return it == socketPools.end() ? std::vector<std::uint16_t>{} : it->second;
}
} // namespace dawn::state::editor
