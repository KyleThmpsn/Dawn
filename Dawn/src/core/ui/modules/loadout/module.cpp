// SPDX-License-Identifier: GPL-3.0-only
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../registry/ui_module_registry.h"
#include "../ui_module_descriptor.h"
#include "art.h"
#include "internal.h"
#include "loadout.h"
#include "presets.h"
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
/** 64 bytes hold where a sent item went: a class name and a one-digit character slot. */
constexpr std::size_t kSentMessageCapacity = 64;
/**
 * Edits kept for undo. A step holds only the characters it changed, about 90 KB each, so this many
 * is a few megabytes at the most.
 */
constexpr std::size_t kHistoryDepth = 30;
/** 128 bytes hold what an undo says it did, with every character on the account named. */
constexpr std::size_t kHistoryMessageCapacity = 128;
/** 32 bytes hold a character's name as the tabs write it: a class name and a one-digit slot. */
constexpr std::size_t kCharacterLabelCapacity = 32;

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
                state.statusFailed = false;
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
    // What the game changed is no edit of the player's, so the next edit is measured from here.
    // The steps already kept stay: each checks the characters it touches before it is retraced.
    if (state.history.baseline) {
        *state.history.baseline = account;
    }
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
    const bool applied = edit::apply(*state.draft, state.catalog, state.status, live);
    state.statusFailed = !applied;
    // An edit's own note, such as what a loadout could not put back, leads the apply's outcome
    // rather than being written over by it. It belongs to this apply whichever way it went.
    if (applied && !state.editNote.empty()) {
        state.status = state.editNote + " " + state.status;
    }
    state.editNote.clear();
    if (!applied) {
        // A refused apply can mean the game moved the account on. Check that before the next frame
        // rather than waiting out the poll interval, so the banner and the message agree.
        state.lastDivergenceCheckTick = 0;
        return;
    }
    // The committed image is now the one the game holds, so a banner raised by the last poll
    // would sit above an apply that already resolved it.
    state.accountDiverged = false;
    state.republishUntilTick = live ? 0 : GetTickCount64() + kRepublishWindowMs;
    // The commit renumbers what it changed. That is no edit, so the next one is measured from here;
    // an edit still being made keeps the image it started from.
    if (state.history.baseline && !state.history.pending) {
        *state.history.baseline = state.draft->after;
    }
}

/**
 * @return What one step changed, named as the tabs name the characters, such as "Hunter 1 and the
 * account items" or "your saved loadouts".
 */
[[nodiscard]] std::string step_subject(const History::Step& step) noexcept {
    const state::AccountState& account = model().draft->after;
    std::string subject;
    const std::size_t parts =
        step.account.characters.size() + (step.account.stacks ? 1U : 0U) + (step.loadouts ? 1U : 0U);
    std::size_t written = 0;
    const auto join = [&](const std::string& part) {
        if (written != 0) {
            subject += written + 1 == parts ? " and " : ", ";
        }
        subject += part;
        ++written;
    };
    for (const edit::EditStep::Character& change : step.account.characters) {
        std::string name = "a character no longer here";
        for (std::size_t c = 0; c < account.characterCount; ++c) {
            if (account.characters[c].soid == change.soid) {
                name = character_label(c);
            }
        }
        join(name);
    }
    if (step.account.stacks) {
        join("the account items");
    }
    if (step.loadouts) {
        join("your saved loadouts");
    }
    return subject;
}

/** Keeps one finished step for undo, which leaves nothing to redo. */
void keep_step(History::Step step) noexcept {
    History& history = model().history;
    history.undo.push_back(std::move(step));
    if (history.undo.size() > kHistoryDepth) {
        history.undo.erase(history.undo.begin());
    }
    history.redo.clear();
}

