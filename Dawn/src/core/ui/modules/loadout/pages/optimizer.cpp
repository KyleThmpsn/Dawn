// SPDX-License-Identifier: GPL-3.0-only
// The sheet follows Sundial's armor stat adjuster by KyleThmpsn. See vendor/sundial/NOTICE.md.
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../art.h"
#include "../controls.h"
#include "../internal.h"
#include "../preview.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;

/** Popup id of the sheet, used by both the action that opens it and the sheet itself. */
constexpr const char* kOptimizerTitle = "Armor Optimizer";
/** The game counts a stat to a hundred, so a minimum goes no higher. */
constexpr int kStatCeiling = edit::kStatCap;
constexpr float kTargetSpeed = 0.25F;
/** Seconds the minimums rest before a plan is worked out for them, as Sundial waits. */
constexpr double kSettleSeconds = 0.15;
/**
 * The table: a column per stat, as wide as the three digits a value runs to and a gap, with its
 * minimum's field a little narrower; the Keep column while swaps are allowed; and the piece column,
 * which takes the rest.
 */
constexpr float kStatColumn = 36.0F;
constexpr float kTargetFieldWidth = 30.0F;
/** One piece row: its icon, the padding around it, and the gap to its name. */
constexpr float kPieceIconExtent = 32.0F;
constexpr float kRowPadding = 4.0F;
constexpr float kIconGap = 8.0F;
/** 256 bytes hold a piece's line, or an outcome naming every change. */
constexpr std::size_t kLineCapacity = 256;
constexpr const char* kClearLabel = "Clear";
constexpr const char* kClearTip = "Reset every minimum.";
constexpr const char* kSwapsLabel = "Swap in Better Armor";
constexpr const char* kSwapsTip = "Allow swapping in stowed armor. Locked armor is never changed.";
/** What each of the Keep column's checkboxes does, said under the pointer, since the column has no head. */
constexpr const char* kKeepTip = "Keep this piece. Its mods can still change.";
constexpr const char* kAdjustLabel = "Adjust Armor";
constexpr const char* kAdjustTip = "Apply every change. Undo reverts it. Shortcut: Enter.";
/** Said under the pointer on Adjust Armor when no plan could be found for the minimums. */
constexpr const char* kNoPlanTip = "No plan meets these minimums.";
constexpr const char* kNowLabel = "Now";
constexpr const char* kProjectedLabel = "Projected";

/** @return True when any stat has a minimum. */
[[nodiscard]] bool targeted() noexcept {
    const edit::Stats& targets = model().optimizer.targets;
    return std::any_of(targets.begin(), targets.end(), [](int value) { return value > 0; });
}

/**
 * @return The last plan found for the armor as it stands, or null while there is none. It may answer
 * minimums since changed, which the sheet shows dimmed until the plan for the new ones is in.
 */
[[nodiscard]] const edit::ArmorPlan* shown_plan() noexcept {
    const Optimizer& optimizer = model().optimizer;
    return optimizer.plan.finished && optimizer.input != nullptr && optimizer.planInput == optimizer.input && targeted()
               ? &optimizer.plan
               : nullptr;
}

/** @return The plan found for the armor and minimums as they stand now, the only one Adjust Armor puts on, or null. */
[[nodiscard]] const edit::ArmorPlan* current_plan() noexcept {
    const Optimizer& optimizer = model().optimizer;
    const edit::ArmorPlan* plan = shown_plan();
    return plan != nullptr && optimizer.planTargets == optimizer.targets ? plan : nullptr;
}

/** @return True while the plan shown answers minimums since changed, so it is drawn dimmed. */
[[nodiscard]] bool plan_stale() noexcept {
    return shown_plan() != nullptr && current_plan() == nullptr;
}

/**
 * @return True when the plan worked out for the armor and minimums as they stand came back
 * unfinished, as it does for armor that has to keep two exotics on. Its job is left standing, so
 * the same plan is not worked out again every frame.
 */
