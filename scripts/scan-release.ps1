<#
.SYNOPSIS
  Uploads a release's files to VirusTotal and runs them in Hybrid Analysis' sandbox, then writes a
  report and, with -UpdateNotes, links the reports from the release notes (F1, docs/f1-plan.md).

.EXAMPLE
  scripts\scan-release.ps1 -Tag v4.1.0 -UpdateNotes          # the release's assets (a draft too), from GitHub
  scripts\scan-release.ps1 -Run 1234567890                    # a workflow run's artifacts (release.yml's dry run)
  scripts\scan-release.ps1 -Files dist\*.exe,dist\*.zip       # local files
  scripts\scan-release.ps1 -FromCi                            # the gate: the CI scan of this commit, fetched
  scripts\scan-release.ps1 -Files x.exe -Offline tests\scan   # canned answers, no network (a test)

.NOTES
  The keys come from the environment: VT_API_KEY (VirusTotal) and HA_API_KEY (Hybrid Analysis), set
  as repository secrets by Roy (step 5). Without them the report says "waiting for step 5" and the
  script exits 0. The scan workflow (.github/workflows/scan-release.yml) runs it in CI, where the keys are.

  Passes when Microsoft Defender (VirusTotal's "Microsoft" engine) finds nothing in any file and the
  sandbox saw no traffic except to GitHub and Windows' own services. Other engines' detections are
  listed, not failed: false positives on new, unsigned files are common.

  Writes into -OutDir (build\scan-release): report.md (the release notes' Scans section),
  scan-report.json (everything), and summary.json for the 4.1 gate (gate-contract.md, review.scans):
  { "pass": true, "verdict": "...", "waiting": false }.

  -FromCi doesn't scan: it finds the latest scan-release run for the commit (HEAD, or -Commit) and
  downloads its report. Without one it says waiting while the repository has no keys, and fails once
  it has them.
#>
param(
  [string]$Tag = "",
  [string]$Run = "",
  [string[]]$Files = @(),
  [switch]$FromCi,
  [string]$Commit = "",
  [string]$OutDir = "",
  [switch]$UpdateNotes,
  [int]$WaitMinutes = 40,
  [string]$Offline = ""  # a folder of canned API answers (tests/scan); nothing is sent
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root "build\scan-release" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Report = Join-Path $OutDir "report.md"
$Json = Join-Path $OutDir "scan-report.json"
$Summary = Join-Path $OutDir "summary.json"

function Write-Summary([bool]$pass, [string]$verdict, [bool]$waiting, [string]$commit = "") {
  [ordered]@{ pass = $pass; verdict = $verdict; waiting = $waiting; tag = $Tag; commit = $commit } |
    ConvertTo-Json | Set-Content $Summary -Encoding utf8
}

# --- The gate: the CI scan of this commit (the keys exist only as repository secrets).
if ($FromCi) {
  $ErrorActionPreference = "Continue"  # gh's messages on stderr aren't failures here
  if (-not $Commit) { $Commit = (git -C $root rev-parse HEAD).Trim() }
  $runs = gh api "repos/{owner}/{repo}/actions/workflows/scan-release.yml/runs?head_sha=$Commit&status=completed&per_page=5" --jq '.workflow_runs[].id' 2>$null
  foreach ($id in @($runs)) {
    if (-not $id) { continue }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("fw-scan-ci-" + [guid]::NewGuid())
    gh run download $id --name scan-report --dir $tmp 2>$null
    $got = Get-ChildItem $tmp -Recurse -Filter summary.json -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($got) {
      Get-ChildItem $got.DirectoryName -File | Copy-Item -Destination $OutDir -Force
      Remove-Item $tmp -Recurse -Force
      Write-Host "The scan of $($Commit.Substring(0, 8)) from run $id"
      Get-Content $Summary -Raw
      exit 0
    }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  }
  $secrets = @(gh secret list --json name --jq '.[].name' 2>$null)
  if ($LASTEXITCODE -ne 0) {
    Write-Summary $false "no scan-release run for this commit, and the repository's secrets can't be read" $true $Commit
  } elseif ('VT_API_KEY' -in $secrets -and 'HA_API_KEY' -in $secrets) {
    Write-Summary $false "no scan-release run for $($Commit.Substring(0, 8)) yet: run it on the release's files" $false $Commit
  } else {
    Write-Summary $false "waiting for Roy's step 5 (the VirusTotal and Hybrid Analysis keys)" $true $Commit
  }
  Get-Content $Summary -Raw
  exit 0
}
$vtKey = $env:VT_API_KEY
$haKey = $env:HA_API_KEY
if ($Offline) { $vtKey = "offline"; $haKey = "offline" }

# Hosts the sandbox may see. GitHub: the update check and its download. Windows: the OS's own
# background traffic in the sandbox VM (time, connectivity, certificate revocation, updates), which
# doesn't come from Featherweight. Anything else fails the scan.
$githubHosts = '(^|\.)github\.com$', '(^|\.)githubusercontent\.com$'
$windowsHosts = @(('(^|\.)(microsoft|windows|windowsupdate|msftncsi|msftconnecttest|msedge|live|bing|azureedge|' +
                   'digicert|msocsp|verisign|globalsign|sectigo|usertrust|comodoca|identrust|letsencrypt|lencr)\.(com|net|org)$'))

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

# --- HTTP through curl.exe (multipart uploads work the same in Windows PowerShell and pwsh). Keys go
# in a header file, so they're never on a command line.
$headerFile = Join-Path ([IO.Path]::GetTempPath()) ("fw-scan-" + [guid]::NewGuid() + ".txt")
function Invoke-Api([string]$service, [string]$method, [string]$url, [string[]]$form = @(), [string]$fixture = "") {
  if ($Offline) {
    $path = Join-Path $Offline "$fixture.json"
    if (-not (Test-Path $path)) { throw "No canned answer $path" }
    return Get-Content $path -Raw | ConvertFrom-Json
  }
  if ($service -eq "vt") { Set-Content $headerFile "x-apikey: $vtKey" -Encoding ascii }
  else { Set-Content $headerFile "api-key: $haKey`r`nUser-Agent: Falcon Sandbox`r`naccept: application/json" -Encoding ascii }
  $curlArgs = @('--silent', '--show-error', '--fail-with-body', '-X', $method, '-H', "@$headerFile")
  foreach ($f in $form) { $curlArgs += '-F'; $curlArgs += $f }
  $out = & curl.exe @curlArgs $url
  $code = $LASTEXITCODE
  Remove-Item $headerFile -ErrorAction SilentlyContinue
  if ($code -eq 22 -and $method -eq 'GET' -and $fixture -like 'vt-file-*') { return $null }  # 404: not known yet
  if ($code) { throw "$service $method $url failed ($code): $out" }
  return ($out -join "`n") | ConvertFrom-Json
}

function Wait-Until([scriptblock]$done, [int]$seconds) {
  $end = (Get-Date).AddMinutes($WaitMinutes)
  while ($true) {
    $r = & $done
    if ($r) { return $r }
    if ((Get-Date) -gt $end) { return $null }
    if (-not $Offline) { Start-Sleep -Seconds $seconds }
  }
}

function Get-Sha256([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# --- VirusTotal (API v3). The public API allows 4 requests a minute: requests are spaced 16 s.
$script:lastVt = [datetime]::MinValue
function Vt([string]$method, [string]$url, [string[]]$form = @(), [string]$fixture) {
  if (-not $Offline) {
    $wait = 16 - ((Get-Date) - $script:lastVt).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Seconds ([math]::Ceiling($wait)) }
    $script:lastVt = Get-Date
  }
  Invoke-Api vt $method $url $form $fixture
}

function Scan-VirusTotal([string]$path, [string]$sha) {
  $api = "https://www.virustotal.com/api/v3"
  $known = Vt GET "$api/files/$sha" -fixture "vt-file-$sha"
  if (-not $known -or -not $known.data.attributes.last_analysis_date) {
    $size = (Get-Item $path).Length
    $uploadUrl = "$api/files"
    if ($size -gt 32MB) { $uploadUrl = (Vt GET "$api/files/upload_url" -fixture "vt-upload-url").data }
    $analysis = Vt POST $uploadUrl @("file=@$path") -fixture "vt-upload-$sha"
    $id = $analysis.data.id
    $done = Wait-Until { $a = Vt GET "$api/analyses/$id" -fixture "vt-analysis-$sha"
                         if ($a.data.attributes.status -eq 'completed') { $a } } 30
    if (-not $done) { return @{ status = 'timeout'; link = "https://www.virustotal.com/gui/file/$sha" } }
    $known = Vt GET "$api/files/$sha" -fixture "vt-file-$sha"
  }
  $attr = $known.data.attributes
  $results = $attr.last_analysis_results
  $detections = @()
  foreach ($engine in $results.PSObject.Properties) {
    if ($engine.Value.category -in 'malicious', 'suspicious') {
      $detections += "$($engine.Name): $($engine.Value.result)"
    }
  }
  $defender = $results.Microsoft
  @{
    status     = 'done'
    link       = "https://www.virustotal.com/gui/file/$sha"
    stats      = $attr.last_analysis_stats
    detections = $detections
    defender   = if ($defender) { $defender.category } else { 'not run' }
    defenderOk = (-not $defender) -or ($defender.category -notin 'malicious', 'suspicious')
  }
}

# --- Hybrid Analysis (API v2), Windows 10 64-bit (environment 160).
function Scan-HybridAnalysis([string]$path, [string]$sha) {
  $api = "https://www.hybrid-analysis.com/api/v2"
  $job = Invoke-Api ha POST "$api/submit/file" @("file=@$path", "environment_id=160") "ha-submit-$sha"
  $id = $job.job_id
  $state = Wait-Until { $s = Invoke-Api ha GET "$api/report/$id/state" -fixture "ha-state-$sha"
                        if ($s.state -in 'SUCCESS', 'ERROR') { $s } } 60
  $link = "https://www.hybrid-analysis.com/sample/$sha/$id"
  if (-not $state) { return @{ status = 'timeout'; link = $link } }
  if ($state.state -eq 'ERROR') { return @{ status = 'error'; link = $link; error = "$($state.error)" } }
  $summary = Invoke-Api ha GET "$api/report/$id/summary" -fixture "ha-summary-$sha"
  # Domains decide; the contacted addresses are kept in the JSON, since an address alone doesn't say
  # whose it is (GitHub's included).
  $github = @(); $windows = @(); $other = @()
  foreach ($h in (@($summary.domains) | Where-Object { $_ } | Sort-Object -Unique)) {
    if ($githubHosts | Where-Object { $h -match $_ }) { $github += $h }
    elseif ($windowsHosts | Where-Object { $h -match $_ }) { $windows += $h }
    else { $other += $h }
  }
  @{
    status = 'done'; link = $link; verdict = "$($summary.verdict)"; threatScore = $summary.threat_score
    github = $github; windows = $windows; other = $other; addresses = @($summary.hosts)
    networkOk = ($other.Count -eq 0)
  }
}

# --- The files
$work = $null
if (($Tag -or $Run) -and -not $Files) {
  $work = Join-Path ([IO.Path]::GetTempPath()) ("fw-scan-" + [guid]::NewGuid())
  New-Item -ItemType Directory $work | Out-Null
  if ($Run) {
    Step "Downloading run $Run's artifacts"
    gh run download $Run --dir $work
    if ($LASTEXITCODE) { throw "gh run download $Run failed" }
  } else {
    Step "Downloading $Tag's assets"
    gh release download $Tag --dir $work --pattern '*.exe' --pattern '*.zip' --pattern '*.msi' --pattern '*.msix'
    if ($LASTEXITCODE) { throw "gh release download $Tag failed" }
  }
  $Files = @(Get-ChildItem $work -File -Recurse -Include *.exe, *.zip, *.msi, *.msix | ForEach-Object FullName)
}
$Files = @($Files | ForEach-Object { Get-ChildItem $_ -File } | ForEach-Object FullName)
if (-not $Files) { throw "No files to scan" }

$entries = @()
$status = 'pass'
if (-not $vtKey -or -not $haKey) {
  $status = 'waiting'
  $missing = @(@{ VT_API_KEY = $vtKey; HA_API_KEY = $haKey }.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object Key)
  Write-Host "Waiting for Roy's step 5: $($missing -join ' and ') not set. Nothing was uploaded."
}
foreach ($path in $Files) {
  $name = Split-Path $path -Leaf
  $sha = Get-Sha256 $path
  $entry = [ordered]@{ name = $name; size = (Get-Item $path).Length; sha256 = $sha }
  if ($status -ne 'waiting') {
    Step "VirusTotal: $name"
    $entry.virustotal = Scan-VirusTotal $path $sha
    Step "Hybrid Analysis: $name"
    $entry.sandbox = Scan-HybridAnalysis $path $sha
    if ($entry.virustotal.status -ne 'done' -or -not $entry.virustotal.defenderOk) { $status = 'fail' }
    if ($entry.sandbox.status -ne 'done' -or -not $entry.sandbox.networkOk) { $status = 'fail' }
  }
  $entries += $entry
}

# --- The report
$md = @("## Scans", "")
if ($status -eq 'waiting') {
  $md += "Waiting for Roy's step 5 (the VirusTotal and Hybrid Analysis keys). The files' SHA-256 sums:", ""
  foreach ($e in $entries) { $md += "- ``$($e.name)``: ``$($e.sha256)``" }
} else {
  $md += "| File | VirusTotal | Microsoft Defender | Other detections | Sandbox | Network |", "|---|---|---|---|---|---|"
  foreach ($e in $entries) {
    $vt = $e.virustotal; $sb = $e.sandbox
    $vtCell = if ($vt.status -eq 'done') { "[$($vt.stats.malicious) of $(($vt.stats.PSObject.Properties | Measure-Object Value -Sum).Sum)]($($vt.link))" } else { "[$($vt.status)]($($vt.link))" }
    $other = if ($vt.detections) { ($vt.detections -join '; ') } else { 'none' }
    $sbCell = if ($sb.status -eq 'done') { "[$($sb.verdict), score $($sb.threatScore)]($($sb.link))" } else { "[$($sb.status)]($($sb.link))" }
    $net = if ($sb.status -ne 'done') { '—' } elseif ($sb.networkOk) { "GitHub only ($($sb.github -join ', '))" } else { "**also $($sb.other -join ', ')**" }
    $md += "| ``$($e.name)`` | $vtCell | $($vt.defender) | $other | $sbCell | $net |"
  }
  $md += "", "Windows' own background traffic in the sandbox is left out: $((@($entries | ForEach-Object { $_.sandbox.windows }) | Where-Object { $_ } | Sort-Object -Unique) -join ', ')."
}
$md += "", "Result: **$status**."
Set-Content $Report ($md -join "`n") -Encoding utf8
$head = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { "$(git -C $root rev-parse HEAD 2>$null)".Trim() }
[ordered]@{ tag = $Tag; commit = $head; status = $status; files = $entries } | ConvertTo-Json -Depth 8 | Set-Content $Json -Encoding utf8

# The gate's one line: what failed, or what passed, in words.
$names = @($entries | ForEach-Object name) -join ', '
$verdict = switch ($status) {
  'waiting' { "waiting for Roy's step 5 (the VirusTotal and Hybrid Analysis keys); nothing uploaded" }
  'pass' {
    $others = @($entries | ForEach-Object { $_.virustotal.detections } | Where-Object { $_ })
    "$($entries.Count) file$(if ($entries.Count -ne 1) { 's' }) ($names): Defender clean, sandbox traffic only to GitHub" +
      $(if ($others.Count) { "; other detections listed: $($others -join '; ')" } else { "; no other detections" })
  }
  default {
    $why = @()
    foreach ($e in $entries) {
      if ($e.virustotal.status -ne 'done') { $why += "$($e.name): VirusTotal $($e.virustotal.status)" }
      elseif (-not $e.virustotal.defenderOk) { $why += "$($e.name): Defender says $($e.virustotal.defender)" }
      if ($e.sandbox.status -ne 'done') { $why += "$($e.name): sandbox $($e.sandbox.status)" }
      elseif (-not $e.sandbox.networkOk) { $why += "$($e.name): sandbox traffic to $($e.sandbox.other -join ', ')" }
    }
    $why -join '; '
  }
}
Write-Summary ($status -eq 'pass') $verdict ($status -eq 'waiting') $head
Step "Scan $status; report in $Report"

if ($UpdateNotes -and $Tag -and $status -ne 'waiting') {
  # The release notes keep one scans section, between markers, replaced on every run.
  $body = (gh release view $Tag --json body --jq .body) -join "`n"
  $section = "<!-- scans -->`n" + ($md -join "`n") + "`n<!-- /scans -->"
  if ($body -match '(?s)<!-- scans -->.*<!-- /scans -->') { $body = $body -replace '(?s)<!-- scans -->.*<!-- /scans -->', $section }
  else { $body = $body.TrimEnd() + "`n`n" + $section }
  $notes = Join-Path ([IO.Path]::GetTempPath()) "fw-scan-notes.md"
  Set-Content $notes $body -Encoding utf8
  gh release edit $Tag --notes-file $notes
  if ($LASTEXITCODE) { throw "gh release edit failed" }
}
if ($work) { Remove-Item $work -Recurse -Force }
if ($status -eq 'fail') { exit 1 }
