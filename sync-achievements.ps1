# sync-achievements.ps1
# Fetches latest netplay_canary_experimental from AdrianCassar upstream, merges
# into netplay-achievements, syncs/updates submodules, builds Xenia, pushes to
# fork, then deploys the exe.
# Run from the xenia-canary-netplay repo root.
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

$UpstreamRemote = 'upstream'
$ForkRemote = 'origin'

$UpstreamBranch = 'netplay_canary_experimental'
$LocalBranch = 'netplay-achievements'

$BuildCommand = 'uv'
$BuildArguments = @(
    'run',
    'xenia-build.py',
    'build',
    '--config',
    'Release'
)

# The netplay build may produce either name depending on project config; prefer
# the netplay-specific artifact, then fall back to the generic canary name.
$BuildArtifactCandidates = @(
    'build\bin\Windows\Release\xenia_canary_netplay.exe',
    'build\bin\Windows\Release\xenia_canary.exe'
)
$DeployDestination = 'E:\xbox360\Emulators\Xenia Netplay'

$SkipBuildWhenAlreadyUpToDate = $true

# Local changes that are allowed to exist while running this script.
$AllowedDirtyPaths = @(
    'sync-achievements.ps1',
    'third_party/fmt'
)

# Old submodules from other branches that may still exist as stale gitlinks in
# the index but no longer exist in .gitmodules. In the netplay variant,
# libcurl/miniupnp/wolfssl are REAL submodules, so this list is empty here.
$StaleSubmodulePathsToAutoRemove = @()

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
        [AllowEmptyCollection()]
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

function Get-BuildArtifact {
    foreach ($candidate in $BuildArtifactCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

# =============================================================================
# Script
# =============================================================================

Write-Host "Fetching latest from $UpstreamRemote..." -ForegroundColor Cyan
# --force lets moved upstream tags (e.g. 'experimental') update instead of
# aborting the fetch with "would clobber existing tag".
Invoke-Git @('fetch', $UpstreamRemote, '--prune', '--tags', '--force')

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

Write-Host "Building Xenia netplay..." -ForegroundColor Cyan
& $BuildCommand @BuildArguments

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

$BuildArtifact = Get-BuildArtifact

if (-not $BuildArtifact) {
    Write-Host "Build artifact not found. Tried: $($BuildArtifactCandidates -join ', ')" -ForegroundColor Red
    exit 1
}

Write-Host "Pushing $LocalBranch to $ForkRemote..." -ForegroundColor Cyan
Invoke-Git @('push', $ForkRemote, $LocalBranch)

if (-not (Test-Path $DeployDestination)) {
    Write-Host "Creating deploy folder: $DeployDestination" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $DeployDestination -Force | Out-Null
}

Write-Host "Copying $BuildArtifact to $DeployDestination..." -ForegroundColor Cyan
Copy-Item $BuildArtifact $DeployDestination -Force

Write-Host "Done. Netplay build deployed to $DeployDestination." -ForegroundColor Green
