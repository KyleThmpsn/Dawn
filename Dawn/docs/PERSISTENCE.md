# Player persistence

Dawn keeps durable player state in `Dawn/player-state.db`, relative to the loaded
`steam_api64.dll`. SQLite is compiled into the module; players do not install a database server or
an SQLite program.

On the first boot without a database, Dawn imports the account, inventory, equipment, sockets,
item flags, settings, unlock banks, objectives, progression lanes, and family-5 overrides from
`settings.json`. That import is one transaction. Later boots load the database as the source of
truth; editing the old seed in `settings.json` does not overwrite saved progress.

The database uses SQLite transactions, foreign keys, and WAL crash recovery. A busy, corrupt, or
newer-schema database stops startup with an error. Dawn does not silently reset it or fall back
to the JSON seed. Close the game before copying or restoring the database. After shutdown, copy
`player-state.db` and any remaining `player-state.db-wal` and `player-state.db-shm` files together.

Mission rows are scoped to the character captured by the authenticated activity connection.
Dawn records completion and the checkpoint identities exposed by reconstructed controllers.
Nightfall completion creates a pending reward debt and marks the mission complete in one
transaction. The later currency credit and debt delivery are also one transaction, so a restart
cannot silently lose or duplicate an earned payout. Pending debt is reclaimed after restart.

The currently wired controller signals cover `raid_envy_v310`, `mission_abs`,
`adventure_ginger`, `adventure_vod`, `adventure_whisk`, `mission_bond`, `strike_bond`,
`mission_pact`, `strike_pact`, and `adventure_rumba`. The raid and both Bond/Pact variants record
their exposed spawn-set and slice-set checkpoint identity; the other controllers record their
monotonic section and terminal completion.

A stored checkpoint is durable progress data; it is not automatically applied. None of the
current reconstructed missions has a verified path that restores its native controller, actors,
and world state as one coherent operation. Dawn therefore does not fake resume by changing a
spawn or section alone. Omega (`mission_scot`) has a verified terminal state, but that callback is
process-global and does not carry an authenticated character owner; its completion is not written
until a safe session-bound terminal exists.

The game's account-update request (opcode 701) saves menu preferences, including FOV, account
key bindings, sensitivity, audio, HUD/chat options, VSync, and post-processing toggles. Partial
updates preserve omitted settings. A successful response follows the database commit; invalid
requests or failed writes leave the previous settings intact. Repeated identical updates do not
rewrite the database. The completed PC seed version is saved too, so later sign-ins retain the
saved preferences instead of reseeding them from local cvars.

Schema version 5 adds these PC/display fields to existing saves without resetting inventory,
characters, or old settings. Existing saves start with the original seed policy until the client
submits its first settings update. Video options owned by the game's local cvars (such as
resolution and graphics quality) continue using that file. Selecting local key-binding storage
also continues using the game's cvars; Dawn retains the selected storage mode.

Family-5 overrides are imported and restored; they remain boot-authored policy rather than
mutable gameplay progress.

The files `settings.json`, `hud.json`, `movement.json`, and `player.json` in a runtime tree are local
configuration. Repository defaults live in `Dawn/resources/default_*.json`. The repository
ignores generated databases, journal files, and local runtime configuration.
