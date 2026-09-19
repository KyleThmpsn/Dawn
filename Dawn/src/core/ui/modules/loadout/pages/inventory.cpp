// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../../scaling/dpi/ui_dpi_scaling.h"
#include "../card.h"
#include "../internal.h"
#include "../controls.h"
#include "../art.h"
#include "../tooltip.h"

namespace dawn::core::ui::modules::loadout::internal {
namespace {

using scaling::dpi::pixels;
namespace inv = state::account::inventory;

/** Width of the search box each inventory page puts at the head of its filter row. */
constexpr float kSearchWidth = 220.0F;
/** 150 authored pixels fit the widest slot name in the slot picker. */
constexpr float kSlotPickerWidth = 150.0F;
/** Icon edge on an account stack row. */
constexpr float kProfileIconExtent = 28.0F;
constexpr float kProfileRowInset = 6.0F;
/** Vertical breathing room the row fill adds above and below its content. */
constexpr float kProfileRowPadding = 2.0F;
/** Narrowest an account stack row is drawn, which sets the column count. */
constexpr float kProfileRowMinimumWidth = 320.0F;
/**
 * Gap between two lines of account stacks.
 * The card grid's own column gap is narrower than the fill each row paints around its content, so
 * lines set at it ran their fills together into one block.
 */
constexpr float kProfileRowGap = 9.0F;
/** 64 authored pixels hold a five-figure stack without the field reading as an empty box. */
constexpr float kQuantityWidth = 64.0F;
/** Rule under each account stack row, as the tooltip rules its own rows. */
constexpr ImVec4 kProfileRowRule{1.0F, 1.0F, 1.0F, 0.08F};
/** The add action on each page's header row. */
constexpr const char* kAddItemLabel = "Add item";
/** The removal says what it does. A glyph on its own read as decoration rather than a control. */
constexpr const char* kRemoveLabel = "Remove";
/** Section a stack falls under when its type is blank, or when the catalog does not carry it. */
constexpr const char* kUntypedGroup = "Other";
constexpr const char* kUnknownGroup = "Unknown";
/** Largest stack the editor offers when the catalog does not name a limit. */
constexpr int kUnknownStackLimit = 9999;
/** Title of the account stack removal, used for both the action and its modal. */
constexpr const char* kRemoveStackTitle = "Remove account stack?";

/**
 * Draws the page's add action at the far end of its header row, which opens the armory on the
 * category the page is made of. The armory is where an item is granted from; the page only has
 * to say where to go.
 * @param category Armory category the page opens on.
 */
void draw_add_action(Category category) noexcept {
    Model& state = model();
    const float width =
        ImGui::CalcTextSize(kAddItemLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    if (!ImGui::SmallButton(kAddItemLabel)) {
        return;
    }
    state.view = View::armory;
    state.browse.category = category;
    state.browse.type.clear();
    state.results.key.clear();
}

/** @return True when one owned item passes the page's search and slot filters. */
[[nodiscard]] bool passes_filters(const edit::Item& item, const std::string& query) noexcept {
    Model& state = model();
    const edit::CatalogItem* definition = state.catalog.find(item.definitionHash);
    if (definition == nullptr || !edit::matches(*definition, query)) {
        return false;
    }
    return state.inventorySlot < 0
           || definition->slot == static_cast<std::size_t>(state.inventorySlot);
}

/**
 * @return Every character item matching the page filters, equipped gear first, then by type and
 * name. The equipped pieces belong in their buckets too: the page is the whole of what the
 * character carries, not only what is stowed.
 */
[[nodiscard]] std::vector<const edit::Item*> filtered_character_items() noexcept {
    Model& state = model();
    const std::string query = edit::searchable(state.inventorySearch);
    const state::CharacterState& owner = character();
    std::vector<const edit::Item*> items;
    for (const auto& slot : owner.equipment.slots) {
        if (slot && passes_filters(*slot, query)) {
            items.push_back(&*slot);
        }
    }
    const std::size_t equippedCount = items.size();
    for (std::size_t i = 0; i < owner.inventory.count; ++i) {
        if (passes_filters(owner.inventory.values[i], query)) {
            items.push_back(&owner.inventory.values[i]);
        }
    }
    // Only items passes_filters already resolved reach this list, so a missing definition sorts
    // last rather than being dereferenced on the strength of that invariant holding elsewhere.
    const auto byTypeThenName = [&state](const edit::Item* a, const edit::Item* b) {
        const edit::CatalogItem* first = state.catalog.find(a->definitionHash);
        const edit::CatalogItem* second = state.catalog.find(b->definitionHash);
        if (first == nullptr || second == nullptr) {
            return first != nullptr;
        }
        return first->type == second->type ? first->name < second->name : first->type < second->type;
    };
    std::sort(items.begin() + static_cast<std::ptrdiff_t>(equippedCount), items.end(), byTypeThenName);
    return items;
}

/** @return True when the instance is sitting in one of the character's equipment slots. */
[[nodiscard]] bool is_equipped(std::uint64_t instance) noexcept {
    for (const auto& slot : character().equipment.slots) {
        if (slot && slot->instanceSoid == instance) {
            return true;
        }
    }
    return false;
}

/** @return How many equipment slots the character has filled. */
[[nodiscard]] std::size_t equipped_count() noexcept {
    std::size_t count = 0;
    for (const auto& slot : character().equipment.slots) {
        count += slot ? 1 : 0;
    }
    return count;
}

/** Draws the slot picker that narrows the character item list. */
void draw_slot_picker() noexcept {
    Model& state = model();
    const char* preview =
        state.inventorySlot < 0 ? "All slots" : edit::kSlots[state.inventorySlot];
    if (!controls::begin_picker("##inventory_slot", preview, pixels(kSlotPickerWidth))) {
        return;
    }
    if (controls::picker_row("All slots", state.inventorySlot < 0)) {
        state.inventorySlot = -1;
    }
    for (int slot = 0; slot < static_cast<int>(kSlotCount); ++slot) {
        if (controls::picker_row(edit::kSlots[slot], state.inventorySlot == slot)) {
            state.inventorySlot = slot;
        }
    }
    controls::end_picker();
}

/**
 * @return The group one stored item belongs to.
 * Buckets carry no name in the installed build, so gear is grouped by the equipment slot its
 * bucket feeds and everything else by its own item type, which is what the game calls it.
 */
[[nodiscard]] std::string item_group(const edit::Item& item,
                                     const edit::CatalogItem& definition) noexcept {
    if (item.postmaster) {
        return "Postmaster";
    }
    if (definition.slot < inv::kEquipmentSlotCount) {
        return edit::kSlots[definition.slot];
    }
    return definition.type.empty() ? std::string("Other") : definition.type;
}

/** The +1 and +2 bucket ranks only sit past the equipment run while the two counts agree. */
static_assert(kSlotCount == inv::kEquipmentSlotCount,
              "Bucket ranks assume the module and the account agree on the slot count.");

/** Draws everything the character carries, equipped and stowed, divided into its buckets. */
void draw_character_items() noexcept {
    Model& state = model();
    ImGui::SetNextItemWidth(pixels(kSearchWidth));
    (void)controls::search("##inventory_search",
                           "Search inventory...",
                           state.inventorySearch,
                           sizeof state.inventorySearch);
    ImGui::SameLine();
    draw_slot_picker();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    // The list is built once, before the child, so the count and the grid agree on the filters.
    const std::vector<const edit::Item*> items = filtered_character_items();
    const std::size_t held = character().inventory.count + equipped_count();
    if (items.size() == held) {
        ImGui::TextDisabled("%zu items", held);
    } else {
        ImGui::TextDisabled("%zu of %zu", items.size(), held);
    }
    draw_add_action(Category::weapons);

    if (!ImGui::BeginChild("owned")) {
        ImGui::EndChild();
        return;
    }
    std::map<std::string, std::vector<const edit::Item*>> groups;
    std::map<std::string, std::size_t> order;
    for (const edit::Item* item : items) {
        const edit::CatalogItem* definition = state.catalog.find(item->definitionHash);
        if (definition == nullptr) {
            continue;
        }
        const std::string group = item_group(*item, *definition);
        groups[group].push_back(item);
        // Rank is decided from the same three cases the name is, so one cannot disagree with the
        // other. `emplace` is first-wins, and the Postmaster is the one bucket whose name is fixed
        // while its rank was taken from whichever item reached it first: a postmastered hand
        // cannon ranked it 0, which floated it up beside Kinetic.
        order.emplace(group,
                      item->postmaster ? kSlotCount + 2
                      : definition->slot < inv::kEquipmentSlotCount
                          ? definition->slot
                      : definition->type.empty() ? kSlotCount + 1
                                                 : kSlotCount);
    }
    std::vector<std::pair<std::string, std::vector<const edit::Item*>>> ordered;
    ordered.reserve(groups.size());
    for (auto& entry : groups) {
        ordered.emplace_back(entry.first, std::move(entry.second));
    }
    std::sort(ordered.begin(), ordered.end(), [&order](const auto& a, const auto& b) {
        const std::size_t left = order[a.first];
        const std::size_t right = order[b.first];
        return left == right ? a.first < b.first : left < right;
    });

    // One bucket per collapsing section, each holding the same responsive card grid the loadout
    // uses. Sundial lays its character inventory out this way; splitting the page into fixed
    // columns of single-column lists wasted most of the width whatever the tiles were sized at.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    for (const auto& [group, members] : ordered) {
        if (!controls::section_header(group.c_str(), members.size())) {
            continue;
        }
        int columns = 1;
        float width = 0.0F;
        card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kCardMinimumWidth), columns, width);
        // Every card is resolved before the grid runs. Re-resolving inside it meant a null could
        // `continue` after SameLine had already fired, which left the next card taking the skipped
        // card's cell and the column accounting drifting for the rest of the bucket; and the row
        // height was being measured on the pre-edit pointer while the card drew from the resolved
        // one, so an equip inside the bucket could clip a card's own socket row.
        std::vector<edit::Item*> live;
        live.reserve(members.size());
        for (const edit::Item* member : members) {
            if (edit::Item* resolved = find_owned_item(member->instanceSoid)) {
                live.push_back(resolved);
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, pixels(kCardColumnSpacing)});
        const auto perRow = static_cast<std::size_t>(columns);
        float rowHeight = 0.0F;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (i % perRow == 0) {
                // The row is drawn at its tallest card, so a bucket of socketless items packs in.
                rowHeight = 0.0F;
                for (std::size_t j = i; j < live.size() && j < i + perRow; ++j) {
                    rowHeight = (std::max)(rowHeight, card::natural_height(live[j], width));
                }
            } else {
                ImGui::SameLine(0.0F, spacing);
            }
            edit::Item* item = live[i];
            const edit::CatalogItem* definition = state.catalog.find(item->definitionHash);
            const bool equipped = is_equipped(item->instanceSoid);
            // The bucket already names the slot, so the card only says when the item is equipped.
            card::draw(item,
                       equipped ? "Equipped" : nullptr,
                       definition != nullptr ? definition->slot : kSlotCount,
                       equipped ? card::Action::swap : card::Action::equip,
                       width,
                       rowHeight);
        }
        ImGui::PopStyleVar();
        controls::space(controls::kSectionSpacing);
    }
    if (items.empty()) {
        ImGui::TextDisabled("No items match.");
    }
    ImGui::EndChild();
}

