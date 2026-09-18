#include <memory>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "core/logging/log.h"
#include "state/build_data/runtime.h"
#include "state/build_data/scenarios/scenario_catalog.h"
#include "server/bap/encrypted/activity_message/omega_roster_readiness.h"
#include "server/bap/encrypted/activity_message/omega_monitor_edges.h"
#include "fixtures/omega_ikora_startup_capture.h"
#include "fixtures/omega_complete_roster_capture.h"

namespace {
namespace build = dawn::state::build_data;
namespace scenarios = build::scenarios;
constexpr std::uint32_t kGeneratorKey = 0x2763EC97U;
int g_failures = 0;
std::vector<std::string> g_events;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << __FILE__ << ':' << line << ": check failed: " << expression << '\n';
        ++g_failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

bool same_layout(const scenarios::Definition& a, const scenarios::Definition& b) {
    // Compare every field; object padding is not part of the catalog contract.
    const auto fields = [](const scenarios::Definition& row) {
        return std::tie(row.name, row.tag, row.nameLength, row.bubbleCount, row.truncated,
                        row.rosterGroupCount, row.bubbleGroupCount, row.spawnStemLength,
                        row.spawnStem, row.rosterGroups, row.bubbleGroups, row.bubbleGroupMasks,
                        row.authoredGroupCounts, row.authoredGroups, row.bubbleStates,
                        row.bubbleHashes, row.bubbleStateCounts, row.bubbleMapIndices,
                        row.packageCount, row.packages);
    };
    return fields(a) == fields(b);
}

scenarios::Definition opening_layout() {
    scenarios::Definition row{};
    constexpr std::string_view name = "mission_scot";
    std::copy(name.begin(), name.end(), row.name.begin());
    row.nameLength = static_cast<std::uint8_t>(name.size());
    row.tag = 0x80F47522U;
    row.bubbleCount = 16;
    std::fill_n(row.bubbleStates.begin(), row.bubbleCount, scenarios::kBubbleEnabledByte);
    row.rosterGroupCount = 3;
    row.rosterGroups = {1, 2, 3, 0};
    row.bubbleGroupCount = 3;
    row.bubbleGroups = {4, 5, 6, 0};
    row.bubbleGroupMasks = {1ULL << 15U, 1ULL << 15U, 1ULL << 15U, 0};
    return row;
}

std::vector<scenarios::RosterGroup> catalog(std::size_t count, std::size_t generatorIndex) {
    std::vector<scenarios::RosterGroup> rows(count);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto& row = rows[i];
        row.registryKey = 0x10000000U + static_cast<std::uint32_t>(i);
        row.objectTag = 0x80F47539U;
        row.slotCount = 1;
        row.slotTypes[0] = 37;
        row.slotIndices[0] = 1;
        row.descriptorTags[0] = 0x80F47530U;
        row.componentClasses[0] = 0x80804EF6U;
        row.senseSchemas[0] = 0xFFFFFFFFU;
        row.authSchemas[0] = 0xFFFFFFFFU;
    }
    if (generatorIndex < rows.size()) {
        rows[generatorIndex].registryKey = kGeneratorKey;
    }
    return rows;
}

void shifted_catalog_and_replacement() {
    // The failing live cache has 1925 rows and places the generator at 1197.
    // Subsequent publications move it again, including both catalog boundaries.
    for (const std::size_t position : {1197U, 0U, 1070U, 2047U, 50U}) {
        const std::size_t count = position == 2047U ? 2048U : 1925U;
        auto rows = catalog(count, position);
        auto layout = opening_layout();
        CHECK(scenarios::replace(std::span{&layout, 1U}, rows));
        g_events.clear();
        build::amend_omega_forest_generator(layout);
        CHECK(layout.bubbleGroupCount == 4U);
        CHECK(layout.bubbleGroups[3] == position);
        CHECK(layout.bubbleGroupMasks[3] == (1ULL << 11U));
        CHECK(scenarios::valid(std::span{&layout, 1U}, rows));
        auto preserved = layout;
        preserved.bubbleGroupCount = 3;
        preserved.bubbleGroups[3] = 0;
        preserved.bubbleGroupMasks[3] = 0;
        const auto original = opening_layout();
        CHECK(same_layout(preserved, original));
        CHECK(g_events.size() == 1U);
        CHECK(!g_events.empty() && g_events.back().find("result=ready") != std::string::npos);
        const auto once = layout;
        build::amend_omega_forest_generator(layout);
        CHECK(same_layout(layout, once));
        CHECK(g_events.size() == 1U);
        scenarios::RosterGroup generator{};
        CHECK(build::find_roster_group(layout.bubbleGroups[3], generator));
        CHECK(generator.registryKey == kGeneratorKey);
        CHECK(generator.slotTypes[0] == 37 && generator.slotIndices[0] == 1);
        generator = {};
        CHECK(build::find_roster_group_by_key(kGeneratorKey, generator));
        CHECK(generator.registryKey == kGeneratorKey && generator.slotTypes[0] == 37);
    }
}