[[nodiscard]] bool plan_failed() noexcept {
    const Optimizer& optimizer = model().optimizer;
    return optimizer.job && optimizer.job->done.load(std::memory_order_acquire) && !optimizer.job->plan.finished
           && optimizer.job->input == optimizer.input && optimizer.job->targets == optimizer.targets;
}

/** @return The other characters of this character's class, whose stowed armor a swap can bring over. */
[[nodiscard]] std::size_t classmates() noexcept {
    const Model& state = model();
    const state::AccountState& account = state.draft->after;
    std::size_t count = 0;
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        count += c != state.character && account.characters[c].characterClass == character().characterClass ? 1U : 0U;
    }
    return count;
}

/**
 * @return A digest of everything a plan's input is taken from: the options, and every piece this
 * character and the others of its class hold, with its plugs. It is compared every frame, so it is
 * made of the account's own values and never of the catalog's.
 */
[[nodiscard]] std::string input_key() {
    const Model& state = model();
    const Optimizer& optimizer = state.optimizer;
    const state::AccountState& account = state.draft->after;
    std::string key;
    const auto put = [&key](const auto& value) { key.append(reinterpret_cast<const char*>(&value), sizeof value); };
    const auto put_item = [&](const edit::Item& item) {
        put(item.instanceSoid);
        put(item.definitionHash);
        put(item.flags);
        put(item.postmaster);
        put(item.sockets.policy);
        put(item.sockets.plugCount);
        for (std::size_t lane = 0; lane < item.sockets.plugCount && lane < item.sockets.plugs.size(); ++lane) {
            put(item.sockets.plugs[lane].value_or(0U));
        }
    };
    put(state.character);
    put(optimizer.options.swaps);
    put(optimizer.options.others);
    put(optimizer.options.kept);
    for (std::size_t c = 0; c < account.characterCount; ++c) {
        const state::CharacterState& holder = account.characters[c];
        if (c != state.character && holder.characterClass != character().characterClass) {
            continue;
        }
        put(c);
        for (std::size_t slot = edit::kFirstPlanSlot; slot < edit::kFirstPlanSlot + edit::kPlanSlots; ++slot) {
            if (holder.equipment.slots[slot]) {
                put_item(*holder.equipment.slots[slot]);
            }
        }
        for (std::size_t i = 0; i < holder.inventory.count; ++i) {
            put_item(holder.inventory.values[i]);
        }
    }
    return key;
}

/** Lets go of the plan being worked out, which the worker then drops as soon as it looks up. */
void cancel_job() noexcept {
    Optimizer& optimizer = model().optimizer;
    if (optimizer.job) {
        optimizer.job->cancel.store(true, std::memory_order_relaxed);
        optimizer.job.reset();
    }
}

/** Starts working out a plan for the armor and minimums as they stand, on a worker so the frame never waits for it. */
void start_job() noexcept {
    Optimizer& optimizer = model().optimizer;
    cancel_job();
    std::shared_ptr<Optimizer::Job> job;
    try {
        job = std::make_shared<Optimizer::Job>();
    } catch (...) {
        return;
    }
    job->input = optimizer.input;
    job->targets = optimizer.targets;
    const auto work = [job]() noexcept {
        try {
            job->plan = edit::plan_armor(*job->input, job->targets, job->cancel);
        } catch (...) {
            job->plan = {};
        }
        job->done.store(true, std::memory_order_release);
    };
    try {
        std::thread(work).detach();
    } catch (...) {
        // With no thread to spare, the plan is worked out here instead, which the frame waits for.
        work();
    }
    optimizer.job = std::move(job);
}

/**
 * Takes in the plan the worker finished. A plan that came back unfinished is not taken, and its job
 * is left standing as the answer for its armor and minimums, so the sheet says no plan meets them
 * rather than working them out again every frame. A new job lets it go.
 */
