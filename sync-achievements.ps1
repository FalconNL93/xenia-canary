# sync-achievements.ps1
# Fetches latest canary_experimental from upstream, merges into achievements,
# syncs/updates submodules, builds Xenia, pushes to fork, then deploys the exe.
# Run from the xenia-canary repo root.
#
# Usage:
#   .\sync-achievements.ps1
#   .\sync-achievements.ps1 -Force

param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# =============================================================================
# Configuration
# =============================================================================

$UpstreamRemote = 'origin'
$ForkRemote = 'myfork'

$UpstreamBranch = 'canary_experimental'
$LocalBranch = 'achievements'

$BuildCommand = 'uv'
$BuildArguments = @(
    'run',
    'xenia-build.py',
    'build',
    '--config',
    'Release'
)

$BuildArtifact = 'build\bin\Windows\Release\xenia_canary.exe'
$DeployDestination = 'E:\xbox360\Emulators\Xenia Canary'

$SkipBuildWhenAlreadyUpToDate = $true

# Local changes that are allowed to exist while running this script.
$AllowedDirtyPaths = @(
    'sync-achievements.ps1',
    'third_party/fmt'
)

# Old submodules from other branches, such as netplay, that may still exist
# as stale gitlinks in the index but no longer exist in .gitmodules.
$StaleSubmodulePathsToAutoRemove = @(
    'third_party/libcurl',
    'third_party/miniupnp',
    'third_party/wolfssl'
)

# =============================================================================
# Helpers
# =============================================================================

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    git @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Git command failed: git $($Arguments -join ' ')"
    }
}

function Get-GitOutput {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = git @Arguments 2>$null

    if ($LASTEXITCODE -ne 0) {
        return @()
    }

    return @($output)
}

function Get-GitModulesPaths {
    $paths = @()
    $lines = Get-GitOutput @('--no-pager', 'config', '--file', '.gitmodules', '--get-regexp', 'path')

    foreach ($line in $lines) {
        $parts = $line -split '\s+', 2

        if ($parts.Count -eq 2) {
            $paths += $parts[1].Trim()
        }
    }

    return $paths
}

function Remove-StaleSubmoduleReferences {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AllowedStalePaths
    )

    $gitModulesPaths = Get-GitModulesPaths
    $removedAny = $false

    foreach ($path in $AllowedStalePaths) {
        $indexLines = Get-GitOutput @('--no-pager', 'ls-files', '--stage', $path)

        if (-not $indexLines) {
            continue
        }

        $isGitLink = $false

        foreach ($indexLine in $indexLines) {
            if ($indexLine -match '^160000\s+') {
                $isGitLink = $true
                break
            }
        }

        $existsInGitModules = $gitModulesPaths -contains $path

        if ($isGitLink -and -not $existsInGitModules) {
            Write-Host "Removing stale submodule reference: $path" -ForegroundColor Yellow

            Invoke-Git @('rm', '--cached', $path)

            if (Test-Path $path) {
                Remove-Item -Recurse -Force $path -ErrorAction SilentlyContinue
            }

            $moduleCachePath = Join-Path (Get-Location) ".git\modules\$path"

            if (Test-Path $moduleCachePath) {
                Remove-Item -Recurse -Force $moduleCachePath -ErrorAction SilentlyContinue
            }

            $removedAny = $true
        }
    }

    return $removedAny
}

function Sync-And-Update-Submodules {
    Write-Host "Synchronizing submodule metadata..." -ForegroundColor Cyan
    Invoke-Git @('submodule', 'sync', '--recursive')

    Write-Host "Checking for stale submodule references..." -ForegroundColor Cyan
    $removedStaleSubmodules = Remove-StaleSubmoduleReferences -AllowedStalePaths $StaleSubmodulePathsToAutoRemove

    if ($removedStaleSubmodules) {
        Write-Host "Stale submodule references were removed from the index." -ForegroundColor Yellow
        Write-Host "These removals will be committed with the normal sync commit/push flow." -ForegroundColor Yellow

        Write-Host "Synchronizing submodule metadata again after cleanup..." -ForegroundColor Cyan
        Invoke-Git @('submodule', 'sync', '--recursive')
    }

    Write-Host "Updating submodules..." -ForegroundColor Cyan
    Invoke-Git @('submodule', 'update', '--init', '--recursive')
}

