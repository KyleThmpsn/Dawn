#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/settings/settings.h"
#include "../src/state/account/settings/settings_runtime.h"
#include "../src/state/persistence/persistence.h"
#include "../src/state/runtime/runtime.h"
#include "../src/state/runtime/state.h"
#include "../src/state/unlocks/unlocks_runtime.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s (Windows %lu)\n", __LINE__, #x, GetLastError()); std::abort(); } } while (false)

namespace {
HANDLE process = GetCurrentProcess();
DWORD64 imageBase{};

struct Matches { std::vector<DWORD64> addresses; };
BOOL CALLBACK collect(SYMBOL_INFO* symbol, ULONG, void* context) {
    static_cast<Matches*>(context)->addresses.push_back(symbol->Address);
    return TRUE;
}

template<class T> T resolve(const char* mask) {
    Matches matches;
    CHECK(SymEnumSymbols(process, imageBase, mask, collect, &matches));
    if (matches.addresses.size() != 1) {
        std::fprintf(stderr, "Expected one production symbol for %s; got %zu\n", mask, matches.addresses.size());
        std::abort();
    }
    return reinterpret_cast<T>(matches.addresses.front());
}
}

// This deliberately calls code from the final DLL, not separately linked copies of its sources.
// Both the DLL and database must be staged in the isolated fixture folder before running.
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    CHECK(argc == 3);
    const auto dllPath = std::filesystem::canonical(argv[1]);
    const auto fixture = dllPath.parent_path();
    CHECK(fixture.filename() == L"fixture");
    CHECK(fixture.parent_path().filename() == L"settings_dll_startup");
    CHECK(std::filesystem::is_regular_file(fixture / L"Dawn" / L"player-state.db"));
    const HMODULE module = LoadLibraryW(dllPath.c_str());
    CHECK(module != nullptr);
    imageBase = reinterpret_cast<DWORD64>(module);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_EXACT_SYMBOLS);
    CHECK(SymInitializeW(process, fixture.c_str(), FALSE));
    CHECK(SymLoadModuleExW(process, nullptr, dllPath.c_str(), nullptr, imageBase, 0, nullptr, 0) != 0);

    namespace state = dawn::state;
    namespace config = dawn::core::settings;
    namespace durable = state::persistence;
    const auto parse = resolve<decltype(&config::parse)>("dawn::core::settings::parse");
    const auto load = resolve<decltype(&durable::initialize)>("dawn::state::persistence::initialize");
    const auto close = resolve<decltype(&durable::shutdown)>("dawn::state::persistence::shutdown");
    const auto setIdentity = resolve<decltype(&state::set_primary_soid)>("dawn::state::set_primary_soid");
    const auto selectCharacter = resolve<decltype(&state::set_selected_character)>("dawn::state::set_selected_character");
    auto& live = *resolve<state::State*>("dawn::state::runtime::storage::g_state");
    auto& liveUnlocks = *resolve<state::unlocks::ScopedTable*>("dawn::state::unlocks::*g_table");

    std::ifstream stream{std::filesystem::path(argv[2])};
    CHECK(stream.good());
    const std::string json{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    static config::Settings settings{};
    CHECK(parse(json, settings));
    static state::AccountState loaded{};
    static state::unlocks::ScopedTable unlocks{};
    static state::Family5State family{};
    CHECK(load(module, settings.initialAccount, settings.initialUnlocks, settings.initialFamily5,
               loaded, unlocks, family));
    live.account = loaded;
    liveUnlocks = unlocks;
    const auto snapshot = [&] { return live.account; };

    // The reported crash occurs here during opcode 503, before the menu save path runs.
    std::puts("Testing final DLL sign-in identity...");
    std::fflush(stdout);
    CHECK(setIdentity(loaded.primarySoid));
    CHECK(snapshot() == loaded);
    CHECK(loaded.characterCount != 0);
    bool changed{};
    CHECK(selectCharacter(loaded.characters[0].soid, changed));
    auto expected = snapshot();
    CHECK(setIdentity(loaded.primarySoid));
    CHECK(snapshot() == expected);
    close();
    CHECK(load(module, settings.initialAccount, settings.initialUnlocks, settings.initialFamily5,
               loaded, unlocks, family));
    // Selection is session state; startup intentionally returns to character selection.
    for (auto& character : expected.characters) character.selected = false;
    CHECK(loaded == expected);
    close();
    CHECK(SymCleanup(process));
    // Keep the DLL loaded until process exit; it owns the objects returned by its C++ functions.
    std::printf("PASS final DLL: sign-in identity, character selection, saved-settings reload; AccountState=%zu bytes\n", sizeof(state::AccountState));
}
