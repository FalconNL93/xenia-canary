# sync-achievements.ps1
# Syncs AdrianCassar netplay upstream into your local netplay-achievements branch.
# Run from the xenia-canary-netplay repo root.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$upstreamRemote = 'upstream'
$upstreamBranch = 'netplay_canary_experimental'
$myBranch = 'netplay-achievements'
$pushRemote = 'origin'

$buildConfig = 'Release'

$artifactCandidates = @(
    "build\bin\Windows\$buildConfig\xenia_canary_netplay.exe",
    "build\bin\Windows\$buildConfig\xenia_canary.exe"
)

$deployDest = "E:\xbox360\Emulators\Xenia Netplay"

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]] $Arguments
    )

    git @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed."
    }
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Message,

        [Parameter(Mandatory = $true)]
        [scriptblock] $Action
    )

    Write-Host $Message -ForegroundColor Cyan
    & $Action
}

function Get-BuildArtifact {
    foreach ($candidate in $artifactCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Build artifact not found. Tried: $($artifactCandidates -join ', ')"
}

Invoke-Step "Fetching latest from $upstreamRemote..." {
    Invoke-Git @('fetch', $upstreamRemote)
}

$currentBranch = git rev-parse --abbrev-ref HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not determine current branch."
}

if ($currentBranch -ne $myBranch) {
    Invoke-Step "Switching to $myBranch..." {
        Invoke-Git @('checkout', $myBranch)
    }
}

$beforeHead = git rev-parse HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not determine HEAD before merge."
}

Invoke-Step "Merging $upstreamRemote/$upstreamBranch into $myBranch..." {
    git merge "$upstreamRemote/$upstreamBranch" --no-edit

    if ($LASTEXITCODE -ne 0) {
        Write-Host "Merge conflict!" -ForegroundColor Red
        Write-Host "Resolve conflicts, then run:" -ForegroundColor Yellow
        Write-Host "  git add <resolved-files>" -ForegroundColor Yellow
        Write-Host "  git commit" -ForegroundColor Yellow
        Write-Host "  git push $pushRemote $myBranch" -ForegroundColor Yellow
        exit 1
    }
}

Invoke-Step "Pushing $myBranch to $pushRemote..." {
    Invoke-Git @('push', $pushRemote, $myBranch)
}

$newHead = git rev-parse HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not determine HEAD after merge."
}

if ($newHead -eq $beforeHead) {
    Write-Host "No new upstream commits merged - skipping build." -ForegroundColor Yellow
    exit 0
}

Invoke-Step "New commits merged, building Xenia netplay..." {
    uv run xenia-build.py build --config $buildConfig

    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
}

$artifact = Get-BuildArtifact

if (-not (Test-Path -LiteralPath $deployDest)) {
    Write-Host "Creating deploy folder: $deployDest" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $deployDest -Force | Out-Null
}

Invoke-Step "Copying $artifact to $deployDest..." {
    Copy-Item -LiteralPath $artifact -Destination $deployDest -Force
}

Write-Host "Done. Netplay build deployed to $deployDest." -ForegroundColor Green