function Get-StatusPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$StatusLine
    )

    $path = $StatusLine.Substring(3).Trim()

    if ($path.Contains(' -> ')) {
        $path = ($path -split ' -> ', 2)[1].Trim()
    }

    return $path
}

function Test-IsAllowedDirtyPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    foreach ($allowedPath in $AllowedDirtyPaths) {
        if ($Path -eq $allowedPath -or $Path.StartsWith("$allowedPath/")) {
            return $true
        }
    }

    return $false
}

# =============================================================================
# Script
# =============================================================================

Write-Host "Fetching latest from $UpstreamRemote..." -ForegroundColor Cyan
Invoke-Git @('fetch', $UpstreamRemote, '--prune', '--tags')

$currentBranch = git rev-parse --abbrev-ref HEAD

if ($currentBranch -ne $LocalBranch) {
    Write-Host "Switching to $LocalBranch..." -ForegroundColor Cyan
    Invoke-Git @('checkout', $LocalBranch)
}

$beforeHead = git rev-parse HEAD

Write-Host "Merging $UpstreamRemote/$UpstreamBranch into $LocalBranch..." -ForegroundColor Cyan

try {
    Invoke-Git @('merge', "$UpstreamRemote/$UpstreamBranch", '--no-edit')
}
catch {
    Write-Host "Merge conflict! Resolve conflicts, then run:" -ForegroundColor Red
    Write-Host "  git merge --continue" -ForegroundColor Yellow
    Write-Host "  .\sync-achievements.ps1 -Force" -ForegroundColor Yellow
    exit 1
}

$newHead = git rev-parse HEAD

try {
    Sync-And-Update-Submodules
}
catch {
    Write-Host "Submodule sync/update failed!" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Yellow
    exit 1
}

$statusLines = @(git status --short)
$unexpectedStatusLines = @()

foreach ($line in $statusLines) {
    $path = Get-StatusPath -StatusLine $line

    if (-not (Test-IsAllowedDirtyPath -Path $path)) {
        $unexpectedStatusLines += $line
    }
}

if ($unexpectedStatusLines.Count -gt 0) {
    Write-Host "Working tree has unexpected changes:" -ForegroundColor Red
    Write-Host ($unexpectedStatusLines -join "`n") -ForegroundColor Yellow
    exit 1
}

if ($statusLines) {
    Write-Host "Ignoring allowed local/submodule changes:" -ForegroundColor Yellow
    Write-Host ($statusLines -join "`n") -ForegroundColor DarkYellow
}

if ($SkipBuildWhenAlreadyUpToDate -and -not $Force -and $newHead -eq $beforeHead) {
    Write-Host "Already up to date - skipping build, push, and deploy." -ForegroundColor Yellow
    Write-Host "Use -Force to build anyway." -ForegroundColor Yellow
    exit 0
}

if ($Force) {
    Write-Host "Force enabled - building even if already up to date." -ForegroundColor Yellow
}

Write-Host "Building Xenia..." -ForegroundColor Cyan
& $BuildCommand @BuildArguments

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $BuildArtifact)) {
    Write-Host "Build artifact not found at $BuildArtifact" -ForegroundColor Red
    exit 1
}

Write-Host "Pushing $LocalBranch to $ForkRemote..." -ForegroundColor Cyan
Invoke-Git @('push', $ForkRemote, $LocalBranch)

if (-not (Test-Path $DeployDestination)) {
    Write-Host "Deploy destination does not exist: $DeployDestination" -ForegroundColor Red
    exit 1
}

Write-Host "Copying $BuildArtifact to $DeployDestination..." -ForegroundColor Cyan
Copy-Item $BuildArtifact $DeployDestination -Force

Write-Host "Done. Xenia deployed to $DeployDestination." -ForegroundColor Green