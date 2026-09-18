#requires -Version 5.1
<# Integration tests in disposable fixture folders. Never installs into the supplied game.
The real game EXE is hard-linked read-only for version inspection; it is never launched.
Run with Windows PowerShell 5.1 as well as current PowerShell. #>
[CmdletBinding()]
param(
    [string] $GameExecutable = 'C:\Destiny 2 Development\destiny2.exe',
    [string] $ReleaseDll
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$testRoot = Join-Path $repo ('build/installer-tests/' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$package = Join-Path $testRoot 'package'
$arguments = @{ Release = 'installer-test'; OutputDirectory = $package }
if ($ReleaseDll) { $arguments.DllPath = $ReleaseDll }
$null = & (Join-Path $repo 'tools/install/New-DawnRelease.ps1') @arguments
$installer = Join-Path $package 'Install-Dawn.ps1'
$manifestPath = Join-Path $package 'release.json'
$manifestText = [IO.File]::ReadAllText($manifestPath)
$manifest = $manifestText | ConvertFrom-Json
$script:passed = 0

function Assert-True([bool] $Value, [string] $Message) {
    if (-not $Value) { throw "ASSERTION FAILED: $Message" }
}
function Write-TestJson([string] $Path, $Value) {
    [IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $Value -Depth 100 -Compress), $utf8)
}
function Pass([string] $Message) { $script:passed++; Write-Host "PASS: $Message" }
function Snapshot([string] $Root) {
    return (@(Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
        $_.FullName -notlike "$Root\.dawn\*" -and $_.Name -ne 'destiny2.exe'
    } | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($Root.Length + 1) + ':' + (Get-FileHash -LiteralPath $_.FullName).Hash
    }) -join "`n")
}
function New-Game([string] $Name) {
    $root = Join-Path $testRoot $Name
    foreach ($relative in @('', 'bin/x64', 'Sunrise', 'Dawn/scripts', 'bin/x64/Dawn/scripts')) {
        [IO.Directory]::CreateDirectory((Join-Path $root $relative)) | Out-Null
    }
    # Tests and the supplied executable must be on the same NTFS volume for this link.
    New-Item -ItemType HardLink -Path (Join-Path $root 'destiny2.exe') -Target $GameExecutable | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'steam_api64.dll'), 'old root DLL')
    [IO.File]::WriteAllText((Join-Path $root 'bin/x64/steam_api64.dll'), 'old bin DLL')
    [IO.File]::WriteAllText((Join-Path $root 'Dawn/scripts/stale.lua'), 'obsolete content')
    [IO.File]::WriteAllText((Join-Path $root 'bin/x64/Dawn/scripts/stale.lua'), 'obsolete bin content')
    [IO.File]::WriteAllText((Join-Path $root 'Dawn/unknown-personal.txt'), 'keep in backup')
    $settings = Get-Content -LiteralPath (Join-Path $package 'payload/Dawn/settings.json') -Raw | ConvertFrom-Json
    $settings.steam.user.persona_name = 'Fixture Player'
    $settings.client.fade_release = $false
    $settings.state.activity.arrival_overrides[0].bubble = 777
    $settings.state.activity.arrival_overrides += [pscustomobject]@{ package_name = 'fixture_custom'; bubble = 123; slice_set = 984; spawn_set_hash = '0x00000001' }
    Write-TestJson (Join-Path $root 'Sunrise/settings.json') $settings
    Write-TestJson (Join-Path $root 'Sunrise/hud.json') ([pscustomobject]@{ dawn_card = $true })
    Write-TestJson (Join-Path $root 'Sunrise/player.json') ([pscustomobject]@{ infinite_ammo_enabled = $false })
    foreach ($name in @('player-state.db', 'player-state.db-wal', 'player-state.db-shm', 'player-state.db-journal',
            'device_identity.key', 'roster_exclude_keys.txt', 'event_music.txt')) {
        [IO.File]::WriteAllText((Join-Path $root "Sunrise/$name"), "fixture bytes: $name")
        [IO.File]::WriteAllText((Join-Path $root "Dawn/$name"), "old Dawn bytes: $name")
        [IO.File]::WriteAllText((Join-Path $root "bin/x64/Dawn/$name"), "old bin Dawn bytes: $name")
    }
    Copy-Item -LiteralPath (Join-Path $root 'Sunrise/settings.json') -Destination (Join-Path $root 'Dawn/settings.json')
    foreach ($legacy in @('Restoration', 'bin/x64/Sunrise')) {
        [IO.Directory]::CreateDirectory((Join-Path $root $legacy)) | Out-Null
        [IO.File]::WriteAllText((Join-Path $root "$legacy/settings.json"), 'old or damaged legacy settings')
    }
    return $root
}
function Expect-Failure([scriptblock] $Action, [string] $Pattern) {
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-True ($null -ne $message) "Expected rejection matching $Pattern"
    Assert-True ($message -like $Pattern) "Wrong rejection: $message (expected $Pattern)"
}

