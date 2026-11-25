# OCR accuracy for the 4.1 gate's "PDF tools / OCR" row: >= 95 % of words right on a generated scan set
# (English, 300 dpi).
#
#   a. The scan set (tests\pdf\make-scan-set.py): image-only PDFs, each page also as a PNG, and truth.json.
#      Made once into -Scans; -Regenerate makes it again.
#   b. Baseline: Windows' own offline OCR (Windows.Media.Ocr, en-US) on every PNG, scored (tests\pdf\ocr-score.py)
#      into <Out>\windows. It shows the set is realistic and that 95 % is reachable; it never passes the row.
#   c. Featherweight: only when fwtool has pdf-ocr (`fwtool pdf-ocr --out DIR <pdf>...` writes OCRed copies with
#      the same names). The text layer is read back with `fwtool pdf-valid --out` (PDFium's text, pages
#      separated by a form feed), page k of scan-NN.pdf scored as scan-NN-pk.png into <Out>\featherweight.
#   d. <Out>\summary.json: Featherweight's summary when it ran (else the baseline's), "through" says which,
#      plus a "windows" member with the baseline. Exit 0 only when Featherweight's OCR ran and got >= 95 %.
#
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\pdf\ocr-accuracy.ps1 [-Regenerate]
param(
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Exe,  # Featherweight.exe: taken for the gate's interface; the OCR runs through fwtool
  [string]$Out = "build\pdf-ocr",
  [string]$Scans,
  [string]$Python = "python",
  [switch]$Regenerate
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$utf8 = New-Object Text.UTF8Encoding $false
$started = Get-Date

function Get-FullPath([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
function Write-Utf8([string]$path, [string]$text) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  [IO.File]::WriteAllText($path, $text, $utf8)
}
function Read-JsonFile([string]$path) {
  if ($path -and (Test-Path $path)) { Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json }
}
# Runs a program; its output (stderr included) as text, echoed unless -Quiet. Never throws.
function Invoke-Native([string]$exe, [string[]]$arguments, [switch]$Quiet) {
  $prev = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $lines = @(& $exe @arguments 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
  } catch {
    $lines = @("$_")
    $code = -1
  } finally {
    $ErrorActionPreference = $prev
  }
  if (-not $Quiet) { foreach ($l in $lines) { Write-Host "  $l" } }
  [pscustomobject]@{ Code = $code; Output = ($lines -join "`n") }
}

$Fwtool = Get-FullPath $Fwtool
$Out = Get-FullPath $Out
if ($Scans) { $Scans = Get-FullPath $Scans } else { $Scans = Join-Path $Out "scans" }
if ($Python -match '[\\/]') { $Python = Get-FullPath $Python }
New-Item -ItemType Directory -Force $Out | Out-Null
$truthPath = Join-Path $Scans "truth.json"
$scorer = Join-Path $PSScriptRoot "ocr-score.py"

# ---- a. The scan set (made again when make-scan-set.py changed since)
$generator = Join-Path $PSScriptRoot "make-scan-set.py"
$stale = $false
if (Test-Path $truthPath) {
  $made = Read-JsonFile $truthPath
  $stale = $made.generatorSha256 -ne (Get-FileHash $generator -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($stale) { Write-Host "The scan set in $Scans comes from another make-scan-set.py" }
}
if ($Regenerate -or $stale -or -not (Test-Path $truthPath)) {
  Write-Host "Making the scan set in $Scans"
  $r = Invoke-Native $Python @("-I", $generator, "--out", $Scans)
  if ($r.Code -ne 0 -or -not (Test-Path $truthPath)) { throw "make-scan-set.py failed (exit $($r.Code))" }
}
$truth = Read-JsonFile $truthPath
$pngs = @($truth.documents | ForEach-Object { $_.pages } | ForEach-Object { $_.png })
$truthWords = 0
foreach ($d in $truth.documents) { foreach ($p in $d.pages) { $truthWords += @($p.words).Count } }
Write-Host "Scan set: $(@($truth.documents).Count) documents, $($pngs.Count) pages, $truthWords words (seed $($truth.seed))"

# Scores an ocr.json; returns the summary, or $null.
function Invoke-Score([string]$ocrJson, [string]$through, [string]$dir) {
  $s = Join-Path $dir "summary.json"
  if (Test-Path $s) { Remove-Item $s -Force }
  $r = Invoke-Native $Python @("-I", $scorer, "--truth", $truthPath, "--ocr", $ocrJson, "--through", $through, "--out", $dir)
  if ($r.Code -ne 0 -and $r.Code -ne 1) { Write-Host "  ocr-score.py failed (exit $($r.Code))" }
  Read-JsonFile $s
}

# ---- b. Baseline: Windows.Media.Ocr on the PNGs
$windows = $null
$windowsError = $null
try {
  Add-Type -AssemblyName System.Runtime.WindowsRuntime
  $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
  $null = [Windows.Storage.FileAccessMode, Windows.Storage, ContentType = WindowsRuntime]
  $null = [Windows.Storage.Streams.IRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
  $null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
  $null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
  $null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
  $null = [Windows.Media.Ocr.OcrResult, Windows.Foundation, ContentType = WindowsRuntime]
  $null = [Windows.Globalization.Language, Windows.Globalization, ContentType = WindowsRuntime]
  $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq "AsTask" -and $_.IsGenericMethodDefinition -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
  function Wait-WinRt($operation, [Type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    $null = $task.Wait(-1)
    $task.Result
  }
  $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage((New-Object Windows.Globalization.Language "en-US"))
  if (-not $engine) { throw "Windows has no en-US OCR language (Settings, Time & language, Language: add English (United States) with its OCR feature)" }
  Write-Host "Windows' OCR (en-US) on $($pngs.Count) pages"
  $texts = [ordered]@{}
  $t0 = Get-Date
  foreach ($png in $pngs) {
    $path = Join-Path $Scans $png
    $file = Wait-WinRt ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
    $stream = Wait-WinRt ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    try {
      $decoder = Wait-WinRt ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
      $bitmap = Wait-WinRt ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
      $result = Wait-WinRt ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
      $texts[$png] = (@($result.Lines | ForEach-Object { $_.Text }) -join "`n")
      $bitmap.Dispose()
    } finally {
      $stream.Dispose()
    }
  }
  Write-Host ("  {0:N1} s" -f ((Get-Date) - $t0).TotalSeconds)
  $ocrWindows = Join-Path $Out "ocr-windows.json"
  Write-Utf8 $ocrWindows (ConvertTo-Json @{ through = "windows"; engine = "Windows.Media.Ocr en-US"; pages = $texts } -Depth 5)
  $windows = Invoke-Score $ocrWindows "windows" (Join-Path $Out "windows")
  if (-not $windows) { $windowsError = "ocr-score.py wrote no summary for Windows' OCR" }
} catch {
  $windowsError = "Windows' OCR baseline failed: $_"
  Write-Host "  $windowsError"
}

# ---- c. Featherweight: fwtool pdf-ocr, the text layer read back with fwtool pdf-valid
$fw = $null
$fwRan = $false
$fwError = $null
$usage = (Invoke-Native $Fwtool @() -Quiet).Output
if ($usage -match "pdf-ocr") {
  $fwRan = $true
  try {
    $ocrDir = Join-Path $Out "ocr"
    $textDir = Join-Path $Out "ocr-text"
    foreach ($d in @($ocrDir, $textDir)) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
    $pdfs = @($truth.documents | ForEach-Object { Join-Path $Scans $_.pdf })
    Write-Host "Featherweight's OCR: fwtool pdf-ocr on $($pdfs.Count) PDFs"
    $t0 = Get-Date
    $r = Invoke-Native $Fwtool (@("pdf-ocr", "--out", $ocrDir) + $pdfs)
    Write-Host ("  exit {0}, {1:N1} s" -f $r.Code, ((Get-Date) - $t0).TotalSeconds)
    if ($r.Code -ne 0) { throw "fwtool pdf-ocr failed (exit $($r.Code))" }
    $ocred = @($truth.documents | ForEach-Object { Join-Path $ocrDir $_.pdf })
    $absent = @($ocred | Where-Object { -not (Test-Path $_) })
    if ($absent.Count) { throw "fwtool pdf-ocr wrote no $(($absent | ForEach-Object { Split-Path -Leaf $_ }) -join ', ')" }
    $r = Invoke-Native $Fwtool (@("pdf-valid", "--out", $textDir) + $ocred)
    if ($r.Code -ne 0) { Write-Host "  fwtool pdf-valid found errors in the OCRed PDFs (exit $($r.Code)); scoring the text it read" }
    $texts = [ordered]@{}
    $unread = @($truth.documents | Where-Object { -not (Test-Path (Join-Path $textDir ([IO.Path]::GetFileNameWithoutExtension($_.pdf) + ".txt"))) })
    if ($unread.Count -eq @($truth.documents).Count) { throw "fwtool pdf-valid read no text from the OCRed PDFs (exit $($r.Code))" }
    foreach ($doc in $truth.documents) {
      $txt = Join-Path $textDir ([IO.Path]::GetFileNameWithoutExtension($doc.pdf) + ".txt")
      $all = if (Test-Path $txt) { [IO.File]::ReadAllText($txt, [Text.Encoding]::UTF8) } else { Write-Host "  no text read from $($doc.pdf)"; "" }
      $pageTexts = $all -split "`f"
      $k = 0
      foreach ($p in $doc.pages) {
        $texts[$p.png] = if ($k -lt $pageTexts.Count) { $pageTexts[$k] } else { "" }
        $k++
      }
    }
    $ocrFw = Join-Path $Out "ocr-featherweight.json"
    Write-Utf8 $ocrFw (ConvertTo-Json @{ through = "featherweight"; engine = "fwtool pdf-ocr"; pages = $texts } -Depth 5)
    $fw = Invoke-Score $ocrFw "featherweight" (Join-Path $Out "featherweight")
    if (-not $fw) { throw "ocr-score.py wrote no summary for Featherweight's OCR" }
  } catch {
    $fwError = "$_"
    Write-Host "  $fwError"
  }
} else {
  Write-Host "Featherweight's OCR: skipped (fwtool has no pdf-ocr: $Fwtool)"
}

# ---- d. The summary
if ($fwRan -and $fw) {
  $summary = $fw
} elseif ($fwRan) {
  $summary = [pscustomobject]@{ through = "featherweight"; pages = $pngs.Count; words = $truthWords; right = 0; percent = 0; error = $fwError }
} elseif ($windows) {
  $summary = $windows
} else {
  $summary = [pscustomobject]@{ through = "windows"; pages = $pngs.Count; words = $truthWords; right = 0; percent = 0; error = $windowsError }
}
$baseline = [ordered]@{ percent = $null; words = $truthWords; right = $null; error = $windowsError }
if ($windows) { $baseline = [ordered]@{ percent = $windows.percent; words = $windows.words; right = $windows.right } }
if (-not $fwRan) {  # the baseline alone never passes the row
  $summary | Add-Member -NotePropertyName passed -NotePropertyValue $false -Force
  $summary | Add-Member -NotePropertyName note -NotePropertyValue "Featherweight has no OCR yet (fwtool pdf-ocr): Windows' OCR alone, as the baseline" -Force
}
$summary | Add-Member -NotePropertyName windows -NotePropertyValue $baseline -Force
$summary | Add-Member -NotePropertyName scans -NotePropertyValue $Scans -Force
$summary | Add-Member -NotePropertyName seconds -NotePropertyValue ([math]::Round(((Get-Date) - $started).TotalSeconds, 1)) -Force
Write-Utf8 (Join-Path $Out "summary.json") (ConvertTo-Json $summary -Depth 6)

$base = if ($windows) { "Windows' OCR $($windows.percent) %" } else { "Windows' OCR baseline didn't run" }
if ($fwRan -and $fw) {
  $verdict = if ($fw.percent -ge 95) { "passes" } else { "fails" }
  Write-Host "OCR accuracy: Featherweight $($fw.percent) % of $($fw.words) words right on $($fw.pages) pages ($verdict, >= 95 %); $base"
  if ($fw.percent -ge 95) { exit 0 } else { exit 1 }
} elseif ($fwRan) {
  Write-Host "OCR accuracy: Featherweight's OCR failed: $fwError; $base"
} elseif ($windows) {
  Write-Host "OCR accuracy: Featherweight has no OCR yet (fwtool pdf-ocr); baseline: Windows' OCR $($windows.percent) % of $($windows.words) words right on $($windows.pages) pages"
} else {
  Write-Host "OCR accuracy: nothing measured: $windowsError"
}
exit 1