/** Removes one account stack from the draft, keeping the remaining rows packed. */
void erase_profile_item(std::size_t index) noexcept {
    state::AccountState& account = model().draft->after;
    for (std::size_t row = index + 1; row < account.profileItemCount; ++row) {
        account.profileItems[row - 1] = account.profileItems[row];
    }
    account.profileItems[--account.profileItemCount] = {};
    mark_changed();
}

/**
 * Draws one account-wide stack as a single compact row.
 * Everything is laid out through the normal cursor: the icon, a spacer that reserves the name
 * column, then the controls. Positioning any of it with SetCursorPos left the parent unable to
 * size itself, which Dear ImGui reports and which broke the later columns.
 * @param index Row in the draft profile list.
 * @param width Card width in framebuffer pixels.
 * @param rowHeight Content height in framebuffer pixels, shared with the caller's clipper so the
 * lines it skips are the same height as the lines it draws.
 * @return True when the card's removal was confirmed. The caller erases it after the loop.
 */
[[nodiscard]] bool draw_profile_item(std::size_t index, float width, float rowHeight) noexcept {
    Model& state = model();
    state::account::inventory::ProfileItem& item = state.draft->after.profileItems[index];
    const edit::CatalogItem* definition = state.catalog.find(item.definitionHash);
    const float icon = pixels(kProfileIconExtent);
    const float inset = pixels(kProfileRowInset);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float quantityWidth = pixels(kQuantityWidth);
    const float removeWidth =
        ImGui::CalcTextSize(kRemoveLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    const float nameWidth = (std::max)(
        0.0F, width - (inset * 3.0F) - icon - quantityWidth - removeWidth - (spacing * 2.0F));

    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginGroup();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // The row's extent is known before its content, so the fill, the hover and the tooltip can all
    // be settled up front. The fill is painted first because the draw list paints in order, and
    // filling after the group had measured itself covered the icon and the name with a blank card.
    const ImVec2 fillMin{origin.x, origin.y - pixels(kProfileRowPadding)};
    const ImVec2 fillMax{origin.x + width, origin.y + rowHeight + pixels(kProfileRowPadding)};
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(fillMin, fillMax);
    // The game lists its currencies and materials as plain rows ruled off from one another, with
    // nothing filled in until the pointer reaches a row.
    if (hovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            fillMin, fillMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    }
    ImGui::GetWindowDrawList()->AddLine({fillMin.x, fillMax.y},
                                        {fillMax.x, fillMax.y},
                                        ImGui::GetColorU32(kProfileRowRule));

    ImGui::Dummy({inset, icon});
    ImGui::SameLine(0.0F, 0.0F);
    const ImVec2 iconAt = ImGui::GetCursorScreenPos();
    if (definition != nullptr) {
        art::icon(*definition, iconAt, icon);
    }
    ImGui::Dummy({icon, icon});

    ImGui::SameLine(0.0F, inset);
    const ImVec2 nameAt = ImGui::GetCursorScreenPos();
    art::clipped_text(definition != nullptr ? definition->name : std::string("Unknown item"),
                      {nameAt.x, nameAt.y + ((icon - ImGui::GetTextLineHeight()) * 0.5F)},
                      nameWidth,
                      ImGui::GetColorU32(definition != nullptr ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    ImGui::Dummy({nameWidth, icon});
    // The item reads the same here as it does anywhere else in the editor. The controls at the far
    // end are excluded, so the tooltip does not stand over the field the player is reaching for.
    if (hovered && definition != nullptr
        && ImGui::GetIO().MousePos.x < nameAt.x + nameWidth) {
        tooltip::draw(*definition, nullptr);
    }

    ImGui::SameLine(0.0F, spacing);
    const int limit = definition != nullptr ? (std::max)(1, definition->detail.maxStackSize)
                                            : kUnknownStackLimit;
    ImGui::SetNextItemWidth(quantityWidth);
    // An empty label keeps Dear ImGui from printing one after the stepper buttons.
    const bool changed = ImGui::InputInt("##quantity", &item.quantity, 0, 0);
    if (changed) {
        item.quantity = std::clamp(item.quantity, 1, limit);
    }
    record_scalar_edit(changed);

    ImGui::SameLine(0.0F, spacing);
    // The removal is offered only on the row under the pointer, so a page of stacks does not read
    // as a page of Remove buttons. Its width is reserved either way, so the columns hold still.
    if (hovered || ImGui::IsPopupOpen(kRemoveStackTitle)) {
        if (ImGui::Button(kRemoveLabel)) {
            ImGui::OpenPopup(kRemoveStackTitle);
        }
    } else {
        ImGui::Dummy({removeWidth, ImGui::GetFrameHeight()});
    }

    bool removed = false;
    if (ImGui::BeginPopupModal(kRemoveStackTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(definition != nullptr ? definition->name.c_str() : "Unknown item");
        if (ImGui::Button("Remove stack")) {
            removed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndGroup();
    ImGui::PopID();
    return removed;
}

/**
 * @return The section one account stack belongs under, which is what the game calls the item.
 * The profile holds currencies, materials, mods and shaders all in one run of rows, so its own
 * order says nothing a player is looking for; the type does.
 */
[[nodiscard]] std::string profile_group(const edit::CatalogItem* definition) noexcept {
    if (definition == nullptr) {
        return kUnknownGroup;
    }
    return definition->type.empty() ? std::string(kUntypedGroup) : definition->type;
}

/** One section of the account page: its heading, and the draft rows filed under it. */
struct ProfileGroup {
    std::string name;
    std::vector<std::size_t> rows;
};

/**
 * @return Every account stack matching the page search, gathered into its sections.
 * Sections read alphabetically, with the two catch-alls last so a named type is never buried
 * under them, and each section's rows read by name.
 */
[[nodiscard]] std::vector<ProfileGroup> grouped_profile_items() noexcept {
    Model& state = model();
    const std::string query = edit::searchable(state.profileSearch);
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < state.draft->after.profileItemCount; ++i) {
        const edit::CatalogItem* definition =
            state.catalog.find(state.draft->after.profileItems[i].definitionHash);
        // A stack the catalog does not carry still has to be reachable, so an empty search keeps
        // it and any typed search drops it: there is no name to match it against.
        if (!query.empty() && (definition == nullptr || !edit::matches(*definition, query))) {
            continue;
        }
        groups[profile_group(definition)].push_back(i);
    }

    const auto rank = [](const std::string& name) {
        return name == kUntypedGroup ? 1 : name == kUnknownGroup ? 2 : 0;
    };
    const auto byName = [&state](std::size_t left, std::size_t right) {
        const edit::CatalogItem* first =
            state.catalog.find(state.draft->after.profileItems[left].definitionHash);
        const edit::CatalogItem* second =
            state.catalog.find(state.draft->after.profileItems[right].definitionHash);
        if (first == nullptr || second == nullptr) {
            return first != nullptr;
        }
        return first->name < second->name;
    };
    std::vector<ProfileGroup> ordered;
    ordered.reserve(groups.size());
    for (auto& [name, rows] : groups) {
        std::sort(rows.begin(), rows.end(), byName);
        ordered.push_back({name, std::move(rows)});
    }
    std::sort(ordered.begin(), ordered.end(), [&rank](const ProfileGroup& a, const ProfileGroup& b) {
        const int left = rank(a.name);
        const int right = rank(b.name);
        return left == right ? a.name < b.name : left < right;
    });
    return ordered;
}

/**
 * Draws one section of account stacks as a responsive grid.
 * @param rows Draft indices filed under this section.
 * @param rowHeight Row content height in framebuffer pixels.
 * @param removal Receives the draft index whose removal was confirmed, if any.
 */
void draw_profile_group(const std::vector<std::size_t>& rows,
                        float rowHeight,
                        std::size_t& removal) noexcept {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    int columns = 1;
    float width = 0.0F;
    card_grid(ImGui::GetContentRegionAvail().x, spacing, pixels(kProfileRowMinimumWidth), columns, width);
    const auto perRow = static_cast<std::size_t>(columns);
    const std::size_t lines = (rows.size() + perRow - 1) / perRow;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{spacing, pixels(kProfileRowGap)});
    // The account holds up to 701 stacks. Building all of them cost more per frame than the whole
    // of the rest of the page, so only the lines the player can see are submitted.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines), rowHeight + pixels(kProfileRowGap));
    while (clipper.Step()) {
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            const std::size_t first = static_cast<std::size_t>(line) * perRow;
            float top = 0.0F;
            for (std::size_t column = 0; column < perRow && first + column < rows.size(); ++column) {
                if (column == 0) {
                    top = ImGui::GetCursorPosY();
                } else {
                    // SameLine carries the baseline of whatever the last row ended on, which left
                    // the columns of one line sitting a couple of pixels apart. Only the X it
                    // works out is wanted, so the line's own top is put back afterwards.
                    ImGui::SameLine(0.0F, spacing);
                    ImGui::SetCursorPosY(top);
                }
                if (draw_profile_item(rows[first + column], width, rowHeight)) {
                    removal = rows[first + column];
                }
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
}

/** Draws every account-wide stack, which belongs to the profile rather than a character. */
void draw_profile_items() noexcept {
    Model& state = model();
    const std::size_t held = state.draft->after.profileItemCount;
    const std::vector<ProfileGroup> groups = grouped_profile_items();
    std::size_t shown = 0;
    for (const ProfileGroup& group : groups) {
        shown += group.rows.size();
    }

    ImGui::SetNextItemWidth(pixels(kSearchWidth));
    (void)controls::search("##profile_search",
                           "Search account items...",
                           state.profileSearch,
                           sizeof state.profileSearch);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (shown == held) {
        ImGui::TextDisabled("%zu stacks", held);
    } else {
        ImGui::TextDisabled("%zu of %zu", shown, held);
    }
    draw_add_action(Category::materials);

    if (!ImGui::BeginChild("profile_items")) {
        ImGui::EndChild();
        return;
    }
    if (groups.empty()) {
        ImGui::TextDisabled("No items match.");
        ImGui::EndChild();
        return;
    }
    const float rowHeight = (std::max)(pixels(kProfileIconExtent), ImGui::GetFrameHeight());
    // A confirmed removal moves every later row, so the list is left and erased afterwards.
    std::size_t removal = held;
    for (const ProfileGroup& group : groups) {
        if (!controls::section_header(group.name.c_str(), group.rows.size())) {
            continue;
        }
        draw_profile_group(group.rows, rowHeight, removal);
        controls::space(controls::kSectionSpacing);
    }
    if (removal < held) {
        erase_profile_item(removal);
    }
    ImGui::EndChild();
}

} // namespace

void draw_character_inventory_page() noexcept {
    draw_character_items();
}

void draw_profile_inventory_page() noexcept {
    draw_profile_items();
}

} // namespace dawn::core::ui::modules::loadout::internal
