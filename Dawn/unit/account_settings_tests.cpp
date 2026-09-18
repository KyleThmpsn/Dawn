#include <Windows.h>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/settings/settings.h"
#include "../src/server/web_service/settings_save.h"
#include "../src/state/runtime/storage/internal.h"
#include "../src/state/persistence/persistence.h"
#include "../src/middleware/encoding/bit_reader.h"
#include "../src/middleware/datagen/family4/account/preferences/preferences_encoder.h"
#include "../vendor/sqlite/sqlite3.h"
#include "fixtures/account_settings_wire.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)

namespace dawn::core::log {
void write(Channel, Level, std::string_view message) noexcept {
    std::fprintf(stderr,"%.*s\n",static_cast<int>(message.size()),message.data());
}
void early(std::string_view) noexcept {}
Settings defaults() noexcept { return {}; }
}
namespace dawn::state::runtime::storage {
State g_state{};
SRWLOCK g_stateLock = SRWLOCK_INIT;
}
namespace dawn::state {
AccountState account_snapshot() noexcept { return runtime::storage::g_state.account; }
}

namespace {
namespace state = dawn::state;
namespace durable = state::persistence;
namespace ws = dawn::middleware::web_service;
namespace codec = ws::messages::opcode701;
namespace prefs = dawn::middleware::datagen::family4::account::preferences;
using Settings = state::account::settings::AccountSettings;

std::filesystem::path database_path() {
    wchar_t path[32768]{};
    CHECK(GetModuleFileNameW(nullptr,path,32768)>0);
    const auto folder=std::filesystem::path(path).parent_path();
    CHECK(folder.filename()==L"account_settings");
    return folder/L"Dawn"/L"player-state.db";
}
void remove_database() {
    const auto path=database_path();
    std::filesystem::remove(path);
    std::filesystem::remove(path.wstring()+L"-wal");
    std::filesystem::remove(path.wstring()+L"-shm");
}
void sql(const char* command) {
    sqlite3* db{};CHECK(sqlite3_open16(database_path().c_str(),&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,command,nullptr,nullptr,nullptr)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
}
int integer(const char* command) {
    sqlite3* db{};CHECK(sqlite3_open16(database_path().c_str(),&db)==SQLITE_OK);
    sqlite3_stmt* query{};CHECK(sqlite3_prepare_v2(db,command,-1,&query,nullptr)==SQLITE_OK);
    CHECK(sqlite3_step(query)==SQLITE_ROW);const int result=sqlite3_column_int(query,0);
    sqlite3_finalize(query);sqlite3_close(db);return result;
}
ws::Message message(std::span<const std::byte> payload) { return {701,0x12345678,payload}; }
bool save(std::span<const std::byte> payload) {
    std::array<std::byte,32> response{};std::size_t size{};
    CHECK(dawn::server::web_service::save_settings(message(payload),response,size));
    CHECK(size==11);
    CHECK(response[0]==std::byte{2}&&response[1]==std::byte{0xbd});
    CHECK(response[2]==std::byte{0x12}&&response[5]==std::byte{0x78});
    dawn::middleware::encoding::bits::Reader reader{std::span{response}.subspan(6,size-6)};
    std::uint64_t status{};CHECK(reader.read(5,status));CHECK(status==1||status==2);
    return status==1;
}
void verify_saved(const Settings& settings) {
    CHECK(settings.controls.mouseLookSensitivity==17);
    CHECK(settings.controls.adsSensitivityModifier==1.25F);
    CHECK(settings.audio.musicVolume==5&&settings.audio.soundEffectsVolume==7);
    CHECK(settings.display.motionBlur&&!settings.display.filmGrain&&settings.display.chromaticAberration);
    CHECK(settings.display.calibrationAlpha==0.5F);
    CHECK(settings.pc.seedVersion==1&&settings.pc.fieldOfViewAdjustment==20);
    CHECK(settings.pc.useLocalKeyBindings&&settings.pc.verticalSyncMode==0);
    const auto& jump=settings.keyBindings.values[static_cast<std::size_t>(state::account::settings::bindings::Action::jump)];
    CHECK(jump.primary==0x39&&jump.secondary==0x6f);
    prefs::Record record{};prefs::BindingsRecord bindings{};
    CHECK(prefs::encode(settings,record,bindings));
    CHECK(bindings.accountSeedVersion==1&&bindings.fieldOfViewAdjustment==20);
    CHECK(bindings.keyBindings[20]==0x006F0039&&bindings.keyBindings[58]==0x00740101);
    CHECK(record.motionBlurMirror==1&&record.chromaticAberrationMirror==1);
}
void fresh_process() {
    wchar_t executable[32768]{};CHECK(GetModuleFileNameW(nullptr,executable,32768)>0);
    std::wstring command=L"\"";command+=executable;command+=L"\" --verify";
    STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process));
    CHECK(WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0);DWORD result=1;
    CHECK(GetExitCodeProcess(process.hProcess,&result));CloseHandle(process.hThread);CloseHandle(process.hProcess);
    CHECK(result==0);
}
}

