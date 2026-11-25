<#
.SYNOPSIS
  Every PDF the page tools, OCR or signing wrote, opened by readers other than our own (F1, f1-gate's
  pdf.readers): PDFium (fwtool pdf-valid, with its structure check), Windows' own PDF parser
  (Windows.Data.Pdf, not PDFium), Word, and Adobe Acrobat when it's installed.

.DESCRIPTION
  -Paths: PDFs or folders of them (comma-separated works). Passwords to try: -Password a,b.
  1. fwtool pdf-valid on all of them.
  2. Windows.Data.Pdf: each file loaded (PdfDocument.LoadFromFileAsync, with its password), its page
     count compared with PDFium's, and every page rendered small (RenderToStreamAsync). Run in child
     processes of -Batch files each, so a file that hangs the parser fails alone (-TimeoutSeconds).
  3. Word and Acrobat (own instances only, never one already running; the gate holds quiet-pc.lock):
     a sample of -OfficeSample files. Not run without an active session (exit 4).
  <Out>\summary.json (pass, verdict, totals, failures) and summary.md. Exit 0 when all opened.
#>
param(
  [string[]]$Paths = @(),
  # The PDFs the outputs were made from (a list file, one per line): checked too, and an output a reader
  # fails is excused when its original fails in that reader as well (matched by name: <name>-r<N>...).
  [string]$Originals,
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\pdf-readers",
  [string]$Password = "featherweight,secret",
  [int]$Batch = 40,
  [int]$TimeoutSeconds = 60,
  # Acrobat: this many outputs, spread over all of them (0: every one).
  [int]$AcrobatSample = 200,
  [switch]$NoAcrobat,
  # Internal: a child that checks the files listed in -ListFile with Windows.Data.Pdf, or in -AcrobatList with Acrobat.
  [string]$ListFile, [string]$AcrobatList, [string]$ResultFile
)
$ErrorActionPreference = "Stop"
$utf8 = New-Object Text.UTF8Encoding $false

# ---------- The child: Adobe Acrobat, through its own COM interface (AcroExch.PDDoc: no window) ----------
if ($AcrobatList) {
  $doc = New-Object -ComObject AcroExch.PDDoc
  foreach ($f in [IO.File]::ReadAllLines($AcrobatList)) {
    if (-not $f) { continue }
    $row = [ordered]@{ file = $f; pages = -1; error = $null }
    try {
      if ($doc.Open($f)) { $row.pages = [int]$doc.GetNumPages(); [void]$doc.Close() }
      else { $row.error = "Acrobat can't open it" }
    } catch { $row.error = "$($_.Exception.Message)".Trim() }
    [IO.File]::AppendAllText($ResultFile, (ConvertTo-Json $row -Compress) + "`n", $utf8)
  }
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($doc)
  exit 0
}

# ---------- The child: Windows.Data.Pdf ----------
if ($ListFile) {
  Add-Type -AssemblyName System.Runtime.WindowsRuntime
  $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
  $null = [Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime]
  $null = [Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
  $ext = [System.WindowsRuntimeSystemExtensions].GetMethods()
  $asTaskOp = $ext | Where-Object { $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
  $asTaskAction = $ext | Where-Object { $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq "IAsyncAction" } | Select-Object -First 1
  function Wait-Operation($op, [Type]$type) {
    $task = $asTaskOp.MakeGenericMethod($type).Invoke($null, @($op))
    if (-not $task.Wait($TimeoutSeconds * 1000)) { throw "timed out" }
    $task.Result
  }
  function Wait-Action($action) {
    $task = $asTaskAction.Invoke($null, @($action))
    if (-not $task.Wait($TimeoutSeconds * 1000)) { throw "timed out" }
  }
  $passwords = @("") + @($Password -split ',' | Where-Object { $_ })
  foreach ($f in [IO.File]::ReadAllLines($ListFile)) {
    if (-not $f) { continue }
    $row = [ordered]@{ file = $f; pages = -1; rendered = 0; error = $null }
    try {
      $file = Wait-Operation ([Windows.Storage.StorageFile]::GetFileFromPathAsync($f)) ([Windows.Storage.StorageFile])
      $doc = $null; $lastError = $null
      foreach ($pw in $passwords) {
        try {
          $doc = if ($pw) { Wait-Operation ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file, $pw)) ([Windows.Data.Pdf.PdfDocument]) }
                 else { Wait-Operation ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)) ([Windows.Data.Pdf.PdfDocument]) }
          break
        } catch { $lastError = $_.Exception.InnerException; if (-not $lastError) { $lastError = $_.Exception } }
      }
      if (-not $doc) { throw $lastError }
      $row.pages = [int]$doc.PageCount
      for ($i = 0; $i -lt $doc.PageCount; $i++) {
        $page = $doc.GetPage([uint32]$i)
        $stream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
        $opts = New-Object Windows.Data.Pdf.PdfPageRenderOptions
        $opts.DestinationWidth = 120
        Wait-Action ($page.RenderToStreamAsync($stream, $opts))
        $stream.Dispose(); $page.Dispose()
        $row.rendered++
      }
    } catch {
      $e = $_.Exception
      while ($e.InnerException) { $e = $e.InnerException }
      $row.error = "$($e.Message)".Trim()
    }
    [IO.File]::AppendAllText($ResultFile, (ConvertTo-Json $row -Compress) + "`n", $utf8)
  }
  exit 0
}

