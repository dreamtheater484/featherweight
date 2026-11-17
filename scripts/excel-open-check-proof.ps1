<#
.SYNOPSIS
  Proves scripts\excel-open-check.ps1 on deliberately broken workbooks: makes copies of an
  Excel-made test workbook with one defect each, runs the check on them, and compares each result
  with the one expected. The exit code is 1 when any result differs.

  The copies (in a new folder under TEMP, removed afterwards unless -Keep):
  - clean: the workbook as it is (ok)
  - shared-string: a cell points past the end of the shared strings (repaired)
  - style: a cell uses a style that doesn't exist (repaired)
  - truncated-sheet: the first sheet's XML cut off halfway (refused: no way of opening works)
  - duplicate-sheet: two sheets with the same name (repaired)
  - no-workbook: the workbook part removed (refused)
  - not-a-zip: random bytes (refused)

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\excel-open-check-proof.ps1
#>
param(
  [string]$Source = "tests\office\excel\sheets.xlsx",
  [switch]$Keep
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$src = if ([IO.Path]::IsPathRooted($Source)) { $Source } else { Join-Path $root $Source }
$dir = Join-Path ([IO.Path]::GetTempPath()) "fw-excel-open-proof-$PID"
New-Item -ItemType Directory -Force $dir | Out-Null

# Copies the workbook and changes one part's text with $edit (a scriptblock: text in, text out), or
# removes the part when $edit is $null.
function New-Variant([string]$name, [string]$part, $edit, [switch]$Remove) {
  $path = Join-Path $dir "$name.xlsx"
  Copy-Item -LiteralPath $src $path
  if (-not $part) { return }
  $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Update)
  try {
    $entry = $zip.GetEntry($part)
    if (-not $entry) { throw "$src has no $part" }
    if ($Remove) { $entry.Delete(); return }
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

$expected = [ordered]@{}
New-Variant "clean" $null $null; $expected["clean"] = "ok"
New-Variant "shared-string" "xl/worksheets/sheet1.xml" { param($t) ([regex]'(<c [^>]*t="s"[^>]*><v>)\d+(</v>)').Replace($t, '${1}99999${2}', 1) }
$expected["shared-string"] = "repaired"
New-Variant "style" "xl/worksheets/sheet1.xml" { param($t) ([regex]'<c r="([A-Z]+\d+)"( s="\d+")?').Replace($t, '<c r="$1" s="9999"', 1) }
$expected["style"] = "repaired"
New-Variant "truncated-sheet" "xl/worksheets/sheet1.xml" { param($t) $t.Substring(0, [int]($t.Length / 2)) }
$expected["truncated-sheet"] = "refused"  # normal, repair and extract all fail (Excel 2021, 17 Nov 2026)
New-Variant "duplicate-sheet" "xl/workbook.xml" {
  param($t)
  $names = [regex]::Matches($t, '<sheet [^>]*name="([^"]*)"')
  if ($names.Count -lt 2) { throw "needs two sheets" }
  $t.Replace('name="' + $names[1].Groups[1].Value + '"', 'name="' + $names[0].Groups[1].Value + '"')
}
$expected["duplicate-sheet"] = "repaired"
New-Variant "no-workbook" "xl/workbook.xml" $null -Remove; $expected["no-workbook"] = "refused"
$bytes = New-Object byte[] 4096
(New-Object Random 7).NextBytes($bytes)
[IO.File]::WriteAllBytes((Join-Path $dir "not-a-zip.xlsx"), $bytes); $expected["not-a-zip"] = "refused"

$json = Join-Path $dir "result.json"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "excel-open-check.ps1") $dir -Json $json
$got = @{}
foreach ($e in (Get-Content $json -Raw | ConvertFrom-Json).files) { $got[[IO.Path]::GetFileNameWithoutExtension($e.file)] = $e }
$wrong = 0
""
foreach ($k in $expected.Keys) {
  $status = if ($got[$k]) { $got[$k].status } else { "(missing)" }
  $mark = if ($status -eq $expected[$k]) { "as expected" } else { $wrong++; "EXPECTED $($expected[$k])" }
  "{0,-16} {1,-9} {2}" -f $k, $status, $mark
}
if (-not $Keep) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue } else { "kept in $dir" }
exit $(if ($wrong) { 1 } else { 0 })