/** The saved loadouts sheet lets go of its choice and anything it was asking, as the list changed under it. */
void reset_loadouts_sheet() noexcept {
    Loadouts& loadouts = model().loadouts;
    loadouts.chosen = -1;
    loadouts.pendingDelete = -1;
    loadouts.pendingSave = -1;
    loadouts.renaming = -1;
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

std::string character_label(std::size_t index) noexcept {
    const state::AccountState& account = model().draft->after;
    char label[kCharacterLabelCapacity]{};
    (void)std::snprintf(
        label, sizeof label, "%s %zu", art::class_name(account.characters[index].characterClass), index + 1);
    return label;
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

bool send_item(std::uint64_t instance, std::size_t target) noexcept {
    Model& state = model();
    const bool sent =
        edit::transfer(*state.draft, state.catalog, state.character, target, instance, state.status);
    if (sent) {
        const state::AccountState& account = state.draft->after;
        char message[kSentMessageCapacity]{};
        (void)std::snprintf(message,
                            sizeof message,
                            "Sent to %s %zu.",
                            art::class_name(account.characters[target].characterClass),
                            target + 1);
        state.status = message;
        // Carried into the apply, which otherwise says only that something was applied.
        state.editNote = message;
        // The item is no longer on this character, so a pane bound to it has nothing to show.
        if (state.selection.instanceSoid == instance) {
            clear_selection();
        }
    }
    record_edit(sent);
    return sent;
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
    state.history.pending = true;
    // Armor stat targets belong to whichever item was inspected, and an edit can replace it.
    state.targets.owner = 0;
    if (publish && state.applyInstantly) {
        state.applyRequested = true;
    }
}

void record_edit(bool succeeded) noexcept {
    // The edit wrote its own outcome into the status; a refusal is marked so the bar sets it apart.
    model().statusFailed = !succeeded;
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
    state.statusFailed = false;
    state.editNote.clear();
    // An account saved before the row-generation fix cannot be published until it is repaired.
    // Staging it here means opening the page is enough; instant mode then commits it at once.
    if (edit::normalize(*state.draft, state.status) && state.applyInstantly) {
        state.applyRequested = true;
    }
    // A reload discards the draft the kept edits were made in, so they go with it. The repair above
    // is no edit of the player's, and is not one to take back.
    state.history.undo.clear();
    state.history.redo.clear();
    state.history.pending = false;
    state.history.loadoutsPending = false;
    state.history.loadoutsBefore.clear();
    state.history.baseline = std::make_unique<state::AccountState>(state.draft->after);
    state.picked.clear();
}

void record_history() noexcept {
    Model& state = model();
    History& history = state.history;
    if (!state.draft) {
        return;
    }
    if (!history.baseline) {
        history.baseline = std::make_unique<state::AccountState>(state.draft->after);
        history.pending = false;
        return;
    }
    // A control still held is an edit still being made: a drag records once, when it is let go.
    if ((!history.pending && !history.loadoutsPending) || ImGui::IsAnyItemActive()) {
        return;
    }
    History::Step step;
    const bool account = history.pending && edit::capture_step(*history.baseline, state.draft->after, step.account);
    // A change the edit made to the saved loadouts goes with it, so one undo takes back both.
    if (history.loadoutsPending) {
        step.loadouts = true;
        step.loadoutsBefore = std::move(history.loadoutsBefore);
        step.loadoutsAfter = state.loadouts.entries;
    }
    history.pending = false;
    history.loadoutsPending = false;
    history.loadoutsBefore.clear();
    if (account || step.loadouts) {
        keep_step(std::move(step));
    }
    *history.baseline = state.draft->after;
}

void record_loadouts_change(std::vector<edit::SavedLoadout> before) noexcept {
    History::Step step;
    step.loadouts = true;
    step.loadoutsBefore = std::move(before);
    step.loadoutsAfter = model().loadouts.entries;
    keep_step(std::move(step));
}

void note_loadouts_change(std::vector<edit::SavedLoadout> before) noexcept {
    History& history = model().history;
    // The first change of the frame holds the list as it stood before any of them.
    if (!history.loadoutsPending) {
        history.loadoutsBefore = std::move(before);
        history.loadoutsPending = true;
    }
}

std::string history_subject(bool forward) noexcept {
    const History& history = model().history;
    const std::vector<History::Step>& steps = forward ? history.redo : history.undo;
    return steps.empty() || !model().draft ? std::string() : step_subject(steps.back());
}

void retrace_edit(bool forward) noexcept {
    Model& state = model();
    History& history = state.history;
    // An edit made this frame, not yet recorded, is recorded first, so it is the one an undo takes.
    record_history();
    std::vector<History::Step>& from = forward ? history.redo : history.undo;
    std::vector<History::Step>& onto = forward ? history.undo : history.redo;
    if (!state.draft || from.empty()) {
        return;
    }
    const History::Step& step = from.back();
    const std::string subject = step_subject(step);
    std::string refused;
    // A step is taken back whole or not at all: its saved loadouts are checked before its account
    // edit is retraced, and the draft is put back if the loadouts cannot then be written.
    if (step.loadouts && state.loadouts.entries != (forward ? step.loadoutsBefore : step.loadoutsAfter)) {
        refused = forward ? "Can't redo: your saved loadouts changed."
                          : "Can't undo: your saved loadouts changed.";
    }
    std::unique_ptr<state::AccountState> draftBefore;
    const bool dirtyBefore = state.draft->dirty;
    if (refused.empty() && (!step.account.characters.empty() || step.account.stacks)) {
        draftBefore = std::make_unique<state::AccountState>(state.draft->after);
        (void)edit::retrace(*state.draft, step.account, forward, refused);
    }
    if (!refused.empty()) {
        // The account has moved past the step, so it will never retrace; left on top, it would stand
        // in front of every step under it.
        from.pop_back();
        state.status = refused + (forward ? " Removed from Redo." : " Removed from Undo.");
        state.statusFailed = true;
        return;
    }
    if (step.loadouts) {
        std::vector<edit::SavedLoadout> previous = state.loadouts.entries;
        state.loadouts.entries = forward ? step.loadoutsAfter : step.loadoutsBefore;
        if (!presets::save(state.loadouts.entries)) {
            state.loadouts.entries = std::move(previous);
            if (draftBefore) {
                state.draft->after = *draftBefore;
                state.draft->dirty = dirtyBefore;
            }
            state.status = forward ? "Can't write the loadouts file. Nothing redone."
                                   : "Can't write the loadouts file. Nothing undone.";
            state.statusFailed = true;
            return;
        }
        reset_loadouts_sheet();
    }
    onto.push_back(std::move(from.back()));
    from.pop_back();
    // The retrace is not an edit of its own, so nothing is recorded for it.
    if (history.baseline) {
        *history.baseline = state.draft->after;
    }
    history.pending = false;
    // Whatever the panes were bound to may have gone with the edit.
    state.targets.owner = 0;
    if (state.selection.instanceSoid != 0 && find_owned_item(state.selection.instanceSoid) == nullptr) {
        clear_selection();
    }
    char message[kHistoryMessageCapacity]{};
    (void)std::snprintf(message,
                        sizeof message,
                        forward ? "Redid the change to %s." : "Undid the last change to %s.",
                        subject.c_str());
    state.status = message;
    state.statusFailed = false;
    // A retrace that lands back on the account the game holds leaves nothing to apply.
    if (state.draft->dirty) {
        state.editNote = message;
        state.applyRequested = state.applyInstantly;
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
        // An edit is recorded against the image before it, so this runs before the apply renumbers.
        internal::record_history();
        internal::consume_queued_apply();
    } catch (...) {
        internal::model().status = internal::kFrameFailure;
        internal::model().statusFailed = true;
    }
}

} // namespace dawn::core::ui::modules::loadout