# ---------- The parent ----------
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
function Get-FullPath([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
$Fwtool = Get-FullPath $Fwtool; $Out = Get-FullPath $Out
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$files = @()
foreach ($p in @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
  $p = Get-FullPath $p
  if (Test-Path $p -PathType Container) { $files += @(Get-ChildItem $p -Recurse -Filter *.pdf | Sort-Object FullName | ForEach-Object FullName) }
  elseif (Test-Path $p) { $files += $p }
}
if (-not $files.Count) { throw "no PDFs in -Paths" }
$outputCount = $files.Count
$origList = @()
if ($Originals -and (Test-Path $Originals)) { $origList = @(Get-Content $Originals -Encoding UTF8 | Where-Object { $_ -and (Test-Path $_) }) }
$files = @($files) + @($origList | Where-Object { $files -notcontains $_ })
$list = Join-Path $Out "files.txt"
[IO.File]::WriteAllLines($list, [string[]]$files, $utf8)

# 1. PDFium, through pdf-valid.
"== PDFium (fwtool pdf-valid): $($files.Count) PDFs"
$prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
& $Fwtool pdf-valid --password $Password --json (Join-Path $Out "pdfium.json") --samples $list 2>&1 | ForEach-Object { "$_" } |
  Out-File (Join-Path $Out "pdfium.log") -Encoding utf8
$ErrorActionPreference = $prev
$pdfium = @{}
foreach ($f in (Get-Content (Join-Path $Out "pdfium.json") -Raw -Encoding UTF8 | ConvertFrom-Json).files) { $pdfium[$f.file.ToLowerInvariant()] = $f }

# 2. Windows.Data.Pdf, in children of -Batch files.
"== Windows.Data.Pdf"
$winResults = Join-Path $Out "windows.jsonl"
$done = 0
while ($done -lt $files.Count) {
  $chunk = @($files[$done..([Math]::Min($files.Count, $done + $Batch) - 1)])
  $chunkList = Join-Path $Out "chunk.txt"
  [IO.File]::WriteAllLines($chunkList, [string[]]$chunk, $utf8)
  $before = if (Test-Path $winResults) { @(Get-Content $winResults).Count } else { 0 }
  $proc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
    "`"$PSCommandPath`"", "-ListFile", "`"$chunkList`"", "-ResultFile", "`"$winResults`"", "-Password", $Password, "-TimeoutSeconds", $TimeoutSeconds)
  $null = $proc.Handle
  if (-not $proc.WaitForExit(($TimeoutSeconds * 2 + 5) * 1000 * $chunk.Count)) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  $got = if (Test-Path $winResults) { @(Get-Content $winResults).Count } else { 0 }
  $doneHere = $got - $before
  if ($doneHere -lt $chunk.Count) {
    # The child stopped on a file: that one failed; go on after it.
    $hung = $chunk[$doneHere]
    [IO.File]::AppendAllText($winResults, (ConvertTo-Json ([ordered]@{ file = $hung; pages = -1; rendered = 0; error = "Windows.Data.Pdf hung or crashed on it" }) -Compress) + "`n", $utf8)
    $doneHere++
  }
  $done += $doneHere
}
$windows = @(Get-Content $winResults -Encoding UTF8 | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })

