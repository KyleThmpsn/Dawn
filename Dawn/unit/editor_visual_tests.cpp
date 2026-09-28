// Real installed definitions, localized names, socket pools and DX11 preview rendering.
// Account services are isolated: this executable cannot write to the installed game.
#include "editor_fixture_backend.h"
#include "../src/core/ui/modules/loadout/preview.cpp"
#include "core/ui/modules/loadout/internal.h"
#include "core/ui/modules/loadout/loadout.h"
#include "core/ui/fonts/runtime/ui_runtime_font_lifecycle.h"
#include "middleware/content/packages/reader/reader.h"
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include "core/ui/memory/allocator.h"
#include "core/ui/theme/dawn_ui_theme.h"
#include "core/ui/layout/ui_layout_lifecycle.h"
#include "core/ui/components/logo/ui_logo_component.h"
#include "state/runtime/storage/internal.h"
#include "core/ui/layout/credits/dawn_credits_badge.h"
#include "../src/core/filesystem/path.cpp"
#include <chrono>
#include "state/build_data/abilities/ability_bucket_catalog.h"
#include "middleware/datagen/family4/loadout/subclass_socket_selection.h"
#include "middleware/datagen/family4/loadout/loadout_resolver.h"
#include "middleware/datagen/family4/character/character_encoder.h"
#include "middleware/datagen/family4/character/layout.h"
#include "middleware/datagen/family4/progression/progression_bank_keys.h"
#include "state/unlocks/unlocks_runtime.h"
#include "state/build_data/vendors/service_catalog.h"

// These inventory fixtures have no active quests, events, or persisted unlocks.
namespace dawn::state::build_data::vendors::services {
bool quest_step(std::uint16_t, QuestStep&) noexcept { return false; }
bool pursuit(std::uint16_t, Pursuit&) noexcept { return false; }
bool objective(std::uint16_t, Objective&) noexcept { return false; }
Binding binding(bool, std::uint16_t) noexcept { return {}; }
}
namespace dawn::state::activity::events {
bool withheld(std::uint32_t) noexcept { return true; }
}
namespace dawn::state::unlocks {
ScopedTable snapshot() noexcept { return {}; }
bool find_character(const ScopedTable&, std::uint64_t, CharacterTable& output) noexcept {
    output = {}; return false;
}
}
namespace dawn::middleware::datagen::family4::progression {
bool key_bank(state::build_data::progressions::Scope, std::uint64_t,
    const state::unlocks::ScopedTable&, std::span<layout::Entry> bank) noexcept {
    for (auto& entry : bank) { entry = {}; entry.definitionIndex = 0xFFFF; }
    return true;
}
}