void collect_job() noexcept {
    Optimizer& optimizer = model().optimizer;
    if (!optimizer.job || !optimizer.job->done.load(std::memory_order_acquire) || !optimizer.job->plan.finished) {
        return;
    }
    optimizer.plan = std::move(optimizer.job->plan);
    optimizer.planInput = optimizer.job->input;
    optimizer.planTargets = optimizer.job->targets;
    optimizer.job.reset();
}

/**
 * Keeps the input and the plan up to date. The input is taken again whenever the armor or the options
 * change. A plan is worked out once the minimums have rested, and not while one is being dragged.
 */
void refresh() noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    try {
        std::string key = input_key();
        if (!optimizer.input || key != optimizer.inputKey) {
            optimizer.input = std::make_shared<const edit::ArmorInput>(
                edit::armor_input(state.draft->after, state.catalog, state.character, optimizer.options));
            optimizer.inputKey = std::move(key);
        }
    } catch (...) {
        return;
    }
    const double now = ImGui::GetTime();
    if (optimizer.targets != optimizer.seenTargets) {
        optimizer.seenTargets = optimizer.targets;
        optimizer.changedAt = now;
    }
    collect_job();
    if (!targeted()) {
        cancel_job();
        return;
    }
    const bool answered = current_plan() != nullptr;
    // A job for the armor and minimums standing is still working, or came back with no plan.
    const bool underway = optimizer.job && optimizer.job->input == optimizer.input && optimizer.job->targets == optimizer.targets;
    if (answered || underway || ImGui::IsAnyItemActive() || now - optimizer.changedAt < kSettleSeconds) {
        return;
    }
    start_job();
}

/** @return The count of something with its noun, as "1 swap" or "3 stat plugs". */
[[nodiscard]] std::string counted(unsigned count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

/** @return A list of phrases read as a sentence: "a", "a and b", "a, b and c". */
[[nodiscard]] std::string listed(const std::vector<std::string>& parts) {
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            text += i + 1 == parts.size() ? " and " : ", ";
        }
        text += parts[i];
    }
    return text;
}

/** @return What one piece's refit changes, as "2 stat plugs and 1 Masterwork", or empty for nothing. */
[[nodiscard]] std::string refit_words(const edit::ArmorPlan::Piece& piece) {
    std::vector<std::string> parts;
    if (piece.allocations != 0) {
        parts.push_back(counted(piece.allocations, "stat plug", "stat plugs"));
    }
    if (piece.mods != 0) {
        parts.push_back(counted(piece.mods, "stat mod", "stat mods"));
    }
    if (piece.masterworks != 0) {
        parts.push_back(counted(piece.masterworks, "Masterwork", "Masterworks"));
    }
    return listed(parts);
}

/** @return The stats a plan falls short on, named with how far: "Recovery 6, Strength 2". */
[[nodiscard]] std::string shortfall_words(const edit::ArmorPlan& plan) {
    const Model& state = model();
    std::vector<std::string> parts;
    for (std::size_t shown = 0; shown < plan.shortfalls.size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        if (plan.shortfalls[stat] > 0) {
            parts.push_back(std::string(edit::stat_label(state.catalog, stat)) + " " + std::to_string(plan.shortfalls[stat]));
        }
    }
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        text += (i != 0 ? ", " : "") + parts[i];
    }
    return text;
}

/**
 * Where the table's columns sit: the piece column's left edge and where its words stop, the centre
 * of the Keep column, which stands while swaps are allowed, and where the first stat column starts.
 */
struct Columns {
    float left{};
    float words{};
    bool keeps{};
    float keep{};
    float stats{};
    float width{};
};

/** @return The Keep column's width, which is its checkbox's. */
[[nodiscard]] float keep_width() noexcept {
    return ImGui::GetFrameHeight();
}