# 3. Adobe Acrobat (on the reference PC), our own instance only: never while an Acrobat is running (one
#    process serves every caller, so ours would be Roy's), only in an active session, files in children of
#    -Batch with a timeout, and the hidden Acrobat this run started closed at the end. Files that need a
#    password are left out (Acrobat would ask for it). Word isn't used: an automated Word stalls on a PDF's
#    conversion (f1-pdf and f1-gate both saw it), so Acrobat is the second reader (proposed in f1-plan).
$acrobatExe = "${env:ProgramFiles}\Adobe\Acrobat DC\Acrobat\Acrobat.exe"
$acrobat = @(); $acrobatNote = ""; $acroChecked = 0
$activeSession = @(qwinsta 2>$null | Where-Object { $_ -match '^>' -and $_ -match '\sActive\s*$' }).Count -gt 0
if ($NoAcrobat) { $acrobatNote = "Acrobat not checked (-NoAcrobat)" }
elseif (-not (Test-Path $acrobatExe)) { $acrobatNote = "Acrobat isn't installed" }
elseif (Get-Process Acrobat -ErrorAction SilentlyContinue) { $acrobatNote = "Acrobat not checked: one is running, never ours to use or close" }
elseif (-not $activeSession) { $acrobatNote = "Acrobat not checked: no active session" }
else {
  $acroStart = Get-Date
  $pick = @($files[0..($outputCount - 1)] | Where-Object { $p = $pdfium[$_.ToLowerInvariant()]; -not ($p -and $p.needsPassword) })
  if ($AcrobatSample -gt 0 -and $pick.Count -gt $AcrobatSample) {
    $stride = $pick.Count / $AcrobatSample
    $pick = @(0..($AcrobatSample - 1) | ForEach-Object { $pick[[int][math]::Floor($_ * $stride)] })
  }
  "== Adobe Acrobat: $($pick.Count) PDFs"
  $acroResults = Join-Path $Out "acrobat.jsonl"
  # Ours: an Acrobat started since this step began, with no window on screen.
  $stopOurs = { Get-Process Acrobat -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $acroStart -and $_.MainWindowHandle -eq 0 } | Stop-Process -Force -ErrorAction SilentlyContinue }
  $done = 0
  while ($done -lt $pick.Count) {
    $chunk = @($pick[$done..([Math]::Min($pick.Count, $done + $Batch) - 1)])
    $chunkList = Join-Path $Out "acrobat-chunk.txt"
    [IO.File]::WriteAllLines($chunkList, [string[]]$chunk, $utf8)
    $before = if (Test-Path $acroResults) { @(Get-Content $acroResults).Count } else { 0 }
    $proc = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
      "`"$PSCommandPath`"", "-AcrobatList", "`"$chunkList`"", "-ResultFile", "`"$acroResults`"")
    $null = $proc.Handle
    if (-not $proc.WaitForExit(($TimeoutSeconds + 5) * 1000 * $chunk.Count)) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; & $stopOurs }
    $got = if (Test-Path $acroResults) { @(Get-Content $acroResults).Count } else { 0 }
    $doneHere = $got - $before
    if ($doneHere -lt $chunk.Count) {
      $hung = $chunk[$doneHere]
      [IO.File]::AppendAllText($acroResults, (ConvertTo-Json ([ordered]@{ file = $hung; pages = -1; error = "Acrobat hung or crashed on it" }) -Compress) + "`n", $utf8)
      & $stopOurs
      $doneHere++
    }
    $done += $doneHere
  }
  # The Acrobat this run started: asked to quit, then stopped if it's still there (hidden, so ours).
  if (Get-Process Acrobat -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $acroStart }) {
    try { $app = New-Object -ComObject AcroExch.App; [void]$app.CloseAllDocs(); [void]$app.Exit(); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($app) } catch { }
  }
  Start-Sleep -Seconds 5
  & $stopOurs
  $acrobat = @(Get-Content $acroResults -Encoding UTF8 | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
  $acroChecked = $acrobat.Count
}
$officeNote = $(if ($acrobatNote) { $acrobatNote } else { "Acrobat $(@($acrobat | Where-Object { -not $_.error }).Count) of $acroChecked open" }) + "; Word not used for PDFs (it stalls converting them)"