# All per-user preference writes stay in this disposable fixture, even on failure.
$originalAppData = $env:APPDATA
$env:APPDATA = Join-Path $testRoot 'user-profile'
try {
$displayPath = Join-Path $env:APPDATA 'Bungie/DestinyPC/prefs/cvars.xml'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($displayPath)) | Out-Null
$displayOriginal = '<?xml version="1.0"?><body><namespace name="graphics"><cvar name="window_mode" value="3" /><cvar name="fullscreen_resolution_width" value="2560" /><cvar name="fullscreen_resolution_height" value="1440" /><cvar name="render_resolution_percentage" value="90" /></namespace><namespace name="key bindings"><cvar name="jump" value="space!unused" /></namespace></body>'
[IO.File]::WriteAllText($displayPath, $displayOriginal, $utf8)
$displayBefore = (Get-FileHash -LiteralPath $displayPath).Hash
$game = New-Game 'fresh-install'
$before = Snapshot $game
& $installer -GameRoot $game -WhatIf
Assert-True ((Snapshot $game) -ceq $before) 'WhatIf changed installation files'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn'))) 'WhatIf created state'
Assert-True ((Get-FileHash -LiteralPath $displayPath).Hash -eq $displayBefore) 'WhatIf changed display preferences'
Pass 'WhatIf validates without creating or replacing files'

& $installer -GameRoot $game
[xml]$display = [IO.File]::ReadAllText($displayPath)
Assert-True ($display.SelectSingleNode('/body/namespace[@name="graphics"]/cvar[@name="window_mode"]').value -eq '2') 'Windowed fullscreen was not selected'
Assert-True ($display.SelectSingleNode('/body/namespace[@name="graphics"]/cvar[@name="fullscreen_resolution_width"]').value -eq '2560') 'Player resolution was overwritten'
Assert-True ($display.SelectSingleNode('/body/namespace[@name="graphics"]/cvar[@name="render_resolution_percentage"]').value -eq '90') 'Render scale was overwritten'
Assert-True ($display.SelectSingleNode('/body/namespace[@name="key bindings"]/cvar[@name="jump"]').value -eq 'space!unused') 'Key binding was overwritten'
Pass 'Installer selects windowed fullscreen and preserves player resolution, render scale, and key bindings'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game 'Dawn/scripts/stale.lua'))) 'Obsolete script survived'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game 'bin/x64/Dawn/scripts/stale.lua'))) 'Obsolete bin script survived'
foreach ($name in @('player-state.db', 'player-state.db-wal', 'player-state.db-shm', 'player-state.db-journal',
        'device_identity.key', 'roster_exclude_keys.txt', 'event_music.txt')) {
    foreach ($runtime in @('Dawn', 'bin/x64/Dawn')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $game "$runtime/$name"))) "Old personal data was imported: $runtime/$name"
    }
}
$actual = Get-Content -LiteralPath (Join-Path $game 'Dawn/settings.json') -Raw | ConvertFrom-Json
$default = Get-Content -LiteralPath (Join-Path $package 'payload/Dawn/settings.json') -Raw | ConvertFrom-Json
Assert-True ($actual.steam.user.persona_name -eq $default.steam.user.persona_name) 'Old player name survived'
Assert-True ($actual.client.fade_release -eq $default.client.fade_release) 'Old preference survived'
Assert-True ($actual.state.activity.arrival_overrides[0].bubble -eq $default.state.activity.arrival_overrides[0].bubble) 'Shipped arrival was not updated'
Assert-True (@($actual.state.activity.arrival_overrides | Where-Object { $_.package_name -eq 'fixture_custom' }).Count -eq 0) 'Old custom arrival survived'
$hud = Get-Content -LiteralPath (Join-Path $game 'Dawn/hud.json') -Raw | ConvertFrom-Json
Assert-True ($hud.dawn_card -eq $false) 'Old HUD choice survived'
foreach ($entry in $manifest.files) {
    foreach ($prefix in @('', 'bin/x64/')) {
        Assert-True ((Get-FileHash -LiteralPath (Join-Path $game ($prefix + $entry.path))).Hash -eq $entry.sha256) "Payload mismatch: $prefix$($entry.path)"
    }
}
Pass 'Default installation starts fresh despite multiple legacy runtimes; every release file matches its hash'

