// SPDX-License-Identifier: GPL-3.0-only
#include "loadout.h"
#include "preview.h"
#include "../registry/ui_module_registry.h"
#include "../../scaling/dpi/ui_dpi_scaling.h"
#include "state/editor/edit.h"
#include "state/runtime/runtime.h"
#include "state/build_data/runtime.h"
#include "state/account/inventory/placement.h"
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <map>
#include <thread>

namespace dawn::core::ui::modules::loadout {
namespace {
namespace edit = state::editor;
namespace inv = state::account::inventory;
registry::PageRegistration g_page;
struct Model {
    edit::Catalog catalog;
    std::unique_ptr<edit::Draft> draft;
    std::thread loader;
    std::atomic_int loading{0}; // 0 idle, 1 loading, 2 ready, 3 failed
    std::atomic_bool cancel{false};
    std::atomic_uint progress{0};
    std::string loadError, status;
    std::size_t character{};
    int page{1}, category{}, rarity{}, sort{}, scope{}, power{edit::kMaximumItemLevel}, quantity{1};
    int inventorySlot{-1}, detailPage{};
    bool classOnly{true}, includeInternal{};
    std::string type;
    char search[160]{}, perkSearch[160]{};
    std::uint32_t selectedHash{};
    std::uint64_t selectedInstance{};
    std::size_t socketLane{};
    edit::Stats targets{};
    std::uint64_t targetOwner{};
    std::array<bool, 16> randomSlots{true, true, true, true, true, true, true, true};
    std::mt19937 random{std::random_device{}()};
    std::vector<const edit::CatalogItem*> filtered;
    std::vector<std::uint16_t> perkOptions;
    std::string filterKey;
};
std::unique_ptr<Model> g;
float px(float value) { return value * scaling::dpi::current(); }
ImVec4 rarity_color(std::uint8_t tier) {
    switch (tier) {
    case 5: return {0.92F, 0.76F, 0.33F, 1};
    case 4: return {0.67F, 0.49F, 0.91F, 1};
    case 3: return {0.34F, 0.62F, 0.88F, 1};
    case 2: return {0.45F, 0.74F, 0.56F, 1};
    default: return {0.69F, 0.74F, 0.82F, 1};
    }
}
const char* tier_name(std::uint8_t tier) {
    constexpr const char* names[]{"Unclassified", "Common", "Uncommon", "Rare", "Legendary", "Exotic"};
    return names[(std::min)(unsigned(tier), 5U)];
}
const char* class_name(state::CharacterClass value) {
    constexpr const char* names[]{"Titan", "Hunter", "Warlock"};
    return names[(std::min)(static_cast<unsigned>(value), 2U)];
}
void heading(const char* title, const char* subtitle) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35F);
    ImGui::TextUnformatted(title); ImGui::PopFont();
    ImGui::TextDisabled("%s", subtitle); ImGui::Spacing();
}
void space(float height = 12) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0});
    ImGui::Dummy({0, px(height)});
    ImGui::PopStyleVar();
}
bool primary_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return pressed;
}
bool disclosure(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{});
    const bool open = ImGui::CollapsingHeader(label);
    ImGui::PopStyleColor();
    return open;
}
bool navigation_tab(const char* label, bool active, float width) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(active ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    const bool pressed = ImGui::Button(label, {width, px(34)});
    if (active) {
        const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled({lo.x + px(10), hi.y - px(2)},
            {hi.x - px(10), hi.y}, ImGui::GetColorU32(ImGuiCol_CheckMark), px(1));
    }
    ImGui::PopStyleColor(2);
    return pressed;
}
void load() {
    if (g->loader.joinable()) g->loader.join();
    g->cancel = false; g->loading = 1; g->progress = 0; g->loadError.clear();
    g->loader = std::thread([] {
        try {
            const bool ok = edit::load_catalog(g->catalog, g->cancel, g->progress, g->loadError);
            g->loading.store(ok ? 2 : 3, std::memory_order_release);
        } catch (...) {
            g->loadError = "Could not load the item catalog. You can retry.";
            g->loading.store(3, std::memory_order_release);
        }
    });
}
void reload() {
    if (!g->draft) g->draft = std::make_unique<edit::Draft>();
    g->draft->before = state::account_snapshot();
    g->draft->after = g->draft->before; g->draft->dirty = false;
    g->character = (std::min)(g->character, g->draft->after.characterCount ? g->draft->after.characterCount - 1 : 0);
    g->selectedHash = 0; g->selectedInstance = 0; g->targetOwner = 0;
    g->status = "Changes stay in your draft. Save, then restart the game to apply them.";
}
state::CharacterState& character() { return g->draft->after.characters[g->character]; }
edit::Item* selected_item() {
    if (!g->selectedInstance) return nullptr;
    auto& c = character();
    for (auto& item : c.equipment.slots) if (item && item->instanceSoid == g->selectedInstance) return &*item;
    for (std::size_t i = 0; i < c.inventory.count; ++i) if (c.inventory.values[i].instanceSoid == g->selectedInstance) return &c.inventory.values[i];
    return nullptr;
}
void select(const edit::CatalogItem& item, std::uint64_t instance = 0) {
    g->selectedHash = item.definition.definitionHash; g->selectedInstance = instance;
    g->targetOwner = 0; g->quantity = 1; g->detailPage = 0;
}
void art(const edit::CatalogItem& item, ImVec2 origin, float extent) {
    auto* draw = ImGui::GetWindowDrawList();
    const auto tint = rarity_color(item.definition.tier);
    draw->AddRectFilled(origin, {origin.x + extent, origin.y + extent}, ImGui::GetColorU32({tint.x * 0.17F, tint.y * 0.17F, tint.z * 0.17F, 1}), px(3));
    if (!preview::draw(item.iconTag, origin, extent)) {
        const char* text = item.iconTag && !preview::unavailable(item.iconTag) ? "Loading image" : "No package image";
        const auto size = ImGui::CalcTextSize(text);
        if (extent >= px(90)) draw->AddText({origin.x + (extent - size.x) * 0.5F, origin.y + extent * 0.5F}, ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        else {
            const char* glyph = item.kind == edit::GearKind::weapon ? "W" : item.kind == edit::GearKind::armor ? "A" : "+";
            draw->AddText({origin.x + extent * 0.4F, origin.y + extent * 0.35F}, ImGui::GetColorU32(ImGuiCol_TextDisabled), glyph);
        }
    }
    draw->AddRect(origin, {origin.x + extent, origin.y + extent}, ImGui::GetColorU32(tint), px(2));
}
void short_text(const std::string& text, ImVec2 at, float width, ImU32 color) {
    auto value = text;
    bool shortened = false;
    while (!value.empty() && ImGui::CalcTextSize(value.c_str()).x > width - px(10)) {
        auto end = value.size() - 1;
        while (end && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) --end;
        value.resize(end);
        shortened = true;
    }
    if (shortened) value += "...";
    ImGui::GetWindowDrawList()->AddText(at, color, value.c_str());
}
bool card(const edit::CatalogItem& item, float width, std::uint64_t instance = 0, const char* slot = nullptr) {
    const bool row = width >= px(420);
    const float height = px(row ? 88.0F : 118.0F);
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(item.definition.definitionIndex));
    ImGui::PushID(static_cast<int>(instance));
    const bool clicked = ImGui::InvisibleButton("item", {width, height});
    const bool chosen = g->selectedHash == item.definition.definitionHash && g->selectedInstance == instance;
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, ImGui::GetColorU32(chosen ? ImGuiCol_Header : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), px(5));
    draw->AddRectFilled(pos, {pos.x + px(3), pos.y + height}, ImGui::GetColorU32(rarity_color(item.definition.tier)), px(2));
    art(item, {pos.x + px(14), pos.y + px(14)}, px(58));
    if (row) {
        short_text(item.name, {pos.x + px(90), pos.y + px(20)}, width - px(128), ImGui::GetColorU32(ImGuiCol_Text));
        const auto detail = std::string(slot ? slot : tier_name(item.definition.tier)) + "  /  " + item.type;
        short_text(detail, {pos.x + px(90), pos.y + px(47)}, width - px(128), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        draw->AddText({pos.x + width - px(24), pos.y + px(33)}, ImGui::GetColorU32(ImGuiCol_TextDisabled), ">");
    } else {
        short_text(slot ? slot : tier_name(item.definition.tier), {pos.x + px(86), pos.y + px(17)}, width - px(96), ImGui::GetColorU32(rarity_color(item.definition.tier)));
        short_text(item.type, {pos.x + px(86), pos.y + px(42)}, width - px(96), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        short_text(item.name, {pos.x + px(14), pos.y + px(87)}, width - px(28), ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (chosen) draw->AddRect(pos, {pos.x + width, pos.y + height}, ImGui::GetColorU32(ImGuiCol_CheckMark), px(5), 0, px(1.5F));
    if (hovered) { ImGui::BeginTooltip(); ImGui::TextUnformatted(item.name.c_str()); ImGui::TextDisabled("%s", item.type.c_str()); ImGui::EndTooltip(); }
    ImGui::PopID(); ImGui::PopID();
    if (clicked) select(item, instance);
    return clicked;
}
void character_bar() {
    auto& account = g->draft->after;
    const auto& selected = character();
    char current[80]{};
    std::snprintf(current, sizeof current, "%s %zu%s", class_name(selected.characterClass),
        g->character + 1, selected.selected ? "  /  Active" : "");
    ImGui::SetNextItemWidth(px(218));
    if (ImGui::BeginCombo("##editing_character", current)) {
        for (std::size_t i = 0; i < account.characterCount; ++i) {
            const auto& c = account.characters[i];
            char label[90]{};
            std::snprintf(label, sizeof label, "%s %zu%s", class_name(c.characterClass), i + 1, c.selected ? "  /  Active" : "");
            if (ImGui::Selectable(label, i == g->character)) {
                g->character = i; g->selectedHash = 0; g->selectedInstance = 0; g->type.clear(); g->filterKey.clear();
            }
        }
        ImGui::EndCombo();
    }
}
void stats_controls(edit::Item& item) {
    const auto totals = edit::item_stats(item, g->catalog);
    if (g->targetOwner != item.instanceSoid) { g->targets = totals; g->targetOwner = item.instanceSoid; }
    if (!ImGui::CollapsingHeader("Armor stats", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Current / target");
    for (std::size_t i = 0; i < totals.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%-11s %2d", edit::kStats[i], totals[i]); ImGui::SameLine(px(158));
        ImGui::SetNextItemWidth((std::max)(px(70), ImGui::GetContentRegionAvail().x));
        ImGui::SliderInt("##target", &g->targets[i], 0, 50);
        ImGui::PopID();
    }
    if (ImGui::Button("Apply closest stat roll", {-FLT_MIN, 0})) {
        edit::Stats achieved{};
        if (edit::adjust_stats(item, g->catalog, g->targets, achieved)) {
            g->draft->dirty = true;
            g->status = achieved == g->targets ? "Exact armor stat targets applied to draft." : "Closest supported armor roll applied. The current values show the result.";
        } else g->status = "This armor has no supported adjustable stat plugs.";
    }
}
void perk_picker() {
    ImGui::SetNextWindowSize({px(660), px(610)}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Choose a perk", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    auto* item = selected_item();
    const auto* definition = item ? g->catalog.find(item->definitionHash) : nullptr;
    if (!definition) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::Text("%s / Socket %zu", definition->name.c_str(), g->socketLane + 1);
    constexpr const char* scopes[]{"Compatible", "Socket + gear type", "Socket type", "Gear type", "All"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##scope", &g->scope, scopes, 5)) g->perkOptions = g->catalog.candidates(*definition, g->socketLane, static_cast<edit::PlugScope>(g->scope));
    if (g->scope != 0) ImGui::TextWrapped(g->scope == 4 ? "All discovered perks. Some combinations may prevent this item from loading." : "Expanded choices can include perks this item does not normally support.");
    ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputTextWithHint("##perk_search", "Search perks, traits, mods, shaders, ornaments...", g->perkSearch, sizeof g->perkSearch);
    const auto query = edit::searchable(g->perkSearch);
    std::vector<const edit::CatalogItem*> options;
    for (const auto id : g->perkOptions) if (const auto* plug = g->catalog.index(id); plug && edit::matches(*plug, query)) options.push_back(plug);
    std::sort(options.begin(), options.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
    ImGui::TextDisabled("%zu perks", options.size());
    if (ImGui::BeginChild("perk_results", {0, -ImGui::GetFrameHeightWithSpacing() * 1.6F})) {
        const float rowHeight = px(54);
        ImGuiListClipper clip; clip.Begin(static_cast<int>(options.size()), rowHeight);
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const auto& plug = *options[static_cast<std::size_t>(i)];
            ImGui::PushID(static_cast<int>(plug.definition.definitionIndex));
            const auto pos = ImGui::GetCursorScreenPos();
            const bool current = item->sockets.plugs[g->socketLane] == plug.definition.definitionHash;
            if (ImGui::Selectable("##perk", current, ImGuiSelectableFlags_DontClosePopups, {0, rowHeight - ImGui::GetStyle().ItemSpacing.y})) {
                if (edit::set_plug(*item, g->catalog, g->socketLane, plug.definition.definitionIndex, static_cast<edit::PlugScope>(g->scope))) {
                    g->draft->dirty = true; g->targetOwner = 0; g->status = "Perk applied to draft."; ImGui::CloseCurrentPopup();
                }
            }
            art(plug, pos, px(40));
            short_text(plug.name, {pos.x + px(50), pos.y + px(2)}, ImGui::GetContentRegionAvail().x - px(55), ImGui::GetColorU32(ImGuiCol_Text));
            short_text(plug.type, {pos.x + px(50), pos.y + px(23)}, ImGui::GetContentRegionAvail().x - px(55), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            if (ImGui::IsItemHovered() && !plug.description.empty()) ImGui::SetTooltip("%s", plug.description.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel", {px(100), 0})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void inspector() {
    const auto* definition = g->catalog.find(g->selectedHash);
    if (!definition) {
        space(36);
        heading("Select an item", "Browse the collection to see its details here.");
        return;
    }
    auto* item = selected_item();
    const bool compact = item || ImGui::GetContentRegionAvail().y < px(440);
    if (!compact) { ImGui::TextDisabled("ITEM DETAILS"); space(8); }
    const float extent = px(compact ? 64.0F : 132.0F);
    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail().x;
    art(*definition, {origin.x + (compact ? 0 : (available - extent) * 0.5F), origin.y}, extent);
    ImGui::Dummy({compact ? extent : available, extent});
    if (compact) { ImGui::SameLine(0, px(14)); ImGui::BeginGroup(); }
    else space(8);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * (compact ? 1.15F : 1.3F));
    ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(definition->name.c_str()); ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextColored(rarity_color(definition->definition.tier), "%s", tier_name(definition->definition.tier));
    ImGui::TextDisabled("%s", definition->type.c_str());
    if (compact) ImGui::EndGroup();
    space(10);
    ImGui::Separator();
    space(8);
    if (!item) {
        if (!definition->description.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", definition->description.c_str());
            ImGui::PopStyleColor();
            space(12);
        }
        if (definition->plug && definition->slot >= inv::kEquipmentSlotCount) {
            ImGui::TextWrapped("Choose a socket on an owned item to apply this perk or cosmetic.");
            space(8);
        }
        ImGui::TextUnformatted("Item level");
        ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderInt("##power", &g->power, 0, edit::kMaximumItemLevel, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (definition->detail.instancedDefinitionState == state::build_data::items::details::InstancedDefinitionState::stackable) {
            ImGui::TextUnformatted("Quantity"); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputInt("##quantity", &g->quantity);
        }
        space(10);
        if (primary_button("Add to inventory", {-FLT_MIN, px(38)}))
            (void)edit::give(*g->draft, g->catalog, g->character, g->selectedHash, g->quantity, g->power, false, g->status);
        if (definition->slot < inv::kEquipmentSlotCount && ImGui::Button("Add and equip", {-FLT_MIN, px(34)})) {
            if (edit::give(*g->draft, g->catalog, g->character, g->selectedHash, g->quantity, g->power, true, g->status)) {
                g->selectedInstance = character().equipment.slots[definition->slot]->instanceSoid;
                // The owned-item editor appears on the following frame.
            }
        }
    } else {
        const float tabWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
        if (navigation_tab("Overview", g->detailPage == 0, tabWidth)) g->detailPage = 0;
        ImGui::SameLine();
        if (navigation_tab("Perks & cosmetics", g->detailPage == 1, tabWidth)) g->detailPage = 1;
        space(8);
        bool equipped = false; std::size_t slot = 0;
        for (std::size_t s = 0; s < character().equipment.slots.size(); ++s)
            if (character().equipment.slots[s] && character().equipment.slots[s]->instanceSoid == item->instanceSoid) { equipped = true; slot = s; }
        if (g->detailPage == 0) {
            ImGui::TextDisabled(equipped ? "Equipped" : item->postmaster ? "Postmaster" : "In inventory");
            if (!equipped && primary_button("Equip item", {-FLT_MIN, px(36)})) {
                (void)edit::equip(*g->draft, g->catalog, g->character, item->instanceSoid, g->status); item = selected_item();
            }
            if (equipped && ImGui::Button("Move to inventory", {-FLT_MIN, px(36)})) {
                (void)edit::unequip(*g->draft, g->catalog, g->character, slot, g->status); item = selected_item();
            }
            if (!item) return;
            space(8);
            ImGui::TextUnformatted("Item level"); ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##owned_power", &item->level)) { item->level = std::clamp(item->level, 0, edit::kMaximumItemLevel); g->draft->dirty = true; }
            if (definition->detail.instancedDefinitionState == state::build_data::items::details::InstancedDefinitionState::stackable) {
                ImGui::TextUnformatted("Quantity"); ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##owned_quantity", &item->quantity)) { item->quantity = std::clamp(item->quantity, 1, (std::max)(1, definition->detail.maxStackSize)); g->draft->dirty = true; }
            }
            bool locked = (item->flags & inv::kLockedItemFlag) != 0;
            if (ImGui::Checkbox("Lock item", &locked)) { item->flags = locked ? item->flags | inv::kLockedItemFlag : item->flags & ~inv::kLockedItemFlag; g->draft->dirty = true; }
            space(8);
            if (definition->kind == edit::GearKind::armor) stats_controls(*item);
            if (!equipped && !(item->flags & inv::kLockedItemFlag)) {
                space(8);
                if (disclosure("Manage item")) {
                    if (ImGui::Button("Remove from draft", {-FLT_MIN, px(34)})) ImGui::OpenPopup("Remove item?");
                }
                if (ImGui::BeginPopupModal("Remove item?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted("Remove this item from the draft inventory?");
                    if (ImGui::Button("Remove")) {
                        auto& c = character();
                        for (std::size_t i = 0; i < c.inventory.count; ++i) if (c.inventory.values[i].instanceSoid == g->selectedInstance) { inv::erase(c, i); break; }
                        g->draft->dirty = true; g->selectedInstance = 0; ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
        } else {
            ImGui::TextDisabled("Select a socket to change its perk.");
            space(4);
            edit::Item resolved = *item;
            if (edit::materialize(resolved, g->catalog)) for (std::size_t lane = 0; lane < resolved.sockets.plugCount; ++lane) {
                ImGui::PushID(static_cast<int>(lane));
                const auto* plug = resolved.sockets.plugs[lane] ? g->catalog.find(*resolved.sockets.plugs[lane]) : nullptr;
                const auto pos = ImGui::GetCursorScreenPos();
                const float rowWidth = ImGui::GetContentRegionAvail().x;
                const bool clicked = ImGui::InvisibleButton(plug ? plug->name.c_str() : "Empty - choose perk", {rowWidth, px(64)});
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(pos, {pos.x + rowWidth, pos.y + px(64)},
                    ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), px(5));
                if (plug) art(*plug, {pos.x + px(10), pos.y + px(12)}, px(40));
                char label[32]{}; std::snprintf(label, sizeof label, "Socket %zu", lane + 1);
                short_text(label, {pos.x + px(60), pos.y + px(10)}, rowWidth - px(72), ImGui::GetColorU32(ImGuiCol_TextDisabled));
                short_text(plug ? plug->name : "Choose a perk", {pos.x + px(60), pos.y + px(32)}, rowWidth - px(72), ImGui::GetColorU32(ImGuiCol_Text));
                if (clicked) {
                    g->socketLane = lane; g->perkSearch[0] = 0;
                    g->perkOptions = g->catalog.candidates(*definition, lane, static_cast<edit::PlugScope>(g->scope));
                    ImGui::PopID(); ImGui::OpenPopup("Choose a perk"); ImGui::PushID(static_cast<int>(lane));
                }
                if (plug && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", plug->name.c_str(), plug->description.c_str());
                ImGui::PopID();
            }
        }
        perk_picker();
    }
    space(16);
    if (disclosure("Item information")) {
        if (definition->slot < inv::kEquipmentSlotCount) ImGui::TextDisabled("Slot: %s", edit::kSlots[definition->slot]);
        ImGui::TextDisabled("Item 0x%08X", definition->definition.definitionHash);
        if (item && !definition->description.empty()) ImGui::TextWrapped("%s", definition->description.c_str());
    }
}
bool in_category(const edit::CatalogItem& item) {
    if (g->category == 3) return item.plug;
    if (g->category == 0) return item.kind == edit::GearKind::weapon && !item.plug;
    if (g->category == 1) return item.kind == edit::GearKind::armor && !item.plug;
    return item.kind == edit::GearKind::cosmetic;
}
void collection_card(const edit::CatalogItem& item, float width, bool compact) {
    const float height = px(compact ? 86.0F : 184.0F);
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(item.definition.definitionIndex));
    const bool clicked = ImGui::InvisibleButton("collection_item", {width, height});
    const bool selected = g->selectedHash == item.definition.definitionHash && !g->selectedInstance;
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + width, pos.y + height},
        ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), px(7));
    const float extent = px(compact ? 58.0F : 92.0F);
    art(item, {pos.x + (compact ? px(12) : (width - extent) * 0.5F), pos.y + px(compact ? 14.0F : 16.0F)}, extent);
    const float textLeft = px(compact ? 84.0F : 14.0F);
    short_text(item.name, {pos.x + textLeft, pos.y + px(compact ? 19.0F : 126.0F)}, width - textLeft - px(14), ImGui::GetColorU32(ImGuiCol_Text));
    const char* slot = item.slot < inv::kEquipmentSlotCount ? edit::kSlots[item.slot] : item.type.c_str();
    const auto detail = compact ? item.type + "  /  " + slot : std::string(slot);
    short_text(detail, {pos.x + textLeft, pos.y + px(compact ? 46.0F : 151.0F)}, width - textLeft - px(14), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    const auto tint = rarity_color(item.definition.tier);
    draw->AddCircleFilled({pos.x + width - px(15), pos.y + px(16)}, px(3), ImGui::GetColorU32(tint));
    draw->AddRect(pos, {pos.x + width, pos.y + height},
        ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Border), px(7), 0, px(selected ? 2.0F : 0.7F));
    if (hovered) {
        ImGui::BeginTooltip(); ImGui::TextUnformatted(item.name.c_str());
        ImGui::TextColored(tint, "%s", tier_name(item.definition.tier));
        ImGui::TextDisabled("%s", item.type.c_str()); ImGui::EndTooltip();
    }
    if (clicked) select(item);
    ImGui::PopID();
}
void armory() {
    constexpr const char* categories[]{"Weapons", "Armor", "Cosmetics", "Perks"};
    const bool compact = ImGui::GetContentRegionAvail().y < px(380);
    const float categoryWidth = (std::min)(px(110), (ImGui::GetContentRegionAvail().x - px(36)) / 4.0F);
    if (compact) {
        ImGui::SetNextItemWidth(px(120));
        if (ImGui::Combo("##collection_category", &g->category, categories, 4)) g->type.clear();
        ImGui::SameLine();
    } else {
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (navigation_tab(categories[i], g->category == i, categoryWidth)) {
                g->category = i; g->type.clear();
            }
        }
        space(8);
    }
    constexpr const char* hints[]{"Search weapons...", "Search armor...", "Search cosmetics...", "Search perks..."};
    const int activeFilters = (g->rarity != 0 ? 1 : 0) + (!g->classOnly ? 1 : 0) + (g->includeInternal ? 1 : 0);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(108) - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##item_search", hints[g->category], g->search, sizeof g->search);
    ImGui::SameLine();
    char filters[40]{};
    if (activeFilters) std::snprintf(filters, sizeof filters, "Filters (%d)", activeFilters);
    else std::snprintf(filters, sizeof filters, "Filters");
    if (ImGui::Button(filters, {px(108), ImGui::GetFrameHeight()})) ImGui::OpenPopup("Collection filters");
    const auto filterEdge = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({filterEdge.x, filterEdge.y + px(8)}, ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSize({px(286), 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Collection filters")) {
        ImGui::TextUnformatted("Filter collection"); space(8);
        ImGui::TextDisabled("RARITY"); ImGui::SetNextItemWidth(-FLT_MIN);
        constexpr const char* rarities[]{"All rarities", "Common", "Uncommon", "Rare", "Legendary", "Exotic"};
        ImGui::Combo("##rarity", &g->rarity, rarities, 6);
        space(8);
        ImGui::Checkbox("Match character class", &g->classOnly);
        space(8); ImGui::Separator(); space(4);
        if (disclosure("Advanced")) ImGui::Checkbox("Include internal items", &g->includeInternal);
        space(8);
        if (ImGui::Button("Reset filters", {-FLT_MIN, px(34)})) {
            g->rarity = 0; g->classOnly = true; g->includeInternal = false;
        }
        ImGui::EndPopup();
    }
    space(10);
    const auto query = edit::searchable(g->search);
    std::map<std::string, std::size_t> types;
    std::size_t allCount{};
    for (const auto& item : g->catalog.items) if (in_category(item)
        && (g->includeInternal || !item.internal) && (!g->rarity || item.definition.tier == g->rarity)
        && (!g->classOnly || edit::fits_class(item, character().characterClass)) && edit::matches(item, query)) {
        ++allCount; if (!item.type.empty()) ++types[item.type];
    }
    const bool typeRail = !compact && ImGui::GetContentRegionAvail().x >= px(760);
    if (typeRail) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
        if (ImGui::BeginChild("collection_types", {px(178), 0})) {
            ImGui::TextDisabled(g->category == 0 ? "WEAPON TYPE" : "ITEM TYPE");
            space(8);
            const auto typeRow = [](const char* label, const std::string& type, std::size_t count) {
                const auto pos = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                const bool chosen = g->type == type;
                ImGui::PushID(label);
                if (ImGui::Selectable("##type_row", chosen, 0, {width, px(30)})) g->type = type;
                char total[16]{}; std::snprintf(total, sizeof total, "%zu", count);
                short_text(label, {pos.x + px(7), pos.y + px(6)}, width - px(39),
                    ImGui::GetColorU32(chosen ? ImGuiCol_Text : ImGuiCol_TextDisabled));
                ImGui::GetWindowDrawList()->AddText({pos.x + width - ImGui::CalcTextSize(total).x - px(5), pos.y + px(6)},
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), total);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
                ImGui::PopID();
            };
            typeRow("All types", "", allCount);
            for (const auto& [type, count] : types) typeRow(type.c_str(), type, count);
        }
        ImGui::EndChild(); ImGui::PopStyleColor();
        ImGui::SameLine(0, px(20));
    } else {
        ImGui::SetNextItemWidth(compact ? ImGui::GetContentRegionAvail().x - px(156) - ImGui::GetStyle().ItemSpacing.x : -FLT_MIN);
        if (ImGui::BeginCombo("##type", g->type.empty() ? "All types" : g->type.c_str())) {
            if (ImGui::Selectable("All types", g->type.empty())) g->type.clear();
            for (const auto& [type, count] : types) {
                (void)count;
                if (ImGui::Selectable(type.c_str(), g->type == type)) g->type = type;
            }
            ImGui::EndCombo();
        }
        if (compact) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(px(156));
            constexpr const char* sorts[]{"By type", "Name A-Z", "Rarity"}; ImGui::Combo("##sort_compact", &g->sort, sorts, 3);
        }
        space(4);
    }
    if (ImGui::BeginChild("collection_results", {0, 0})) {
        const auto key = query + "|" + g->type + "|" + std::to_string(g->category) + ":" + std::to_string(g->rarity)
            + ":" + std::to_string(g->sort) + ":" + std::to_string(g->classOnly) + ":" + std::to_string(g->includeInternal)
            + ":" + std::to_string(static_cast<unsigned>(character().characterClass));
        const bool changed = key != g->filterKey;
        if (changed) {
            g->filterKey = key; g->filtered.clear();
            for (const auto& item : g->catalog.items) if (in_category(item) && (g->includeInternal || !item.internal)
                && (g->type.empty() || item.type == g->type) && (!g->rarity || item.definition.tier == g->rarity)
                && (!g->classOnly || edit::fits_class(item, character().characterClass)) && edit::matches(item, query)) g->filtered.push_back(&item);
            std::sort(g->filtered.begin(), g->filtered.end(), [](const auto* a, const auto* b) {
                if (g->sort == 0 && a->type != b->type) return a->type < b->type;
                if (g->sort == 2 && a->definition.tier != b->definition.tier) return a->definition.tier > b->definition.tier;
                return a->name == b->name ? a->definition.definitionHash < b->definition.definitionHash : a->name < b->name;
            });
        }
        if (!compact) {
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%zu items", g->filtered.size());
            ImGui::SameLine((std::max)(px(90), ImGui::GetWindowContentRegionMax().x - px(156)));
            ImGui::SetNextItemWidth(px(156));
            constexpr const char* sorts[]{"By type", "Name A-Z", "Rarity"}; ImGui::Combo("##sort", &g->sort, sorts, 3);
            space(6);
        }
        if (ImGui::BeginChild("catalog_grid", {0, 0})) {
            if (changed) ImGui::SetScrollY(0);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{px(14), px(14)});
            const float available = ImGui::GetContentRegionAvail().x;
            const int columns = compact ? 1 : (std::max)(1, static_cast<int>((available + px(14)) / px(220)));
            const float width = (available - px(14) * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const bool grouped = !compact && g->sort == 0 && g->type.empty();
            for (std::size_t first = 0; first < g->filtered.size();) {
                std::size_t end = first + 1;
                if (grouped) while (end < g->filtered.size() && g->filtered[end]->type == g->filtered[first]->type) ++end;
                else end = g->filtered.size();
                if (grouped) {
                    if (first) space(8);
                    ImGui::TextUnformatted(g->filtered[first]->type.c_str());
                    ImGui::SameLine(); ImGui::TextDisabled("%zu", end - first);
                }
                const int rows = static_cast<int>((end - first + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
                ImGuiListClipper clip; clip.Begin(rows, px(compact ? 100.0F : 198.0F));
                while (clip.Step()) for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                    for (int col = 0; col < columns; ++col) {
                        const auto index = first + static_cast<std::size_t>(row * columns + col);
                        if (index >= end) break;
                        if (col) ImGui::SameLine();
                        collection_card(*g->filtered[index], width, compact);
                    }
                }
                first = end;
            }
            if (g->filtered.empty()) {
                space(32); heading("No matching items", "Try another search or reset your filters.");
                if (ImGui::Button("Clear search & filters", {px(200), px(36)})) {
                    g->search[0] = 0; g->type.clear(); g->rarity = 0; g->classOnly = true; g->includeInternal = false;
                }
            }
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}
void equipment() {
    heading("Your loadout", "Select a piece to change perks, cosmetics, item level or armor stats.");
    if (ImGui::Button("Randomize loadout...")) ImGui::OpenPopup("Randomize loadout");
    if (ImGui::BeginPopupModal("Randomize loadout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Choose the slots to randomize.");
        for (std::size_t slot = 0; slot < g->randomSlots.size(); ++slot) {
            ImGui::Checkbox(edit::kSlots[slot], &g->randomSlots[slot]);
            if (slot % 2 == 0) ImGui::SameLine(px(190));
        }
        ImGui::SliderInt("New item level", &g->power, 0, edit::kMaximumItemLevel, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextDisabled("Class-correct gear, compatible perks, one exotic per category.");
        ImGui::TextDisabled("Previous equipment is kept in inventory.");
        if (ImGui::Button("Create random draft")) { (void)edit::randomize(*g->draft, g->catalog, g->character, g->randomSlots, g->power, g->random, g->status); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginChild("equipped_cards", {0, 0})) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = (std::max)(1, static_cast<int>((available + gap) / px(220)));
        const float width = (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        for (std::size_t slot = 0; slot < character().equipment.slots.size(); ++slot) {
            if (slot % static_cast<std::size_t>(columns)) ImGui::SameLine();
            const auto& item = character().equipment.slots[slot];
            const auto* definition = item ? g->catalog.find(item->definitionHash) : nullptr;
            if (definition) (void)card(*definition, width, item->instanceSoid, edit::kSlots[slot]);
            else { ImGui::PushID(static_cast<int>(slot)); if (ImGui::Button(edit::kSlots[slot], {width, px(118)})) { g->page = 2; g->category = slot <= 2 ? 0 : slot <= 7 ? 1 : 2; g->type.clear(); } ImGui::PopID(); }
        }
    }
    ImGui::EndChild();
}
void inventory() {
    heading("Inventory", "Character gear and account-wide items.");
    if (ImGui::BeginTabBar("inventory_tabs")) {
        if (ImGui::BeginTabItem("Character items")) {
            ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputTextWithHint("##inventory_search", "Search your inventory...", g->search, sizeof g->search);
            ImGui::SetNextItemWidth(px(180));
            if (ImGui::BeginCombo("##inventory_slot", g->inventorySlot < 0 ? "All slots" : edit::kSlots[g->inventorySlot])) {
                if (ImGui::Selectable("All slots", g->inventorySlot < 0)) g->inventorySlot = -1;
                for (int slot = 0; slot < 16; ++slot) if (ImGui::Selectable(edit::kSlots[slot], g->inventorySlot == slot)) g->inventorySlot = slot;
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%zu stored items", character().inventory.count);
            if (ImGui::BeginChild("owned", {0, 0})) {
                std::vector<const edit::Item*> items;
                const auto query = edit::searchable(g->search);
                for (std::size_t i = 0; i < character().inventory.count; ++i) {
                    const auto& item = character().inventory.values[i]; const auto* def = g->catalog.find(item.definitionHash);
                    if (def && edit::matches(*def, query) && (g->inventorySlot < 0 || def->slot == static_cast<std::size_t>(g->inventorySlot))) items.push_back(&item);
                }
                std::sort(items.begin(), items.end(), [](const auto* a, const auto* b) {
                    const auto* da = g->catalog.find(a->definitionHash); const auto* db = g->catalog.find(b->definitionHash);
                    return da->type == db->type ? da->name < db->name : da->type < db->type;
                });
                for (const auto* item : items) (void)card(*g->catalog.find(item->definitionHash), ImGui::GetContentRegionAvail().x, item->instanceSoid, item->postmaster ? "Postmaster" : nullptr);
                if (items.empty()) ImGui::TextDisabled("No items match.");
            }
            ImGui::EndChild(); ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Account items")) {
            if (ImGui::BeginChild("profile_items", {0, 0})) {
                auto& account = g->draft->after;
                for (std::size_t i = 0; i < account.profileItemCount; ++i) {
                    auto& item = account.profileItems[i]; const auto* def = g->catalog.find(item.definitionHash);
                    ImGui::PushID(static_cast<int>(i));
                    if (def) { const auto pos = ImGui::GetCursorScreenPos(); art(*def, pos, px(42)); ImGui::Dummy({px(42), px(42)}); ImGui::SameLine(); ImGui::TextWrapped("%s", def->name.c_str()); }
                    else ImGui::Text("Item 0x%08X", item.definitionHash);
                    ImGui::SetNextItemWidth(px(160));
                    if (ImGui::InputInt("Quantity", &item.quantity)) {
                        item.quantity = std::clamp(item.quantity, 1, def ? (std::max)(1, def->detail.maxStackSize) : 9999); g->draft->dirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Remove stack")) ImGui::OpenPopup("Remove account stack?");
                    if (ImGui::BeginPopupModal("Remove account stack?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextUnformatted("Remove this whole stack from the draft?");
                        if (ImGui::Button("Remove")) {
                            for (std::size_t row = i + 1; row < account.profileItemCount; ++row) account.profileItems[row - 1] = account.profileItems[row];
                            account.profileItems[--account.profileItemCount] = {}; g->draft->dirty = true;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                    ImGui::Separator(); ImGui::PopID();
                }
            }
            ImGui::EndChild(); ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
void character_controls() {
    heading("Character", "Identity and progression for the selected character.");
    auto& c = character();
    int cls = static_cast<int>(c.characterClass), race = static_cast<int>(c.race), gender = static_cast<int>(c.gender), level = c.level;
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::Combo("Class", &cls, "Titan\0Hunter\0Warlock\0")) { c.characterClass = static_cast<state::CharacterClass>(cls); g->draft->dirty = true; }
    ImGui::TextWrapped("After changing class, equip matching armor and a subclass before saving.");
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::Combo("Race", &race, "Human\0Awoken\0Exo\0")) { c.race = static_cast<state::CharacterRace>(race); g->draft->dirty = true; }
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::Combo("Gender", &gender, "Male\0Female\0")) { c.gender = static_cast<state::CharacterGender>(gender); g->draft->dirty = true; }
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::SliderInt("Level", &level, 1, 50)) { c.level = static_cast<std::uint8_t>(level); g->draft->dirty = true; }
    ImGui::SetNextItemWidth(px(220));
    if (ImGui::InputFloat("Appearance", &c.appearanceValue, 0.1F, 1.0F)) g->draft->dirty = true;
    if (ImGui::Checkbox("Character preview available", &c.previewAvailable)) g->draft->dirty = true;
    if (ImGui::Checkbox("Intro accepted", &c.accepted)) g->draft->dirty = true;
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    edit::Stats totals{};
    for (std::size_t slot = 3; slot <= 7; ++slot) if (c.equipment.slots[slot]) {
        const auto stats = edit::item_stats(*c.equipment.slots[slot], g->catalog);
        for (std::size_t i = 0; i < totals.size(); ++i) totals[i] += stats[i];
    }
    heading("Armor totals", "Combined stat contributions from your equipped armor.");
    for (std::size_t i = 0; i < totals.size(); ++i) {
        ImGui::Text("%-12s %3d", edit::kStats[i], totals[i]); ImGui::SameLine(px(175));
        ImGui::ProgressBar(std::clamp(static_cast<float>(totals[i]) / 100.0F, 0.0F, 1.0F), {px(210), px(10)}, "");
    }
}
void subclass() {
    heading("Subclass", "Choose your subclass and its available ability options.");
    auto& c = character();
    for (const auto& definition : g->catalog.items) if (definition.kind == edit::GearKind::subclass && !definition.abilities[0].empty() && edit::fits_class(definition, c.characterClass)) {
        const bool current = c.equipment.slots[11] && c.equipment.slots[11]->definitionHash == definition.definition.definitionHash;
        ImGui::PushID(static_cast<int>(definition.definition.definitionIndex));
        if (ImGui::Selectable(definition.name.c_str(), current)) {
            std::uint64_t owned{};
            for (std::size_t i = 0; i < c.inventory.count; ++i) if (c.inventory.values[i].definitionHash == definition.definition.definitionHash) owned = c.inventory.values[i].instanceSoid;
            if (owned) (void)edit::equip(*g->draft, g->catalog, g->character, owned, g->status);
            else (void)edit::give(*g->draft, g->catalog, g->character, definition.definition.definitionHash, 1, g->power, true, g->status);
            if (c.equipment.slots[11]) select(*g->catalog.find(c.equipment.slots[11]->definitionHash), c.equipment.slots[11]->instanceSoid);
        }
        ImGui::PopID();
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    const auto* def = c.equipment.slots[11] ? g->catalog.find(c.equipment.slots[11]->definitionHash) : nullptr;
    if (!def || def->abilities[0].empty()) { ImGui::TextDisabled("Choose a subclass to see abilities."); return; }
    const edit::SubclassPath* currentPath = nullptr;
    for (const auto& path : def->paths) if (path.super == c.superAbilityEntry && path.melee == c.meleeAbilityEntry) currentPath = &path;
    ImGui::SetNextItemWidth(px(320));
    if (ImGui::BeginCombo("Path", currentPath ? currentPath->name.c_str() : "Custom ability selection")) {
        for (const auto& path : def->paths) if (ImGui::Selectable(path.name.c_str(), &path == currentPath)) {
            c.superAbilityEntry = path.super; c.meleeAbilityEntry = path.melee; g->draft->dirty = true;
        }
        ImGui::EndCombo();
    }
    if (currentPath) for (const auto& perk : currentPath->perks) ImGui::BulletText("%s", perk.c_str());
    ImGui::Spacing();
    struct Ability { const char* name; std::uint8_t* value; };
    const Ability abilities[]{{"Jump", &c.movementAbilityEntry}, {"Grenade", &c.grenadeAbilityEntry}, {"Super", &c.superAbilityEntry}, {"Melee", &c.meleeAbilityEntry}, {"Class ability", &c.classAbilityEntry}};
    for (std::size_t i = 0; i < std::size(abilities); ++i) {
        const auto& ability = abilities[i];
        const auto& options = def->abilities[i];
        const char* active = "Choose an ability";
        for (const auto& choice : options) if (choice.entry == *ability.value) active = choice.name.c_str();
        ImGui::SetNextItemWidth(px(320));
        if (ImGui::BeginCombo(ability.name, active)) {
            for (const auto& choice : options) {
                ImGui::PushID(choice.entry);
                if (ImGui::Selectable(choice.name.c_str(), choice.entry == *ability.value)) { *ability.value = choice.entry; g->draft->dirty = true; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
}
void draft_footer() {
    ImGui::Separator(); space(4);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    if (g->draft->dirty) ImGui::TextColored({0.88F, 0.76F, 0.47F, 1}, "Unsaved changes");
    else ImGui::TextDisabled("Account up to date");
    ImGui::SameLine(right - px(236));
    if (ImGui::Button("Reload account", {px(112), px(34)})) {
        if (g->draft->dirty) ImGui::OpenPopup("Discard draft?"); else reload();
    }
    ImGui::SameLine(); ImGui::BeginDisabled(!g->draft->dirty);
    if (primary_button("Save changes", {px(112), px(34)})) (void)edit::save(*g->draft, g->catalog, g->status);
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Discard draft?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Discard unsaved edits and reload the current account?");
        if (ImGui::Button("Discard and reload")) { reload(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Keep editing")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", g->status.c_str()); ImGui::PopStyleColor();
}
void content() {
    if (!g) return;
    const auto state = g->loading.load(std::memory_order_acquire);
    if (state == 0) { load(); return; }
    if (state == 1) { heading("Loadout studio", "Loading your collection..."); ImGui::ProgressBar(static_cast<float>(g->progress.load()) / 100.0F, {px(360), 0}); return; }
    if (state == 3) { ImGui::TextWrapped("%s", g->loadError.c_str()); if (ImGui::Button("Retry catalog")) load(); return; }
    if (!g->draft) reload();
    const bool compactHeader = ImGui::GetContentRegionAvail().y < px(460);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    constexpr const char* pages[]{"Character", "Equipment", "Armory", "Inventory", "Subclass"};
    if (!compactHeader) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5F);
        ImGui::TextUnformatted("Loadout studio"); ImGui::PopFont();
    }
    if (!g->draft->after.characterCount) {
        ImGui::TextWrapped("Create a character in the game, then reload the account here.");
        if (ImGui::Button("Reload account")) reload();
        return;
    }
    if (compactHeader) {
        ImGui::SetNextItemWidth(px(180)); ImGui::Combo("##loadout_page", &g->page, pages, 5);
    }
    if (ImGui::GetContentRegionAvail().x >= px(440) || compactHeader) ImGui::SameLine(right - px(218));
    character_bar();
    space(8);
    if (!compactHeader) {
        const float tabWidth = (std::min)(px(116), (ImGui::GetContentRegionAvail().x - px(40)) / 5.0F);
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            if (navigation_tab(pages[i], g->page == i, tabWidth)) g->page = i;
        }
        space(10);
    }
    const bool narrow = ImGui::GetContentRegionAvail().x < px(790);
    const bool showDetails = g->page != 0 && g->page != 4;
    const float footerHeight = px(72) + ImGui::CalcTextSize(g->status.c_str(), nullptr, false,
        ImGui::GetContentRegionAvail().x).y;
    const float bodyHeight = (std::max)(px(120), ImGui::GetContentRegionAvail().y - footerHeight);
    if (narrow && showDetails && g->selectedHash) {
        if (ImGui::BeginChild("item_details_narrow", {0, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding)) {
            if (ImGui::Button("< Back to items", {px(150), px(32)})) { g->selectedHash = 0; g->selectedInstance = 0; }
            space(8); inspector();
        }
        ImGui::EndChild();
    } else {
        const float inspectorWidth = px(320), gap = px(24);
        const float bodyWidth = narrow || !showDetails ? 0 : ImGui::GetContentRegionAvail().x - inspectorWidth - gap;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{});
        if (ImGui::BeginChild("loadout_body", {bodyWidth, bodyHeight})) {
            switch (g->page) {
            case 0: character_controls(); break;
            case 1: equipment(); break;
            case 2: armory(); break;
            case 3: inventory(); break;
            case 4: subclass(); break;
            }
        }
        ImGui::EndChild(); ImGui::PopStyleColor();
        if (!narrow && showDetails) {
            ImGui::SameLine(0, gap);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{px(20), px(20)});
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(8));
            if (ImGui::BeginChild("item_details", {0, bodyHeight}, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders)) inspector();
            ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopStyleColor();
        }
    }
    space(8);
    draft_footer();
}

}
bool initialize() noexcept {
    try { g = std::make_unique<Model>(); return g_page.acquire(Owner::core, "core.loadout", "Loadout", &draw); }
    catch (...) { return false; }
}
void shutdown() noexcept {
    g_page.release();
    if (g) { g->cancel = true; if (g->loader.joinable()) g->loader.join(); }
    preview::shutdown(); g.reset();
}
void draw() noexcept {
    try { content(); }
    catch (...) { if (g) g->status = "The editor could not complete that action. Your saved account is unchanged."; }
}
}
