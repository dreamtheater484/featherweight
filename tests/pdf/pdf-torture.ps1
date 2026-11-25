<#
.SYNOPSIS
  The PDF torture run (F1, f1-gate's damage.pdf): random page operations on the PDF corpus, saved and
  opened again, every output checked; and PDFs saved without edits through the app, byte for byte.

.DESCRIPTION
  1. The page-tools corpus (tests\pdf\make-pdf-corpus.py, made once into build\pdf-corpus), tests\pdfs,
     and every -ReferenceEvery'th PDF Office made in the private corpus (<Corpus>\reference).
     Files a page tool must refuse (restricted, XFA) go in too, and a few scanned pages for OCR.
  2. fwtool pdf-torture: R rounds of N seeded operations per PDF (rotate, delete, move, insert pages of
     another PDF, a drawn signature, OCR, extract and split into new files; with f1-pdf's test signer,
     certificate signatures every third round), saved as the app saves,
     opened again and checked page by page; failures leave reproducers in <Out>\repro.
  3. fwtool pdf-valid on every file written (<Out>\saved, extracted, signed, refused): structure, PDFium, fonts.
  4. Saved without edits: each input opened in the app (-Exe) and saved with the script op saveas:,
     which goes through the PDF tab's own save; the file must be the same bytes.
  <Out>\summary.json and summary.md; exit 1 when anything failed. The outputs then go to
  tests\pdf\pdf-readers.ps1 (Windows' PDF parser, Word, Acrobat).
  Until f1-pdf's page tools are in, step 2 calls PDFium directly (the functions the tools use); the
  summary says "through": "pdfium" then.
#>
param(
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Exe,
  [string]$Out = "build\pdf-torture",
  [string]$Corpus = $(if ($env:FW_CORPUS) { $env:FW_CORPUS } else { "D:\Documents\Codex\FeatherweightCorpus" }),
  [string]$PdfCorpus = "build\pdf-corpus",
  # The OCR scan set (tests\pdf\ocr-accuracy.ps1 makes it): a few of its scanned pages go in, for OCR.
  [string]$Scans = "build\pdf-ocr\scans",
  [uint32]$Seed = 1,
  [int]$Edits = 30,
  [int]$Rounds = 3,
  [int]$ReferenceEvery = 12,
  [int]$TimeoutSeconds = 120,
  [string]$Python = "python"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
function Get-FullPath([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
$Fwtool = Get-FullPath $Fwtool; $Out = Get-FullPath $Out; $PdfCorpus = Get-FullPath $PdfCorpus
if (-not $Exe) { $Exe = @("build-release\Release\Featherweight.exe", "build\Release\Featherweight.exe") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1 }
$utf8 = New-Object Text.UTF8Encoding $false
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# 1. The inputs.
if (-not (Test-Path (Join-Path $PdfCorpus "manifest.json"))) {
  & $Python -I (Join-Path $root "tests\pdf\make-pdf-corpus.py") --out $PdfCorpus | Out-Host
  if ($LASTEXITCODE) { throw "make-pdf-corpus.py failed" }
}
$manifest = Get-Content (Join-Path $PdfCorpus "manifest.json") -Raw -Encoding UTF8 | ConvertFrom-Json
# The ones the tools must refuse (XFA, restricted) go in too: the torture checks that every tool refuses.
$inputs = @($manifest.files | ForEach-Object { Join-Path $PdfCorpus $_.file })
$refused = @($manifest.files | Where-Object { $_.expect -like "refuse:*" } | ForEach-Object { "$($_.file) ($($_.expect))" })
$inputs += @(Get-ChildItem (Join-Path $root "tests\pdfs") -Filter *.pdf | ForEach-Object FullName)
$refDir = Join-Path $Corpus "reference"
if (Test-Path $refDir) {
  $refs = @(Get-ChildItem $refDir -Recurse -Filter *.pdf | Sort-Object FullName)
  for ($i = 0; $i -lt $refs.Count; $i += $ReferenceEvery) { $inputs += $refs[$i].FullName }
}
# Scanned pages, for OCR as an operation: a few of the OCR scan set (tests\pdf\ocr-accuracy.ps1 makes it).
$scanNote = ""
$Scans = Get-FullPath $Scans
if (Test-Path $Scans) { $inputs += @(Get-ChildItem $Scans -Filter scan-*.pdf | Sort-Object Name | Select-Object -First 4 | ForEach-Object FullName) }
else { $scanNote = "no scanned pages ($Scans isn't there: tests\pdf\ocr-accuracy.ps1 makes it)" }
# Certificate signatures every third round: f1-pdf's test signer, loaded in memory only.
$signArgs = @()
$pfx = Join-Path $root "tests\pdfs\signing\test-signer.pfx"
if (Test-Path $pfx) { $signArgs = @("--pfx", $pfx, "--pfx-password", "featherweight") }
$passwords = (@($manifest.files | Where-Object { $_.password } | ForEach-Object password) + "secret" | Sort-Object -Unique) -join ","
$list = Join-Path $Out "inputs.txt"
[IO.File]::WriteAllLines($list, [string[]]$inputs, $utf8)
"== $($inputs.Count) PDFs ($(@($manifest.files).Count) made, of which refusals: $($refused -join ', '); $(@(Get-ChildItem (Join-Path $root 'tests\pdfs') -Filter *.pdf).Count) from tests\pdfs; scanned pages and Office's)$(if ($scanNote) { "; $scanNote" })"

# 2. The torture.
"== fwtool pdf-torture: seed $Seed, $Rounds rounds of $Edits operations"
$prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
& $Fwtool pdf-torture --seed $Seed --edits $Edits --rounds $Rounds --password $passwords @signArgs --out $Out --json (Join-Path $Out "torture.json") --samples $list 2>&1 |
  ForEach-Object { "$_" } | Tee-Object -FilePath (Join-Path $Out "torture.log") | Select-String -Pattern '^\[FAIL\]|^\d+ PDFs' | ForEach-Object { "   $_" }
$tortureExit = $LASTEXITCODE
$ErrorActionPreference = $prev
$t = Get-Content (Join-Path $Out "torture.json") -Raw -Encoding UTF8 | ConvertFrom-Json

# 3. Every file written, through pdf-valid.
# The signed ones' trust root beside them, for pyHanko (tests\pdf\validate-signatures.py --dir <Out>\signed).
if (Test-Path (Join-Path $Out "signed\signatures.json")) { Copy-Item (Join-Path $root "tests\pdfs\signing\test-signer.crt") (Join-Path $Out "signed\test-signer.crt") }
$outputs = @(Get-ChildItem (Join-Path $Out "saved"), (Join-Path $Out "extracted"), (Join-Path $Out "signed"), (Join-Path $Out "refused") -Filter *.pdf -ErrorAction SilentlyContinue | ForEach-Object FullName)
$outList = Join-Path $Out "outputs.txt"
[IO.File]::WriteAllLines($outList, [string[]]$outputs, $utf8)
"== fwtool pdf-valid on $($outputs.Count) files written"
$ErrorActionPreference = "Continue"
& $Fwtool pdf-valid --password $passwords --json (Join-Path $Out "outputs-valid.json") --samples $outList 2>&1 | ForEach-Object { "$_" } |
  Tee-Object -FilePath (Join-Path $Out "outputs-valid.log") | Select-String -Pattern '^\[FAIL\]|PDFs, ' | Select-Object -First 12 | ForEach-Object { "   $_" }
$ErrorActionPreference = $prev
$valid = Get-Content (Join-Path $Out "outputs-valid.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$invalid = @($valid.files | Where-Object { -not $_.ok })

# 4. Saved without edits, through the app's own save.
$unedited = @()
if ($Exe -and (Test-Path $Exe)) {
  "== saved without edits through the app ($Exe)"
  $udir = Join-Path $Out "unedited"
  New-Item -ItemType Directory -Force $udir | Out-Null
  $n = 0
  foreach ($f in $inputs) {
    $n++
    $name = "{0:D3}-{1}" -f $n, [IO.Path]::GetFileName($f)
    $target = Join-Path $udir $name
    $log = Join-Path $udir "$name.log"
    $row = [ordered]@{ file = $f; saved = $target; identical = $false; error = $null }
    try {
      $proc = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden -RedirectStandardError $log -ArgumentList @(
        "--snapshot", "`"$udir\$name.png`"", "--size", "1000x700", "--script", "`"saveas:$target`"", "`"$f`"")
      $null = $proc.Handle
      if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; throw "the app didn't finish within $TimeoutSeconds s" }
      if (-not (Test-Path $target)) { throw "nothing saved: " + ((Get-Content $log -ErrorAction SilentlyContinue) -join "; ") }
      $row.identical = (Get-FileHash $f).Hash -eq (Get-FileHash $target).Hash
    } catch { $row.error = $_.Exception.Message }
    $unedited += [pscustomobject]$row
  }
}
$uneditedSame = @($unedited | Where-Object identical).Count
# A password-protected PDF can't be opened without its password in the app's snapshot mode: those count
# as not checked, not as different.
$uneditedDiffer = @($unedited | Where-Object { -not $_.identical -and -not $_.error })
$uneditedErrors = @($unedited | Where-Object { $_.error })

$totals = [ordered]@{
  through = $t.through; files = $t.totals.files; skipped = $t.totals.skipped; operations = $t.totals.operations; failures = $t.totals.failures
  written = $t.totals.written; outputsValid = $outputs.Count - $invalid.Count; outputsInvalid = $invalid.Count
  uneditedThroughApp = $unedited.Count; uneditedIdentical = $uneditedSame; uneditedDiffer = $uneditedDiffer.Count; uneditedNotChecked = $uneditedErrors.Count
  refusalsChecked = $t.totals.refused; signed = $t.totals.signed; ocrPages = $t.totals.ocrPages; ocrWords = $t.totals.ocrWords
}
$pass = $tortureExit -eq 0 -and $t.totals.failures -eq 0 -and $invalid.Count -eq 0 -and $unedited.Count -gt 0 -and $uneditedDiffer.Count -eq 0
$verdict = "$($t.totals.files) PDFs, $($t.totals.operations) operations, $($t.totals.failures) failures; $($outputs.Count - $invalid.Count) of $($outputs.Count) files written pass pdf-valid; " +
           "saved without edits through the app: $uneditedSame of $($unedited.Count) the same bytes$(if ($uneditedErrors.Count) { " ($($uneditedErrors.Count) not checked)" })" +
           "; $($t.totals.refused) refusals checked, $($t.totals.signed) signed, OCR $($t.totals.ocrWords) words on $($t.totals.ocrPages) pages" +
           $(if ($t.through -eq "pdfium") { "; operations through PDFium directly until f1-pdf's page tools are in" })
$summary = [ordered]@{ pass = $pass; verdict = $verdict; totals = $totals
  failures = @($t.files | Where-Object { $_.failures } | ForEach-Object { foreach ($x in $_.failures) { "$([IO.Path]::GetFileName($_.file)): $($x.category): $($x.message)" } } | Select-Object -First 200)
  invalidOutputs = @($invalid | ForEach-Object { "$([IO.Path]::GetFileName($_.file)): $($_.errors[0])" })
  uneditedDiffer = @($uneditedDiffer | ForEach-Object { $_.file }); uneditedNotChecked = @($uneditedErrors | ForEach-Object { "$($_.file): $($_.error)" })
  refusals = $refused }
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), (ConvertTo-Json $summary -Depth 6), $utf8)
$md = @("# PDF torture", "", "$(if ($pass) { 'Passed' } else { 'Failed' }): $verdict.", "", "| | |", "|---|---|")
foreach ($k in $totals.Keys) { $md += "| $k | $($totals[$k]) |" }
if ($summary.failures.Count) { $md += @("", "## Failures", "") + @($summary.failures | ForEach-Object { "- $_" }) }
if ($summary.invalidOutputs.Count) { $md += @("", "## Files written that fail pdf-valid", "") + @($summary.invalidOutputs | ForEach-Object { "- $_" }) }
if ($summary.uneditedDiffer.Count) { $md += @("", "## Saved without edits, not the same bytes", "") + @($summary.uneditedDiffer | ForEach-Object { "- $_" }) }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", $utf8)
"Summary: $(Join-Path $Out 'summary.md')"
"$(if ($pass) { 'PASSED' } else { 'FAILED' }): $verdict"
if ($pass) { exit 0 } else { exit 1 }