$state = Get-Content -LiteralPath (Join-Path $game '.dawn/release.json') -Raw | ConvertFrom-Json
Assert-True ((Get-FileHash -LiteralPath (Join-Path $state.backup 'previous/user-display/cvars.xml')).Hash -eq $displayBefore) 'Original display preferences were not backed up exactly'
Assert-True (Test-Path -LiteralPath (Join-Path $state.backup 'previous/Dawn/unknown-personal.txt')) 'Unknown original file was not backed up'
foreach ($name in @('player-state.db', 'player-state.db-wal', 'player-state.db-shm', 'player-state.db-journal', 'settings.json')) {
    Assert-True (Test-Path -LiteralPath (Join-Path $state.backup "previous/Dawn/$name")) "Missing original data in backup: $name"
}
[IO.File]::WriteAllText((Join-Path $game 'Dawn/player-state.db'), 'progress since installation')
& $installer -GameRoot $game -Restore
Assert-True ((Get-FileHash -LiteralPath $displayPath).Hash -eq $displayBefore) 'Rollback did not restore exact original display preferences'
Assert-True ((Snapshot $game) -ceq $before) 'Rollback did not restore exact original installation'
$retained = @(Get-ChildItem -LiteralPath (Join-Path $state.backup 'after-restore') -Filter player-state.db -Recurse -File)
Assert-True (@($retained | Where-Object { [IO.File]::ReadAllText($_.FullName) -eq 'progress since installation' }).Count -eq 1) 'Rollback lost post-install progress'
Pass 'Rollback restores the original files and separately retains newer player progress'

$game = New-Game 'new-user-display'
$env:APPDATA = Join-Path $testRoot 'new-user-profile'
$newDisplayPath = Join-Path $env:APPDATA 'Bungie/DestinyPC/prefs/cvars.xml'
& $installer -GameRoot $game -WhatIf
Assert-True (-not (Test-Path -LiteralPath $env:APPDATA)) 'WhatIf created a new preference directory'
& $installer -GameRoot $game
[xml]$display = [IO.File]::ReadAllText($newDisplayPath)
Assert-True ($display.SelectSingleNode('/body/namespace[@name="graphics"]/cvar[@name="window_mode"]').value -eq '2') 'New user did not receive windowed fullscreen'
Assert-True ($display.SelectNodes('//cvar').Count -eq 1) 'New user received hard-coded resolution or other preferences'
# Recovery must not write the installing user's settings into a different account.
$env:APPDATA = Join-Path $testRoot 'different-user-profile'
Expect-Failure { & $installer -GameRoot $game -Restore } '*same Windows user profile*'
Assert-True (-not (Test-Path -LiteralPath $env:APPDATA)) 'Recovery created another user profile'
$env:APPDATA = Join-Path $testRoot 'new-user-profile'
& $installer -GameRoot $game -Restore
Assert-True (-not (Test-Path -LiteralPath $newDisplayPath)) 'Rollback retained newly created display preferences'
Pass 'New users receive only the mode default; rollback restores absence and rejects a different Windows profile'

