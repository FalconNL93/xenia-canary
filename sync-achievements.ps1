# sync-achievements.ps1
# Fetches latest canary_experimental from origin and merges into achievements branch.
# Run from the xenia-canary repo root.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$upstream_branch = 'canary_experimental'
$my_branch = 'achievements'
$remote = 'origin'

Write-Host "Fetching latest from $remote..." -ForegroundColor Cyan
git fetch $remote

$current = git rev-parse --abbrev-ref HEAD
if ($current -ne $my_branch) {
    Write-Host "Switching to $my_branch..." -ForegroundColor Cyan
    git checkout $my_branch
}

$before_head = git rev-parse HEAD

Write-Host "Merging $remote/$upstream_branch into $my_branch..." -ForegroundColor Cyan
git merge "$remote/$upstream_branch" --no-edit

if ($LASTEXITCODE -ne 0) {
    Write-Host "Merge conflict! Resolve conflicts, then run:" -ForegroundColor Red
    Write-Host "  git merge --continue" -ForegroundColor Yellow
    Write-Host "  git push myfork $my_branch" -ForegroundColor Yellow
    exit 1
}

Write-Host "Pushing to myfork/$my_branch..." -ForegroundColor Cyan
git push myfork $my_branch

Write-Host "Done. $my_branch is up to date with $remote/$upstream_branch." -ForegroundColor Green

# Check if HEAD actually moved (i.e. new commits were merged)
$new_head = git rev-parse HEAD
if ($new_head -eq $before_head) {
    Write-Host "Already up to date — skipping build." -ForegroundColor Yellow
    exit 0
}

Write-Host "New commits merged, building xenia..." -ForegroundColor Cyan
uv run xenia-build.py build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

$exe = "build\bin\Windows\Debug\xenia_canary.exe"
$dest = "E:\xbox360\Emulators\Xenia-Achievements"

if (-not (Test-Path $exe)) {
    Write-Host "Build artifact not found at $exe" -ForegroundColor Red
    exit 1
}

Write-Host "Copying $exe to $dest..." -ForegroundColor Cyan
Copy-Item $exe $dest -Force
Write-Host "Done. Xenia deployed to $dest." -ForegroundColor Green
