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
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Dol)) { throw "main.dol not found: $Dol (tools/ios/extract_dol.py copies it out of an ISO)" }
$vis = gh repo view $Repo --json visibility -q .visibility
if ($vis -ne 'PRIVATE') { throw "$Repo is $vis. Game files and builds go only into a private repository." }

$sha = (Get-FileHash -Algorithm SHA1 $Dol).Hash.ToLower()
Write-Host "main.dol SHA-1 $sha ($((Get-Item $Dol).Length) bytes)"

$exists = $true
try { gh release view game-data --repo $Repo --json tagName | Out-Null } catch { $exists = $false }
if ($LASTEXITCODE -ne 0) { $exists = $false }
if (-not $exists) {
    gh release create game-data --repo $Repo --title "Game data (private)" `
        --notes "Your own main.dol for the iOS build. Never make this repository public."
}
$notes = gh release view game-data --repo $Repo --json body -q .body
if ($notes -match "sha1: $sha") {
    Write-Host "game-data already has this main.dol; nothing to upload."
    exit 0
}
gh release upload game-data $Dol --repo $Repo --clobber
gh release edit game-data --repo $Repo --notes "Your own main.dol for the iOS build. Never make this repository public.`nsha1: $sha"
Write-Host "Uploaded main.dol to $Repo (release game-data)."
