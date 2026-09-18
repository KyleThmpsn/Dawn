#requires -Version 5.1
<#
.SYNOPSIS
Completely removes Dawn, including saves, settings, caches, and backups.
.DESCRIPTION
Standalone uninstaller; no release payload or build tools are needed. Restores
an original Steam DLL when available. Unrecognized DLLs are left in place.
All Dawn data and the .dawn installer/backup directory are permanently deleted.
.EXAMPLE
.\Uninstall-Dawn.ps1 -GameRoot 'D:\Games\Destiny 2' -WhatIf
.EXAMPLE
.\Uninstall-Dawn.ps1 -GameRoot 'D:\Games\Destiny 2'
.EXAMPLE
.\Uninstall-Dawn.ps1 -GameRoot 'D:\Games\Destiny 2' -OriginalDll 'D:\Backup\steam_api64.dll'
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string] $GameRoot,
    [string] $OriginalDll
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-PlainPath([string] $Path) {
    $probe = [IO.Path]::GetFullPath($Path)
    while ($probe) {
        $item = Get-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Links and junctions are not supported: $probe"
        }
        $probe = [IO.Path]::GetDirectoryName($probe)
    }
}

function Get-SafePath([string] $Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $script:Root $Relative))
    if (-not $path.StartsWith($script:Root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the game folder: $path"
    }
    Assert-PlainPath $path
    return $path
}

function Assert-PlainTree([string] $Path) {
    Assert-PlainPath $Path
    foreach ($item in Get-ChildItem -LiteralPath $Path -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked entry: $($item.FullName)" }
        if ($item.PSIsContainer) { Assert-PlainTree $item.FullName }
    }
}

function Assert-GameClosed {
    if (Get-Process -Name destiny2 -ErrorAction SilentlyContinue) { throw 'Close Destiny 2 before uninstalling Dawn.' }
}

function Test-SteamDll([string] $Path) {
    Assert-PlainPath $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $Path).VersionInfo.ProductName -ne 'Steam Client API') { return $false }
    $reader = New-Object IO.BinaryReader([IO.File]::OpenRead($Path))
    try {
        if ($reader.BaseStream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5A4D) { return $false }
        $reader.BaseStream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset + 24 -gt $reader.BaseStream.Length) { return $false }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { return $false }
        $reader.BaseStream.Position = $offset + 22
        return [bool]($reader.ReadUInt16() -band 0x2000)
    } finally { $reader.Dispose() }
}

