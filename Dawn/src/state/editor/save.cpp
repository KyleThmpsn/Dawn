// SPDX-License-Identifier: GPL-3.0-only
#include <Windows.h>
#include "edit.h"
#include "../runtime/runtime.h"
#include "../runtime/storage/internal.h"
#include "../build_data/abilities/ability_bucket_catalog.h"
#include "../persistence/persistence.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../client/content/items/packages/internal.h"
#include "../../server/bap/runtime.h"
#include <atomic>
#include <memory>

namespace dawn::state::editor {
namespace {
namespace packages = client::content::items::packages;
using Selection_t = build_data::abilities::Selection;

/** Slot 11 carries the subclass, which is the only equipment an ability row depends on. */
constexpr std::size_t kSubclassSlot = 11;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/**
 * One restore point covers the whole editing session.
 * Applying is a live action a player repeats freely, so copying the player database on every apply
 * would stall the frame and bury the one image worth keeping: the account as it stood before any
 * editing. The copy is taken once and later applies reuse it.
 */
std::atomic_bool g_restorePointTaken{false};

/** @return The ability selection one character currently has. */
Selection_t ability_selection(const CharacterState& character) {
    return {character.movementAbilityEntry, character.grenadeAbilityEntry,
        character.superAbilityEntry, character.meleeAbilityEntry, character.classAbilityEntry};
}

/**
 * Adds one row to the set when an equal row is not already in it.
 * @return False only when the fixed row storage is full.
 */
bool add_unique(std::span<build_data::abilities::Definition> rows, std::size_t& count,
    const build_data::abilities::Definition& row) {
    for (std::size_t i = 0; i < count; ++i) {
        if (rows[i].socketEntryListIndex == row.socketEntryListIndex && rows[i].selection == row.selection) return true;
    }
    if (count == rows.size()) return false;
    rows[count++] = row;
    return true;
}

/**
 * @return True when every equipped subclass combination is already published.
 * Applying is now a per-edit action, so the common case must not open the package files at all.
 */
bool ability_rows_published(const AccountState& account, const Catalog& catalog,
    std::span<build_data::abilities::Definition> rows, std::size_t& count) {
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        const auto& subclass = account.characters[i].equipment.slots[kSubclassSlot];
        if (!subclass) continue;
        const auto* item = catalog.find(subclass->definitionHash);
        build_data::abilities::Definition row{};
        if (!item || !build_data::find_ability_buckets(item->detail.socketEntryListIndex,
                ability_selection(account.characters[i]), row)) return false;
        if (!add_unique(rows, count, row)) return false;
    }
    return true;
}

/** Builds the subclass ability combinations every character in the account needs. */
bool ability_rows(const AccountState& account, const Catalog& catalog,
    std::span<build_data::abilities::Definition> rows, std::size_t& count) {
    // Keep the prebuilt combinations available after saving any character's loadout.
    if (!build_data::abilities::snapshot(rows, count)) return false;
    const std::size_t published = count;
    if (ability_rows_published(account, catalog, rows, count)) return true;
    count = published;
    reader::BlockKeys keys{};
    auto scratch = std::make_unique<reader::Scratch>();
    struct Cleanup { reader::BlockKeys& keys; reader::Scratch& scratch;
        ~Cleanup() { reader::close_files(scratch); SecureZeroMemory(&keys, sizeof keys); } } cleanup{keys, *scratch};
    core::path::Buffer directory{};
    if (!packages::collect_keys(keys) || !packages::package_directory(directory)) return false;
    reader::Source source{directory.chars.data(), &keys};
    std::array<std::uint32_t, packages::kContainerCandidates> tags{};
    std::size_t tagCount{}; tables::Array array{};
    std::vector<std::byte> globals, root, table, definition, blob;
    bool found = false;
    if (!packages::investment_globals_tags(tags, tagCount)) return false;
    for (std::size_t i = 0; i < tagCount && !found; ++i) {
        std::uint32_t rootTag{}, tableTag{};
        found = reader::read_tag(source, *scratch, tags[i], globals) && tables::child_tag(globals, 0, rootTag)
            && reader::read_tag(source, *scratch, rootTag, root) && tables::slot_tag(root, 97, tableTag)
            && reader::read_tag(source, *scratch, tableTag, table) && tables::find_array_at(table, 8, array);
    }
    if (!found) return false;
    for (std::size_t i = 0; i < account.characterCount; ++i) {
        const auto& character = account.characters[i];
        const auto& subclass = character.equipment.slots[kSubclassSlot];
        if (!subclass) continue;
        const auto* item = catalog.find(subclass->definitionHash);
        if (!item) return false;
        const auto selection = ability_selection(character);
        build_data::abilities::Definition row{};
        if (!build_data::find_ability_buckets(item->detail.socketEntryListIndex, selection, row)) {
            tables::IndexRow index{};
            if (!tables::index_row(table, array, item->detail.socketEntryListIndex, index)
                || !reader::read_tag(source, *scratch, index.targetTag, definition)
                || !packages::build_ability_buckets(source, *scratch, definition, blob, selection, row)) return false;
            row.socketEntryListIndex = item->detail.socketEntryListIndex; row.selection = selection;
        }
        if (!add_unique(rows, count, row)) return false;
    }
    return true;
}

/**
 * Checks that every character's equipment is one the installed build can carry.
 * The account is shared, so a character the editor never opened can still refuse the apply.
 * @param account Draft after-image to check.
 * @param catalog Loaded item catalog.
 * @param message Receives the reason when a character is refused.
 * @return True when every character resolves through its own selected-character loadout.
 */
bool every_character_resolves(const AccountState& account, const Catalog& catalog, std::string& message) {
    auto validation = std::make_unique<AccountState>(account);
    auto resolved = std::make_unique<middleware::datagen::family4::loadout::ResolvedLoadout>();
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const auto& character = account.characters[c];
        unsigned exoticWeapons = 0, exoticArmor = 0;
        for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
            const auto& item = character.equipment.slots[slot];
            if (!item) continue;
            const auto* definition = catalog.find(item->definitionHash);
            if (!definition || definition->slot != slot || !fits_class(*definition, character.characterClass)) {
                message = "Equipped gear must match the character class and equipment slot."; return false;
            }
            exoticWeapons += definition->kind == GearKind::weapon && definition->definition.tier == 5;
            exoticArmor += definition->kind == GearKind::armor && definition->definition.tier == 5;
        }
        if (exoticWeapons > 1 || exoticArmor > 1) { message = "Only one exotic weapon and one exotic armor piece can be equipped."; return false; }
        // Each character encodes as the selected one, which is the only form the resolver accepts.
        for (std::size_t i = 0; i < validation->characterCount; ++i) validation->characters[i].selected = i == c;
        if (!middleware::datagen::family4::loadout::resolve(*validation, c, *resolved)) {
            message = "The loadout does not fit the game's inventory or socket layout. Check your changes."; return false;
        }
    }
    return true;
}
}