[[nodiscard]] Columns columns() noexcept {
    Columns table;
    table.left = ImGui::GetCursorScreenPos().x;
    table.width = ImGui::GetContentRegionAvail().x;
    table.stats = table.left + table.width - (pixels(kStatColumn) * static_cast<float>(edit::Stats{}.size()));
    table.keeps = model().optimizer.options.swaps;
    const float gap = pixels(controls::kGroupGap);
    table.words = table.stats - gap;
    if (table.keeps) {
        table.keep = table.words - (keep_width() * 0.5F);
        table.words -= keep_width() + gap;
    }
    return table;
}

[[nodiscard]] float column_center(const Columns& columns, std::size_t shown) noexcept {
    return columns.stats + (pixels(kStatColumn) * (static_cast<float>(shown) + 0.5F));
}

/** Draws one number centred in a stat's column. */
void draw_value(const Columns& columns, std::size_t shown, float top, int value, ImU32 color) noexcept {
    char text[16]{};
    (void)std::snprintf(text, sizeof text, "%d", value);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::GetWindowDrawList()->AddText({column_center(columns, shown) - (width * 0.5F), top}, color, text);
}

/** Heads one stat's column with its icon, centred, or its first letters until the icon is in, and names it under the pointer. */
void draw_stat_head(std::size_t stat, float center, float top) noexcept {
    const Model& state = model();
    const float icon = pixels(kStatIconExtent);
    const float line = ImGui::GetTextLineHeight();
    const std::string_view label = edit::stat_label(state.catalog, stat);
    const std::uint32_t tag = state.catalog.statIconTags[stat];
    ImVec2 min{center - (icon * 0.5F), top + ((line - icon) * 0.5F)};
    ImVec2 max{min.x + icon, min.y + icon};
    if (tag == 0 || !preview::draw(tag, min, icon)) {
        const std::string brief(label.substr(0, (std::min)(label.size(), kStatAbbreviation)));
        const float width = ImGui::CalcTextSize(brief.c_str()).x;
        min = {center - (width * 0.5F), top};
        max = {min.x + width, top + line};
        ImGui::GetWindowDrawList()->AddText(min, ImGui::GetColorU32(ImGuiCol_TextDisabled), brief.c_str());
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max)) {
        ImGui::SetTooltip("%.*s", static_cast<int>(label.size()), label.data());
    }
}

/** Draws the row of heads over the table's columns: each stat's icon. */
void draw_heads(const Columns& columns) noexcept {
    const Model& state = model();
    const float top = ImGui::GetCursorScreenPos().y;
    for (std::size_t shown = 0; shown < state.catalog.statOrder.size(); ++shown) {
        draw_stat_head(state.catalog.statOrder[shown], column_center(columns, shown), top);
    }
    ImGui::Dummy({columns.width, ImGui::GetTextLineHeight()});
}

/** Draws the minimums row: a field under each stat, which the stat's own head names. */
void draw_minimums(const Columns& columns) noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    const float top = ImGui::GetCursorScreenPos().y;
    const float field = pixels(kTargetFieldWidth);
    for (std::size_t shown = 0; shown < optimizer.targets.size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        ImGui::PushID(static_cast<int>(stat));
        ImGui::SetCursorScreenPos({column_center(columns, shown) - (field * 0.5F), top});
        ImGui::SetNextItemWidth(field);
        (void)ImGui::DragInt("##minimum", &optimizer.targets[stat], kTargetSpeed, 0, kStatCeiling, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered()) {
            const std::string_view label = edit::stat_label(state.catalog, stat);
            ImGui::SetTooltip("Minimum %.*s. 0 ignores it.", static_cast<int>(label.size()), label.data());
        }
        ImGui::PopID();
    }
}

