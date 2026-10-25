<#
.SYNOPSIS
  One command for an iPhone build: pushes the current branch to the build repository (source only;
  may be public, where GitHub's Mac minutes are free), runs its "iOS IPA" workflow, waits, and
  downloads the IPA from the private data repository into build/ios-ipa.

  Setup once (docs/IOS.md "Building the IPA"): the data repository holds your main.dol
  (tools/ios/publish-dol.ps1); the build repository has the variable GAME_DATA_REPO and the secret
  GAME_DATA_TOKEN.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/ios/build-ipa.ps1 -Repo you/lm-ios -DataRepo you/lm-ios-data
  powershell -ExecutionPolicy Bypass -File tools/ios/build-ipa.ps1 -Repo you/lm-ios -DataRepo you/lm-ios-data -Version GLMP01 -NoPush
#>
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][string]$DataRepo,
    [ValidateSet('GLME01', 'GLMP01')][string]$Version = 'GLME01',
    [string]$Remote = 'ios',
    [switch]$NoPush
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Set-Location $root

$vis = gh repo view $DataRepo --json visibility -q .visibility
if ($vis -ne 'PRIVATE') { throw "$DataRepo is $vis. It holds your main.dol and the IPA: it must be private." }
$var = gh variable get GAME_DATA_REPO --repo $Repo 2>$null
if ($var -ne $DataRepo) { throw "Set the variable first: gh variable set GAME_DATA_REPO --repo $Repo --body $DataRepo" }
if (-not (gh secret list --repo $Repo | Select-String -SimpleMatch -Quiet 'GAME_DATA_TOKEN')) {
    throw "Set the secret GAME_DATA_TOKEN in $Repo (a fine-grained token with Contents read/write on $DataRepo only; docs/IOS.md)"
}

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
if (-not $NoPush) {
    if (-not (git remote | Select-String -SimpleMatch -Quiet $Remote)) {
        git remote add $Remote "https://github.com/$Repo.git"
    }
    Write-Host "Pushing $branch to $Repo ..."
    git push $Remote "${branch}:${branch}"
    if ($LASTEXITCODE -ne 0) { throw "git push failed" }
}

$started = (Get-Date).ToUniversalTime()
gh workflow run ios.yml --repo $Repo --ref $branch -f dol_version=$Version
if ($LASTEXITCODE -ne 0) { throw "could not start the workflow (is .github/workflows/ios.yml on $branch?)" }

# The new run: the newest workflow_dispatch run on this branch created after it was started.
$run = $null
for ($i = 0; $i -lt 30 -and -not $run; $i++) {
    Start-Sleep -Seconds 4
    $runs = gh run list --repo $Repo --workflow ios.yml --branch $branch --event workflow_dispatch --limit 5 `
        --json databaseId,number,createdAt | ConvertFrom-Json
    $run = $runs | Where-Object { ([datetime]$_.createdAt).ToUniversalTime() -ge $started.AddSeconds(-5) } | Select-Object -First 1
}
if (-not $run) { throw "the workflow run did not appear; check https://github.com/$Repo/actions" }
Write-Host "Run $($run.number): https://github.com/$Repo/actions/runs/$($run.databaseId)"

gh run watch $run.databaseId --repo $Repo --interval 30 --exit-status
$ok = $LASTEXITCODE -eq 0
$out = Join-Path $root "build/ios-ipa/ios-$($run.number)"
New-Item -ItemType Directory -Force $out | Out-Null
gh release download "ios-$($run.number)" --repo $DataRepo --dir $out --clobber 2>$null
if ($ok) {
    Get-ChildItem $out -Filter *.ipa | ForEach-Object { Write-Host "IPA: $($_.FullName) ($([math]::Round($_.Length / 1MB)) MB)" }
    Write-Host "On the phone: the release ios-$($run.number) in https://github.com/$DataRepo/releases"
} else {
    Write-Host "The build failed. Public log tail:"
    gh run view $run.databaseId --repo $Repo --log-failed | Select-Object -Last 80
    $log = Get-ChildItem $out -Filter build-*.log | Select-Object -First 1
    if ($log) { Write-Host "Full build log (private): $($log.FullName)" }
    exit 1
}
