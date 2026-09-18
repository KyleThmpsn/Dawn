#include "scenario_catalog.h"

#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "../table.h"

namespace dawn::state::build_data::scenarios {
namespace {

// One lock covers both tables: a reader must never see new layouts against old roster groups.
Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<RosterGroup, kRosterGroupCapacity> g_groups;

/** @param definition Candidate row. @return Its name as a bounded view. */
[[nodiscard]] std::string_view name_of(const Definition& definition) noexcept {
    return {definition.name.data(), definition.nameLength};
}

/** @param group Candidate roster group. @return True when every field is canonical. */
[[nodiscard]] bool canonical(const RosterGroup& group) noexcept {
    namespace tables = middleware::content::packages::tables;
    if (group.registryKey == 0 || group.slotCount == 0 || group.slotCount > kRosterSlotCapacity) {
        return false;
    }
    for (std::size_t slot = 0; slot < kRosterSlotCapacity; ++slot) {
        const bool declared = slot < group.slotCount;
        if (declared
            && (group.slotTypes[slot] == 0 || group.slotTypes[slot] > kMaximumSlotType
                || (group.slotFlags[slot] & ~kSlotFlagMask) != 0
                || group.slotIndices[slot] >= kRosterSlotCapacity
                || group.descriptorTags[slot] == 0
                || group.descriptorOffsets[slot] > 0x00FFFFFFU
                || !tables::is_class_id(group.componentClasses[slot])
                || !tables::is_schema_id(group.senseSchemas[slot])
                || !tables::is_schema_id(group.authSchemas[slot]))) {
            return false;
        }
        if (declared) {
            const std::uint8_t expectedFlags =
                (group.senseSchemas[slot] != tables::kAbsentSchema ? kSlotSenseFlag : 0U)
                | (group.authSchemas[slot] != tables::kAbsentSchema ? kSlotAuthFlag : 0U);
            if (group.slotFlags[slot] != expectedFlags) {
                return false;
            }
        }
        if (declared && slot != 0 && group.slotIndices[slot] <= group.slotIndices[slot - 1]) {
            return false;
        }
        // Storage past the declared count must stay zero, or two caches of the same packages
        // could differ byte for byte while meaning the same thing.
        if (!declared
            && (group.slotTypes[slot] != 0 || group.slotFlags[slot] != 0
                || group.slotIndices[slot] != 0 || group.descriptorTags[slot] != 0
                || group.descriptorOffsets[slot] != 0 || group.componentClasses[slot] != 0
                || group.senseSchemas[slot] != 0 || group.authSchemas[slot] != 0)) {
            return false;
        }
    }
    return true;
}

/**
 * @param definition Candidate row.
 * @param groups Published roster groups.
 * @return True when every field is canonical.
 */
[[nodiscard]] bool canonical(const Definition& definition,
                             std::span<const RosterGroup> groups) noexcept {
    if (definition.nameLength == 0 || definition.nameLength > kNameCapacity
        || definition.bubbleCount > kBubbleCapacity || definition.truncated > 1
        || definition.rosterGroupCount > kDestinationGroupCapacity
        || definition.bubbleGroupCount > kDestinationBubbleGroupCapacity
        || definition.spawnStemLength > kSpawnStemCapacity) {
        return false;
    }
    for (std::size_t index = definition.spawnStemLength; index < kSpawnStemCapacity; ++index) {
        if (definition.spawnStem[index] != '\0') {
            return false;
        }
    }
    for (std::size_t group = 0; group < kDestinationGroupCapacity; ++group) {
        const bool declared = group < definition.rosterGroupCount;
        if (declared && definition.rosterGroups[group] >= groups.size()) {
            return false;
        }
        if (!declared && definition.rosterGroups[group] != 0) {
            return false;
        }
    }
    for (std::size_t group = 0; group < kDestinationBubbleGroupCapacity; ++group) {
        const bool declared = group < definition.bubbleGroupCount;
        if (declared
            && (definition.bubbleGroups[group] >= groups.size()
                || definition.bubbleGroupMasks[group] == 0)) {
            return false;
        }
        if (!declared
            && (definition.bubbleGroups[group] != 0 || definition.bubbleGroupMasks[group] != 0)) {
            return false;
        }
    }
    const std::size_t ordinaryGroupCount =
        std::size_t{definition.rosterGroupCount} + std::size_t{definition.bubbleGroupCount};
    for (std::size_t left = 0; left < ordinaryGroupCount; ++left) {
        const std::uint16_t leftIndex =
            left < definition.rosterGroupCount
                ? definition.rosterGroups[left]
                : definition.bubbleGroups[left - definition.rosterGroupCount];
        for (std::size_t right = left + 1; right < ordinaryGroupCount; ++right) {
            const std::uint16_t rightIndex =
                right < definition.rosterGroupCount
                    ? definition.rosterGroups[right]
                    : definition.bubbleGroups[right - definition.rosterGroupCount];
            if (groups[leftIndex].registryKey == groups[rightIndex].registryKey) {
                return false;
            }
        }
    }
    for (std::size_t slice = 0; slice < definition.authoredGroups.size(); ++slice) {
        const std::size_t authoredCount = definition.authoredGroupCounts[slice];
        if (authoredCount > kDestinationAuthoredGroupCapacity) {
            return false;
        }
        std::size_t additionalCount = 0;
        for (std::size_t group = 0; group < kDestinationAuthoredGroupCapacity; ++group) {
            const bool declared = group < authoredCount;
            const std::uint16_t tableIndex = definition.authoredGroups[slice][group];
            if ((declared && tableIndex >= groups.size()) || (!declared && tableIndex != 0)) {
                return false;
            }
            if (!declared) {
                continue;
            }
            for (std::size_t earlier = 0; earlier < group; ++earlier) {
                if (definition.authoredGroups[slice][earlier] == tableIndex) {
                    return false;
                }
                if (groups[definition.authoredGroups[slice][earlier]].registryKey
                    == groups[tableIndex].registryKey) {
                    return false;
                }
            }
            bool ordinary = false;
            for (std::size_t top = 0; top < definition.rosterGroupCount; ++top) {
                if (definition.rosterGroups[top] != tableIndex
                    && groups[definition.rosterGroups[top]].registryKey
                           == groups[tableIndex].registryKey) {
                    return false;
                }
                ordinary = ordinary || definition.rosterGroups[top] == tableIndex;
            }
            for (std::size_t bubble = 0; bubble < definition.bubbleGroupCount; ++bubble) {
                if (definition.bubbleGroups[bubble] != tableIndex
                    && groups[definition.bubbleGroups[bubble]].registryKey
                           == groups[tableIndex].registryKey) {
                    return false;
                }
                ordinary = ordinary || definition.bubbleGroups[bubble] == tableIndex;
            }
            additionalCount += ordinary ? 0U : 1U;
        }
        if (ordinaryGroupCount + additionalCount > kDestinationWireGroupCapacity) {
            return false;
        }
    }
    // Storage past the declared name and bubble count must stay zero, or two caches of the same
    // packages could differ byte for byte while meaning the same thing.
    for (std::size_t index = definition.nameLength; index < kNameCapacity; ++index) {
        if (definition.name[index] != '\0') {
            return false;
        }
    }
    for (std::size_t index = 0; index < kBubbleCapacity; ++index) {
        const bool declared = index < definition.bubbleCount;
        if (declared && definition.bubbleStates[index] != kBubbleEnabledByte
            && definition.bubbleStates[index] != kBubbleDisabledByte) {
            return false;
        }
        if (!declared
            && (definition.bubbleStates[index] != 0 || definition.bubbleHashes[index] != 0
                || definition.bubbleStateCounts[index] != 0
                || definition.bubbleMapIndices[index] != 0)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears every extracted destination layout and roster group under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
    g_groups.clear();
}

/** Checks that the rows are canonical and uniquely named. */
bool valid(std::span<const Definition> definitions, std::span<const RosterGroup> groups) noexcept {
    if (definitions.size() > kDefinitionCapacity || groups.size() > kRosterGroupCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < groups.size(); ++row) {
        if (!canonical(groups[row])) {
            return false;
        }
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        if (!canonical(definitions[row], groups)) {
            return false;
        }
        // A duplicate name would make the destination lookup depend on row order.
        for (std::size_t earlier = 0; earlier < row; ++earlier) {
            if (name_of(definitions[earlier]) == name_of(definitions[row])) {
                return false;
            }
        }
    }
    return true;
}

/** Replaces the extracted destination layouts and their roster groups in one step. */
bool replace(std::span<const Definition> definitions,
             std::span<const RosterGroup> groups) noexcept {
    if (!valid(definitions, groups)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    // Both run, with no short-circuit, so the pair cannot be left half replaced. valid() already
    // checked each against its size, which is the only reason either can refuse.
    const bool storedDefinitions = g_definitions.replace(definitions);
    const bool storedGroups = g_groups.replace(groups);
    return storedDefinitions && storedGroups;
}

/** Copies one roster group by table index. */
bool group(std::size_t index, RosterGroup& group) noexcept {
    group = {};
    const Lock::Shared guard(g_lock);
    const std::span<const RosterGroup> rows = g_groups.rows();
    const bool present = index < rows.size();
    if (present) {
        group = rows[index];
    }
    return present;
}

bool find_group_index(std::uint32_t registryKey, std::uint16_t& index) noexcept {
    index = 0;
    const Lock::Shared guard(g_lock);
    const std::span<const RosterGroup> rows = g_groups.rows();
    for (std::size_t candidate = 0; candidate < rows.size(); ++candidate) {
        if (rows[candidate].registryKey == registryKey) {
            index = static_cast<std::uint16_t>(candidate);
            return true;
        }
    }
    return false;
}

bool group_by_key(std::uint32_t registryKey, RosterGroup& output) noexcept {
    output = {};
    const Lock::Shared guard(g_lock);
    for (const auto& row : g_groups.rows()) {
        if (row.registryKey == registryKey) { output = row; return true; }
    }
    return false;
}

bool find_group_index(std::uint32_t registryKey, std::uint32_t objectTag, std::uint16_t& index) noexcept {
    index = 0;
    const Lock::Shared guard(g_lock);
    const auto rows = g_groups.rows();
    std::size_t found = rows.size();
    for (std::size_t candidate = 0; candidate < rows.size(); ++candidate) {
        if (rows[candidate].registryKey != registryKey || rows[candidate].objectTag != objectTag) continue;
        if (found != rows.size()) return false;
        found = candidate;
    }
    if (found == rows.size()) return false;
    index = static_cast<std::uint16_t>(found);
    return true;
}

bool group_by_key(std::uint32_t registryKey, std::uint32_t objectTag, RosterGroup& output) noexcept {
    output = {};
    const Lock::Shared guard(g_lock);
    const RosterGroup* found = nullptr;
    for (const auto& row : g_groups.rows()) {
        if (row.registryKey != registryKey || row.objectTag != objectTag) continue;
        if (found != nullptr) return false;
        found = &row;
    }
    if (found == nullptr) return false;
    output = *found;
    return true;
}

DescriptorLookup find_published_descriptor(CueNodeId node,
                                           SlotDescriptorMetadata& output) noexcept {
    const Lock::Shared guard(g_lock);
    return find_descriptor(g_groups.rows(), node, output);
}

bool map_published_observations(
    std::string_view mission,
    const middleware::bap::activity_message::sense_update::SenseUpdate& update,
    ObservationMappingReport& output) noexcept {
    output = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Definition> definitions = g_definitions.rows();
    for (const Definition& definition : definitions) {
        if (name_of(definition) != mission) {
            continue;
        }
        output = map_observations(definition, g_groups.rows(), update);
        return true;
    }
    return false;
}

/** @return Published roster group count. */
std::size_t group_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_groups.count();
}

/** Copies every roster group in extraction order. */
bool snapshot_groups(std::span<RosterGroup> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_groups.snapshot(output, count);
}

/** Finds one destination layout by package name. */
bool find(std::string_view name, Definition& definition) noexcept {
    definition = {};
    if (name.empty() || name.size() > kNameCapacity) {
        return false;
    }
    const Lock::Shared guard(g_lock);
    for (const Definition& row : g_definitions.rows()) {
        if (name_of(row) == name) {
            definition = row;
            return true;
        }
    }
    return false;
}

/** Copies every row in extraction order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return The number of extracted destination layouts, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace dawn::state::build_data::scenarios