$env:APPDATA = Join-Path $testRoot 'user-profile'
$game = New-Game 'missing-window-mode'
[IO.File]::WriteAllText($displayPath, '<body><namespace name="graphics"><cvar name="gamma_control" value="4" /></namespace></body>', $utf8)
& $installer -GameRoot $game
[xml]$display = [IO.File]::ReadAllText($displayPath)
Assert-True ($display.SelectSingleNode('//cvar[@name="window_mode"]').value -eq '2') 'Missing mode was not added'
Assert-True ($display.SelectSingleNode('//cvar[@name="gamma_control"]').value -eq '4') 'Existing graphics preference changed'
& $installer -GameRoot $game -Restore
Pass 'Existing graphics preferences without a window mode receive the default'

$game = New-Game 'invalid-display'
foreach ($invalidDisplay in @('<broken', '<body><namespace name="graphics"><cvar name="window_mode" value="0"/><cvar name="window_mode" value="3"/></namespace></body>', '<!DOCTYPE body [<!ENTITY test SYSTEM "file:///not-read">]><body>&test;</body>')) {
    [IO.File]::WriteAllText($displayPath, $invalidDisplay, $utf8)
    Expect-Failure { & $installer -GameRoot $game } '*'
    Assert-True ([IO.File]::ReadAllText($displayPath) -ceq $invalidDisplay) 'Invalid display preferences were changed'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn'))) 'Invalid display preferences changed the installation'
}
[IO.File]::WriteAllText($displayPath, $displayOriginal, $utf8)
Pass 'Malformed, duplicate, and DTD-based preferences fail before any installation changes'

$game = New-Game 'upgrade'
& $installer -GameRoot $game
[IO.File]::WriteAllText((Join-Path $game 'Dawn/player-state.db'), 'current Dawn progress')
[IO.File]::WriteAllText((Join-Path $game 'Dawn/scripts/removed-in-new-release.lua'), 'old release script')
$before = Snapshot $game
& $installer -GameRoot $game
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game 'Dawn/player-state.db'))) 'Reinstall retained the existing Dawn save'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game 'bin/x64/Dawn/player-state.db'))) 'Reinstall retained the bin Dawn save'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game 'Dawn/scripts/removed-in-new-release.lua'))) 'Upgrade retained obsolete content'
# A standalone copy can restore without needing any of the package's payload files.
$recovery = Join-Path $testRoot 'Recover-Dawn.ps1'
Copy-Item -LiteralPath $installer -Destination $recovery
& $recovery -GameRoot $game -Restore
Assert-True ((Snapshot $game) -ceq $before) 'Upgrade rollback did not restore previous Dawn state'
Pass 'Reinstall resets an existing Dawn save, removes stale content, and restores without a payload'

$game = New-Game 'failed-copy'
$before = Snapshot $game
$lockedFile = [IO.File]::Open((Join-Path $game 'bin/x64/steam_api64.dll'), 'Open', 'ReadWrite', 'None')
try { Expect-Failure { & $installer -GameRoot $game } '*previous files were restored*' }
finally { $lockedFile.Dispose() }
Assert-True ((Snapshot $game) -ceq $before) 'Failed replacement did not roll back'
Pass 'A locked second DLL rolls back already-replaced DLL/runtime files'