function Find-OriginalDll([string] $Relative) {
    if ($OriginalDll) { return $OriginalDll }
    $candidates = @((Get-SafePath ('.dawn/original/' + $Relative)))
    if ($Relative -ne 'steam_api64.dll') { $candidates += Get-SafePath '.dawn/original/steam_api64.dll' }
    foreach ($location in @('.dawn/release-backups', '.dawn/backup')) {
        $history = Get-SafePath $location
        if (-not (Test-Path -LiteralPath $history)) { continue }
        foreach ($backup in Get-ChildItem -LiteralPath $history -Directory | Sort-Object Name) {
            Assert-PlainPath $backup.FullName
            if ($location -eq '.dawn/release-backups') {
                $journalPath = Join-Path $backup.FullName 'journal.json'
                Assert-PlainPath $journalPath
                if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) { continue }
                $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
                if ($journal.gameRoot -ne $script:Root) { throw "Backup belongs to another game folder: $journalPath" }
                if ($journal.state -notin @('complete', 'restored')) {
                    throw "Recover the interrupted installation with Install-Dawn.ps1 -Restore -BackupPath `"$($backup.FullName)`" first."
                }
                if ($journal.state -ne 'complete') { continue }
                $candidates += Join-Path $backup.FullName ('previous/' + $Relative)
            } else { $candidates += Join-Path $backup.FullName $Relative }
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-SteamDll $candidate) { return $candidate }
    }
    return $null
}

function Get-UninstallPlan {
    $state = Get-SafePath '.dawn'
    if (Test-Path -LiteralPath $state) {
        Assert-PlainTree $state
        $history = Get-SafePath '.dawn/release-backups'
        if (Test-Path -LiteralPath $history) {
            foreach ($directory in Get-ChildItem -LiteralPath $history -Directory) {
                $journalPath = Join-Path $directory.FullName 'journal.json'
                if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) { continue }
                $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
                if ($journal.gameRoot -ne $script:Root -or $journal.state -notin @('complete', 'restored')) {
                    throw "Recover the interrupted or mismatched installation at $($directory.FullName) before uninstalling."
                }
            }
        }
        if ($PSScriptRoot -eq $state -or $PSScriptRoot.StartsWith($state + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Run the uninstaller from outside the .dawn folder.'
        }
    }
    $plan = @()
    foreach ($relative in @('steam_api64.dll', 'bin/x64/steam_api64.dll', 'Dawn', 'bin/x64/Dawn', '.dawn/release.json', '.dawn/install-state.json')) {
        $target = Get-SafePath $relative
        if (-not (Test-Path -LiteralPath $target)) { continue }
        $original = $null
        if ($relative.EndsWith('.dll')) {
            if ((Get-Item -LiteralPath $target).VersionInfo.ProductName -ne 'Dawn') {
                Write-Host "Keeping non-Dawn DLL: $relative"
                continue
            }
            $original = Find-OriginalDll $relative
            if ($original) { Write-Host "Restore $relative from $original" }
            else { Write-Warning "No original Steam DLL found for $relative. Dawn will be removed; restore this DLL from your original game backup before launching the game." }
        } elseif ($relative -in @('Dawn', 'bin/x64/Dawn')) {
            if (-not (Test-Path -LiteralPath $target -PathType Container)) { throw "Expected a runtime folder: $target" }
            if ((Test-Path -LiteralPath (Join-Path $target 'src')) -or (Test-Path -LiteralPath (Join-Path $target 'Dawn.vcxproj'))) {
                throw "A source checkout occupies $target. Use a separate game installation."
            }
            if ($PSScriptRoot -eq $target -or $PSScriptRoot.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Run the uninstaller from outside the Dawn runtime folders.'
            }
            Assert-PlainTree $target
        }
        $plan += [pscustomobject]@{ target = $relative; original = $original; saved = $false; placing = $false }
        if ($relative.EndsWith('.dll')) {
            $symbols = [IO.Path]::ChangeExtension($relative, '.pdb')
            if (Test-Path -LiteralPath (Get-SafePath $symbols) -PathType Leaf) {
                $plan += [pscustomobject]@{ target = $symbols; original = $null; saved = $false; placing = $false }
            }
        }
    }
    return $plan
}

function Move-WithinGame([string] $From, [string] $To) {
    # Both resolved paths must remain inside this explicitly selected game folder.
    foreach ($path in @($From, $To)) {
        $resolved = [IO.Path]::GetFullPath($path)
        if (-not $resolved.StartsWith($script:Root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe move: $resolved" }
        Assert-PlainPath $resolved
    }
    if (Test-Path -LiteralPath $To) { throw "Backup destination already exists: $To" }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($To)) | Out-Null
    Move-Item -LiteralPath $From -Destination $To
}

if (-not $GameRoot) { $GameRoot = (Read-Host 'Game folder (the folder containing destiny2.exe)').Trim().Trim('"') }
$script:Root = [IO.Path]::GetFullPath($GameRoot).TrimEnd('\', '/')
Assert-PlainPath $script:Root
if (-not (Test-Path -LiteralPath (Get-SafePath 'destiny2.exe') -PathType Leaf)) { throw "No destiny2.exe in $script:Root" }
Assert-GameClosed
if ($OriginalDll) {
    $OriginalDll = [IO.Path]::GetFullPath($OriginalDll)
    if (-not (Test-SteamDll $OriginalDll)) { throw '-OriginalDll must be an original x64 Steam Client API DLL.' }
}
$plan = @(Get-UninstallPlan)
if (-not $plan.Count -and -not (Test-Path -LiteralPath (Get-SafePath '.dawn'))) { Write-Host 'No Dawn installation found. Nothing changed.'; return }
Write-Host 'This permanently deletes all Dawn saves, settings, caches, and installer backups.'
if (-not $PSCmdlet.ShouldProcess($script:Root, 'Permanently delete Dawn, ALL saves/settings/caches and .dawn backups; restore available original Steam DLLs')) { return }

$stateDirectory = Get-SafePath '.dawn'
[IO.Directory]::CreateDirectory($stateDirectory) | Out-Null
$lock = [IO.File]::Open((Get-SafePath '.dawn/release-install.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    Assert-GameClosed
    $plan = @(Get-UninstallPlan)
    $backup = Get-SafePath ('.dawn/uninstall-backups/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    $journalPath = Join-Path $backup 'uninstall.json'
    $journal = [pscustomobject]@{ schema = 1; gameRoot = $script:Root; state = 'prepared'; operations = $plan }
    function Save-Journal { $journal | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $journalPath -Encoding UTF8 }
    # Stage and verify originals before moving any active files, even if supplied from a runtime folder.
    foreach ($operation in $plan) {
        if (-not $operation.original) { continue }
        $staged = Join-Path $backup ('originals/' + $operation.target)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($staged)) | Out-Null
        Copy-Item -LiteralPath $operation.original -Destination $staged
        if ((Get-FileHash -LiteralPath $staged).Hash -ne (Get-FileHash -LiteralPath $operation.original).Hash) { throw "Original DLL copy failed: $staged" }
    }
    Save-Journal
    try {
        $journal.state = 'uninstalling'
        Save-Journal
        foreach ($operation in $plan) {
            Assert-GameClosed
            $target = Get-SafePath $operation.target
            Move-WithinGame $target (Join-Path $backup ('previous/' + $operation.target))
            $operation.saved = $true
            Save-Journal
            if ($operation.original) {
                $operation.placing = $true
                Save-Journal
                $staged = Join-Path $backup ('originals/' + $operation.target)
                Copy-Item -LiteralPath $staged -Destination $target
                if ((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $staged).Hash) { throw "DLL restoration failed: $target" }
            }
        }
        $journal.state = 'complete'
        Save-Journal
    } catch {
        $failure = $_.Exception.Message
        try {
            $reverse = @($plan)
            [array]::Reverse($reverse)
            foreach ($operation in $reverse) {
                if (-not $operation.saved) { continue }
                $target = Get-SafePath $operation.target
                if ($operation.placing -and (Test-Path -LiteralPath $target)) {
                    Move-WithinGame $target (Join-Path $backup ('failed-restore/' + $operation.target))
                }
                Move-WithinGame (Join-Path $backup ('previous/' + $operation.target)) $target
            }
            $journal.state = 'rolled-back'
            Save-Journal
        } catch { throw "Uninstall failed and rollback needs manual recovery. Keep all files in $backup. Error: $failure. Recovery error: $($_.Exception.Message)" }
        throw "Uninstall failed; previous files were restored. $failure"
    }
} finally { $lock.Dispose() }

# Originals have been restored successfully. Remove the temporary rollback files
# together with ALL older Dawn backups. Resolve and check the exact deletion root.
Assert-GameClosed
$purge = Get-SafePath '.dawn'
Assert-PlainTree $purge
try { Remove-Item -LiteralPath $purge -Recurse -Force }
catch { throw "Dawn was removed, but cleanup of $purge did not finish. Re-run the uninstaller to delete the remaining backups. $($_.Exception.Message)" }
Write-Host 'Dawn uninstalled. All Dawn saves, settings, caches, and backups were deleted.'
if (@($plan | Where-Object { $_.target.EndsWith('.dll') -and -not $_.original }).Count) {
    Write-Warning 'Restore the missing steam_api64.dll file(s) from your original game backup before launching.'
}