bool apply(Draft& draft, const Catalog& catalog, std::string& message, bool& live) {
    live = false;
    if (!draft.dirty) { message = "No changes to apply."; return false; }
    auto prepared = std::make_unique<AccountState>(draft.after);
    if (!prepare_commit(draft, catalog, *prepared, message)) return false;
    if (!every_character_resolves(*prepared, catalog, message)) return false;
    std::vector<build_data::abilities::Definition> abilities(build_data::abilities::kDefinitionCapacity);
    std::size_t abilityCount{};
    if (!ability_rows(*prepared, catalog, abilities, abilityCount)) {
        message = "The selected subclass abilities could not be resolved."; return false;
    }
    // One restore point covers the whole session; later applies reuse the image taken here.
    const bool firstApply = !g_restorePointTaken.load(std::memory_order_acquire);
    if (firstApply) {
        if (!persistence::backup_for_editor()) {
            message = "Could not create the save backup. Your account was not changed."; return false;
        }
        g_restorePointTaken.store(true, std::memory_order_release);
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    auto& account = runtime::storage::g_state.account;
    bool committed = false;
    if (account != draft.before) {
        message = "Your account changed while you were editing. Reload it, then apply again; your edits have been kept.";
    } else if (!persistence::commit_account(account, *prepared)) {
        message = "Could not commit this change. Your account was not changed.";
    } else {
        account = *prepared;
        draft.after = draft.before = *prepared;
        draft.dirty = false;
        committed = true;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    if (!committed) return false;

    // Publish the new subclass combinations immediately and persist them for the next launch.
    (void)build_data::publish_ability_buckets(std::span(abilities).first(abilityCount));
    // Every peer holding the account rebuilds its inventory, appearance and roster from the
    // committed state on its next service poll, so the change shows in game with no restart.
    live = server::bap::publish_external_account_mutation() != 0;
    if (firstApply) {
        message = live ? "Applied in game. Your save before this session is in Dawn/editor-backups."
                       : "Saved. It loads when you next sign in. Your previous save is in Dawn/editor-backups.";
    } else {
        message = live ? "Applied in game." : "Saved. It loads when you next sign in.";
    }
    return true;
}

bool republish() noexcept {
    return server::bap::publish_external_account_mutation() != 0;
}
}
