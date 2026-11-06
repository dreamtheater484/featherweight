<#
.SYNOPSIS
  D5's full corpus pass over the new formats. Every .doc, .odt, .dotx and password-protected copy
  (tests\office and FW_CORPUS, made by scripts\make-test-files.ps1) is opened the way the app opens
  it and kept as the .docx it becomes (fwtool convert, odt and decrypt), then:
  1. the Open XML SDK validator: no errors in the converted .docx that its .docx original doesn't
     have (the importer added none);
  2. Word: each converted .docx opens without a repair prompt or an error (what Save As .docx of
     an imported file gives, before any edit);
  3. the editing torture test (scripts\torture.ps1) over the converted files and any -Extra
     documents (the distinct documents added in D5): edits, save, fwtool check, validator, Word.
  Passwords come from the reference manifests ("password", scripts\reference-renders.ps1);
  "featherweight" when a manifest has none. The summary goes to <out>\summary.md.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\corpus-pass.ps1 -Extra $env:FW_CORPUS\docx\features
#>
param(
  [string]$Corpus = $env:FW_CORPUS,
  [string]$Out = "build\corpus-pass",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [uint32]$Seed = 31,
  [int]$Edits = 40,
  [int]$Rounds = 1,
  [string[]]$Extra,
  [switch]$NoWord
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($old in "open", "decrypted") { if (Test-Path (Join-Path $Out $old)) { Remove-Item -Recurse -Force (Join-Path $Out $old) } }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out "convert.log"
Set-Content $log ""
$problems = New-Object Collections.Generic.List[string]
# fwtool, its output appended to the log; stderr from a native tool mustn't stop the script (PowerShell 5.1).
function Invoke-Fw { $ErrorActionPreference = "Continue"; & $Fwtool @args *>> $log }
$pairs = New-Object Collections.Generic.List[string]  # original <tab> converted, for the validator

$sets = [ordered]@{ public = Join-Path $root "tests\office" }
if ($Corpus) { $sets.private = (Resolve-Path $Corpus).Path }

# The .docx a copy was made from (as reference-renders.ps1's copyOf): private docx\<category>\, the
# same name; public generated\ or torture\, the name without "-protected".
function Find-Original([string]$set, [string]$base, [IO.FileInfo]$f) {
  $stem = $f.BaseName -replace '-protected$', ''
  $candidates = if ($set -eq "private") { @(Join-Path $base "docx\$($f.Directory.Name)\$stem.docx") }
                else { @("generated", "torture") | ForEach-Object { Join-Path $base "$_\$stem.docx" } }
  $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

function Add-Converted([string]$set, [string]$base, [IO.FileInfo]$source, [string]$converted) {
  if (-not (Test-Path -LiteralPath $converted)) { $problems.Add("$($source.FullName): not converted"); return }
  $original = Find-Original $set $base $source
  if ($original) { $pairs.Add("$original`t$converted") }
}

# Passwords recorded in the reference manifests, by full path.
$passwords = @{}
foreach ($m in @((Join-Path $root "tests\office\reference\manifest.json"), $(if ($Corpus) { Join-Path $sets.private "reference\manifest.json" }))) {
  if (-not $m -or -not (Test-Path $m)) { continue }
  $base = if ($m -like "$root*") { Join-Path $root "tests\office" } else { $sets.private }
  foreach ($e in (Get-Content $m -Raw -Encoding UTF8 | ConvertFrom-Json).files) {
    if ($e.password) { $passwords[[IO.Path]::GetFullPath((Join-Path $base $e.source))] = $e.password }
  }
}

"== Converting the copies as the app opens them"
foreach ($set in $sets.Keys) {
  $base = $sets[$set]
  # .doc: read as the .docx it becomes.
  $dir = Join-Path $base "doc"
  if (Test-Path $dir) {
    $dest = Join-Path $Out "open\$set-doc"
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($f in Get-ChildItem $dir -Recurse -File -Filter *.doc) {
      $to = Join-Path $dest "$($f.BaseName).docx"
      Invoke-Fw convert $f.FullName $to
      Add-Converted $set $base $f $to
    }
  }
  # .odt and .dotx: a new document from each (separate folders: a template and an .odt copy share a name).
  foreach ($kind in "odt", "dotx") {
    $dir = Join-Path $base $kind
    if (-not (Test-Path $dir)) { continue }
    $dest = Join-Path $Out "open\$set-$kind"
    Invoke-Fw odt --out $dest $dir
    foreach ($f in Get-ChildItem $dir -Recurse -File -Filter "*.$kind") { Add-Converted $set $base $f (Join-Path $dest "$($f.BaseName).docx") }
  }
  # Password-protected: opened with the manifest's password, then a .doc read as above.
  $dir = Join-Path $base "protected"
  if (Test-Path $dir) {
    $dest = Join-Path $Out "open\$set-protected"
    New-Item -ItemType Directory -Force $dest | Out-Null
    $files = @(Get-ChildItem $dir -Recurse -File | Where-Object { $_.Extension -in ".docx", ".doc" })
    foreach ($group in $files | Group-Object { if ($passwords[$_.FullName]) { $passwords[$_.FullName] } else { "featherweight" } }) {
      $dec = Join-Path $Out "decrypted\$set\$($group.Name.GetHashCode())"
      Invoke-Fw decrypt --password $group.Name --out $dec @($group.Group | ForEach-Object { $_.FullName })
      foreach ($f in $group.Group) {
        $d = Join-Path $dec $f.Name
        if (-not (Test-Path -LiteralPath $d)) { if ($f.Name -notlike "*-plain*") { $problems.Add("$($f.FullName): not decrypted") }; continue }
        $to = Join-Path $dest ("{0}{1}.docx" -f $f.BaseName, $(if ($f.Extension -eq ".doc") { "-doc" } else { "" }))
        if ($f.Extension -eq ".doc") { Invoke-Fw convert $d $to } else { Copy-Item -LiteralPath $d $to }
        Add-Converted $set $base $f $to
      }
    }
  }
}
$converted = @(Get-ChildItem (Join-Path $Out "open") -Recurse -File -Filter *.docx)
"$($converted.Count) converted, $($problems.Count) problems"

"== Validator: errors the conversion added (each converted .docx against its original)"
$validator = Join-Path $root "build\validate\fwvalidate.exe"
& dotnet build (Join-Path $root "tools\validate\Validate.csproj") -c Release -o (Join-Path $root "build\validate") --nologo -v q | Out-Null
if ($LASTEXITCODE) { throw "building tools\validate failed" }
$pairsFile = Join-Path $Out "convert-pairs.tsv"
[IO.File]::WriteAllLines($pairsFile, $pairs)
& $validator --compare --pairs $pairsFile --json (Join-Path $Out "convert-validate.json") | Where-Object { $_ -match '^\[FAIL\]|files,' }
$added = @((Get-Content (Join-Path $Out "convert-validate.json") -Raw | ConvertFrom-Json).files | Where-Object { @($_.newErrors).Count })

$wordBad = @()
if (-not $NoWord) {
  "== Word: does each converted .docx open without a repair prompt?"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "word-open-check.ps1") (Join-Path $Out "open") -Json (Join-Path $Out "convert-word.json")
  $wordBad = @((Get-Content (Join-Path $Out "convert-word.json") -Raw | ConvertFrom-Json).files | Where-Object { $_.status -notin "ok", "notice", "unchecked" })
}

"== Torture over the converted files and the extra documents"
$paths = @(Get-ChildItem (Join-Path $Out "open") -Directory | ForEach-Object { $_.FullName }) + $Extra
$tortureArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "torture.ps1"), "-Paths", ($paths -join ","),
  "-Seed", $Seed, "-Edits", $Edits, "-Rounds", $Rounds, "-Out", (Join-Path $Out "torture"), "-Fwtool", $Fwtool)
if ($NoWord) { $tortureArgs += "-NoWord" }
& powershell.exe @tortureArgs | Select-Object -Last 8

$md = @("# Corpus pass over the new formats", "", "$($converted.Count) copies converted as the app opens them; $($problems.Count) not converted.", "")
foreach ($p in $problems) { $md += "- $p" }
$md += "", "Validator: $($added.Count) converted files with errors their original doesn't have."
foreach ($a in $added) { $md += "- $($a.saved): " + ((@($a.newErrors) | Select-Object -First 3) -join " | ") }
$md += "", "Word: $($wordBad.Count) converted files didn't open cleanly."
foreach ($w in $wordBad) { $md += "- $($w.file): $($w.status) " + ((@($w.dialogs | ForEach-Object { $_.text }) + @($w.error) | Where-Object { $_ }) -join " / ") }
$md += "", "Torture: see torture\summary.md."
[IO.File]::WriteAllLines((Join-Path $Out "summary.md"), $md)
$md -join "`n"
if ($problems.Count -or $added.Count -or $wordBad.Count) { exit 1 }
