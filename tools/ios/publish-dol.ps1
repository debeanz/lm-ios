<#
.SYNOPSIS
  Uploads your own main.dol to the PRIVATE data repository's "game-data" release, where the iOS
  workflow (.github/workflows/ios.yml, running in the build repository) fetches it with the
  GAME_DATA_TOKEN secret. Skips the upload when the same file is already there.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/ios/publish-dol.ps1 -Dol gamedata/main.dol -Repo you/lm-ios-data
#>
param(
    [Parameter(Mandatory = $true)][string]$Dol,
    [Parameter(Mandatory = $true)][string]$Repo
)
# Not 'Stop': Windows PowerShell 5.1 turns any stderr line of gh (e.g. "release not found", an
# expected answer) into a terminating error. Failures are checked with $LASTEXITCODE (Check).
$ErrorActionPreference = 'Continue'

if (-not (Test-Path $Dol)) { throw "main.dol not found: $Dol (tools/ios/extract_dol.py copies it out of an ISO)" }
$vis = gh repo view $Repo --json visibility -q .visibility
if ($vis -ne 'PRIVATE') { throw "$Repo is $vis. Game files and builds go only into a private repository." }

$sha = (Get-FileHash -Algorithm SHA1 $Dol).Hash.ToLower()
Write-Host "main.dol SHA-1 $sha ($((Get-Item $Dol).Length) bytes)"
function Check([string]$what) { if ($LASTEXITCODE -ne 0) { throw "$what failed" } }

# A release needs a commit to tag: give a new, empty data repository a README first.
$branches = gh api "repos/$Repo/branches" --jq 'length'
Check "reading the repository's branches"
if ($branches -eq '0') {
    $readme = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(
        "# Private iOS build data`n`nYour own main.dol (release game-data) and the iOS builds (releases ios-N). Keep this repository private.`n"))
    gh api -X PUT "repos/$Repo/contents/README.md" -f message="Private iOS build data" -f content=$readme | Out-Null
    Check "creating the first commit"
}

gh release view game-data --repo $Repo --json tagName 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    gh release create game-data --repo $Repo --title "Game data (private)" `
        --notes "Your own main.dol for the iOS build. Never make this repository public."
    Check "creating the game-data release"
}
$notes = gh release view game-data --repo $Repo --json body -q .body
if ($notes -match "sha1: $sha") {
    Write-Host "game-data already has this main.dol; nothing to upload."
    exit 0
}
gh release upload game-data $Dol --repo $Repo --clobber
Check "uploading main.dol"
gh release edit game-data --repo $Repo --notes "Your own main.dol for the iOS build. Never make this repository public.`nsha1: $sha"
Check "updating the release notes"
Write-Host "Uploaded main.dol to $Repo (release game-data)."
