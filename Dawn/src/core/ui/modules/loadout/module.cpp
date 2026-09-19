// SPDX-License-Identifier: GPL-3.0-only
#include <Windows.h>

#include <algorithm>
#include <imgui.h>
#include <string_view>

#include "../registry/ui_module_registry.h"
#include "../ui_module_descriptor.h"
#include "internal.h"
#include "loadout.h"
#include "preview.h"
#include "state/account/inventory/placement.h"
#include "state/equipment/light/definition.h"
#include "state/runtime/runtime.h"

namespace dawn::core::ui::modules::loadout {
namespace {

/** Namespaced stable ID keeps this page distinct from feature modules. */
constexpr std::string_view kStableId = "core.loadout";
/** Menu label for the gear editor. */
constexpr std::string_view kDisplayName = "Loadout";

registry::PageRegistration g_page;

} // namespace

namespace internal {
namespace {

/** The whole editor lives here so every page reaches it without passing it around. */
std::unique_ptr<Model> g_model;

/** Nothing has happened yet, so the bar shows only the sync state until an apply reports. */
constexpr const char* kIdleStatus = "";
/** Shown when the catalog worker throws rather than reporting its own reason. */
constexpr const char* kCatalogFailure = "Could not load the item catalog.";
/** Shown when an edit escapes as an exception, which leaves the account untouched. */
constexpr const char* kFrameFailure = "That action failed; the account is unchanged.";
/** 500 ms between account divergence checks, which copies the account to compare it. */
constexpr std::uint64_t kDivergenceCheckIntervalMs = 500;
/** An apply nobody was signed in for is re-offered to the game for this long. */
constexpr std::uint64_t kRepublishWindowMs = 60000;
/** Shown once a saved-only apply finally reaches a signed-in peer. */
constexpr const char* kLateApplied = "Applied in game.";

/**
 * Notices when the game changed the account under an editor that is not the one editing it.
 * A clean draft is rebased in place, because nothing the player typed can be lost that way. A
 * draft holding edits is left alone and the frame offers the reload instead.
 */
void poll_account_divergence() noexcept {
    Model& state = model();
    if (!state.draft) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (now - state.lastDivergenceCheckTick < kDivergenceCheckIntervalMs) {
        return;
    }
    state.lastDivergenceCheckTick = now;
    // A change applied before sign-in reached nobody; keep offering it until a peer takes it.
    if (state.republishUntilTick != 0) {
        if (now >= state.republishUntilTick) {
            state.republishUntilTick = 0;
        } else if (edit::republish()) {
            state.republishUntilTick = 0;
            // The retry speaks only for the apply it belongs to. An edit made since owns the
            // line, and saying this over a refusal would report that refusal as applied.
            if (!state.draft->dirty) {
                state.status = kLateApplied;
            }
        }
    }
    const state::AccountState account = state::account_snapshot();
    if (account == state.draft->before) {
        state.accountDiverged = false;
        return;
    }
    if (state.draft->dirty) {
        state.accountDiverged = true;
        return;
    }
    state.draft->before = account;
    state.draft->after = account;
    state.accountDiverged = false;
    state.character = (std::min)(state.character,
                                 account.characterCount != 0 ? account.characterCount - 1 : 0);
}

/** Runs the queued apply after every widget of the frame has been submitted. */
void consume_queued_apply() noexcept {
    Model& state = model();
    if (!state.applyRequested) {
        return;
    }
    state.applyRequested = false;
    if (!state.draft || !state.draft->dirty) {
        return;
    }
    bool live = false;
    if (!edit::apply(*state.draft, state.catalog, state.status, live)) {
        // A refused apply can mean the game moved the account on. Check that before the next frame
        // rather than waiting out the poll interval, so the banner and the message agree.
        state.lastDivergenceCheckTick = 0;
        return;
    }
    // The committed image is now the one the game holds, so a banner raised by the last poll
    // would sit above an apply that already resolved it.
    state.accountDiverged = false;
    state.republishUntilTick = live ? 0 : GetTickCount64() + kRepublishWindowMs;
}

} // namespace

Model& model() noexcept {
    return *g_model;
}

bool live() noexcept {
    return g_model != nullptr;
}

state::CharacterState& character() noexcept {
    return model().draft->after.characters[model().character];
}

edit::Item* find_owned_item(std::uint64_t instance) noexcept {
    if (instance == 0 || !model().draft) {
        return nullptr;
    }
    state::CharacterState& owner = character();
    for (auto& slot : owner.equipment.slots) {
        if (slot && slot->instanceSoid == instance) {
            return &*slot;
        }
    }
    for (std::size_t i = 0; i < owner.inventory.count; ++i) {
        if (owner.inventory.values[i].instanceSoid == instance) {
            return &owner.inventory.values[i];
        }
    }
    return nullptr;
}

edit::Item* selected_item() noexcept {
    return find_owned_item(model().selection.instanceSoid);
}

void select(const edit::CatalogItem& item, std::uint64_t instance) noexcept {
    Model& state = model();
    state.selection = {item.definition.definitionHash, instance};
    state.grant = {state.grant.power, 1};
}

void clear_selection() noexcept {
    model().selection = {};
}

bool erase_owned_item(std::uint64_t instance) noexcept {
    Model& state = model();
    if (instance == 0 || !state.draft) {
        return false;
    }
    state::CharacterState& owner = character();
    for (std::size_t i = 0; i < owner.inventory.count; ++i) {
        if (owner.inventory.values[i].instanceSoid != instance) {
            continue;
        }
        state::account::inventory::erase(owner, i);
        if (state.selection.instanceSoid == instance) {
            clear_selection();
        }
        mark_changed();
        return true;
    }
    return false;
}

int power_of(int level) noexcept {
    std::int32_t power = 0;
    return state::equipment::light::item_power(level, power) ? power : 0;
}

int level_of(int power) noexcept {
    return (std::max)(0, power / state::equipment::light::kPowerPerLevel);
}

void mark_changed(bool publish) noexcept {
    Model& state = model();
    if (!state.draft) {
        return;
    }
    state.draft->dirty = true;
    // Armor stat targets belong to whichever item was inspected, and an edit can replace it.
    state.targets.owner = 0;
    if (publish && state.applyInstantly) {
        state.applyRequested = true;
    }
}

void record_edit(bool succeeded) noexcept {
    if (succeeded) {
        mark_changed(true);
    }
}

void record_scalar_edit(bool changed) noexcept {
    if (changed) {
        mark_changed(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        mark_changed(true);
    }
}

void reload_account() noexcept {
    Model& state = model();
    if (!state.draft) {
        state.draft = std::make_unique<edit::Draft>();
    }
    state.draft->before = state::account_snapshot();
    state.draft->after = state.draft->before;
    state.draft->dirty = false;
    const std::size_t count = state.draft->after.characterCount;
    state.character = (std::min)(state.character, count != 0 ? count - 1 : 0);
    state.selection = {};
    state.targets = {};
    state.picker = {};
    state.accountDiverged = false;
    // A discarded draft has no apply left to report on, so the retry must not speak for it later.
    state.republishUntilTick = 0;
    state.status = kIdleStatus;
    // An account saved before the row-generation fix cannot be published until it is repaired.
    // Staging it here means opening the page is enough; instant mode then commits it at once.
    if (edit::normalize(*state.draft, state.status) && state.applyInstantly) {
        state.applyRequested = true;
    }
}

void start_catalog_load() noexcept {
    Model& state = model();
    if (state.loader.joinable()) {
        state.loader.join();
    }
    state.cancelLoad = false;
    state.loadProgress = 0;
    state.loadError.clear();
    state.phase.store(CatalogPhase::loading, std::memory_order_release);
    state.loader = std::thread([] {
        Model& worker = model();
        CatalogPhase phase = CatalogPhase::failed;
        try {
            if (edit::load_catalog(
                    worker.catalog, worker.cancelLoad, worker.loadProgress, worker.loadError)) {
                phase = CatalogPhase::ready;
            }
        } catch (...) {
            worker.loadError = kCatalogFailure;
        }
        worker.phase.store(phase, std::memory_order_release);
    });
}

void reset() noexcept {
    if (g_model == nullptr) {
        return;
    }
    g_model->cancelLoad = true;
    if (g_model->loader.joinable()) {
        g_model->loader.join();
    }
    if (g_model->iconSweeper.joinable()) {
        g_model->iconSweeper.join();
    }
    g_model.reset();
}

} // namespace internal

/** @return True when the Core Loadout page owns its registry slot. */
bool initialize() noexcept {
    try {
        internal::g_model = std::make_unique<internal::Model>();
    } catch (...) {
        return false;
    }
    if (!g_page.acquire(Owner::core, kStableId, kDisplayName, &draw)) {
        internal::reset();
        return false;
    }
    return true;
}

/** Removes the page, stops the catalog worker, and releases every cached package image. */
void shutdown() noexcept {
    g_page.release(&internal::reset);
    preview::shutdown();
}

/** Frame entry. An edit that throws leaves the account and the draft as they were. */
void draw() noexcept {
    if (!internal::live()) {
        return;
    }
    try {
        internal::poll_account_divergence();
        internal::draw();
        internal::consume_queued_apply();
    } catch (...) {
        internal::model().status = internal::kFrameFailure;
    }
}

} // namespace dawn::core::ui::modules::loadout