void missing_group_and_full_layout() {
    auto rows = catalog(1925U, 1925U);
    auto layout = opening_layout();
    const auto before = layout;
    CHECK(scenarios::replace(std::span{&layout, 1U}, rows));
    build::amend_omega_forest_generator(layout);
    CHECK(same_layout(layout, before));
    CHECK(!g_events.empty() && g_events.back().find("result=missing_group") != std::string::npos);
    std::uint16_t absent = 42;
    CHECK(!scenarios::find_group_index(kGeneratorKey, absent));
    CHECK(absent == 0U);
    scenarios::RosterGroup absentRow{};
    absentRow.registryKey = 42;
    CHECK(!build::find_roster_group_by_key(kGeneratorKey, absentRow));
    CHECK(absentRow.registryKey == 0 && absentRow.slotCount == 0);

    rows[1197].registryKey = kGeneratorKey;
    layout.bubbleGroupCount = 4;
    layout.bubbleGroups[3] = 7;
    layout.bubbleGroupMasks[3] = 1ULL << 11U;
    CHECK(scenarios::replace(std::span{&layout, 1U}, rows));
    const auto full = layout;
    build::amend_omega_forest_generator(layout);
    CHECK(same_layout(layout, full));
    CHECK(!g_events.empty() && g_events.back().find("result=capacity_full") != std::string::npos);
    scenarios::clear();
    layout = opening_layout();
    build::amend_omega_forest_generator(layout);
    CHECK(same_layout(layout, before));
}

