#include "../runtime.h"
#include "../scenarios/scenario_catalog.h"

#include <atomic>
#include <cstdio>

#include "../../../core/logging/log.h"

namespace dawn::state::build_data {
namespace {

enum class ForestRosterResult : std::uint8_t { ready = 1, missing, full };

/** Report changed outcomes once, since activity snapshots repeat the same amendment. */
void trace_forest_roster(ForestRosterResult result, std::uint16_t index,
                         const scenarios::Definition& definition) noexcept {
    static std::atomic<std::uint64_t> last{0};
    const auto groups = static_cast<unsigned>(definition.rosterGroupCount)
                        + static_cast<unsigned>(definition.bubbleGroupCount);
    const auto signature = (static_cast<std::uint64_t>(result) << 32U)
                           | (static_cast<std::uint64_t>(index) << 8U) | groups;
    if (last.exchange(signature, std::memory_order_relaxed) == signature) {
        return;
    }
    std::array<char, 256> line{};
    const char* name = result == ForestRosterResult::ready ? "ready"
                       : result == ForestRosterResult::missing ? "missing_group"
                                                               : "capacity_full";
    const int written = std::snprintf(
        line.data(), line.size(),
        "ev=omega_forest_roster result=%s key=0x2763EC97 index=%u groups=%u bubble=11",
        name, static_cast<unsigned>(index), groups);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        core::log::write(core::log::Channel::state,
                         result == ForestRosterResult::ready ? core::log::Level::info
                                                            : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Copies one roster group by the table index a destination row carries. */
bool find_roster_group(std::size_t index, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::group(index, group);
}

bool find_roster_group_by_key(std::uint32_t key, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::group_by_key(key, group);
}

bool find_roster_group_by_key(std::uint32_t key, std::uint32_t objectTag, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::group_by_key(key, objectTag, group);
}

/** Publishes Omega's generator by stable key; catalog indices change after extraction. */
void amend_omega_forest_generator(scenarios::Definition& definition) noexcept {
    constexpr std::uint32_t kForestGeneratorKey = 0x2763EC97U;
    constexpr std::uint64_t kForestBubbleMask = std::uint64_t{1} << 11U;
    if (!scenario_layouts_ready()) {
        return;
    }
    std::uint16_t resolved = 0;
    if (!scenarios::find_group_index(kForestGeneratorKey, resolved)) {
        trace_forest_roster(ForestRosterResult::missing, 0xFFFFU, definition);
        return;
    }
    for (std::size_t index = 0; index < definition.bubbleGroupCount; ++index) {
        if (definition.bubbleGroups[index] == resolved) {
            trace_forest_roster(ForestRosterResult::ready, resolved, definition);
            return;
        }
    }
    if (definition.bubbleGroupCount >= definition.bubbleGroups.size()) {
        trace_forest_roster(ForestRosterResult::full, resolved, definition);
        return;
    }
    definition.bubbleGroups[definition.bubbleGroupCount] = resolved;
    definition.bubbleGroupMasks[definition.bubbleGroupCount] = kForestBubbleMask;
    ++definition.bubbleGroupCount;
    trace_forest_roster(ForestRosterResult::ready, resolved, definition);
}

} // namespace dawn::state::build_data