/** @return What a piece row says under its name: where the piece comes from, then what the plan changes on it. */
[[nodiscard]] std::string piece_line(const edit::ArmorCandidate& piece, const edit::ArmorPlan::Piece* planned) {
    const std::size_t who = model().character;
    if (piece.instance == 0) {
        return "Nothing equipped";
    }
    if (piece.locked) {
        return "Locked";
    }
    std::string line = !planned || planned->candidate == 0 ? std::string("Equipped")
                       : piece.holder == who             ? std::string("From inventory")
                                                         : "From " + character_label(piece.holder);
    if (planned != nullptr) {
        const std::string refit = refit_words(*planned);
        line += refit.empty() ? "" : ", " + refit;
    }
    return line;
}

/** @return The piece a candidate names, wherever on the account it is held, or null. */
[[nodiscard]] const edit::Item* held_piece(const edit::ArmorCandidate& piece) noexcept {
    const state::AccountState& account = model().draft->after;
    if (piece.instance == 0 || piece.holder >= account.characterCount) {
        return nullptr;
    }
    const state::CharacterState& holder = account.characters[piece.holder];
    for (const auto& item : holder.equipment.slots) {
        if (item && item->instanceSoid == piece.instance) {
            return &*item;
        }
    }
    for (std::size_t i = 0; i < holder.inventory.count; ++i) {
        if (holder.inventory.values[i].instanceSoid == piece.instance) {
            return &holder.inventory.values[i];
        }
    }
    return nullptr;
}

/** Shows the piece under the pointer in the game's own tooltip, as the plan would leave it. */
void explain_piece(const edit::ArmorCandidate& piece, const edit::ArmorPlan::Piece* planned) noexcept {
    const Model& state = model();
    const edit::CatalogItem* definition = state.catalog.find(piece.definition);
    const edit::Item* held = held_piece(piece);
    if (definition == nullptr || held == nullptr) {
        return;
    }
    edit::Item projected = *held;
    if (planned != nullptr && !planned->plugs.empty() && edit::materialize(projected, state.catalog)) {
        for (const auto& [lane, plug] : planned->plugs) {
            if (lane < projected.sockets.plugCount) {
                projected.sockets.plugs[lane] = plug != 0 ? std::optional<std::uint32_t>(plug) : std::nullopt;
            }
        }
    }
    tooltip::draw(*definition, &projected);
}

/**
 * Sets one line of a piece row within a width, and cuts it short only when it runs past it.
 * @return True when it was cut, so the row can show it whole under the pointer.
 */
[[nodiscard]] bool fitted_text(const std::string& text, ImVec2 at, float width, ImU32 color) noexcept {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        ImGui::GetWindowDrawList()->AddText(at, color, text.c_str());
        return false;
    }
    art::clipped_text(text, at, width, color);
    return true;
}

/** Dims what is drawn until the matching `ImGui::PopStyleVar`, as a disabled control is dimmed. */
void push_dimmed() noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ImGui::GetStyle().DisabledAlpha);
}

/**
 * Draws one armor slot's row: the piece the plan puts there, what it changes on it, its stats once
 * refitted, and its Keep checkbox while swaps are allowed. A stat the plan moves is set in full text,
 * one it leaves is muted, so the changes read at a glance, and a plan answering minimums since
 * changed is dimmed. The piece's own tooltip shows over the row, and the whole second line over
 * that line when it had to be cut.
 */