# The results, per file: the originals' first, to excuse what fails in them already.
function Get-Problems([string]$f) {
  $p = $pdfium[$f.ToLowerInvariant()]
  $w = @($windows | Where-Object { $_.file -eq $f }) | Select-Object -First 1
  $problems = [ordered]@{}
  if (-not $p -or -not $p.ok) { $problems.pdfium = "PDFium/pdf-valid: $(if ($p) { $p.errors[0] } else { 'no result' })" }
  if (-not $w -or $w.error) { $problems.windows = "Windows.Data.Pdf: $(if ($w) { $w.error } else { 'no result' })" }
  elseif ($p -and $p.pages -gt 0 -and $w.pages -ne $p.pages) { $problems.windows = "Windows.Data.Pdf: $($w.pages) pages, PDFium $($p.pages)" }
  elseif ($w.rendered -ne $w.pages) { $problems.windows = "Windows.Data.Pdf: rendered $($w.rendered) of $($w.pages) pages" }
  $a = @($acrobat | Where-Object { $_.file -eq $f }) | Select-Object -First 1
  if ($a -and $a.error) { $problems.acrobat = "Acrobat: $($a.error)" }
  elseif ($a -and $p -and $p.pages -gt 0 -and $a.pages -ne $p.pages) { $problems.acrobat = "Acrobat: $($a.pages) pages, PDFium $($p.pages)" }
  $problems
}
$originalProblems = @{}
foreach ($o in $origList) { $originalProblems[[IO.Path]::GetFileNameWithoutExtension($o).ToLowerInvariant()] = Get-Problems $o }
$excused = @()
$rows = @()
foreach ($f in $files[0..($outputCount - 1)]) {
  $pr = Get-Problems $f
  $stem = [IO.Path]::GetFileNameWithoutExtension($f) -replace '-r\d+(-incremental|-signed(-changed)?|-(extract|split)\d+)?$', ''
  $orig = $originalProblems[$stem.ToLowerInvariant()]
  foreach ($k in @($pr.Keys)) {
    if ($orig -and $orig.Contains($k)) { $excused += "$([IO.Path]::GetFileName($f)): $($pr[$k]) (its original fails too)"; $pr.Remove($k) }
  }
  $rows += [pscustomobject]@{ file = $f; pdfiumOk = -not $pr.Contains('pdfium'); windowsOk = -not $pr.Contains('windows'); acrobatOk = -not $pr.Contains('acrobat'); problems = @($pr.Values) }
}
$failed = @($rows | Where-Object { $_.problems.Count })
$totals = [ordered]@{ files = $outputCount; originals = $origList.Count; excused = $excused.Count; pdfiumFailed = @($rows | Where-Object { -not $_.pdfiumOk }).Count
  windowsFailed = @($rows | Where-Object { -not $_.windowsOk }).Count; acrobatChecked = $acroChecked
  acrobatFailed = $(if ($acroChecked) { @($rows | Where-Object { -not $_.acrobatOk }).Count } else { $null }); acrobatNote = $acrobatNote }
# Acrobat must have read its sample: without it the second reader is missing.
$pass = -not $failed.Count -and $acroChecked -gt 0
$verdict = "$outputCount PDFs: PDFium $($outputCount - $totals.pdfiumFailed) open and valid, Windows.Data.Pdf $($outputCount - $totals.windowsFailed) open and render ($($excused.Count) failures excused: their originals fail too); $officeNote"
$summary = [ordered]@{ pass = $pass; verdict = $verdict; totals = $totals; failures = @($failed | ForEach-Object { "$([IO.Path]::GetFileName($_.file)): $($_.problems -join '; ')" }); excused = $excused }
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), (ConvertTo-Json $summary -Depth 5), $utf8)
$md = @("# PDF readers", "", "$verdict.", "", "| | |", "|---|---|") + @($totals.Keys | ForEach-Object { "| $_ | $($totals[$_]) |" })
if ($failed.Count) { $md += @("", "## Failures", "") + @($summary.failures | ForEach-Object { "- $_" }) }
if ($excused.Count) { $md += @("", "## Excused: the original fails in that reader too", "") + @($excused | ForEach-Object { "- $_" }) }
[IO.File]::WriteAllText((Join-Path $Out "summary.md"), ($md -join "`n") + "`n", $utf8)
$verdict
if ($pass) { exit 0 } else { exit 1 }