namespace ui = dawn::core::ui;
namespace editor = dawn::state::editor;
namespace panel = ui::modules::loadout;
namespace studio = ui::modules::loadout::internal;
namespace {
float scale = 1;
unsigned errors{};
bool backupAllowed = true, commitAllowed = true;
unsigned backups{}, commits{};
std::vector<dawn::state::build_data::abilities::Definition> publishedAbilities;
ui::layout::StateSnapshot layout;
ID3D11Device* gpu{};
ID3D11DeviceContext* context{};
ID3D11Texture2D* target{};
ID3D11RenderTargetView* view{};
constexpr UINT width = 1500, height = 1040;
void screenshot(const std::filesystem::path& path) {
    D3D11_TEXTURE2D_DESC desc{}; target->GetDesc(&desc); desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* copy{};
    fixture::check(SUCCEEDED(gpu->CreateTexture2D(&desc, nullptr, &copy)), "screenshot texture");
    context->CopyResource(copy, target); D3D11_MAPPED_SUBRESOURCE mapped{};
    fixture::check(SUCCEEDED(context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped)), "screenshot readback");
    std::ofstream output(path, std::ios::binary); output << "P6\n" << width << ' ' << height << "\n255\n";
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) output.write(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * 4, 3);
    context->Unmap(copy, 0); copy->Release();
}
void frame(float w = width, float h = height, ImGuiID activate = 0) {
    ImGui::GetIO().DisplaySize = {w, h}; ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
    if (activate) ImGui::ActivateItemByID(activate);
    (void)ui::layout::render(true); ImGui::Render();
    const float clear[]{0.018F,0.024F,0.035F,1}; context->OMSetRenderTargets(1, &view, nullptr); context->ClearRenderTargetView(view, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); fixture::check(errors == 0, "no ImGui errors");
}
void settle() { for (unsigned i = 0; i < 40; ++i) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } }
/** Runs frames until the optimizer's plan answers the armor and minimums as they stand; a worker works it out. */
void wait_for_plan() {
    const auto& optimizer = studio::model().optimizer;
    for (unsigned i = 0; i < 1000; ++i) {
        frame(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (optimizer.plan.finished && optimizer.input && optimizer.planInput == optimizer.input
            && optimizer.planTargets == optimizer.targets && !optimizer.job) { settle(); return; }
    }
    fixture::check(false, "the optimizer works out a plan");
}
/** @return The active window whose name holds this part, such as a page's scrolling child. */
ImGuiWindow* window_named(const char* part) {
    for (auto* window : ImGui::GetCurrentContext()->Windows) if (window->Active && std::strstr(window->Name, part)) return window;
    return nullptr;
}
/** Presses the button with this label in the window whose name holds `part`, as a click would. */
void press(const char* part, const char* label) {
    ImGuiWindow* window = window_named(part);
    fixture::check(window != nullptr, part);
    frame(width, height, ImHashStr(label, 0, window->IDStack.back())); settle();
}
const editor::CatalogItem& named(std::string_view name) {
    for (const auto& item : studio::model().catalog.items) if (item.name == name && !item.plug && item.slot < 16) return item;
    std::cerr << "Name missing: " << name << '\n'; std::exit(1);
}
void mutations() {
    auto draft = std::make_unique<editor::Draft>(); draft->after = *fixture::account; draft->before = draft->after;
    const auto& catalog = studio::model().catalog; const auto& weapon = named("Riskrunner");
    std::string error;
    fixture::check(editor::give(*draft, catalog, 0, weapon.definition.definitionHash, 1, 105, true, error), "give and equip native gun");
    const auto id = draft->after.characters[0].equipment.slots[weapon.slot]->instanceSoid;
    const auto before = std::make_unique<editor::Draft>(*draft);
    fixture::check(!editor::give(*draft,catalog,0,weapon.definition.definitionHash,2,105,true,error) && draft->after == before->after, "instanced stack rejection is atomic");
    auto& item = *draft->after.characters[0].equipment.slots[weapon.slot];
    fixture::check(editor::materialize(item,catalog), "native sockets materialize");
    bool expanded = false;
    for (std::size_t lane = 0; lane < weapon.detail.ordinarySocketCount; ++lane) {
        const auto allowed = catalog.candidates(weapon,lane,editor::PlugScope::compatible);
        const auto all = catalog.candidates(weapon,lane,editor::PlugScope::all);
        fixture::check(all.size() > allowed.size(), "All includes the full perk pool");
        for (auto plug : all) if (!std::binary_search(allowed.begin(),allowed.end(),plug)) {
            const auto unchanged = item;
            fixture::check(!editor::set_plug(item,catalog,lane,plug,editor::PlugScope::compatible) && item == unchanged, "compatible mode rejects unrelated perk");
            fixture::check(editor::set_plug(item,catalog,lane,plug,editor::PlugScope::all) && item.instanceSoid == id, "full pool authors any discovered perk without changing identity");
            expanded = true; break;
        }
        if (expanded) break;
    }
    fixture::check(expanded,"full pool exercised");
    *draft = *before;
    for (unsigned i = 0; i < 9; ++i) fixture::check(editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,105,false,error),"fill weapon bucket");
    const auto full = std::make_unique<editor::Draft>(*draft);
    fixture::check(!editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,105,false,error) && draft->after == full->after,"full bucket preserves all existing items");
    auto slots = std::array<bool,16>{}; slots[weapon.slot] = true; std::mt19937 random{17};
    fixture::check(editor::randomize(*draft,catalog,0,slots,106,random,error), "randomizer reuses owned gear in full bucket");
    fixture::check(draft->after.characters[0].inventory.count == 9,"randomizer preserves item count when full");
    fixture::check(draft->after.characters[0].equipment.slots[weapon.slot]->level == 106,"randomizer applies selected level to reused gear");
    *draft = *before; draft->before = draft->after;
    draft->after.characters[0].equipment.slots[weapon.slot]->level = 106;
    auto prepared = std::make_unique<dawn::state::AccountState>();
    fixture::check(editor::prepare_commit(*draft,catalog,*prepared,error),"edited item prepares to save");
    fixture::check(prepared->characters[0].equipment.slots[weapon.slot]->mutationSerial > draft->before.characters[0].equipment.slots[weapon.slot]->mutationSerial,"direct edits advance item revision");
    // A known armor piece must derive a realizable result from its native stat plugs.
    const auto& armor = named("Dunemarchers");
    fixture::check(editor::give(*draft,catalog,0,armor.definition.definitionHash,1,105,true,error),"give native armor");
    auto& equipped = *draft->after.characters[0].equipment.slots[armor.slot];
    editor::Stats achieved{}, desired{30,0,0,30,0,0};
    fixture::check(editor::adjust_stats(equipped,catalog,desired,achieved),"armor stat allocation has native candidates");
    fixture::check(achieved == editor::item_stats(equipped,catalog),"reported armor stats equal actual plug contributions");
    fixture::check(dawn::state::account::valid(draft->after),"mutated account remains structurally valid");
}
/** The editor bounds an item level only by the Power the game can hold, ten to a level in 32 bits. */
void item_level_limits() {
    auto draft = std::make_unique<editor::Draft>();
    draft->after = *fixture::account; draft->before = draft->after;
    const auto& catalog = studio::model().catalog; const auto& weapon = named("Riskrunner");
    std::string error;
    constexpr int highest = editor::kMaximumItemLevel;
    fixture::check(editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,highest,true,error),"highest holdable level accepted");
    const auto before = std::make_unique<editor::Draft>(*draft);
    auto slots = std::array<bool,16>{}; slots[weapon.slot] = true;
    std::mt19937 random{17};
    for (int level : {-1,highest + 1}) {
        fixture::check(!editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,level,true,error)
            && draft->after == before->after,"invalid creation level rejected atomically");
        fixture::check(!editor::randomize(*draft,catalog,0,slots,level,random,error)
            && draft->after == before->after,"invalid randomizer level rejected atomically");
    }
    auto prepared = std::make_unique<dawn::state::AccountState>();
    draft->before = draft->after;
    draft->after.characters[0].equipment.slots[weapon.slot]->level = highest + 1;
    fixture::check(!editor::prepare_commit(*draft,catalog,*prepared,error),"a level past the holdable Power cannot save");
    draft->after.characters[0].equipment.slots[weapon.slot]->level = highest;
    fixture::check(editor::prepare_commit(*draft,catalog,*prepared,error),"the highest holdable level saves");
    draft->after.characters[0].equipment.slots[weapon.slot]->level = highest + 5;
    draft->before = draft->after;
    fixture::check(editor::prepare_commit(*draft,catalog,*prepared,error),"an existing unholdable level is preserved");
    draft->after.characters[0].equipment.slots[weapon.slot]->level = highest + 1;
    fixture::check(!editor::prepare_commit(*draft,catalog,*prepared,error),"a new unholdable level on legacy gear is refused");
}
bool character_selection_encodes(const dawn::state::AccountState& account) {
    namespace family = dawn::middleware::datagen::family4;
    auto resolved = std::make_unique<family::loadout::ResolvedLoadout>();
    if (!family::loadout::resolve(account, 0, *resolved)) return false;
    dawn::state::equipment::light::Evaluation light{};
    for (std::size_t i = 0; i < resolved->itemCount; ++i) {
        const auto& item = resolved->items[i];
        if (!item.equipped) continue;
        std::int32_t power{};
        if (!dawn::state::equipment::light::item_power(item.instance.level, power)) return false;
        light.character[item.equipmentSlot] = {item.instance.baseDefinitionIndex, power};
        if (power > 0) { light.total += power; ++light.divisor; }
    }
    if (!light.divisor) return false;
    light.average = light.total / light.divisor;
    light.averageFloat = static_cast<float>(light.total) / static_cast<float>(light.divisor);
    std::vector<std::byte> bytes(family::character::layout::kObjectSize);
    return family::character::encode(account.characters[0], *resolved, light, bytes, &account);
}
void inventory_serial_selection() {
    auto draft = std::make_unique<editor::Draft>();
    draft->after = *fixture::account; draft->before = draft->after;
    const auto& catalog = studio::model().catalog; const auto& weapon = named("Riskrunner");
    std::string error;
    draft->after.characters[0].nextInventorySerial = 72;
    fixture::check(editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,106,true,error),"serial fixture creates equipped item");
    fixture::check(character_selection_encodes(draft->after),"editor-created gear passes real character selection encoder");
    draft->before = draft->after;
    draft->after.characters[0].equipment.slots[weapon.slot]->level = 105;
    auto prepared = std::make_unique<dawn::state::AccountState>();
    fixture::check(editor::prepare_commit(*draft,catalog,*prepared,error) && character_selection_encodes(*prepared),"direct level edit survives character selection");
    draft->before = draft->after = *prepared;
    fixture::check(editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,106,false,error)
        && character_selection_encodes(draft->after),"inventory creation survives character selection");
    fixture::check(editor::equip(*draft,catalog,0,draft->after.characters[0].inventory.values[0].instanceSoid,error)
        && character_selection_encodes(draft->after),"equipment swap survives character selection");
    auto slots = std::array<bool,16>{}; slots[weapon.slot] = true; std::mt19937 random{17};
    fixture::check(editor::randomize(*draft,catalog,0,slots,106,random,error)
        && character_selection_encodes(draft->after),"randomization survives character selection");
    // Exact failing invariant from the user's save: max item revision 74, next revision 74.
    auto& character = draft->after.characters[0];
    for (auto& item : character.equipment.slots) if (item) item->mutationSerial = 70;
    for (std::size_t i = 0; i < character.inventory.count; ++i) character.inventory.values[i].mutationSerial = 70;
    character.inventory.values[0].mutationSerial = 74; character.nextInventorySerial = 74;
    draft->before = draft->after;
    fixture::check(!character_selection_encodes(draft->after),"captured 74/74 invariant reproduces selection failure");
    fixture::check(editor::prepare_commit(*draft,catalog,*prepared,error)
        && prepared->characters[0].nextInventorySerial == 75
        && character_selection_encodes(*prepared),"repair counter without changing any owned item");
    auto expected = std::make_unique<dawn::state::AccountState>(draft->after);
    expected->characters[0].nextInventorySerial = 75;
    fixture::check(*expected == *prepared,"repair preserves every item and account setting");
    character.nextInventorySerial = INT32_MAX;
    const auto before = std::make_unique<dawn::state::AccountState>(draft->after);
    fixture::check(!editor::give(*draft,catalog,0,weapon.definition.definitionHash,1,106,false,error)
        && draft->after == *before,"serial exhaustion cannot overflow or alter draft");
    std::cout << "PASS: inventory edits and captured 74/74 repair through production character encoder\n";
}
/** Commits a draft as the editor's live apply does, keeping only whether it went through. */
bool commit(editor::Draft& draft, std::string& message) {
    bool live = false;
    return editor::apply(draft, studio::model().catalog, message, live);
}
void transactions() {
    auto draft = std::make_unique<editor::Draft>(); draft->after = *fixture::account;
    const auto& catalog = studio::model().catalog; const auto& subclass = named("Striker"); std::string error;
    fixture::check(subclass.paths.size() == 3 && subclass.abilities[0].size() == 3, "native subclass paths and jump choices");
    for (const auto& item : catalog.items) if (!item.abilities[0].empty()) {
        fixture::check(item.paths.size() == 3,"every subclass exposes three paths");
        for (const auto& choices : item.abilities) for (const auto& choice : choices)
            fixture::check(!choice.name.starts_with("Ability "),"localized ability display names");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        auto& c = draft->after.characters[i]; c.characterClass = dawn::state::CharacterClass::titan;
        fixture::check(editor::give(*draft,catalog,i,subclass.definition.definitionHash,1,105,true,error),"equip subclass on each character");
        c.movementAbilityEntry = 4; c.grenadeAbilityEntry = 7; c.superAbilityEntry = subclass.paths[0].super;
        c.meleeAbilityEntry = subclass.paths[0].melee; c.classAbilityEntry = 2;
    }
    auto& live = dawn::state::runtime::storage::g_state.account;
    live = draft->before = draft->after; draft->after.characters[1].level = 40; draft->dirty = true;
    live.characters[0].level = 1;
    fixture::check(!commit(*draft,error) && backups == 0 && commits == 0 && draft->dirty,"stale draft rejected before database writes");
    live = draft->before; backupAllowed = false;
    fixture::check(!commit(*draft,error) && commits == 0 && live == draft->before,"backup failure preserves live account");
    backupAllowed = true; commitAllowed = false;
    fixture::check(!commit(*draft,error) && live == draft->before && draft->dirty,"commit failure preserves draft and live account");
    commitAllowed = true;
    fixture::check(commit(*draft,error),error.c_str());
    fixture::check(!draft->dirty && live == draft->after && live.characters[0].selected && !live.characters[1].selected,"multi-character save preserves active selection");
    fixture::check(publishedAbilities.size() == 1,"duplicate subclass selections publish once");
    draft->after.characters[1].level = 41; draft->dirty = true;
    fixture::check(commit(*draft,error) && publishedAbilities.size() == 1,"cached duplicate subclass selections publish once");
    // Warm the complete catalog, including subclasses absent from the current account.
    namespace packages = dawn::client::content::items::packages;
    namespace abilities = dawn::state::build_data::abilities;
    auto scratch = std::make_unique<packages::reader::Scratch>();
    std::uint32_t rootTag{};
    auto globals = fixture::read(fixture::globals);
    fixture::check(fixture::tables::child_tag(globals, 0, rootTag), "ability fixture root");
    const auto root = fixture::read(rootTag);
    std::vector<std::byte> table, definition, blob;
    std::vector<abilities::Definition> all(abilities::kDefinitionCapacity);
    std::size_t count{};
    fixture::check(packages::build_character_abilities({}, *scratch, root, table, definition, blob, all, count), "prebuild all subclass choices");
    fixture::check(count >= 486, "catalog holds every standard subclass combination");
    all.resize(count);
    fixture::check(dawn::state::build_data::publish_ability_buckets(all), "publish complete subclass catalog");
    // An exact-size buffer works; an undersized one must not silently publish a partial catalog.
    const auto originalAccount = std::make_unique<dawn::state::AccountState>(*fixture::account);
    *fixture::account = draft->after;
    std::size_t exactCount{};
    fixture::check(packages::build_character_abilities({}, *scratch, root, table, definition, blob, all, exactCount)
        && exactCount == count, "configured duplicates fit exact catalog capacity");
    fixture::check(!packages::build_character_abilities({}, *scratch, root, table, definition, blob,
        std::span(all).first(8), exactCount), "old eight-row capacity is rejected");
    *fixture::account = *originalAccount;
    std::cout << "Subclass catalog: " << count << " standard combinations.\n";
    // Every offered path and every movement, grenade and class choice must reach a row. The nine stock
    // subclasses' lists are prebuilt whole; any other subclass builds its row from its own list when it
    // is applied, as the save does.
    constexpr std::array<std::uint16_t, 9> stockLists{1, 2, 3, 5, 6, 7, 9, 10, 11};
    fixture::tables::Array listRows{};
    fixture::check(fixture::tables::find_array_at(table, fixture::tables::kTableArrayDescriptor, listRows), "subclass list rows");
    for (const auto& item : catalog.items) if (!item.abilities[0].empty()) {
        const std::uint16_t list = item.detail.socketEntryListIndex;
        const bool stock = std::find(stockLists.begin(), stockLists.end(), list) != stockLists.end();
        std::vector<std::byte> listDefinition;
        if (!stock) {
            fixture::tables::IndexRow listRow{};
            fixture::check(fixture::tables::index_row(table, listRows, list, listRow), "subclass list row");
            listDefinition = fixture::read(listRow.targetTag);
        }
        for (const auto& path : item.paths) for (const auto& movement : item.abilities[0])
            for (const auto& grenade : item.abilities[1]) for (const auto& classAbility : item.abilities[4]) {
                abilities::Definition row;
                const abilities::Selection selection{movement.entry, grenade.entry, path.super, path.melee, classAbility.entry};
                const bool reached = stock ? abilities::find(list, selection, row)
                    : packages::build_ability_buckets({}, *scratch, listDefinition, blob, selection, row);
                if (!reached) std::cerr << item.name << " super=" << unsigned(path.super) << " melee=" << unsigned(path.melee) << '\n';
                fixture::check(reached, stock ? "stock subclass choice has a prebuilt ability row" : "subclass choice builds its ability row");
                for (unsigned bucket : {0U,1U,2U,3U,4U})
                    fixture::check(row.buckets[bucket].kind != abilities::kEmptyBucketKind, "selected ability has a resolved bucket kind");
            }
    }
    // Every offered path must also survive saving without dropping other cached combinations. The other
    // choices are the subclass's own first ones, since a list past the stock ones numbers its entries anew.
    for (const auto& item : catalog.items) if (!item.abilities[0].empty()) for (const auto& path : item.paths) {
        fixture::check(!item.abilities[1].empty() && !item.abilities[4].empty(), "subclass offers grenade and class choices");
        auto c = std::make_unique<editor::Draft>(); c->after = *fixture::account;
        c->after.characters[0].characterClass = static_cast<dawn::state::CharacterClass>(item.characterClass);
        fixture::check(editor::give(*c,catalog,0,item.definition.definitionHash,1,105,true,error),"path fixture subclass");
        auto& character = c->after.characters[0];
        character.movementAbilityEntry = item.abilities[0].front().entry; character.grenadeAbilityEntry = item.abilities[1].front().entry;
        character.superAbilityEntry = path.super; character.meleeAbilityEntry = path.melee; character.classAbilityEntry = item.abilities[4].front().entry;
        live = c->before = c->after; character.level = 40; c->dirty = true;
        fixture::check(commit(*c,error),error.c_str());
        fixture::check(abilities::count() >= count, "saving keeps other subclass combinations available");
        namespace loadout = dawn::middleware::datagen::family4::loadout;
        namespace instance = dawn::middleware::datagen::family4::instance;
        std::array<instance::SocketEntryState, instance::layout::kSocketEntryStateCapacity> states;
        std::array<instance::SocketSelector, loadout::kSelectorBucketCount> selectors;
        loadout::resolve_socket_states(fixture::lists[item.detail.socketEntryListIndex], character, states, selectors);
        fixture::check(states[path.melee] == instance::SocketEntryState::active, "selected tree melee socket is active");
        fixture::check(selectors[1].entry == path.super && selectors[2].entry == path.melee, "super and melee selectors retain their destination buckets");
    }
}
void migration(const std::filesystem::path& screens) {
    const auto root = screens / ("migration-" + std::to_string(GetTickCount64()));
    const auto source = root/"previous-runtime", destination = root/"Dawn";
    std::filesystem::create_directories(source/"scripts"); std::filesystem::create_directories(destination);
    std::ofstream(source/"settings.json") << "{\"existing\":true}";
    std::ofstream(source/"player-state.db") << "original account";
    std::ofstream(source/"scripts/mission.lua") << "existing script";
    dawn::core::path::Buffer buffer;
    fixture::check(dawn::core::path::assign(buffer,destination.wstring()),"migration destination");
    fixture::check(dawn::core::path::migrate_runtime(buffer),"native runtime migration");
    fixture::check(std::filesystem::exists(source/"player-state.db") && std::filesystem::exists(destination/"scripts/mission.lua"),"native migration preserves source and scripts");
    std::ofstream(destination/"player-state.db") << "edited account";
    std::filesystem::remove(destination/"settings.json");
    fixture::check(dawn::core::path::migrate_runtime(buffer),"native migration respects settings reset");
    std::string contents; std::getline(std::ifstream(destination/"player-state.db"),contents);
    fixture::check(contents == "edited account","native migration never overwrites an existing Dawn database");
}
/** The editor's pages and sheets, drawn and saved, at the width a player's overlay gives them. */
void pages(const std::filesystem::path& screens) {
    auto& model = studio::model();
    const auto& catalog = model.catalog;
    // The armory, on weapons and then filtered to one type.
    model.view = studio::View::armory; model.browse.category = studio::Category::weapons; model.results.key.clear();
    studio::select(named("Riskrunner")); settle(); screenshot(screens/"armory-weapons.ppm");
    model.browse.type = "Submachine Gun"; model.results.key.clear(); settle(); screenshot(screens/"armory-filtered.ppm");
    fixture::check(!model.results.items.empty() && std::all_of(model.results.items.begin(), model.results.items.end(),
        [](const auto* item) { return item->type == "Submachine Gun"; }), "weapon type selection filters the armory");
    model.browse.type.clear(); model.browse.category = studio::Category::armor; model.results.key.clear();
    studio::select(named("Dunemarchers")); settle(); screenshot(screens/"armory-armor.ppm");
    fixture::check(panel::preview::g_textures.at(named("Dunemarchers").iconTag).count > 0,"armor preview layers uploaded");
    fixture::check(panel::preview::g_textures.at(named("Riskrunner").iconTag).count > 0,"weapon preview layers uploaded");
    // The character's inventory, an owned item in the pane, and its perk picker on the full pool.
    model.view = studio::View::characterInventory; studio::clear_selection(); settle(); screenshot(screens/"inventory.ppm");
    auto& owned = studio::character().inventory.values[0];
    const auto* definition = catalog.find(owned.definitionHash);
    studio::select(*definition, owned.instanceSoid); settle(); screenshot(screens/"owned-overview.ppm");
    studio::open_perk_picker(*definition, 0); settle();
    fixture::check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1,"a socket opens the perk picker");
    model.picker.scope = editor::PlugScope::all; model.picker.options = catalog.candidates(*definition, 0, editor::PlugScope::all);
    settle(); screenshot(screens/"full-perk-pool.ppm"); ImGui::ClosePopupToLevel(0,true); settle();
    // Several items selected at once, with the bar that acts on all of them.
    model.picked.clear();
    for (std::size_t i = 0; i < studio::character().inventory.count && i < 3; ++i) model.picked.push_back(studio::character().inventory.values[i].instanceSoid);
    studio::clear_selection(); settle(); screenshot(screens/"inventory-selected.ppm");
    model.picked.clear();
    // A search reaches every character: what the others keep shows under this one's own results. The
    // second character is given a legendary weapon for it to find.
    std::string status;
    const auto legendary = std::find_if(catalog.items.begin(), catalog.items.end(), [](const auto& item) {
        return item.kind == editor::GearKind::weapon && !item.plug && item.definition.tier == 4;
    });
    fixture::check(legendary != catalog.items.end() && editor::give(*model.draft,catalog,1,legendary->definition.definitionHash,1,105,false,status),"search fixture weapon");
    std::snprintf(model.inventorySearch, sizeof model.inventorySearch, "%s", "is:legendary");
    settle(); screenshot(screens/"inventory-search.ppm");
    model.inventorySearch[0] = '\0';
    // The character page: identity, subclass, abilities and the equipped loadout.
    model.view = studio::View::characters; settle(); screenshot(screens/"character.ppm");
    const auto& striker = named("Striker");
    fixture::check(editor::give(*model.draft,catalog,0,striker.definition.definitionHash,1,105,true,status),"subclass preview setup");
    auto& guardian = studio::character(); guardian.movementAbilityEntry = 4; guardian.grenadeAbilityEntry = 7;
    guardian.superAbilityEntry = striker.paths[0].super; guardian.meleeAbilityEntry = striker.paths[0].melee; guardian.classAbilityEntry = 2;
    settle(); screenshot(screens/"character-subclass.ppm");
    // Two legendary pieces for every armor slot, so a loadout and the optimizer have whole sets to show.
    for (std::size_t slot = studio::kLastWeaponSlot + 1; slot <= studio::kLastArmorSlot; ++slot) {
        int given = 0;
        for (const auto& item : catalog.items) {
            if (given == 2) break;
            if (item.kind != editor::GearKind::armor || item.plug || item.internal || item.slot != slot || item.definition.tier != 4
                || !editor::fits_class(item, guardian.characterClass)) continue;
            fixture::check(editor::give(*model.draft,catalog,0,item.definition.definitionHash,1,105,false,status),"optimizer fixture armor");
            ++given;
        }
        fixture::check(given == 2, "two legendary pieces for each armor slot");
    }
    // Saved loadouts: what is on now, and one made of other pieces, so one row reads as equipped.
    model.loadouts.loaded = true; model.loadouts.writable = true; model.loadouts.entries.clear();
    model.loadouts.entries.push_back(editor::capture_loadout(guardian, catalog, "Raid"));
    auto crucible = editor::capture_loadout(guardian, catalog, "Crucible");
    for (std::size_t i = 0; i < guardian.inventory.count; ++i) {
        const auto* piece = catalog.find(guardian.inventory.values[i].definitionHash);
        if (piece && piece->slot < crucible.pieces.size() && piece->slot != 11)
            crucible.pieces[piece->slot] = {guardian.inventory.values[i].instanceSoid, piece->definition.definitionHash, 105, {}};
    }
    model.loadouts.entries.push_back(crucible);
    model.loadouts.entries.push_back(editor::capture_loadout(guardian, catalog, "Patrol"));
    press("characters_scroll", "Loadouts");
    fixture::check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1,"Loadouts opens the saved loadouts sheet");
    screenshot(screens/"loadouts.ppm");
    model.loadouts.chosen = 1; settle(); screenshot(screens/"loadouts-chosen.ppm");
    ImGui::ClosePopupToLevel(0,true); settle();
    // The armor optimizer: as it opens, with minimums set and the plan found, and once it is put on.
    press("characters_scroll", "Optimize Armor");
    fixture::check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1,"Optimize armor opens the optimizer");
    screenshot(screens/"optimizer.ppm");
    model.optimizer.targets = {};
    model.optimizer.targets[catalog.statOrder[0]] = 50; model.optimizer.targets[catalog.statOrder[1]] = 70;
    model.optimizer.targets[catalog.statOrder[2]] = 90;
    wait_for_plan(); screenshot(screens/"optimizer-minimums.ppm");
    // A piece row under the pointer shows the piece as the plan leaves it, in the item tooltip.
    ImGui::GetIO().AddMousePosEvent(600.0F, 405.0F); settle(); screenshot(screens/"optimizer-tooltip.ppm");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); settle();
    fixture::check(model.optimizer.plan.changes(), "the optimizer plans changes for minimums above the armor on now");
    press("Armor Optimizer", "Adjust Armor");
    wait_for_plan(); screenshot(screens/"optimizer-adjusted.ppm");
    fixture::check(!model.optimizer.plan.changes(), "once adjusted, the optimizer has nothing more to change");
    ImGui::ClosePopupToLevel(0,true); settle();
}
}
namespace dawn::core::ui::scaling::dpi {
float current() noexcept { return scale; }
float pixels(float v) noexcept { return v * scale; }
ImVec2 pixels(const ImVec2& v) noexcept { return {v.x * scale, v.y * scale}; }
}
namespace dawn::core::ui::layout {
StateSnapshot snapshot() noexcept { return ::layout; }
namespace internal {
bool context_is_current() noexcept { return ImGui::GetCurrentContext() != nullptr; }
void select_module(std::string_view id) noexcept { ::layout.selectedStableId.fill(0); std::copy(id.begin(),id.end(),::layout.selectedStableId.begin()); ::layout.selectedStableIdLength = id.size(); }
}
}
namespace dawn::core::ui::components::logo { bool draw(float) noexcept { return false; } }
// The fixtures carry the regular face alone, so a heavier line is struck by hand, as it is on an
// install that ships no heavier cut.
namespace dawn::core::ui::fonts::runtime { ImFont* weight(Weight) noexcept { return nullptr; } }
// The icon sweep walks every installed package, which the fixtures do not hold; the pages under test
// never start it.
namespace dawn::middleware::content::packages::reader {
bool scan_class_entries(std::wstring_view, std::uint32_t, ClassEntryVisitor, void*, ScanResult&) noexcept { return false; }
void release_caches() noexcept {}
}
// No game is signed in to take a published account; the editor then says it saved for next sign-in.
namespace dawn::server::bap {
std::size_t publish_external_account_mutation() noexcept;
std::size_t publish_external_account_mutation() noexcept { return 0; }
}
namespace dawn::state::runtime::storage { State g_state; SRWLOCK g_stateLock = SRWLOCK_INIT; }
namespace dawn::state::persistence {
bool backup_for_editor() noexcept { ++backups; return backupAllowed; }
bool commit_account(const AccountState&,const AccountState&) noexcept { ++commits; return commitAllowed; }
}
namespace dawn::state::build_data {
bool find_ability_buckets(std::uint16_t id,const abilities::Selection& selection,abilities::Definition& value) noexcept {
    return abilities::find(id, selection, value);
}
bool publish_ability_buckets(std::span<const abilities::Definition> rows) noexcept {
    for (std::size_t i = 0; i < rows.size(); ++i) for (std::size_t j = 0; j < i; ++j)
        fixture::check(rows[i].socketEntryListIndex != rows[j].socketEntryListIndex || rows[i].selection != rows[j].selection,"published ability keys are unique");
    publishedAbilities.assign(rows.begin(),rows.end()); return abilities::replace(rows);
}
}
namespace dawn::client::content::items::packages {
void report_ability_failure(const char* stage, std::size_t character, std::size_t first, std::size_t second) noexcept {
    std::cerr << "ability diagnostic: " << stage << ' ' << character << ' ' << first << ' ' << second << '\n';
}
}
int main(int argc, char** argv) {
    fixture::check(argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--serial-only"),"usage: editor_visual_tests <fixtures> <screenshots> <installed-font> [--serial-only]");
    fixture::load(argv[1]); std::filesystem::create_directories(argv[2]);
    const std::filesystem::path screens = argv[2];
    migration(screens);
    fixture::account->primarySoid = 1; fixture::account->characterCount = 3;
    auto& settings = fixture::account->settings;
    settings.configured = settings.keyBindings.configured = true;
    settings.controls.mouseLookSensitivity = 1; settings.controls.adsSensitivityModifier = 1;
    settings.audio.migrationVersion = dawn::state::account::settings::kCompletedAudioMigrationVersion;
    settings.display.calibrationPrimary = 10000;
    for (std::size_t i = 0; i < 3; ++i) {
        auto& c = fixture::account->characters[i]; c.soid = i + 2; c.characterClass = static_cast<dawn::state::CharacterClass>(i); c.level = 50; c.selected = i == 0;
    }
    fixture::check(panel::initialize(), "editor registered");
    fixture::check(ui::layout::credits::initialize(), "dedicated Credits tab registered");
    ui::layout::credits::request_open(ui::layout::credits::Project::sundial);
    fixture::check(ui::layout::credits::dispatch_pending([](const wchar_t* url) noexcept { return std::wstring_view(url) == ui::layout::credits::kSundialUrl; }),"Credits dispatches Sundial URL after user action");
    ui::layout::credits::request_open(ui::layout::credits::Project::original);
    fixture::check(ui::layout::credits::dispatch_pending([](const wchar_t* url) noexcept { return std::wstring_view(url) == ui::layout::credits::kSourceUrl; }),"Credits preserves original source URL");
    auto& model = studio::model();
    // The load runs first, so the reason it gives is the one printed.
    const bool loaded = editor::load_catalog(model.catalog,model.cancelLoad,model.loadProgress,model.loadError);
    fixture::check(loaded,model.loadError.c_str());
    fixture::write_missing();
    inventory_serial_selection();
    if (argc == 5) return 0;
    model.phase = studio::CatalogPhase::ready;
    const auto& catalog = model.catalog;
    std::size_t names{}, weapons{}, armor{}, previews{};
    for (const auto& item : catalog.items) {
        names += item.name.find("Unnamed") == std::string::npos; weapons += item.kind == editor::GearKind::weapon && !item.plug;
        armor += item.kind == editor::GearKind::armor && !item.plug;
        if ((item.kind == editor::GearKind::weapon || item.kind == editor::GearKind::armor) && !item.plug) previews += item.iconTag != 0;
    }
    std::cout << "Catalog: " << catalog.items.size() << " definitions, " << names << " names, " << weapons << " weapons, " << armor << " armor, " << catalog.plugs.size() << " perks, " << previews << " preview references\n";
    fixture::check(weapons > 500 && armor > 1000 && catalog.plugs.size() > 1000,"full installed catalog categories");
    std::ofstream report(screens/"catalog.tsv"); report << "name\ttype\tkind\tslot\tplug\ticon\tinstanced\n";
    for (const auto& item : catalog.items) report << item.name << '\t' << item.type << '\t' << static_cast<int>(item.kind) << '\t' << item.slot << '\t' << item.plug << '\t' << item.iconTag << '\t' << static_cast<int>(item.detail.instancedDefinitionState) << '\n';
    report.close();
    fixture::check(previews > 4000,"installed weapon and armor preview coverage");
    for (const auto& item : catalog.items) if ((item.kind == editor::GearKind::weapon || item.kind == editor::GearKind::armor) && !item.plug && !item.internal)
        fixture::check(item.iconTag != 0, "every named weapon and armor has its preview");
    std::unordered_map<std::uint32_t,bool> artwork;
    auto scratch = std::make_unique<dawn::middleware::content::packages::reader::Scratch>();
    unsigned missingArtwork{};
    for (const auto& item : catalog.items) if ((item.kind == editor::GearKind::weapon || item.kind == editor::GearKind::armor) && !item.plug && !item.internal) {
        if (!artwork.contains(item.iconTag)) artwork[item.iconTag] = panel::preview::read_icon(item.iconTag,*scratch).primary;
        if (!artwork[item.iconTag]) std::cerr << "Preview failed: " << item.name << '\n';
        missingArtwork += !artwork[item.iconTag];
    }
    // The six character stats are marked by the game's own icons wherever the editor lines them up.
    for (const auto tag : catalog.statIconTags) {
        fixture::check(tag != 0, "every character stat names an icon");
        missingArtwork += !panel::preview::read_icon(tag,*scratch).primary;
    }
    fixture::write_missing();
    std::cout << "Artwork: " << artwork.size() << " distinct installed previews decoded.\n";
    fixture::check(missingArtwork == 0,"every named weapon and armor preview, and every stat icon, decodes");
    mutations(); item_level_limits(); transactions(); studio::reload_account();
    std::string status;
    for (const auto* name : {"Riskrunner","Dunemarchers","Peacekeepers","Synthoceps"}) {
        const auto& item = named(name); (void)editor::give(*model.draft,catalog,0,item.definition.definitionHash,1,105,false,status);
    }
    // Another character keeps one too, so a search finds it elsewhere on the account.
    (void)editor::give(*model.draft,catalog,1,named("Riskrunner").definition.definitionHash,1,105,false,status);
    fixture::check(ui::memory::initialize(),"fixed UI arena"); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DeltaTime = 1.0F / 60;
    io.ConfigErrorRecoveryEnableAssert = false; io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext*,void*,const char* message) { ++errors; std::cerr << message << '\n'; };
    std::ifstream fontFile(argv[3], std::ios::binary | std::ios::ate);
    fixture::check(static_cast<bool>(fontFile), "installed font file");
    const auto fontSize = fontFile.tellg(); fontFile.seekg(0);
    std::vector<char> font(static_cast<std::size_t>(fontSize)); fontFile.read(font.data(), fontSize);
    ImFontConfig fontConfig; fontConfig.FontDataOwnedByAtlas = false; fontConfig.RasterizerDensity = 2;
    fixture::check(io.Fonts->AddFontFromMemoryTTF(font.data(),static_cast<int>(font.size()),16,&fontConfig) != nullptr,"installed game font"); ImGui::GetStyle().FontSizeBase = 16;
    D3D_FEATURE_LEVEL feature{};
    fixture::check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&gpu,&feature,&context)),"offscreen DX11 device");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    fixture::check(SUCCEEDED(gpu->CreateTexture2D(&desc,nullptr,&target)) && SUCCEEDED(gpu->CreateRenderTargetView(target,nullptr,&view)),"render target");
    fixture::check(ImGui_ImplDX11_Init(gpu,context),"ImGui DX11"); panel::preview::attach(gpu); ui::theme::apply();
    ui::layout::internal::select_module("core.loadout");
    pages(screens);
    // The same pages at a larger display scale, in a smaller window.
    scale = 1.5F; ui::theme::apply();
    model.view = studio::View::armory; model.browse.category = studio::Category::weapons; model.results.key.clear(); studio::clear_selection();
    frame(1000,800); frame(1000,800); screenshot(screens/"compact-armory.ppm");
    studio::select(named("Riskrunner")); frame(1000,800); frame(1000,800); screenshot(screens/"compact-details.ppm");
    scale = 1; ui::theme::apply(); ui::layout::internal::select_module("core.credits"); settle(); screenshot(screens/"credits.ppm");
    ui::layout::credits::shutdown();
    panel::shutdown(); panel::preview::release(); ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext();
    fixture::check(ui::memory::shutdown(),"no UI allocation leaks"); view->Release(); target->Release(); context->Release(); gpu->Release();
    std::cout << "PASS " << fixture::checks << " assertions; " << errors << " UI errors.\n";
}
