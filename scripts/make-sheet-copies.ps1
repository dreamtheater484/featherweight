<#
.SYNOPSIS
  S4's .xls and .ods corpora (docs/s4-plan.md, "Files"): Excel saves each workbook in the private
  corpus's xlsx folder as Excel 97-2003 (.xls, FileFormat 56) and as OpenDocument (.ods, 60), into
  <corpus>\xls and <corpus>\ods, by category, the same names. Each folder gets a manifest.json:
  every copy, its source ("copyOf"), both files' SHA-256, its size, and what went wrong if Excel
  couldn't make it. Then scripts\reference-renders.ps1 -Corpus renders them: Excel's PDFs and its
  cell texts (the cells files), the expected values for fwtool compare.

  Excel: a new, separate instance for each workbook (the script checks that the Excel it talks to
  is a process it started), hidden, with alerts, events and link updates off and macros forced
  off; only that Excel is ever ended, and it always is, also when a workbook hangs it. One Office
  automation at a time on this PC (the global mutex reference-renders.ps1 and the open checks
  share), taken per workbook, so other checks run in between. Sources open read-only and are never
  saved over.

  Excel saves row heights and column widths for Windows' default printer and the display, so, as
  scripts\make-sheet-files.ps1, it stops unless the default printer is the reference printer and a
  blank sheet's standard width is the references' (tests\office\reference\manifest.json).

  A copy whose source hasn't changed since it was made is kept (-Force makes them all again), so a
  run that stops part way goes on where it was.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-sheet-copies.ps1 -Corpus $env:FW_CORPUS
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-sheet-copies.ps1 -Corpus D:\corpus -Only budgets -Formats xls
#>
param(
  [string]$Corpus = $env:FW_CORPUS,
  [string[]]$Formats = @("xls", "ods"),
  # Only sources whose path under xlsx\ contains one of these (a category, a file name).
  [string[]]$Only,
  [switch]$Force,
  [int]$TimeoutSeconds = 300,
  [string]$ExcelPrinter,
  # Internal: one workbook in this process (the parent runs one child per workbook).
  [string]$Child, [string]$Source, [string]$Targets, [string]$ResultFile
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$fileFormats = @{ xls = 56; ods = 60 }  # xlExcel8, xlOpenDocumentSpreadsheet

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# ---- Child: open the source once per target and save it in that format.
if ($Child) {
  Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FwCopyExcel {
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static int Of(int hwnd) { uint pid; GetWindowThreadProcessId(new IntPtr(hwnd), out pid); return (int)pid; }
}
"@
  $result = [ordered]@{ pid = 0; excel = $null; targets = @() }
  $xl = $null
  try {
    $before = @(Get-Process EXCEL -ErrorAction SilentlyContinue | ForEach-Object Id)
    $since = Get-Date
    $xl = New-Object -ComObject Excel.Application
    $xlPid = [FwCopyExcel]::Of([int]$xl.Hwnd)
    $proc = Get-Process -Id $xlPid -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne "EXCEL" -or $before -contains $xlPid -or $proc.StartTime -lt $since.AddSeconds(-5)) {
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl); $xl = $null
      throw "COM attached to an Excel this script didn't start (process $xlPid); nothing was done to it"
    }
    $result.pid = $xlPid
    Write-Json $result $ResultFile  # the parent can end this Excel from here on
    $xl.Visible = $false
    $xl.DisplayAlerts = $false
    $xl.EnableEvents = $false
    $xl.AskToUpdateLinks = $false
    $xl.ScreenUpdating = $false
    $xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    $result.excel = "Excel $($xl.Version) (build $($xl.Build)), for $($xl.ActivePrinter)"
    $blank = $xl.Workbooks.Add()
    $result.standardWidth = [Math]::Round([double]$blank.Worksheets.Item(1).StandardWidth, 4)
    $blank.Close($false)
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($blank)
    $result.printer = [string]$xl.ActivePrinter
    foreach ($t in $Targets -split '\|') {
      $format, $dest = $t -split '>', 2
      $entry = [ordered]@{ format = $format; path = $dest; error = $null }
      $wb = $null
      try {
        # Never update links; read-only; a dummy password: a protected file fails instead of prompting.
        try { $wb = $xl.Workbooks.Open($Source, 0, $true, [Type]::Missing, "-") }
        catch { throw "Excel didn't open it: $($_.Exception.Message)" }
        $wb.CheckCompatibility = $false  # no compatibility checker dialog on saving as .xls
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest }
        $wb.SaveAs($dest, $fileFormats[$format])
      } catch {
        $entry.error = $_.Exception.Message
      } finally {
        if ($wb) { try { $wb.Close($false) } catch {}; [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wb) }
      }
      $result.targets += [pscustomobject]$entry
    }
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($xl) {
      try { $xl.Quit() } catch {}
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl); $xl = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
Set-Location $root
if (-not $Corpus) { throw "Pass -Corpus (or set FW_CORPUS): the private corpus with its xlsx folder." }
$corpusRoot = (Resolve-Path $Corpus).Path.TrimEnd('\')
if ($corpusRoot.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "$corpusRoot is inside the repository; the private corpus stays outside it." }
$from = Join-Path $corpusRoot "xlsx"
if (-not (Test-Path $from)) { throw "$corpusRoot has no xlsx folder." }
$Formats = @($Formats | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($f in $Formats) { if (-not $fileFormats.ContainsKey($f)) { throw "Unknown format '$f' (xls, ods)" } }
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$refManifest = Join-Path $root "tests\office\reference\manifest.json"
$ref = if (Test-Path $refManifest) { (Get-Content $refManifest -Raw | ConvertFrom-Json).excel } else { $null }
if (-not $ExcelPrinter -and $ref) { $ExcelPrinter = [string]$ref.printer }
$expectedWidth = if ($ref) { [double]$ref.standardWidth } else { 0 }
if ($ExcelPrinter) {
  $device = @(([string](Get-ItemProperty 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Windows').Device) -split ',')
  $default = if ($device.Count -ge 3) { "$($device[0]) on $($device[2])" } else { "" }
  if ($default -ne $ExcelPrinter) {
    throw "Windows' default printer is '$default', but the copies are made for the reference printer '$ExcelPrinter'. Make that the default again, or pass -ExcelPrinter."
  }
}

# The manifests: each copy by its path under the corpus ("xls/budgets/a.xls").
$manifests = @{}
foreach ($fmt in $Formats) {
  $path = Join-Path $corpusRoot "$fmt\manifest.json"
  $entries = [ordered]@{}
  $excel = $null
  if ((Test-Path $path) -and -not $Force) {
    $old = Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($e in $old.files) { $entries[$e.copy] = $e }
    $excel = $old.excel
  }
  $manifests[$fmt] = @{ Path = $path; Entries = $entries; Excel = $excel }
}
function Save-Manifests {
  foreach ($fmt in $Formats) {
    $m = $manifests[$fmt]
    New-Item -ItemType Directory -Force (Split-Path -Parent $m.Path) | Out-Null
    $files = @($m.Entries.Values | Sort-Object copy)
    Write-Json ([ordered]@{
      made = (Get-Date).ToString("yyyy-MM-dd HH:mm")
      excel = $m.Excel
      format = $fmt
      note = "Excel's SaveAs (FileFormat $($fileFormats[$fmt])) of each workbook in xlsx\ (scripts\make-sheet-copies.ps1)"
      files = $files
    }) $m.Path
  }
}
function Get-Sha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() }

$sources = @(Get-ChildItem $from -Recurse -File -Filter *.xlsx | Sort-Object FullName)
if ($Only) { $sources = @($sources | Where-Object { $p = $_.FullName.Substring($from.Length + 1); @($Only | Where-Object { $p -like "*$_*" }).Count }) }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-sheet-copies-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$made = 0; $kept = 0; $failed = @()
$n = 0
foreach ($src in $sources) {
  $n++
  $rel = $src.FullName.Substring($from.Length + 1) -replace '\\', '/'
  $sha = Get-Sha $src.FullName
  $todo = @()
  foreach ($fmt in $Formats) {
    $copy = "$fmt/" + ($rel -replace '\.xlsx$', ".$fmt")
    $dest = Join-Path $corpusRoot ($copy -replace '/', '\')
    $e = $manifests[$fmt].Entries[$copy]
    if ($e -and $e.sourceSha256 -eq $sha -and -not $e.error -and (Test-Path -LiteralPath $dest)) { $kept++; continue }
    $todo += @{ Format = $fmt; Copy = $copy; Dest = $dest }
  }
  if (-not $todo.Count) { continue }
  $mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
  try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
  $r = $null
  try {
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $targets = ($todo | ForEach-Object { "$($_.Format)>$($_.Dest)" }) -join '|'
    $args_ = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", "1",
               "-Source", "`"$($src.FullName)`"", "-Targets", "`"$targets`"", "-ResultFile", "`"$resultPath`"")
    $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $args_
    $finished = $p.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
    # Only the Excel this workbook's child started is ever ended.
    if ($r -and $r.pid) {
      $x = Get-Process -Id $r.pid -ErrorAction SilentlyContinue
      if ($x -and $x.ProcessName -eq "EXCEL" -and -not $x.WaitForExit($(if ($finished) { 15000 } else { 0 }))) { Stop-Process -Id $r.pid -Force -ErrorAction SilentlyContinue }
    }
    if (-not $finished) { $r = [pscustomobject]@{ error = "timeout after $TimeoutSeconds s"; targets = @() } }
  } finally {
    $mutex.ReleaseMutex()
  }
  if ($r -and $r.standardWidth -and $expectedWidth -gt 0 -and [Math]::Abs([double]$r.standardWidth - $expectedWidth) -gt 0.0001) {
    throw "Excel lays out differently here: a blank sheet's standard width is $($r.standardWidth), not $expectedWidth (the reference printer the default, and the Remote Desktop session connected). Stopped before $rel."
  }
  # The printer by name: Excel numbers network ports its own way ("on Ne01:" for Windows' Ne02:).
  $name = { param($p) ([string]$p -replace ' on [^ ]+$', '') }
  if ($r -and $ExcelPrinter -and $r.printer -and (& $name $r.printer) -ne (& $name $ExcelPrinter)) { throw "Excel lays out for $($r.printer), not $ExcelPrinter. Stopped before $rel." }
  foreach ($t in $todo) {
    $res = if ($r) { @($r.targets) | Where-Object { $_.format -eq $t.Format } | Select-Object -First 1 } else { $null }
    $err = if (-not $r) { "no result from Excel" } elseif ($res) { $res.error } elseif ($r.error) { $r.error } else { "not made" }
    $entry = [ordered]@{ copy = $t.Copy; copyOf = "xlsx/$rel"; sourceSha256 = $sha }
    if (-not $err -and (Test-Path -LiteralPath $t.Dest)) {
      $entry.sha256 = Get-Sha $t.Dest
      $entry.bytes = (Get-Item -LiteralPath $t.Dest).Length
      $made++
    } else {
      $entry.error = $(if ($err) { $err } else { "Excel saved nothing" })
      $failed += "$($t.Copy): $($entry.error)"
    }
    $manifests[$t.Format].Entries[$t.Copy] = [pscustomobject]$entry
    if ($r -and $r.excel) { $manifests[$t.Format].Excel = $r.excel }
  }
  Save-Manifests  # after every workbook: a stopped run keeps what it made
  $status = @($todo | ForEach-Object { $e = $manifests[$_.Format].Entries[$_.Copy]; if ($e.error) { "$($_.Format) FAILED ($($e.error))" } else { "$($_.Format) $($e.bytes) bytes" } }) -join ", "
  "[{0}/{1}] {2}: {3}" -f $n, $sources.Count, $rel, $status
}
Save-Manifests
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
"$made made, $kept kept (unchanged), $($failed.Count) failed; manifests in $corpusRoot\<format>\manifest.json"
if ($failed.Count) { $failed | ForEach-Object { "FAILED $_" }; exit 1 }