$game = New-Game 'tampered'
$before = Snapshot $game
$tampered = Join-Path $package 'payload/Dawn/scripts/omega.lua'
$original = [IO.File]::ReadAllBytes($tampered)
try {
    [IO.File]::AppendAllText($tampered, '-- changed')
    Expect-Failure { & $installer -GameRoot $game } '*missing or changed*'
} finally { [IO.File]::WriteAllBytes($tampered, $original) }
Assert-True ((Snapshot $game) -ceq $before) 'Tampered package touched game'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn'))) 'Tampered package created state'
Pass 'Checksum mismatch rejects the package before changes'

try {
    $bad = $manifestText | ConvertFrom-Json
    $bad.files[0].path = '../outside.dll'
    Write-TestJson $manifestPath $bad
    Expect-Failure { & $installer -GameRoot $game } '*Invalid or duplicate*'
} finally { [IO.File]::WriteAllText($manifestPath, $manifestText, $utf8) }
Pass 'Manifest traversal is rejected'

$game = New-Game 'old-settings'
$settingsPath = Join-Path $game 'Dawn/settings.json'
[IO.File]::WriteAllText($settingsPath, 'broken old settings')
$before = Snapshot $game
& $installer -GameRoot $game
Assert-True ((Get-FileHash -LiteralPath $settingsPath).Hash -eq (Get-FileHash -LiteralPath (Join-Path $package 'payload/Dawn/settings.json')).Hash) 'Malformed settings were not replaced with release defaults'
& $installer -GameRoot $game -Restore
Assert-True ((Snapshot $game) -ceq $before) 'Rollback did not retain original malformed settings'
Pass 'Old damaged settings are replaced without migration and remain recoverable'

$game = New-Game 'interrupted'
$before = Snapshot $game
& $installer -GameRoot $game
$state = Get-Content -LiteralPath (Join-Path $game '.dawn/release.json') -Raw | ConvertFrom-Json
$journalPath = Join-Path $state.backup 'journal.json'
$journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
$journal.state = 'installing'
Write-TestJson $journalPath $journal
Expect-Failure { & $installer -GameRoot $game } '*interrupted installation needs recovery*'
& $installer -GameRoot $game -Restore -BackupPath $state.backup
Assert-True ((Snapshot $game) -ceq $before) 'Interrupted transaction did not recover'
Pass 'Interrupted installations block another update and can be recovered'

$game = New-Game 'junction'
$external = Join-Path $testRoot 'outside-runtime'
[IO.Directory]::CreateDirectory($external) | Out-Null
[IO.File]::WriteAllText((Join-Path $external 'keep.txt'), 'outside data')
New-Item -ItemType Junction -Path (Join-Path $game 'Dawn/linked') -Target $external | Out-Null
Expect-Failure { & $installer -GameRoot $game } '*Linked entry*'
Assert-True ([IO.File]::ReadAllText((Join-Path $external 'keep.txt')) -eq 'outside data') 'Junction target changed'
Pass 'Nested junctions cannot redirect runtime replacement'

$game = Join-Path $testRoot 'wrong-build'
[IO.Directory]::CreateDirectory($game) | Out-Null
Copy-Item -LiteralPath "$env:WINDIR\System32\whoami.exe" -Destination (Join-Path $game 'destiny2.exe')
Expect-Failure { & $installer -GameRoot $game } '*Unsupported Destiny build*'
Pass 'Wrong executable version is rejected'

