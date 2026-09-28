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
/** The table: a column per stat, each as wide as its minimum's field and a gap, and the piece column takes the rest. */
constexpr float kStatColumn = 44.0F;
constexpr float kTargetFieldWidth = 38.0F;
/** One piece row: its icon, the padding around it, and the gap to its name. */
constexpr float kPieceIconExtent = 32.0F;
constexpr float kRowPadding = 4.0F;
constexpr float kIconGap = 8.0F;
constexpr float kCloseWidth = 100.0F;
/** 256 bytes hold the head's muted line, a piece's line, or an outcome naming every change. */
constexpr std::size_t kLineCapacity = 256;
constexpr const char* kTargetsHeading = "Minimums";
constexpr const char* kClearLabel = "Clear";
constexpr const char* kClearTip = "Reset every minimum.";
constexpr const char* kSwapsLabel = "Swap in Better Armor";
constexpr const char* kSwapsTip = "Allow swapping in stowed armor. Locked armor is never changed.";
constexpr const char* kKeepLabel = "Keep";
constexpr const char* kKeepTip = "Keep this piece. Its mods can still change.";
constexpr const char* kAdjustLabel = "Adjust Armor";
constexpr const char* kAdjustTip = "Apply every change. Undo reverts it.";
constexpr const char* kNowLabel = "Now";
constexpr const char* kProjectedLabel = "Projected";

/** @return True when any stat has a minimum. */
[[nodiscard]] bool targeted() noexcept {
    const edit::Stats& targets = model().optimizer.targets;
    return std::any_of(targets.begin(), targets.end(), [](int value) { return value > 0; });
}

