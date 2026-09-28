#pragma once
// Installed package fixtures supply catalog data; account writes are isolated from the game.
#include "client/content/items/packages/internal.h"
#include "state/editor/edit.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <unordered_map>

namespace fixture {
namespace state = dawn::state;
namespace tables = dawn::middleware::content::packages::tables;
inline std::filesystem::path directory;
inline std::uint32_t globals{};
inline std::unordered_map<std::uint32_t, std::uint32_t> classes;
inline std::vector<tables::items::Row> rows;
inline std::vector<std::vector<std::byte>> blobs;
inline std::vector<std::byte> plugs;
inline std::vector<state::build_data::socket_entry_lists::Definition> lists;
inline std::unordered_map<std::uint16_t, state::build_data::socket_entry_lists::EntryTable> entryTables;
inline state::build_data::constants::InvestmentConstants constants;
inline auto account = std::make_unique<state::AccountState>();
inline unsigned checks{};
/**
 * Tags a read asked for that the fixtures do not hold. The extractor fetches them on its next pass,
 * which is how the fixtures keep up with a catalog that reads more of the packages than it did.
 */
inline std::set<std::uint32_t> missing;
/** Writes the tags the run asked for and could not find, one decimal tag a line, beside the fixtures. */
inline void write_missing() {
    std::ofstream output(directory / "missing.txt");
    for (const auto tag : missing) output << tag << '\n';
}
inline void check(bool value, const char* message) {
    ++checks;
    if (!value) { write_missing(); std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
inline std::vector<std::byte> read(std::uint32_t tag) {
    char name[32]{}; std::snprintf(name, sizeof name, "%08X.bin", tag);
    std::ifstream input(directory / name, std::ios::binary | std::ios::ate);
    if (!input) {
        if (tag >= 0x80800000U && tag < 0x82000000U) missing.insert(tag);
        return {};
    }
    const auto size = input.tellg(); input.seekg(0);
    std::vector<std::byte> output(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(output.data()), size); return output;
}
inline void load(const char* path) {
    directory = path; std::ifstream(directory / "globals.txt") >> globals;
    std::ifstream cls(directory / "classes.tsv"); std::uint32_t tag{}, type{};
    while (cls >> tag >> type) classes[tag] = type;
    auto root = read(globals); check(tables::child_tag(root, 0, tag), "fixture investment root"); root = read(tag);
    check(tables::slot_tag(root, 48, tag), "fixture item index"); auto index = read(tag); tables::Array array{};
    check(tables::find_array_at(index, 8, array), "fixture index array");
    rows.resize(static_cast<std::size_t>(array.count)); blobs.resize(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        tables::IndexRow entry{}; check(tables::index_row(index, array, i, entry), "item index row");
        blobs[i] = read(entry.targetTag);
        check(tables::items::read_definition(blobs[i], rows[i]), "production item decoder");
        rows[i].definitionHash = entry.definitionHash; rows[i].definitionIndex = static_cast<std::uint16_t>(i);
    }
    check(tables::slot_tag(root, 51, tag), "fixture socket pools"); plugs = read(tag);
    check(tables::slot_tag(root, 11, tag), "fixture constants"); const auto data = read(tag);
    constexpr std::size_t offsets[]{593,594,595,622,623,624};
    check(data.size() > 632, "installed constants bounds"); constants.extracted = true;
    for (std::size_t i = 0; i < 6; ++i) constants.characterStatRows[i] = std::to_integer<std::uint8_t>(data[8 + offsets[i]]);
    check(tables::slot_tag(root,97,tag), "fixture subclass table"); index = read(tag);
    check(tables::find_array_at(index,8,array), "subclass table array"); lists.resize(static_cast<std::size_t>(array.count));
    for (std::size_t i = 0; i < lists.size(); ++i) {
        tables::IndexRow row{}; check(tables::index_row(index,array,i,row), "subclass index row");
        auto& value = lists[i]; value.definitionIndex = static_cast<std::uint16_t>(i); value.definitionHash = row.definitionHash;
        const auto bytes = read(row.targetTag); tables::Array entries{};
        if (!tables::find_array_at(bytes,16,entries)) continue;
        value.entryCount = static_cast<std::uint8_t>(entries.count);
        state::build_data::socket_entry_lists::EntryTable entryTable; entryTable.definitionIndex = value.definitionIndex;
        bool super = false;
        for (std::size_t e = 0; e < entries.count && e < entryTable.entries.size(); ++e) {
            const auto base = entries.dataOffset + e * 64; auto& entry = entryTable.entries[e];
            std::memcpy(&entry.plugSource,bytes.data()+base+8,4);
            entry.group = std::to_integer<std::uint8_t>(bytes[base+12]); entry.kind = std::to_integer<std::uint8_t>(bytes[base+13]);
            if (entry.plugSource != state::account::inventory::kNoDefinitionHash) value.readyMask |= std::uint64_t{1} << e;
            super |= entry.kind == 34;
        }
        if (super) entryTables[value.definitionIndex] = entryTable;
    }
}
inline state::build_data::items::details::Definition detail(const tables::items::Row& row) {
    state::build_data::items::details::Definition value;
    value.definitionHash = row.definitionHash; value.definitionIndex = row.definitionIndex; value.bucketId = row.bucketId;
    value.maxStackSize = row.maxStackSize; value.equipmentSlot = row.equipmentSlot;
    constexpr std::uint8_t buckets[]{16,3,4,36,5,6,7,0,1,2,10,9,8,27,41,17,255,47,49};
    if (!value.equipmentSlot) for (std::size_t i = 0; i < std::size(buckets); ++i)
        if (row.bucketId == buckets[i]) value.equipmentSlot = static_cast<std::int8_t>(i);
    value.instancedDefinitionState = row.instanced ? state::build_data::items::details::InstancedDefinitionState::instanced : state::build_data::items::details::InstancedDefinitionState::stackable;
    value.ordinarySocketState = row.hasSockets ? state::build_data::items::details::OrdinarySocketState::present : state::build_data::items::details::OrdinarySocketState::absent;
    value.ordinarySocketCount = row.socketCount; value.socketEntryListIndex = row.socketEntryListIndex;
    std::copy_n(row.initialPlugs, value.initialPlugIndices.size(), value.initialPlugIndices.begin());
    std::copy_n(row.socketTypes, value.socketTypes.size(), value.socketTypes.begin());
    value.statCount = static_cast<std::uint8_t>((std::min)(std::size_t(row.statCount),value.stats.size()));
    for (std::size_t i = 0; i < value.statCount; ++i) value.stats[i] = {row.statRows[i], row.statValues[i]};
    return value;
}
}
namespace dawn::middleware::content::packages::reader {
bool read_tag(const Source&, Scratch&, std::uint32_t tag, std::vector<std::byte>& output, std::uint32_t& type) noexcept {
    output = fixture::read(tag); type = fixture::classes.contains(tag) ? fixture::classes.at(tag) : 0; return !output.empty();
}
bool read_tag(const Source& source, Scratch& scratch, std::uint32_t tag, std::vector<std::byte>& output) noexcept {
    std::uint32_t type{}; return read_tag(source, scratch, tag, output, type);
}
void close_files(Scratch&) noexcept {}
}
namespace dawn::client::content::items::packages {
bool collect_keys(reader::BlockKeys&) noexcept { return true; }
bool package_directory(core::path::Buffer& output) noexcept { output.chars[0] = L'.'; output.length = 1; return true; }
bool investment_globals_tags(std::array<std::uint32_t, kContainerCandidates>& output, std::size_t& count) noexcept { output[0] = fixture::globals; count = 1; return true; }
}
namespace dawn::state::runtime::detail {
bool semantic_equipment_slot(std::uint8_t slot, std::size_t& output) noexcept {
    constexpr std::size_t slots[]{11,3,4,16,5,6,7,0,1,2,10,9,8,13,14,12,16,15,16};
    if (slot >= std::size(slots)) return false; output = slots[slot]; return output < 16;
}
}
namespace dawn::state::build_data {
bool configured_item_details_ready() noexcept { return true; }
bool item_definitions_ready() noexcept { return true; }
bool inventory_bucket_descriptors_ready() noexcept { return true; }
bool socket_entry_lists_ready() noexcept { return true; }
std::size_t socket_entry_list_count() noexcept { return fixture::lists.size(); }
bool socket_plug_rules_ready() noexcept { return true; }
std::size_t item_definition_count() noexcept { return fixture::rows.size(); }
bool find_item_definition_index(std::uint16_t id, items::Definition& value) noexcept {
    if (id >= fixture::rows.size()) return false;
    const auto& row = fixture::rows[id]; value = {}; value.definitionIndex = id; value.definitionHash = row.definitionHash;
    value.bucketId = row.bucketId; value.tier = row.tier; value.plugCategoryHash = row.plugCategoryHash; return true;
}
bool find_item_definition_hash(std::uint32_t hash, items::Definition& value) noexcept {
    for (const auto& row : fixture::rows) if (row.definitionHash == hash) return find_item_definition_index(row.definitionIndex, value);
    return false;
}
bool find_configured_item_detail(std::uint16_t id, items::details::Definition& value) noexcept {
    if (id >= fixture::rows.size()) return false; value = fixture::detail(fixture::rows[id]); return true;
}
bool find_investment_constants(constants::InvestmentConstants& value) noexcept { value = fixture::constants; return true; }
bool is_socket_plug_pooled(std::uint16_t) noexcept { return false; }
bool visit_socket_plug_pool(std::uint16_t id, std::uint8_t lane, items::socket_plugs::MemberVisitor visitor, void* context) noexcept {
    struct Adapt { items::socket_plugs::MemberVisitor visitor; void* context; } adapt{visitor, context};
    return fixture::tables::items::visit_allowed_plugs(fixture::blobs[id], fixture::plugs, lane, [](void* p, std::uint32_t member) noexcept {
        auto& a = *static_cast<Adapt*>(p); return member <= 0xffff && a.visitor(a.context, static_cast<std::uint16_t>(member));
    }, &adapt);
}
bool visit_socket_roll_pool(std::uint16_t, std::uint8_t, items::socket_plugs::MemberVisitor, void*) noexcept { return true; }
bool find_socket_entry_table(std::uint16_t id, socket_entry_lists::EntryTable& value) noexcept {
    if (!fixture::entryTables.contains(id)) return false; value = fixture::entryTables.at(id); return true;
}
bool find_socket_entry_list(std::uint16_t id, socket_entry_lists::Definition& value) noexcept {
    if (id >= fixture::lists.size()) return false; value = fixture::lists[id]; return true;
}
bool is_profile_action_source(std::uint16_t, std::uint8_t bucket) noexcept { return bucket == 19 || bucket == 21; }
bool find_inventory_bucket_descriptor(std::uint8_t id, inventory::buckets::Descriptor& value) noexcept {
    // Synthetic capacities isolate mutation tests; native placement is covered by loadout tests.
    constexpr std::uint8_t gear[]{0,1,2,3,4,5,6,7,8,9,10,16,17,27,41,47};
    value = {}; value.bucketId = id; value.slotCount = 10;
    const auto it = std::find(std::begin(gear),std::end(gear),id);
    if (it != std::end(gear)) {
        value.firstSlot = static_cast<std::uint16_t>((it - std::begin(gear)) * 10);
        constexpr std::uint8_t physical[]{16,3,4,36,5,6,7,0,1,2,10,9,8,27,41,17,255,47,49};
        for (std::size_t i = 0; i < std::size(physical); ++i) if (id == physical[i]) value.equipmentSlot = static_cast<std::int8_t>(i);
        return true;
    }
    if (id == 19 || id == 21) { value.arraySelector = inventory::buckets::ArraySelector::profile; value.slotCount = 50; return true; }
    return false;
}
}
namespace dawn::state {
AccountState account_snapshot() noexcept { return *fixture::account; }
}
namespace dawn::state::persistence {
bool next_item_instance_soid(const AccountState& account, std::uint64_t& id) noexcept {
    id = 1000;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        for (const auto& item : account.characters[c].equipment.slots) if (item) id = (std::max)(id, item->instanceSoid + 1);
        for (std::size_t i = 0; i < account.characters[c].inventory.count; ++i) id = (std::max)(id, account.characters[c].inventory.values[i].instanceSoid + 1);
    }
    return true;
}
bool next_profile_item_instance_soid(const AccountState& account, std::uint64_t& id) noexcept {
    id = account::inventory::kFirstProfileItemInstanceSoid;
    for (std::size_t i = 0; i < account.profileItemCount; ++i) id = (std::max)(id, account.profileItems[i].instanceSoid + 1);
    return true;
}
}