int main(int argc,char**) {
    const auto fixture=std::filesystem::path(__FILE__).parent_path().parent_path()/L"resources"/L"default_settings.json";
    std::ifstream stream(fixture);CHECK(stream.good());
    const std::string json{std::istreambuf_iterator<char>{stream},std::istreambuf_iterator<char>{}};
    static dawn::core::settings::Settings config{};CHECK(dawn::core::settings::parse(json,config));
    static state::AccountState loaded{};static state::unlocks::ScopedTable unlocks{};static state::Family5State family{};
    const auto load=[&] { return durable::initialize(GetModuleHandleW(nullptr),config.initialAccount,
        config.initialUnlocks,config.initialFamily5,loaded,unlocks,family); };
    if (argc>1) { CHECK(load());verify_saved(loaded.settings);durable::shutdown();return 0; }
    remove_database();CHECK(load());
    state::runtime::storage::g_state.account=loaded;
    const auto original=loaded.settings;
    auto decoded=original;
    CHECK(codec::parse_settings(message(settings_wire_fixture::full),original,decoded));
    CHECK(decoded.pc.fieldOfViewAdjustment==30&&decoded.pc.seedVersion==1);
    // Every truncation of a complete native snapshot must leave output untouched.
    for(std::size_t size=0;size<settings_wire_fixture::full.size();++size) {
        auto out=original;
        CHECK(!codec::parse_settings(message(std::span{settings_wire_fixture::full}.first(size)),original,out));
        CHECK(out==original);
    }
    auto trailing=std::vector<std::byte>{settings_wire_fixture::full.begin(),settings_wire_fixture::full.end()};
    trailing.push_back(std::byte{});
    CHECK(!codec::parse_settings(message(trailing),original,decoded));
    trailing.pop_back();trailing.back()|=std::byte{1};
    CHECK(!codec::parse_settings(message(trailing),original,decoded));
    const int initialRevision=integer("SELECT value FROM metadata WHERE key='account_revision'");
    std::array<std::byte,2> tiny{};std::size_t written{};
    CHECK(!dawn::server::web_service::save_settings(message(settings_wire_fixture::full),tiny,written));
    CHECK(integer("SELECT value FROM metadata WHERE key='account_revision'")==initialRevision);
    CHECK(save(settings_wire_fixture::full));
    CHECK(save(settings_wire_fixture::partial));
    verify_saved(state::account_snapshot().settings);
    const auto saved=state::account_snapshot();
    auto inventoryCheck=saved;inventoryCheck.settings=loaded.settings;CHECK(inventoryCheck==loaded);
    const int savedRevision=integer("SELECT value FROM metadata WHERE key='account_revision'");
    CHECK(savedRevision==initialRevision+2);
    CHECK(save(settings_wire_fixture::partial)&&save(settings_wire_fixture::empty));
    CHECK(integer("SELECT value FROM metadata WHERE key='account_revision'")==savedRevision);
    CHECK(!save(settings_wire_fixture::invalid_mouse));CHECK(state::account_snapshot()==saved);
    bool changed=true;
    CHECK(!state::account::settings::save(saved.primarySoid,original,original,changed)&&!changed);
    CHECK(!state::account::settings::save(saved.primarySoid+1,saved.settings,original,changed));
    // Force a real disk-write failure and prove neither database nor memory reports success.
    sql("CREATE TRIGGER deny_settings BEFORE INSERT ON settings_values BEGIN SELECT RAISE(ABORT,'test refusal'); END;");
    CHECK(!save(settings_wire_fixture::full));CHECK(state::account_snapshot()==saved);
    CHECK(integer("SELECT value FROM metadata WHERE key='account_revision'")==savedRevision);
    sql("DROP TRIGGER deny_settings;");
    sql("UPDATE metadata SET value=value+1 WHERE key='account_revision';");
    CHECK(!save(settings_wire_fixture::full));CHECK(state::account_snapshot()==saved);
    CHECK(integer("SELECT integer_value FROM settings_values WHERE key='pc.fieldOfViewAdjustment'")==20);
    CHECK(integer("SELECT value FROM metadata WHERE key='account_revision'")==savedRevision+1);
    durable::shutdown();fresh_process();CHECK(load());CHECK(loaded==saved);durable::shutdown();
    // Simulate the actual old schema: retain every old preference and key binding, remove new fields.
    sql("DELETE FROM settings_values WHERE key LIKE 'pc.%' OR key IN ('display.motionBlur','display.filmGrain','display.chromaticAberration');PRAGMA user_version=4;");
    CHECK(load());CHECK(integer("PRAGMA user_version")==5);
    CHECK(loaded.settings.controls==saved.settings.controls&&loaded.settings.keyBindings==saved.settings.keyBindings);
    CHECK(loaded.settings.pc.seedVersion==0&&loaded.settings.pc.fieldOfViewAdjustment==0);
    CHECK(loaded.settings.pc.voiceChatEnabled==loaded.settings.social.voiceChatEnabled);
    CHECK(loaded.characters==saved.characters&&loaded.profileItems==saved.profileItems);
    durable::shutdown();remove_database();
    std::puts("PASS account settings: native wire, partial updates, atomic failure, migration, fresh-process reload");
}