void live_ikora_startup() {
    namespace sense = dawn::middleware::bap::activity_message::sense_update;
    namespace readiness = dawn::server::bap::encrypted::activity_message::omega_roster_readiness;
    namespace monitor = dawn::server::bap::encrypted::activity_message::omega_monitor_edges;
    const auto decode = [](std::string_view hex, sense::SenseUpdate& update,
                           std::size_t& consumed) {
        std::vector<std::byte> bytes;
        const auto nibble = [](char value) { return value <= '9' ? value - '0' : value - 'A' + 10; };
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            bytes.push_back(static_cast<std::byte>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
        }
        return sense::parse_omega_sense_update(bytes, update, consumed);
    };
    // Keep the two retained decoded captures off the bounded thread stack.
    auto startupStorage=std::make_unique<sense::SenseUpdate>();
    auto& startup=*startupStorage;
    std::size_t consumed{};
    CHECK(decode(omega_ikora_startup_capture::kStartup, startup, consumed));
    CHECK(consumed == 2302U);
    CHECK(startup.rosterEntryCount == 13U && startup.bubbleBlockCount == 2U);
    CHECK(!readiness::exact_omega_initial_report(startup));
    auto approachStorage=std::make_unique<sense::SenseUpdate>();
    auto& approach=*approachStorage;
    CHECK(decode(omega_ikora_startup_capture::kApproach, approach, consumed));
    CHECK(monitor::entered(approach, 20));

    auto rows = catalog(1925U, 1197U);
    auto layout = opening_layout();
    CHECK(scenarios::replace(std::span{&layout, 1U}, rows));
    build::amend_omega_forest_generator(layout);
    CHECK(layout.bubbleGroupCount == 4U && layout.bubbleGroups[3] == 1197U);
    scenarios::RosterGroup generator{};
    CHECK(build::find_roster_group(layout.bubbleGroups[3], generator));

    // The corrected publication adds the generator's bubble-11 acknowledgement.
    // Keep all six captured native object bodies and every other roster entry.
    std::move_backward(startup.rosterEntries.begin() + 3,
                       startup.rosterEntries.begin() + startup.rosterEntryCount,
                       startup.rosterEntries.begin() + startup.rosterEntryCount + 1);
    startup.rosterEntries[3] = {generator.registryKey, 11, 0x83U, true};
    ++startup.rosterEntryCount;
    ++startup.bubbleBlockCount;
    CHECK(readiness::exact_omega_initial_report(startup));
    CHECK(monitor::entered(approach, 20));

    // Restoring the missing entry must not weaken native readiness validation.
    startup.rosterEntries[3].bubble = 12;
    CHECK(!readiness::exact_omega_initial_report(startup));
    startup.rosterEntries[3].bubble = 11;
    startup.rosterEntries[3].active = false;
    CHECK(!readiness::exact_omega_initial_report(startup));
    startup.rosterEntries[3].active = true;
    startup.objects[3].bodyFirst ^= 1U;
    CHECK(!readiness::exact_omega_initial_report(startup));
    scenarios::clear();
}
void complete_loading_roster() {
    namespace readiness = dawn::server::bap::encrypted::activity_message::omega_roster_readiness;
    namespace sense = dawn::middleware::bap::activity_message::sense_update;
    auto captured = std::make_unique<sense::SenseUpdate>();
    auto invalid = std::make_unique<sense::SenseUpdate>();
    complete_opening_capture::fill(*captured);
    CHECK(readiness::exact_omega_initial_report(*captured));
    for (std::size_t i = 0; i < captured->rosterEntryCount; ++i) {
        *invalid = *captured; invalid->rosterEntries[i].registryKey ^= 1U;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
        *invalid = *captured; ++invalid->rosterEntries[i].bubble;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
        *invalid = *captured; invalid->rosterEntries[i].active = false;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
        *invalid = *captured; invalid->rosterEntries[i].state = 0x82;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
        *invalid = *captured;
        invalid->rosterEntries[i] = captured->rosterEntries[(i + 1) % captured->rosterEntryCount];
        CHECK(!readiness::exact_omega_initial_report(*invalid));
        *invalid = *captured;
        std::move(invalid->rosterEntries.begin() + i + 1,
            invalid->rosterEntries.begin() + invalid->rosterEntryCount,
            invalid->rosterEntries.begin() + i);
        --invalid->rosterEntryCount;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
    }
    for (std::size_t i = 0; i < captured->objectCount; ++i) {
        *invalid = *captured; invalid->objects[i].bodyFirst ^= 1U;
        CHECK(!readiness::exact_omega_initial_report(*invalid));
    }
    *invalid = *captured; invalid->bubbleBlockCount = 3;
    CHECK(!readiness::exact_omega_initial_report(*invalid));
    *invalid = *captured; invalid->topLevelRosterCount = 4;
    CHECK(!readiness::exact_omega_initial_report(*invalid));
    *invalid = *captured; invalid->rosterEntries[invalid->rosterEntryCount++] = {0xDEADBEEFU, 12, 0x83, true};
    CHECK(!readiness::exact_omega_initial_report(*invalid));
    // The added area groups may be reported after the Lair block as well.
    *invalid = *captured;
    std::rotate(invalid->rosterEntries.begin() + 4, invalid->rosterEntries.begin() + 6,
        invalid->rosterEntries.begin() + invalid->rosterEntryCount);
    CHECK(readiness::exact_omega_initial_report(*invalid));
}
} // namespace

// Same readiness predicate as the catalog runtime, without unrelated domain publishers.
namespace dawn::state::build_data {
bool scenario_layouts_ready() noexcept { return scenarios::count() != 0; }
}
namespace dawn::core::log {
void write(Channel, Level, std::string_view event) noexcept { g_events.emplace_back(event); }
}

int main() {
    shifted_catalog_and_replacement();
    missing_group_and_full_layout();
    live_ikora_startup();
    complete_loading_roster();
    if (g_failures != 0) {
        std::cerr << g_failures << " checks failed\n";
        return 1;
    }
    std::cout << "Omega forest roster regression checks passed\n";
    return 0;
}