$updater = Join-Path $package 'Update-Dawn.ps1'
Assert-True (Test-Path -LiteralPath $updater) 'Updater is missing from the package'
$game = New-Game 'preserving-update'
foreach ($runtime in @('Dawn', 'bin/x64/Dawn')) {
    foreach ($name in @('settings.json', 'hud.json', 'movement.json', 'player.json')) {
        if ($name -ne 'settings.json' -or $runtime -ne 'Dawn') {
            Copy-Item -LiteralPath (Join-Path $package "payload/Dawn/$name") -Destination (Join-Path $game "$runtime/$name")
        }
    }
    [IO.Directory]::CreateDirectory((Join-Path $game "$runtime/cache")) | Out-Null
    [IO.File]::WriteAllText((Join-Path $game "$runtime/cache/old.bin"), 'stale derived cache')
    [IO.File]::WriteAllText((Join-Path $game "$runtime/scripts/custom.lua"), 'personal custom mission')
}
[IO.Directory]::CreateDirectory((Join-Path $game '.dawn')) | Out-Null
Write-TestJson (Join-Path $game 'Dawn/hud.json') ([pscustomobject]@{ dawn_card = $true })
Move-Item -LiteralPath (Join-Path $game 'Dawn/hud.json') -Destination (Join-Path $game 'Dawn/hud-rename.tmp')
Move-Item -LiteralPath (Join-Path $game 'Dawn/hud-rename.tmp') -Destination (Join-Path $game 'Dawn/HUD.JSON')
Write-TestJson (Join-Path $game '.dawn/release.json') ([pscustomobject]@{
    schema = 1; release = '0.1.2'; files = @($manifest.files) + @([pscustomobject]@{ path = 'Dawn/scripts/stale.lua' })
})
$before = Snapshot $game
$personal = @{}
foreach ($runtime in @('Dawn', 'bin/x64/Dawn')) {
    foreach ($name in @('player-state.db', 'player-state.db-wal', 'player-state.db-shm', 'player-state.db-journal',
            'device_identity.key', 'roster_exclude_keys.txt', 'event_music.txt', 'settings.json', 'hud.json', 'movement.json', 'player.json', 'scripts/custom.lua')) {
        $relative = "$runtime/$name"
        $personal[$relative] = (Get-FileHash -LiteralPath (Join-Path $game $relative)).Hash
    }
}
$displayBefore = (Get-FileHash -LiteralPath $displayPath).Hash
& $updater -GameRoot $game -WhatIf
Assert-True ((Snapshot $game) -ceq $before) 'Update WhatIf changed game files'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn/release-backups'))) 'Update WhatIf created a backup'
& $updater -GameRoot $game
foreach ($relative in $personal.Keys) {
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $game $relative)).Hash -eq $personal[$relative]) "Update changed personal data: $relative"
}
foreach ($runtime in @('Dawn', 'bin/x64/Dawn')) {
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $game "$runtime/cache"))) 'Update retained stale caches'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $game "$runtime/scripts/stale.lua"))) 'Update retained retired release content'
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $game "$runtime/scripts/omega.lua")).Hash -eq
        (Get-FileHash -LiteralPath (Join-Path $package 'payload/Dawn/scripts/omega.lua')).Hash) 'Update did not install current mission content'
}
Assert-True ((Get-FileHash -LiteralPath $displayPath).Hash -eq $displayBefore) 'Update changed native display/key-binding preferences'
$state = Get-Content -LiteralPath (Join-Path $game '.dawn/release.json') -Raw | ConvertFrom-Json
Assert-True ($state.profileMode -eq 'preserve') 'Update receipt does not identify preserved profile'
$updateJournal = Get-Content -LiteralPath (Join-Path $state.backup 'journal.json') -Raw | ConvertFrom-Json
Assert-True (@($updateJournal.operations).Count -eq 5) 'Update journal includes an unwanted display operation'
Assert-True (Test-Path -LiteralPath (Join-Path $state.backup 'previous/Dawn/cache/old.bin')) 'Old cache was not retained in rollback backup'
$originalProfile = $env:APPDATA
$env:APPDATA = Join-Path $testRoot 'other-update-user'
try { & $updater -GameRoot $game -Restore }
finally { $env:APPDATA = $originalProfile }
Assert-True ((Snapshot $game) -ceq $before) 'Update rollback did not restore exact original files'
Assert-True ((Get-FileHash -LiteralPath $displayPath).Hash -eq $displayBefore) 'Update rollback touched display preferences'
Pass 'Updater preserves separate profiles, WAL/journal files, all settings and custom content; replaces release content and supports exact rollback'

