#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "definition.h"
#include "descriptor_catalog.h"
#include "cue_graph_manifest.h"

namespace dawn::state::build_data::scenarios {

/** Clears every extracted destination layout and roster group. */
void clear() noexcept;

/**
 * Checks that the rows are canonical and uniquely named.
 * Both arrays are checked together. A destination names its groups by index, so a group table
 * short by one row silently repoints every destination above it.
 * @param definitions Candidate rows.
 * @param groups Candidate roster groups.
 * @return True when every row fits storage, names itself, and declares a usable bubble array.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions,
                         std::span<const RosterGroup> groups) noexcept;

/**
 * Replaces the extracted destination layouts and their roster groups in one step.
 * @param definitions Complete rows, or an empty complete domain.
 * @param groups Complete roster groups, or an empty complete table.
 * @return True when the rows validate and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions,
                           std::span<const RosterGroup> groups) noexcept;

/**
 * Copies one roster group by table index.
 * @param index Index a destination row carries.
 * @param group Receives the group.
 * @return True when the index is inside the published table.
 */
[[nodiscard]] bool group(std::size_t index, RosterGroup& group) noexcept;

/** Finds a registry key across the published roster table, independent of extraction order. */
[[nodiscard]] bool find_group_index(std::uint32_t registryKey, std::uint16_t& index) noexcept;
/** Resolves a unique registry/object identity; registry keys can be shared across activities. */
[[nodiscard]] bool find_group_index(std::uint32_t registryKey, std::uint32_t objectTag, std::uint16_t& index) noexcept;
/** Copies one identity-matched row while holding the published catalog stable. */
[[nodiscard]] bool group_by_key(std::uint32_t registryKey, RosterGroup& output) noexcept;
/** Copies only the unique registry/object identity, rejecting ambiguous matches. */
[[nodiscard]] bool group_by_key(std::uint32_t registryKey, std::uint32_t objectTag, RosterGroup& output) noexcept;

/** Finds descriptor metadata in the currently published mission-independent catalog. */
[[nodiscard]] DescriptorLookup
find_published_descriptor(CueNodeId node, SlotDescriptorMetadata& output) noexcept;

/** Maps one sense update against a published mission while holding the catalog pair stable. */
[[nodiscard]] bool map_published_observations(
    std::string_view mission,
    const middleware::bap::activity_message::sense_update::SenseUpdate& update,
    ObservationMappingReport& output) noexcept;

/** @return Published roster group count. */
[[nodiscard]] std::size_t group_count() noexcept;

/**
 * Copies every roster group in extraction order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_groups(std::span<RosterGroup> output, std::size_t& count) noexcept;

/**
 * Finds one destination layout by package name.
 * @param name Package name the client's selection carried.
 * @param definition Receives the matching row.
 * @return True when the domain is complete and carries that exact name.
 */
[[nodiscard]] bool find(std::string_view name, Definition& definition) noexcept;

/**
 * Copies every row in extraction order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return The number of extracted destination layouts, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace dawn::state::build_data::scenarios
