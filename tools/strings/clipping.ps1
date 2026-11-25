# The languages' clipping check (F1, f1-lang): the 4.1 gate's lang.clipping row
# (FeatherweightNotes\f1\gate-contract.md).
#
#   tools\strings\clipping.ps1 -Exe <Featherweight.exe> -Json <file> -Out <folder> [-Languages en,nl,de,fr,es]
#
# Each editor (simple.docx, .xlsx, .pptx from tests\office\generated) and a PDF (tests\pdfs\mixed-sizes.pdf)
# at 1366 x 768 in each language, with the script step clipcheck: the window drawn with every ribbon tab,
# side panel and File page section, and the print options dialog, and every label that doesn't fit its
# box listed. The exe must be a test build (FW_TEST_SCREENS, the
# default; clipcheck isn't in the shipped app). Snapshots never touch the user's settings, clipboard
# or a running Featherweight. Writes
#   { "surfaces": 312, "languages": ["en","nl",...], "clipped": ["de / docx / Home / \"Format Painter\" needs 96 px, has 80"], ... }
# and exits 0 when nothing is clipped and every language asked for was the one in use.
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Json,
  [Parameter(Mandatory)][string]$Out,
  [string[]]$Languages = @("en,nl,de,fr,es"),  # "en,nl" or, from PowerShell, en,nl (an array)
  [string]$Files = "",  # default: tests\office\generated
  [string]$Pdf = "")  # default: tests\pdfs\mixed-sizes.pdf
$ErrorActionPreference = "Stop"
$repo = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."
if (-not $Files) { $Files = Join-Path $repo "tests\office\generated" }
if (-not $Pdf) { $Pdf = Join-Path $repo "tests\pdfs\mixed-sizes.pdf" }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$surfaces = 0
$clipped = New-Object System.Collections.Generic.List[string]
$missing = New-Object System.Collections.Generic.List[string]
$ran = New-Object System.Collections.Generic.List[string]
foreach ($lang in ($Languages -join ",") -split ",") {
  $inUse = $true
  foreach ($kind in "docx", "xlsx", "pptx", "pdf") {
    $file = if ($kind -eq "pdf") { $Pdf } else { Join-Path $Files "simple.$kind" }
    $png = Join-Path $Out "$lang-$kind.png"
    $err = Join-Path $Out "$lang-$kind.log"
    $p = Start-Process -FilePath $Exe -ArgumentList @("--snapshot", "`"$png`"", "--size", "1366x768", "--theme", "light",
      "--lang", $lang, "--script", "`"wait:100;clipcheck`"", "`"$file`"") -NoNewWindow -Wait -PassThru -RedirectStandardError $err
    $lines = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue | Where-Object { $_ -like "clipcheck: *" })
    $summary = $lines | Where-Object { $_ -match '^clipcheck: language (\S+), (\d+) surfaces, (\d+) clipped' } | Select-Object -Last 1
    if (-not $summary) { $missing.Add("$lang / $kind : no clipcheck summary (exit $($p.ExitCode); $err)"); continue }
    if ($Matches[1] -ne $lang) { $inUse = $false; $missing.Add("$lang / $kind : the app used '$($Matches[1])' (no catalog?)") }
    $surfaces += [int]$Matches[2]
    foreach ($l in $lines) {
      if ($l -eq $summary) { continue }
      $clipped.Add("$lang / $kind / " + $l.Substring("clipcheck: ".Length))
    }
  }
  if ($inUse) { $ran.Add($lang) }
}
$result = [ordered]@{ surfaces = $surfaces; languages = @($ran); clipped = @($clipped); problems = @($missing) }
$result | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 $Json
"clipping: $surfaces surfaces in $($ran.Count) languages ($($ran -join ', ')); $($clipped.Count) clipped; $($missing.Count) problems"
$clipped | Select-Object -First 40
$missing
if ($clipped.Count -or $missing.Count) { exit 1 }
exit 0