void draw_piece(const Columns& columns, std::size_t slot) noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    const edit::ArmorPlan* plan = shown_plan();
    const bool stale = plan_stale();
    const auto& candidates = optimizer.input->slots[slot];
    const edit::ArmorPlan::Piece* planned = plan != nullptr ? &plan->pieces[slot] : nullptr;
    const std::size_t index = planned != nullptr && planned->candidate < candidates.size() ? planned->candidate : 0;
    const edit::ArmorCandidate& worn = candidates.front();
    const edit::ArmorCandidate& piece = candidates[index];
    const edit::CatalogItem* definition = state.catalog.find(piece.definition);

    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float padding = pixels(kRowPadding);
    const float extent = pixels(kPieceIconExtent);
    const float height = extent + (padding * 2.0F);
    const float line = ImGui::GetTextLineHeight();
    if (definition != nullptr) {
        art::icon(*definition, {origin.x, origin.y + padding}, extent);
    }
    // The Keep checkbox stands in its own column, under its head, and only while a swap could take
    // the slot's piece away.
    ImGui::PushID(static_cast<int>(slot));
    bool keepHovered = false;
    if (columns.keeps && !worn.locked && worn.instance != 0) {
        const float box = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos({columns.keep - (box * 0.5F), origin.y + ((height - box) * 0.5F)});
        (void)controls::checkbox("##keep", &optimizer.options.kept[slot]);
        keepHovered = ImGui::IsItemHovered();
        if (keepHovered) {
            ImGui::SetTooltip("%s", kKeepTip);
        }
    }
    const float textLeft = origin.x + extent + pixels(kIconGap);
    const float textWidth = (std::max)(0.0F, columns.words - textLeft);
    const float textTop = origin.y + ((height - (line * 2.0F)) * 0.5F);
    const std::string words = piece_line(piece, planned);
    if (stale) {
        push_dimmed();
    }
    art::clipped_text(definition != nullptr ? definition->name : std::string(edit::kSlots[edit::kFirstPlanSlot + slot]),
                      {textLeft, textTop}, textWidth, ImGui::GetColorU32(definition != nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    const bool cut = fitted_text(words, {textLeft, textTop + line}, textWidth, ImGui::GetColorU32(tooltip::muted()));
    const edit::Stats& after = planned != nullptr ? planned->shown : piece.shown;
    for (std::size_t shown = 0; shown < after.size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        const bool moved = planned != nullptr && after[stat] != worn.shown[stat];
        draw_value(columns, shown, origin.y + ((height - line) * 0.5F), after[stat],
                   ImGui::GetColorU32(moved ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : tooltip::muted()));
    }
    if (stale) {
        ImGui::PopStyleVar();
    }
    const ImVec2 corner{origin.x + columns.width, origin.y + height};
    if (!keepHovered && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, corner)) {
        const ImVec2 wordsTop{textLeft, textTop + line};
        if (cut && ImGui::IsMouseHoveringRect(wordsTop, {textLeft + textWidth, wordsTop.y + line})) {
            ImGui::SetTooltip("%s", words.c_str());
        } else {
            explain_piece(piece, planned);
        }
    }
    draw->AddLine({origin.x, corner.y}, corner, ImGui::GetColorU32(tooltip::rule_color()));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({columns.width, height});
    ImGui::PopID();
}

/**
 * Draws one totals row: its label set against the first stat column, as a table's total row is, and
 * a total under each stat. The projected row is the shown plan's, dimmed while that plan answers
 * minimums since changed.
 * @param totals The totals, or null for a row that has none to show yet.
 */
void draw_totals(const Columns& columns, const char* label, const edit::Stats* totals, bool projected) noexcept {
    const Model& state = model();
    const bool stale = projected && plan_stale();
    if (stale) {
        push_dimmed();
    }
    const float top = ImGui::GetCursorScreenPos().y;
    const float labelLeft = columns.stats - ImGui::GetStyle().ItemSpacing.x - controls::spaced_width(label);
    (void)controls::spaced(label, {(std::max)(columns.left, labelLeft), top}, ImGui::GetColorU32(tooltip::muted()));
    for (std::size_t shown = 0; totals != nullptr && shown < totals->size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        const int total = (*totals)[stat];
        // A projected total short of the minimum its plan answers is set in the waiting colour.
        const int minimum = state.optimizer.planTargets[stat];
        const bool short_of = projected && minimum > 0 && (std::min)(total, kStatCeiling) < minimum;
        const ImVec4 color = !projected ? tooltip::muted() : short_of ? tooltip::pending() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_value(columns, shown, top, total, ImGui::GetColorU32(color));
    }
    ImGui::Dummy({columns.width, ImGui::GetTextLineHeight()});
    if (stale) {
        ImGui::PopStyleVar();
    }
}

