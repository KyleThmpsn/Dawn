#pragma once

#include <cstdint>

#include "../../ui/runtime/settings.h"
#include "external/definition.h"

namespace dawn::core::settings::client {

/** A load this long has stopped making progress, so the spawn stops waiting for it. */
inline constexpr std::uint64_t kDefaultSpawnHoldMs = 30'000;
/** A load past this is a hang, not a slow machine, and holding the spawn would never end. */
inline constexpr std::uint64_t kMaximumSpawnHoldMs = 600'000;

/** Read-only Client settings parsed by Core. */
struct Settings {
    /** In-game UI visibility and input policy. */
    ui::runtime::Settings userInterface;
    /** Points the Client at a server outside this process. Off answers everything in process. */
    external::Settings externalServer;
    /**
     * Replaces the title-screen (bootflow) artwork with the DDS files embedded in the DLL.
     * Off leaves the stock package art untouched. On by default.
     */
    bool customBootflowTextures{true};
    /**
     * Inverts the white boot screens (the Bungie splash and the loading screens before the
     * title) so they are dark. Bright frames are inverted until the title screen arrives.
     * On by default.
     */
    bool invertSplashScreens{true};
    /**
     * Draws the title-screen filigree: two mirrored, morphing Julia medallions rendered by the
     * DLL over the title screen, under the logo and the text. On by default.
     */
    bool titleFiligree{true};
    /**
     * Research aid: logs every GPU entry the client's decoded-entry dispatcher receives (class,
     * tag, size, first bytes) and writes each texture payload once to exports\texdump\, so one
     * launch shows which textures a screen is drawn from. Off by default.
     */
    bool dumpGpuEntries{false};
    /**
     * Research aid for a stalled game. On the first hitch the watchdog reports, logs the world tag
     * loader and every thread's stack, and the watchdog's own in-flight jobs with the stack of
     * the thread the stalled job waits on. When the callback pump stops, dumps every thread even
     * if the watchdog has stopped too, and samples the networking tick's latch throughout. The
     * dumps suspend game threads and can perturb the stall they measure. Off by default.
     */
    bool stallTrace{false};
    /**
     * Research aid: logs every effect the game applies or removes, with the definition tag that
     * owns it, up to 16 lines per definition and action every 10 seconds; perk spawn requests, up
     * to eight per graph every 10 seconds; every weapon fire source; and every projectile launch
     * with its early flight. Off by default.
     */
    bool effectTrace{false};
    /**
     * Research aid: watches up to four addresses listed in `selector_watch.txt` and logs the code
     * that reads or writes each one. Installs a process-wide exception handler and a polling
     * thread, even before the file exists. Off by default.
     */
    bool selectorWatch{false};
    /**
     * Research aid: traces the Wwise bank, sound, stream and event paths for the IDs listed in
     * `audio_trace_watch.txt` into `audio_trace.log`. Hooks the game only while the file exists
     * at startup. Off by default.
     */
    bool audioTrace{false};
    /**
     * Research aid: traces input, ability, animation and health paths for the IDs listed in
     * `action_trace_watch.txt` into `action_trace.log`. Hooks the game only while the file
     * exists at startup. Off by default.
     */
    bool actionTrace{false};
    /**
     * Releases the world-transition fade channel at the in-world step.
     * The client only releases it on the player spawn, so this covers a spawn that never runs
     * and leaves the world black. On by default.
     */
    bool fadeRelease{true};
    /**
     * Forces the activity session's status 5-to-6 ready check.
     * Two of its five terms are client flags no host message reaches, so the host cannot open it.
     */
    bool forceJoinRequestReady{true};
    /**
     * Reports a public region as private to the region transition.
     * On, a public region loads solo. Off, it waits for a public activity host, which is the
     * route to the citizen join. A forced destination loads solo either way.
     */
    bool regionPrivate{false};
    /**
     * Pins the participation record to the replicated snapshot at `comp + 496`.
     * Off, the record is the local one at `comp + 1256`, whose spawn-gate byte no wire field
     * reaches.
     */
    bool pinReplicatedRecord{true};
    /**
     * Restores Omega's measured authored opening groups to the ordinary roster intersection.
     * Those objects carry directive, dialogue, music, and gate components but do not advertise
     * the generic roster marker used by the destination-wide discovery pass.
     */
    bool rosterForceAuthored{false};
    /**
     * Runs the player spawn after the world-transition fade is armed.
     * A spawn before the arm releases nothing, so the screen stays black. Settable because it is
     * the only thing that can turn an allowed spawn into a refusal.
     */
    bool holdSpawn{true};
    /** How long the spawn waits for a load. `hold_spawn` decides whether it waits at all. */
    std::uint64_t spawnHoldMs{kDefaultSpawnHoldMs};
    /**
     * Seeds the published authored mission components with their neutral schema bodies. The seed
     * is deferred until the client first reaches in-world so it cannot filter the phase-zero
     * player citizen out of the opening slice. Off by default.
     */
    bool seedAuthoredSensors{false};
};

} // namespace dawn::core::settings::client
