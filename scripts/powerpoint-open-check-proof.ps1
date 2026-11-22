<#
.SYNOPSIS
  Proves scripts\powerpoint-open-check.ps1 on deliberately broken decks: makes copies of a test deck
  with one defect each, runs the check on them, and compares each result with the one expected. A
  broken deck must never come back "ok". The exit code is 1 when any result differs.

  The copies (in a new folder under TEMP, removed afterwards unless -Keep):
  - clean: the deck as it is (ok)
  - bad-number: a shape's offset is not a number (an XML schema error PowerPoint repairs)
  - truncated-slide: the first slide's XML cut off halfway
  - no-layout: the first slide's relationship to its layout removed (PowerPoint 2021 refuses it)
  - duplicate-slide: the first slide listed twice in the slide list
  - missing-slide: the slide list names a relationship to a slide part that isn't there
  - duplicate-shape-id: two shapes on a slide with the same id (recorded only: PowerPoint may fix it silently)
  - not-a-zip: random bytes (refused)
  - section-missing-slide, section-slide-twice, section-slide-in-none, section-duplicate-id: copies of
    sections-custom-shows.pptx whose section list names a missing slide (expected repaired or refused),
    lists a slide twice, leaves one out, or gives two sections one id (the last three recorded only:
    PowerPoint's verdict sets fwtool check's severity for them)
  Every broken copy is expected to be "repaired" or "refused" (which of the two is recorded, not
  required: it's PowerPoint's choice); clean must be "ok".

  PowerPoint only runs in an active session with no PowerPoint of anyone's running (the check's own
  rules); otherwise the proof stops with the check's exit code (3 or 4).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\powerpoint-open-check-proof.ps1
#>
param(
  [string]$Source = "tests\office\presentations\bullets.pptx",
  [switch]$Keep
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$src = if ([IO.Path]::IsPathRooted($Source)) { $Source } else { Join-Path $root $Source }
$dir = Join-Path ([IO.Path]::GetTempPath()) "fw-powerpoint-open-proof-$PID"
New-Item -ItemType Directory -Force $dir | Out-Null

# Copies the deck (or $from) and changes one part's text with $edit (a scriptblock: text in, text out).
function New-Variant([string]$name, [string]$part, $edit, [string]$from = $src) {
  $path = Join-Path $dir "$name.pptx"
  Copy-Item -LiteralPath $from $path
  if (-not $part) { return }
  $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Update)
  try {
    $entry = $zip.GetEntry($part)
    if (-not $entry) { throw "$src has no $part" }
    $reader = New-Object IO.StreamReader($entry.Open())
    $text = $reader.ReadToEnd()
    $reader.Close()
    $new = & $edit $text
    if ($new -eq $text) { throw "$name changed nothing in $part" }
    $entry.Delete()
    $writer = New-Object IO.StreamWriter($zip.CreateEntry($part).Open(), [Text.UTF8Encoding]::new($false))
    $writer.Write($new)
    $writer.Close()
  } finally { $zip.Dispose() }
}