/** Puts the plan on, all at once, as one edit that Undo takes back whole. */
void adjust() noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    const edit::ArmorPlan* plan = current_plan();
    if (plan == nullptr || !plan->changes()) {
        return;
    }
    std::string refused;
    bool adjusted = false;
    try {
        adjusted = edit::apply_armor_plan(*state.draft, state.catalog, *optimizer.planInput, *plan, refused);
    } catch (...) {
        refused = "Can't adjust your armor.";
    }
    if (!adjusted) {
        state.status = refused;
        state.statusFailed = true;
        return;
    }
    unsigned allocations = 0, mods = 0, masterworks = 0;
    for (const auto& piece : plan->pieces) {
        allocations += piece.allocations;
        mods += piece.mods;
        masterworks += piece.masterworks;
    }
    std::vector<std::string> parts;
    if (allocations != 0) parts.push_back(counted(allocations, "stat plug", "stat plugs"));
    if (mods != 0) parts.push_back(counted(mods, "stat mod", "stat mods"));
    if (masterworks != 0) parts.push_back(counted(masterworks, "Masterwork", "Masterworks"));
    if (plan->swaps != 0) parts.push_back(counted(plan->swaps, "swap", "swaps"));
    std::string message = "Adjusted armor: " + listed(parts) + ". ";
    message += plan->exact() ? std::string("Every minimum met.") : "Short: " + shortfall_words(*plan) + ".";
    state.status = message;
    state.editNote = message;
    record_edit(true);
}

/** @return The width a checkbox takes with its label, as `controls::checkbox` lays it out. */
[[nodiscard]] float checkbox_width(const char* label) noexcept {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
}

/**
 * Draws the head's own controls at the far end of its two lines: whether a plan may swap pieces,
 * level with the title, and level with the muted line under it, whether it may reach the other
 * characters' armor, while there are any, and Clear, over the minimum fields it empties.
 * @param head Top-left of the head in screen space.
 * @param under Where the head ended, under its muted line.
 * @param right Screen X both lines end at.
 */
void draw_head_controls(ImVec2 head, ImVec2 under, float right) noexcept {
    Optimizer& optimizer = model().optimizer;
    edit::ArmorOptions& options = optimizer.options;
    const float box = ImGui::GetFrameHeight();
    const float line = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float detail = under.y - spacing - line;
    const float title = detail - spacing - head.y;
    ImGui::SetCursorScreenPos({right - checkbox_width(kSwapsLabel), head.y + ((title - box) * 0.5F)});
    (void)controls::checkbox(kSwapsLabel, &options.swaps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kSwapsTip);
    }
    const float clear = ImGui::CalcTextSize(kClearLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SetCursorScreenPos({right - clear, detail});
    ImGui::BeginDisabled(!targeted());
    if (ImGui::SmallButton(kClearLabel)) {
        optimizer.targets = {};
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", kClearTip);
    }
    // The other characters of this class hold armor a swap can bring over, when there are any.
    if (classmates() != 0) {
        char label[kLineCapacity]{};
        (void)std::snprintf(label, sizeof label, "Use Other %ss' Armor", art::class_name(character().characterClass));
        ImGui::SetCursorScreenPos(
            {right - clear - pixels(controls::kGroupGap) - checkbox_width(label), detail + ((line - box) * 0.5F)});
        ImGui::BeginDisabled(!options.swaps);
        (void)controls::checkbox(label, &options.others);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Also swap in armor stowed on your other characters.");
        }
    }
    ImGui::SetCursorScreenPos(under);
}

/** @return The height of the footer: the outcome line, a row gap, and the row of actions. */
[[nodiscard]] float footer_height() noexcept {
    return ImGui::GetTextLineHeight() + pixels(controls::kRowSpacing) + pixels(controls::kActionHeight);
}