/** @return The plan found for the armor and minimums as they stand now, or null while there is none. */
[[nodiscard]] const edit::ArmorPlan* current_plan() noexcept {
    const Optimizer& optimizer = model().optimizer;
    return optimizer.plan.finished && optimizer.input != nullptr && optimizer.planInput == optimizer.input
                   && optimizer.planTargets == optimizer.targets && targeted()
               ? &optimizer.plan
               : nullptr;
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

/** Takes in the plan the worker finished, when it answers the armor and minimums still standing. */
void collect_job() noexcept {
    Optimizer& optimizer = model().optimizer;
    if (!optimizer.job || !optimizer.job->done.load(std::memory_order_acquire)) {
        return;
    }
    if (optimizer.job->plan.finished) {
        optimizer.plan = std::move(optimizer.job->plan);
        optimizer.planInput = optimizer.job->input;
        optimizer.planTargets = optimizer.job->targets;
    }
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

/** @return What one piece's refit changes, as "2 stat plugs and a Masterwork", or empty for nothing. */
[[nodiscard]] std::string refit_words(const edit::ArmorPlan::Piece& piece) {
    std::vector<std::string> parts;
    if (piece.allocations != 0) {
        parts.push_back(counted(piece.allocations, "stat plug", "stat plugs"));
    }
    if (piece.mods != 0) {
        parts.push_back(piece.mods == 1 ? std::string("a stat mod") : counted(piece.mods, "stat mod", "stat mods"));
    }
    if (piece.masterworks != 0) {
        parts.push_back("a Masterwork");
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

/** @return The head's muted line: whose armor, then what the plan comes to. */
[[nodiscard]] std::string head_line() {
    const Model& state = model();
    std::string status;
    const edit::ArmorPlan* plan = current_plan();
    if (!targeted()) {
        status = "Set a minimum";
    } else if (plan == nullptr) {
        status = "Working";
    } else if (plan->exact()) {
        status = plan->changes() ? "Every minimum met" : "Already meets every minimum";
    } else {
        std::size_t short_of = 0;
        for (const int value : plan->shortfalls) {
            short_of += value > 0 ? 1U : 0U;
        }
        status = counted(static_cast<unsigned>(short_of), "minimum", "minimums") + " short";
    }
    return character_label(state.character) + "  /  " + status;
}

/** Where the table's columns sit: the piece column's left edge, and where the first stat column starts. */
struct Columns {
    float left{};
    float stats{};
    float width{};
};

[[nodiscard]] Columns columns() noexcept {
    const float left = ImGui::GetCursorScreenPos().x;
    const float width = ImGui::GetContentRegionAvail().x;
    return {left, left + width - (pixels(kStatColumn) * static_cast<float>(edit::Stats{}.size())), width};
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

/** Draws the row of stat icons that heads the table's columns. */
void draw_heads(const Columns& columns) noexcept {
    const Model& state = model();
    const float top = ImGui::GetCursorScreenPos().y;
    for (std::size_t shown = 0; shown < state.catalog.statOrder.size(); ++shown) {
        draw_stat_head(state.catalog.statOrder[shown], column_center(columns, shown), top);
    }
    ImGui::Dummy({columns.width, ImGui::GetTextLineHeight()});
}

/** Draws the minimums row: its label and Clear in the piece column, then a field under each stat. */
void draw_minimums(const Columns& columns) noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    const float top = ImGui::GetCursorScreenPos().y;
    ImGui::AlignTextToFramePadding();
    tooltip::draw_label(kTargetsHeading);
    ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
    ImGui::BeginDisabled(!targeted());
    if (ImGui::SmallButton(kClearLabel)) {
        optimizer.targets = {};
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", kClearTip);
    }
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
        return "Nothing Equipped";
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
 * Draws one armor slot's row: the piece the plan puts there, what it changes on it, its stats once
 * refitted, and a Keep toggle while swaps are allowed. A stat the plan moves is set in full text, one
 * it leaves is muted, so the changes read at a glance.
 */
void draw_piece(const Columns& columns, std::size_t slot) noexcept {
    Model& state = model();
    Optimizer& optimizer = state.optimizer;
    const edit::ArmorPlan* plan = current_plan();
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
    // The Keep toggle sits at the far end of the piece column, and the words stop short of it. It only
    // shows while a swap could take the slot's piece away.
    ImGui::PushID(static_cast<int>(slot));
    float wordsEnd = columns.stats - pixels(controls::kGroupGap);
    bool keepHovered = false;
    if (optimizer.options.swaps && !worn.locked && worn.instance != 0) {
        const float keepWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(kKeepLabel).x;
        ImGui::SetCursorScreenPos({wordsEnd - keepWidth, origin.y + ((height - ImGui::GetFrameHeight()) * 0.5F)});
        (void)ImGui::Checkbox(kKeepLabel, &optimizer.options.kept[slot]);
        keepHovered = ImGui::IsItemHovered();
        if (keepHovered) {
            ImGui::SetTooltip("%s", kKeepTip);
        }
        wordsEnd -= keepWidth + pixels(controls::kGroupGap);
    }
    const float textLeft = origin.x + extent + pixels(kIconGap);
    const float textWidth = (std::max)(0.0F, wordsEnd - textLeft);
    const float textTop = origin.y + ((height - (line * 2.0F)) * 0.5F);
    art::clipped_text(definition != nullptr ? definition->name : std::string(edit::kSlots[edit::kFirstPlanSlot + slot]),
                      {textLeft, textTop}, textWidth, ImGui::GetColorU32(definition != nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    art::clipped_text(piece_line(piece, planned), {textLeft, textTop + line}, textWidth, ImGui::GetColorU32(tooltip::muted()));
    const edit::Stats& after = planned != nullptr ? planned->shown : piece.shown;
    for (std::size_t shown = 0; shown < after.size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        const bool moved = planned != nullptr && after[stat] != worn.shown[stat];
        draw_value(columns, shown, origin.y + ((height - line) * 0.5F), after[stat],
                   ImGui::GetColorU32(moved ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : tooltip::muted()));
    }
    const ImVec2 corner{origin.x + columns.width, origin.y + height};
    if (!keepHovered && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(origin, corner)) {
        explain_piece(piece, planned);
    }
    draw->AddLine({origin.x, corner.y}, corner, ImGui::GetColorU32(tooltip::rule_color()));
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({columns.width, height});
    ImGui::PopID();
}

/** Draws one totals row: its label in the piece column and a total under each stat. */
void draw_totals(const Columns& columns, const char* label, const edit::Stats& totals, bool projected) noexcept {
    const Model& state = model();
    const float top = ImGui::GetCursorScreenPos().y;
    tooltip::draw_label(label);
    for (std::size_t shown = 0; shown < totals.size(); ++shown) {
        const std::size_t stat = state.catalog.statOrder[shown];
        // A projected total short of its minimum is set in the waiting colour, and its row's words say by how much.
        const bool short_of = projected && state.optimizer.targets[stat] > 0 && (std::min)(totals[stat], kStatCeiling) < state.optimizer.targets[stat];
        const ImVec4 color = !projected ? tooltip::muted() : short_of ? tooltip::pending() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_value(columns, shown, top, totals[stat], ImGui::GetColorU32(color));
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
        refused = "Armor not adjusted.";
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

/** Draws the options: whether a plan may swap pieces, and whether it may reach the other characters' armor. */
void draw_options() noexcept {
    Model& state = model();
    edit::ArmorOptions& options = state.optimizer.options;
    (void)ImGui::Checkbox(kSwapsLabel, &options.swaps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kSwapsTip);
    }
    // The other characters of this class hold armor a swap can bring over, when there are any.
    if (classmates() != 0) {
        ImGui::SameLine(0.0F, pixels(controls::kGroupGap));
        char label[kLineCapacity]{};
        (void)std::snprintf(label, sizeof label, "Use Other %ss' Armor", art::class_name(character().characterClass));
        ImGui::BeginDisabled(!options.swaps);
        (void)ImGui::Checkbox(label, &options.others);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Also swap in armor stowed on your other characters.");
        }
    }
}

/**
 * Draws the sheet's actions: Adjust Armor, white only while there is a change to make, and Close at the
 * far end. They stand at the height of a field, as every sheet's footer does.
 */
void draw_actions() noexcept {
    const edit::ArmorPlan* plan = current_plan();
    const bool ready = plan != nullptr && plan->changes();
    const float action = ImGui::GetFrameHeight();
    const float adjustWidth = ImGui::CalcTextSize(kAdjustLabel).x + (ImGui::GetStyle().FramePadding.x * 4.0F);
    if (ready) {
        if (controls::primary_button(kAdjustLabel, {adjustWidth, action})) {
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
            ImGui::SetTooltip("%s", !targeted()     ? "Set a minimum first."
                                    : plan == nullptr ? "Still working."
                                                      : "No changes needed.");
        }
    }
    const float closeWidth = pixels(kCloseWidth);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    // Escape closes a field's own popup first, and a field being typed in keeps it.
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput
                        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (ImGui::Button("Close", {closeWidth, action}) || escape) {
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
    tooltip::draw_sheet_head(kOptimizerTitle, head_line().c_str());
    controls::space(controls::kSectionSpacing);
    draw_options();
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
        draw_totals(table, kNowLabel, optimizer.input->current, false);
        if (const edit::ArmorPlan* plan = current_plan()) {
            draw_totals(table, kProjectedLabel, plan->totals, true);
        } else {
            tooltip::draw_label(kProjectedLabel);
        }
    }
    // The outcome and the actions sit at the foot of the sheet, whatever the table above them takes.
    const float below = ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() + (pixels(controls::kRowSpacing) * 2.0F)
                        + (ImGui::GetStyle().ItemSpacing.y * 3.0F);
    const float room = ImGui::GetContentRegionAvail().y - below;
    if (room > 0.0F) {
        ImGui::Dummy({0.0F, room});
    }
    controls::space(controls::kRowSpacing);
    draw_sheet_outcome();
    controls::space(controls::kRowSpacing);
    draw_actions();
    tooltip::end_sheet();
}

} // namespace dawn::core::ui::modules::loadout::internal