$broken = "repaired", "refused"
$expected = [ordered]@{}
New-Variant "clean" $null $null; $expected["clean"] = @("ok")
New-Variant "bad-number" "ppt/slides/slide2.xml" {
  param($t)
  if ($t -match '<a:off x="') { ([regex]'<a:off x="\d+"').Replace($t, '<a:off x="abc"', 1) }
  else { ([regex]'<p:spPr/>').Replace($t, '<p:spPr><a:xfrm><a:off x="abc" y="0"/><a:ext cx="100" cy="100"/></a:xfrm></p:spPr>', 1) }
}
$expected["bad-number"] = $broken
New-Variant "truncated-slide" "ppt/slides/slide1.xml" { param($t) $t.Substring(0, [int]($t.Length / 2)) }
$expected["truncated-slide"] = $broken
New-Variant "no-layout" "ppt/slides/_rels/slide1.xml.rels" { param($t) ([regex]'<Relationship [^>]*slideLayout"?[^>]*/>').Replace($t, '', 1) }
$expected["no-layout"] = $broken
New-Variant "duplicate-slide" "ppt/presentation.xml" { param($t) ([regex]'(<p:sldId id="256" (r:id="rId\d+")/>)').Replace($t, '$1<p:sldId id="300" $2/>', 1) }
$expected["duplicate-slide"] = $broken
New-Variant "missing-slide" "ppt/_rels/presentation.xml.rels" { param($t) ([regex]'Target="slides/slide1.xml"').Replace($t, 'Target="slides/slide99.xml"', 1) }
$expected["missing-slide"] = $broken
New-Variant "duplicate-shape-id" "ppt/slides/slide1.xml" { param($t) ([regex]'<p:cNvPr id="3" ').Replace($t, '<p:cNvPr id="2" ', 1) }
$expected["duplicate-shape-id"] = @("ok", "repaired", "refused")  # recorded only: PowerPoint may renumber them silently
# Sections (p14:sectionLst), which P2's slide operations keep: what PowerPoint does with each defect
# decides whether fwtool check calls it an error (src\pkg\Check.cpp). A section naming a missing slide
# is an error already; the others are recorded only.
$sections = Join-Path $root "tests\office\presentations\sections-custom-shows.pptx"
$anyway = @("ok", "repaired", "refused")
New-Variant "section-missing-slide" "ppt/presentation.xml" { param($t) $t.Replace('<p14:sldId id="261"/>', '<p14:sldId id="261"/><p14:sldId id="999"/>') } $sections
$expected["section-missing-slide"] = $broken
New-Variant "section-slide-twice" "ppt/presentation.xml" { param($t) $t.Replace('<p14:sldIdLst><p14:sldId id="261"/>', '<p14:sldIdLst><p14:sldId id="256"/><p14:sldId id="261"/>') } $sections
$expected["section-slide-twice"] = $anyway
New-Variant "section-slide-in-none" "ppt/presentation.xml" { param($t) $t.Replace('<p14:sldId id="261"/>', '') } $sections
$expected["section-slide-in-none"] = $anyway
New-Variant "section-duplicate-id" "ppt/presentation.xml" { param($t) $t.Replace('id="{7C6D5E4F-3A2B-4C1D-8E9F-0A7B6C5D4E43}"', 'id="{4F3A2B1C-0D9E-4F8A-9B7C-6D5E4F3A2B10}"') } $sections
$expected["section-duplicate-id"] = $anyway
$garbage = Join-Path $dir "not-a-zip.pptx"
$bytes = New-Object byte[] 4096
(New-Object Random 7).NextBytes($bytes)
[IO.File]::WriteAllBytes($garbage, $bytes)
$expected["not-a-zip"] = @("refused")

$json = Join-Path $dir "result.json"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "powerpoint-open-check.ps1") $dir -Json $json
$code = $LASTEXITCODE
if ($code -eq 3 -or $code -eq 4) {
  if (-not $Keep) { Remove-Item -Recurse -Force $dir }
  "The check couldn't run PowerPoint now (exit $code): run the proof again then."
  exit $code
}
$results = (Get-Content $json -Raw | ConvertFrom-Json).files
$wrong = 0
""
"{0,-20} {1,-10} {2,-18} {3}" -f "deck", "result", "expected", "how"
foreach ($name in $expected.Keys) {
  $r = $results | Where-Object { [IO.Path]::GetFileNameWithoutExtension($_.file) -eq $name } | Select-Object -First 1
  $status = if ($r) { $r.status } else { "missing" }
  $ok = $expected[$name] -contains $status
  if (-not $ok) { $wrong++ }
  $how = if ($r) { if ($r.load) { "opened: $($r.load)" } elseif ($r.openErrors) { ($r.openErrors.PSObject.Properties | ForEach-Object { "$($_.Name): $($_.Value)" }) -join " | " } else { "$($r.error)" } } else { "" }
  if ($how.Length -gt 120) { $how = $how.Substring(0, 120) + "..." }
  "{0,-20} {1,-10} {2,-18} {3}{4}" -f $name, $status, ($expected[$name] -join "/"), $how, $(if ($ok) { "" } else { "   <-- WRONG" })
}
if ($Keep) { "Kept: $dir" } else { Remove-Item -Recurse -Force $dir }
""
if ($wrong) { "$wrong results differ from what's expected."; exit 1 }
"Every broken deck was caught, and the clean one opened."
exit 0
