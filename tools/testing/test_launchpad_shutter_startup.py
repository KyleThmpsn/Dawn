"""Execute the production shutter-hook lifecycle against fake native hooks.

The real startup expression and owner functions are compiled, not reimplemented.
This catches a release that contains the fix but never installs its observer.
Native signatures and Detours are fakes; animation needs an in-game check.
Run on Windows with the same MSBuild toolchain as Dawn.
"""
from pathlib import Path
import json
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "Dawn/src/client/hooks/bootflow"


def function(text, name):
    start = text.index(name + "() noexcept {")
    start = text.rfind("\n", 0, start) + 1
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class LaunchpadShutterStartupTests(unittest.TestCase):
    def test_release_startup_and_hook_lifetime(self):
        source = (BOOT / "omega_ikora_origin_probe.cpp").read_text()
        bootstrap = (BOOT / "bootflow_hook_lifecycle.cpp").read_text()
        admission = re.search(r"const bool omegaIkoraOriginInstalled\s*=.*?;", bootstrap, re.S).group()
        settings = json.loads((ROOT / "Dawn/resources/default_settings.json").read_text())["experiments"]["omega"]
        defaults = ",".join(str(settings[key]).lower() for key in
                            ("ikora_carrier_model_suppression", "ikora_vfx_rebind", "unsafe_diagnostics"))
        constants = "\n".join(re.findall(r"constexpr std::(?:uintptr_t|uint32_t) k\w+ = .*?;", source))
        prefixes = "\n".join(re.findall(r"constexpr std::array<std::byte, \d+> k\w+Prefix\{.*?\};", source, re.S))
        slots = re.search(r"enum class HookSlot.*?constexpr std::size_t kHookCount.*?;", source, re.S).group()
        declarations = re.search(r"std::array<hooking::detour::Handle, kHookCount> g_handles\{\};.*?hooking::CallGate g_callGate\{\};", source, re.S).group()
        aliases = "\n".join("using " + name + " = void(*)();" for name in
                            ("SpawnerDeficit", "SceneActorScheduler", "EntityFactory", "ComponentStart",
                             "EffectTransformCompose", "SelectorChildCreate", "SelectorObjectResolve"))
        callbacks = "\n".join("void " + name + "() {}" for name in
                              ("spawner_deficit", "scene_actor_scheduler", "entity_factory", "component_start",
                               "effect_transform_compose", "selector_child_create", "selector_object_resolve"))
        harness = r'''
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include "client/hooking/call_gate.h"
#include "client/hooking/detour.h"
#define CHECK(v) do { if(!(v)) {std::fprintf(stderr,"line %d: %s\n",__LINE__,#v);std::abort();} } while(false)
namespace hooking=dawn::client::hooking;
namespace core::settings {
struct Omega { bool ikoraCarrierModelSuppression{},ikoraVfxRebind{},unsafeDiagnostics{}; };
struct Settings { Omega omegaExperiments; } current;
const Settings& get() noexcept {return current;}
}
std::array<std::byte,0x1C00000> image{};
void* GetModuleHandleW(const wchar_t*) {return image.data();}
std::uintptr_t badPrefix{};
template<class T> bool prefix_matches(std::byte* p,const T&) {return p!=image.data()+badPrefix;}
template<class... T> void report(const char*,T...) {}
std::size_t installed{},removed{},transactions{};
bool failAttach{},deferDetach{};
namespace dawn::client::hooking::detour {
bool install(std::span<const Spec> specs,std::span<Handle> handles) noexcept {
    CHECK(specs.size()==handles.size());++transactions;
    if(failAttach) return false;
    installed=specs.size();
    for(std::size_t i=0;i<specs.size();++i) {
        CHECK(!handles[i].attached && specs[i].target && specs[i].replacement);
        handles[i]={specs[i].target,specs[i].replacement,true};
    }
    return true;
}
UninstallResult uninstall(std::span<Handle> handles,std::span<const ProtectedCodeEntry>,IdleCheck idle) noexcept {
    CHECK(idle());
    for(const auto& handle:handles) CHECK(handle.attached && handle.original && handle.replacement);
    if(deferDetach) return UninstallResult::protectedCodeActive;
    removed=handles.size();for(auto& handle:handles) handle={};return UninstallResult::removed;
}
}
'''
        harness += constants + prefixes + slots + aliases + declarations + callbacks
        harness += r'''
struct Context {};
thread_local Context g_sourceContext{};
thread_local bool g_ikoraFactoryActive{};
thread_local std::uint32_t g_ikoraFactoryScene{};
bool calls_idle() noexcept {return g_callGate.idle();}
'''
        for name in ("install_omega_ikora_origin_probe", "quiesce_omega_ikora_origin_probe", "uninstall_omega_ikora_origin_probe"):
            harness += function(source, name)
        harness += "bool boot() {const auto& omega=core::settings::get().omegaExperiments;"
        harness += "const bool ikoraProbe=omega.ikoraCarrierModelSuppression||omega.ikoraVfxRebind||omega.unsafeDiagnostics;(void)ikoraProbe;"
        harness += admission + "return omegaIkoraOriginInstalled;}\nint main() {\n"
        harness += "core::settings::current.omegaExperiments={" + defaults + "};\n"
        harness += r'''
CHECK(!core::settings::current.omegaExperiments.unsafeDiagnostics);
// Production must still install with an unrelated diagnostic signature missing.
badPrefix=kSpawnerDeficitRva;
CHECK(boot());CHECK(installed==2 && g_handles[2].attached && g_handles[3].attached && g_callGate.accepting());
CHECK(g_handles[2].original==image.data()+kEntityFactoryRva);
CHECK(g_handles[3].original==image.data()+kSelectorChildCreateRva);
CHECK(g_selectorChildCreateOriginal.load()==reinterpret_cast<SelectorChildCreate>(image.data()+kSelectorChildCreateRva));
CHECK(!g_handles[0].attached && !g_omegaProbeActive.load());
auto before=transactions;CHECK(boot() && transactions==before);
// Quiescing preserves ownership and a deferred removal can be retried.
deferDetach=true;CHECK(!uninstall_omega_ikora_origin_probe());
CHECK(!g_callGate.accepting() && g_handles[2].attached && g_handles[3].attached);
CHECK(!boot());deferDetach=false;
CHECK(uninstall_omega_ikora_origin_probe());CHECK(removed==2 && !g_handles[2].attached && !g_handles[3].attached);
CHECK(!g_selectorChildCreateOriginal.load());
CHECK(uninstall_omega_ikora_origin_probe());
// Both required signatures and attachment failure still fail closed.
badPrefix=kEntityFactoryRva;before=transactions;CHECK(!boot() && transactions==before);
badPrefix=kSelectorChildCreateRva;CHECK(!boot() && transactions==before);
badPrefix=0;failAttach=true;CHECK(!boot() && !g_callGate.accepting());failAttach=false;
CHECK(!g_handles[2].attached && !g_handles[3].attached);
CHECK(boot());CHECK(uninstall_omega_ikora_origin_probe());
// Each existing opt-in still installs/removes the full original bundle.
for(unsigned flag=0;flag<3;++flag) {
    core::settings::current.omegaExperiments={flag==0,flag==1,flag==2};
    badPrefix=kSpawnerDeficitRva;CHECK(!boot());badPrefix=0;
    CHECK(boot());CHECK(installed==7 && g_omegaProbeActive.load());
    // Removal follows installed ownership even if settings change meanwhile.
    core::settings::current.omegaExperiments={};
    CHECK(uninstall_omega_ikora_origin_probe());CHECK(removed==7);
}
std::puts("Release startup, optional hooks, signature failures, retry and teardown passed.");
}
'''
        output = ROOT / "build/unit/launchpad_startup"
        output.mkdir(parents=True, exist_ok=True)
        (output / "test.cpp").write_text(harness)
        (output / "test.vcxproj").write_text('''<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><WindowsTargetPlatformVersion>10.0.26100.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v145</PlatformToolset></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
<PropertyGroup><OutDir>$(ProjectDir)out\\</OutDir><IntDir>$(ProjectDir)obj\\</IntDir></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp20</LanguageStandard><AdditionalIncludeDirectories>$(ProjectDir)..\\..\\..\\Dawn\\src</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup>
<ItemGroup><ClCompile Include="test.cpp" /></ItemGroup><Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" /></Project>''')
        compiler = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe")
        result = subprocess.run([str(compiler), str(output / "test.vcxproj"), "/p:Configuration=Release", "/p:Platform=x64", "/nologo", "/v:minimal"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(output / "out/test.exe")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
