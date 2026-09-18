@echo off
setlocal
set "DAWN_CACHE_SCRIPT=%~f0"
set "DAWN_CACHE_GAME_ROOT=%~1"
set "DAWN_CACHE_PREVIEW=%~2"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$body = Get-Content -LiteralPath $env:DAWN_CACHE_SCRIPT -Raw; & ([scriptblock]::Create($body.Substring($body.LastIndexOf('# POWERSHELL-BEGIN') + 18)))"
set "DAWN_CACHE_EXIT=%ERRORLEVEL%"
if not "%DAWN_CACHE_NO_PAUSE%"=="1" pause
exit /b %DAWN_CACHE_EXIT%
# POWERSHELL-BEGIN
$ErrorActionPreference = 'Stop'
try {
    $gameRoot = $env:DAWN_CACHE_GAME_ROOT
    if ([string]::IsNullOrWhiteSpace($gameRoot)) {
        $besideScript = Split-Path -Parent $env:DAWN_CACHE_SCRIPT
        if (Test-Path -LiteralPath (Join-Path $besideScript 'destiny2.exe') -PathType Leaf) {
            $gameRoot = $besideScript
        } else {
            $gameRoot = (Read-Host 'Game folder containing destiny2.exe').Trim().Trim('"')
        }
    }
    if ([string]::IsNullOrWhiteSpace($gameRoot)) { throw 'No game folder supplied.' }
    $root = (Resolve-Path -LiteralPath $gameRoot).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $root 'destiny2.exe') -PathType Leaf)) {
        throw 'This folder does not contain destiny2.exe. Nothing was deleted.'
    }
    if (@(Get-Process -Name destiny2 -ErrorAction SilentlyContinue).Count) {
        throw 'Close Destiny 2 before clearing its cache. Nothing was deleted.'
    }
    $preview = $env:DAWN_CACHE_PREVIEW -eq '/preview'
    if ($env:DAWN_CACHE_PREVIEW -and -not $preview) { throw 'Optional second argument must be /preview.' }

    function Assert-PlainPath([string] $path) {
        $current = $path
        while ($current) {
            if (Test-Path -LiteralPath $current) {
                $item = Get-Item -LiteralPath $current -Force
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Refusing a linked/junction path: $current. Nothing was deleted."
                }
            }
            $parent = Split-Path -Parent $current
            if ($parent -eq $current) { break }
            $current = $parent
        }
    }
    Assert-PlainPath $root
    $targets = @()
    foreach ($relative in @('Dawn\cache', 'bin\x64\Dawn\cache')) {
        $target = [IO.Path]::GetFullPath((Join-Path $root $relative))
        $boundary = $root.TrimEnd('\') + '\'
        if (-not $target.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Cache target escapes the selected game folder. Nothing was deleted.'
        }
        Assert-PlainPath $target
        if (Test-Path -LiteralPath $target) {
            if (-not (Test-Path -LiteralPath $target -PathType Container)) {
                throw "Expected a cache directory: $target. Nothing was deleted."
            }
            $descendants = @(Get-ChildItem -LiteralPath $target -Force -Recurse)
            if (@($descendants | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
                throw "Cache contains a link/junction: $target. Nothing was deleted."
            }
            $targets += $target
        } else {
            Write-Host "No cache found: $target"
        }
    }
    # Validate every target before deleting anything. Keep the cache directories themselves.
    foreach ($target in $targets) {
        if ($preview) {
            Write-Host "PREVIEW - would clear contents of: $target"
        } else {
            Get-ChildItem -LiteralPath $target -Force | ForEach-Object {
                Remove-Item -LiteralPath $_.FullName -Recurse -Force
            }
            Write-Host "Cleared: $target"
        }
    }
    Write-Host 'Saves, settings, scripts, DLLs, and backups are untouched.'
    Write-Host 'Dawn will rebuild its caches on the next launch; that launch may take longer.'
    exit 0
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