/**
 * Moves the cursor to the top of the footer, so it stands on the sheet's bottom edge whatever the
 * sheet holds above it. The foot of the content region is the sheet's height less its padding.
 * @return Where the row of actions starts, as a window position.
 */
[[nodiscard]] float anchor_footer() noexcept {
    const float foot = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y;
    const float top = (std::max)(ImGui::GetCursorPosY(), foot - footer_height());
    ImGui::SetCursorPosY(top);
    return top + ImGui::GetTextLineHeight() + pixels(controls::kRowSpacing);
}

/**
 * Draws the sheet's action at the height of a sheet's actions: Adjust Armor, white only while there
 * is a change to make. Enter adjusts as the button does, unless a minimum is being typed in or held,
 * and Escape closes the sheet.
 */
void draw_actions() noexcept {
    const edit::ArmorPlan* plan = current_plan();
    const bool ready = plan != nullptr && plan->changes();
    const float action = pixels(controls::kActionHeight);
    const float adjustWidth = ImGui::CalcTextSize(kAdjustLabel).x + (ImGui::GetStyle().FramePadding.x * 4.0F);
    const bool entered = ready && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()
                         && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    if (ready) {
        if (controls::primary_button(kAdjustLabel, {adjustWidth, action}) || entered) {
            adjust();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", kAdjustTip);
        }
    } else {
        ImGui::BeginDisabled();
        (void)ImGui::Button(kAdjustLabel, {adjustWidth, action});
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", !targeted()       ? "Set a minimum first."
                                    : plan_failed()   ? kNoPlanTip
                                    : plan == nullptr ? "Still working."
                                                      : "No changes needed.");
        }
    }
    // Escape closes a field's own popup first, and a field being typed in keeps it.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        ImGui::CloseCurrentPopup();
    }
}

} // namespace

void open_optimizer() noexcept {
    Optimizer& optimizer = model().optimizer;
    optimizer.inputKey.clear();
    optimizer.plan = {};
    optimizer.planInput.reset();
    model().statusAtSheet = model().status;
    ImGui::OpenPopup(kOptimizerTitle);
}

void draw_optimizer_modal() noexcept {
    if (!tooltip::begin_sheet(kOptimizerTitle)) {
        // A plan no one will read is let go.
        cancel_job();
        return;
    }
    refresh();
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    // The head names the sheet and whose armor it is; the options and Clear sit at its far end, so
    // the table starts right under it.
    const ImVec2 head = ImGui::GetCursorScreenPos();
    const float right = head.x + ImGui::GetContentRegionAvail().x;
    tooltip::draw_sheet_head(kOptimizerTitle, character_label(state.character).c_str());
    draw_head_controls(head, ImGui::GetCursorScreenPos(), right);
    controls::space(controls::kSectionSpacing);
    if (!optimizer.input) {
        ImGui::TextColored(tooltip::muted(), "Can't read your armor.");
    } else {
        const Columns table = columns();
        draw_heads(table);
        draw_minimums(table);
        controls::space(controls::kRuleSpacing);
        ImGui::Separator();
        for (std::size_t slot = 0; slot < edit::kPlanSlots; ++slot) {
            draw_piece(table, slot);
        }
        controls::space(controls::kRowSpacing);
        draw_totals(table, kNowLabel, &optimizer.input->current, false);
        const edit::ArmorPlan* plan = shown_plan();
        draw_totals(table, kProjectedLabel, plan != nullptr ? &plan->totals : nullptr, true);
    }
    // The outcome and the actions stand on the sheet's bottom edge, whatever the table above them takes.
    const float actions = anchor_footer();
    draw_sheet_outcome();
    ImGui::SetCursorPosY(actions);
    draw_actions();
    tooltip::end_sheet();
}

} // namespace dawn::core::ui::modules::loadout::internal