& $updater -GameRoot $game
[IO.File]::WriteAllText((Join-Path $game 'Dawn/player-state.db'), 'progress after first update')
& $updater -GameRoot $game
Assert-True ([IO.File]::ReadAllText((Join-Path $game 'Dawn/player-state.db')) -ceq 'progress after first update') 'Repeated update reset progress'
Pass 'Repeating an update keeps progress created since the previous update'

$single = Join-Path $testRoot 'single-runtime-update'
[IO.Directory]::CreateDirectory((Join-Path $single 'bin/x64')) | Out-Null
New-Item -ItemType HardLink -Path (Join-Path $single 'destiny2.exe') -Target $GameExecutable | Out-Null
Copy-Item -LiteralPath (Join-Path $game 'Dawn') -Destination (Join-Path $single 'Dawn') -Recurse
& $updater -GameRoot $single
Assert-True ((Get-FileHash -LiteralPath (Join-Path $single 'Dawn/player-state.db')).Hash -eq
    (Get-FileHash -LiteralPath (Join-Path $single 'bin/x64/Dawn/player-state.db')).Hash) 'Single-folder profile was not copied to the missing runtime'
Pass 'Older single-folder installations carry their profile into both DLL load locations'

$game = New-Game 'failed-preserving-update'
$before = Snapshot $game
$lockedFile = [IO.File]::Open((Join-Path $game 'bin/x64/steam_api64.dll'), 'Open', 'ReadWrite', 'None')
try { Expect-Failure { & $updater -GameRoot $game } '*previous files were restored*' }
finally { $lockedFile.Dispose() }
Assert-True ((Snapshot $game) -ceq $before) 'Failed update lost profile data'
Pass 'Failed update restores the original saves and binaries'

$game = New-Game 'malformed-update-settings'
[IO.File]::WriteAllText((Join-Path $game 'Dawn/settings.json'), 'broken personal settings')
$before = Snapshot $game
Expect-Failure { & $updater -GameRoot $game } '*'
Assert-True ((Snapshot $game) -ceq $before) 'Malformed settings were replaced during update'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn'))) 'Invalid update created installer state'
Pass 'Malformed personal settings stop an update before replacement'

$game = Join-Path $testRoot 'missing-update-profile'
[IO.Directory]::CreateDirectory($game) | Out-Null
New-Item -ItemType HardLink -Path (Join-Path $game 'destiny2.exe') -Target $GameExecutable | Out-Null
Expect-Failure { & $updater -GameRoot $game } '*No existing Dawn profile*'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $game '.dawn'))) 'Update without a profile started a fresh installation'
Pass 'Updater refuses to silently create a fresh profile'

$game = New-Game 'update-downgrade'
[IO.Directory]::CreateDirectory((Join-Path $game '.dawn')) | Out-Null
Write-TestJson (Join-Path $game '.dawn/release.json') ([pscustomobject]@{ release = '9.0.0'; files = @() })
try {
    $versioned = $manifestText | ConvertFrom-Json
    $versioned.release = '0.1.3'
    Write-TestJson $manifestPath $versioned
    Expect-Failure { & $updater -GameRoot $game } '*will not downgrade*'
} finally { [IO.File]::WriteAllText($manifestPath, $manifestText, $utf8) }
Pass 'Updater rejects a known newer installed release'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($package + '.zip')
try {
    $extra = @($zip.Entries | Where-Object { $_.FullName -match '(^|/)(src|tools|tests|cache|logs)/|\.pdb$|player-state\.db' })
    Assert-True ($extra.Count -eq 0) 'Player ZIP contains development/private files'
} finally { $zip.Dispose() }
Pass 'Distributable ZIP excludes development tools, symbols, caches, and saves'
Write-Host "$script:passed integration checks passed on PowerShell $($PSVersionTable.PSVersion). Fixtures: $testRoot"
} finally { $env:APPDATA = $originalAppData }
