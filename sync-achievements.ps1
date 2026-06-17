# sync-achievements.ps1
# Fetches latest canary_experimental from upstream, merges into achievements,
# updates submodules, builds Xenia, pushes to fork, then deploys the exe.
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

$AllowedDirtyPaths = @(
    'sync-achievements.ps1',
    'third_party/fmt',
    'third_party/libcurl',
    'third_party/miniupnp',
    'third_party/wolfssl'
)

# =============================================================================
# Script
# =============================================================================

Write-Host "Fetching latest from $UpstreamRemote..." -ForegroundColor Cyan
git fetch $UpstreamRemote --prune --tags

if ($LASTEXITCODE -ne 0) {
    Write-Host "Fetch failed!" -ForegroundColor Red
    exit 1
}

$currentBranch = git rev-parse --abbrev-ref HEAD

if ($currentBranch -ne $LocalBranch) {
    Write-Host "Switching to $LocalBranch..." -ForegroundColor Cyan
    git checkout $LocalBranch

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Checkout failed!" -ForegroundColor Red
        exit 1
    }
}

$beforeHead = git rev-parse HEAD

Write-Host "Merging $UpstreamRemote/$UpstreamBranch into $LocalBranch..." -ForegroundColor Cyan
git merge "$UpstreamRemote/$UpstreamBranch" --no-edit

if ($LASTEXITCODE -ne 0) {
    Write-Host "Merge conflict! Resolve conflicts, then run:" -ForegroundColor Red
    Write-Host "  git merge --continue" -ForegroundColor Yellow
    Write-Host "  git submodule update --init --recursive" -ForegroundColor Yellow
    Write-Host "  .\sync-achievements.ps1 -Force" -ForegroundColor Yellow
    exit 1
}

$newHead = git rev-parse HEAD

Write-Host "Updating submodules..." -ForegroundColor Cyan
git submodule update --init --recursive

if ($LASTEXITCODE -ne 0) {
    Write-Host "Submodule update failed!" -ForegroundColor Red
    exit 1
}

$statusLines = git status --short
$unexpectedStatusLines = @()

foreach ($line in $statusLines) {
    $path = $line.Substring(3).Trim()
    $isAllowed = $false

    foreach ($allowedPath in $AllowedDirtyPaths) {
        if ($path -eq $allowedPath -or $path.StartsWith("$allowedPath/")) {
            $isAllowed = $true
            break
        }
    }

    if (-not $isAllowed) {
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
    Write-Host "Already up to date — skipping build, push, and deploy." -ForegroundColor Yellow
    Write-Host "Use -Force to build anyway." -ForegroundColor Yellow
    exit 0
}

if ($Force) {
    Write-Host "Force enabled — building even if already up to date." -ForegroundColor Yellow
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
git push $ForkRemote $LocalBranch

if ($LASTEXITCODE -ne 0) {
    Write-Host "Push failed!" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $DeployDestination)) {
    Write-Host "Deploy destination does not exist: $DeployDestination" -ForegroundColor Red
    exit 1
}

Write-Host "Copying $BuildArtifact to $DeployDestination..." -ForegroundColor Cyan
Copy-Item $BuildArtifact $DeployDestination -Force

Write-Host "Done. Xenia deployed to $DeployDestination." -ForegroundColor Green