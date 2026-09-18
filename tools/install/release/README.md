# Player release installer

This installer consumes an extracted prebuilt release. It does not build code or
use the development installer in the parent directory. It supports Windows
PowerShell 5.1 and newer.

Publisher workflow, from the repository root:

```powershell
.\tools\install\New-DawnRelease.ps1 -Release '0.1.0-test1'
```

The default input is `build/x64/Release/steam_api64.dll`. `-DllPath` and
`-OutputDirectory` can override those paths. Use a DLL built and validated with
the current content. The tool creates a new directory and ZIP, refuses to
overwrite an existing release, and prints the ZIP hash. Send players the ZIP.
They extract it and double-click `Update-Dawn.cmd` to keep an existing save,
or `Install-Dawn.cmd` to start fresh.

## Preserving updates

`Update-Dawn.ps1` delegates to the same transaction engine with `-Update`.
It requires an existing Dawn profile and supports `-GameRoot`, `-WhatIf`,
`-Restore`, and `-BackupPath`. It does not require build tools or Python.

- Keep each runtime's database, WAL/SHM/journal companions, identity, settings,
  HUD/movement/player preferences, event selections, and custom files.
- Keep existing configuration bytes; missing configuration files use the release
  defaults. The runtime handles supported older configuration/save migrations.
- Keep separate root/bin profiles separate. If a runtime folder is absent,
  copy the existing Dawn profile into that load location.
- Install current packaged scripts, vendor rules, presets and licenses. Remove
  retired managed content only when the prior release receipt identifies it.
  Other custom files remain. Previous versions remain in the full backup.
- Omit derived caches from the new runtime. Do not touch native CVARS or display
  preferences, including during rollback of an update.
- Validate personal JSON and all package hashes before replacement. Check every
  preserved file against its original hash before and after replacement.
- Reject known release downgrades and refuse an update with no Dawn profile.
  Sunrise/Restoration profiles are not selected implicitly.
- Use the same backup, interrupted-transaction recovery, process checks and
  automatic rollback as fresh installation. Repeat updates preserve progress.

Run `tools/install/tests/release_installer.tests.ps1` on Windows PowerShell 5.1
and current PowerShell. It tests both modes in disposable fixtures.

## Fresh-install contract

- Check the executable's exact file version, `86657.20.08.23.1800.d2_rc`.
- Validate every payload file against its required SHA-256 manifest; reject
  unknown paths, extra files, traversal, and links/junctions.
- `Install-Dawn.ps1` without `-Update`, including a reinstall, starts a fresh save. No migration
  or `-SourceRuntime` selection is performed. Multiple legacy profiles and
  malformed old settings do not block installation.
- Install configuration byte-for-byte from the release. Do not copy or merge
  old databases, WAL/SHM/journal files, identity, settings, or event selections.
  The runtime creates its new database from the shipped settings on first boot.
- Stage and verify the complete release before replacing anything. Install
  identical fresh runtime content at root `Dawn` and `bin/x64/Dawn`, and the
  Release DLL at both corresponding locations.
- Replace whole Dawn runtime folders. This removes stale scripts and presets.
  Prior Dawn saves, preferences, unknown files, caches, and logs remain in the
  full backup. Caches regenerate on first boot. Legacy Sunrise/Restoration
  directories stay intact but are not imported or used by the new runtime.
- Record every replacement in a durable journal under `.dawn/release-backups`.
  Failed replacements roll back. An interrupted run must be restored before
  the next installation. `-Restore` works without a payload or a network.
- A rollback also preserves displaced current files under `after-restore`,
  including player progress created after the installation.
- Refuse running Destiny processes and runtime folders containing source
  checkouts. Never stop the game, launch it, or change execution policy globally.
- Set native `graphics.window_mode` to `2` (Windowed Fullscreen) at installation.
  Preserve resolution, render scale, quality, and bindings; do not ship the
  publisher's monitor dimensions. A missing CVARS file receives only this mode.
  Players can choose another mode later; the DLL does not force it at startup.
- Back up the installing Windows user's `%APPDATA%/Bungie/DestinyPC/prefs/cvars.xml`
  as the journal's `user-display/cvars.xml` operation. This is a shared Destiny
  preference and also affects other game installations under that Windows user.
  Rollback restores the exact previous file or its absence, retaining newer
  preferences under `after-restore`. Recovery requires the same Windows profile.
  Older journals without a display operation remain supported. Invalid XML,
  duplicate mode entries, DTDs, and linked preference paths are rejected.

`-WhatIf` validates the package and installation and prints
the intended change without modifying game files. `-Restore -BackupPath ...`
selects a particular backup beneath the chosen game root.

Manifest hashing detects missing or altered payload files. It is not publisher
authentication; distribute the installer and bundle through a trusted channel.
This script does not make an untested DLL or mission release ready for public use.
