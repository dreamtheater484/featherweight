<#
.SYNOPSIS
  S1's workbooks as Excel writes them (docs/s1-plan.md, "Corpus and measuring"): Excel opens each
  workbook in tests\office\sheet (made by tests\office\make_sheet_tests.py), adds what openpyxl
  can't write, recalculates and saves a copy in tests\office\excel:
  - every formula gets Excel's cached value (openpyxl writes none);
  - formulas.xlsx: shared formulas (a column filled at once) and dynamic arrays that spill
    (SEQUENCE, SORT, UNIQUE, FILTER, a range times 2, XLOOKUP across, LET, a spill reference and
    one blocked: #SPILL!);
  - pictures-shapes.xlsx: shapes (rectangle, rounded rectangle with text, ellipse, arrow, text
    box, line, a group);
  - charts.xlsx: sparklines (line, column, win/loss).
  (Threaded comments are written by make_sheet_tests.py itself: Excel 2021's object model can't
  add them.) Then make_sheet_tests.py --scrub takes this PC's traces out of the copies: the
  folder Excel saved them in (x15ac:absPath), and the author and any threaded comments' person
  (a name and an account id) become "Featherweight tests".

  Excel: always a new, separate instance (COM starts one per call; the script checks that the
  Excel it talks to is a process it started), hidden, with alerts, events and link updates off
  and macros forced off. Only that Excel is ever ended, and it always is. One Office automation
  at a time on this PC (the mutex reference-renders.ps1 and word-open-check.ps1 share). A
  workbook Excel would have to repair is refused (Excel doesn't open it with alerts off), and
  the script says so instead of saving a repaired copy.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-sheet-files.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-sheet-files.ps1 -Only formulas.xlsx
#>
param(
  [string]$From = "tests\office\sheet",
  [string]$To = "tests\office\excel",
  [string[]]$Only,
  # The reference printer. Excel lays out and saves for Windows' default printer as it is when Excel
  # starts (it decides row heights and column widths), so the script stops unless that is this
  # printer. By default the one tests\office\reference\manifest.json records.
  [string]$ExcelPrinter
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$from = Join-Path $root $From
$to = Join-Path $root $To
New-Item -ItemType Directory -Force $to | Out-Null
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { throw "No venv at $python (tests\office\README.md, Regenerating)." }

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FwExcelProcess {
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static int Of(int hwnd) { uint pid; GetWindowThreadProcessId(new IntPtr(hwnd), out pid); return (int)pid; }
}
"@

# Starts a new, separate Excel. Returns @{ App; Pid }, or throws without touching an Excel this
# script didn't start.
function Start-OwnExcel {
  $before = @(Get-Process EXCEL -ErrorAction SilentlyContinue | ForEach-Object Id)
  $since = Get-Date
  $xl = New-Object -ComObject Excel.Application
  $xlPid = [FwExcelProcess]::Of([int]$xl.Hwnd)
  $proc = Get-Process -Id $xlPid -ErrorAction SilentlyContinue
  if (-not $proc -or $proc.ProcessName -ne "EXCEL" -or $before -contains $xlPid -or $proc.StartTime -lt $since.AddSeconds(-5)) {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl)
    throw "COM attached to an Excel this script didn't start (process $xlPid); nothing was done to it"
  }
  $xl.Visible = $false
  $xl.DisplayAlerts = $false
  $xl.EnableEvents = $false
  $xl.AskToUpdateLinks = $false
  $xl.ScreenUpdating = $false
  $xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
  @{ App = $xl; Pid = $xlPid }
}

function Stop-OwnExcel($excel) {
  if (-not $excel) { return }
  try { $excel.App.Quit() } catch {}
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($excel.App)
  [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  $p = Get-Process -Id $excel.Pid -ErrorAction SilentlyContinue
  if ($p -and $p.ProcessName -eq "EXCEL" -and -not $p.WaitForExit(15000)) { Stop-Process -Id $excel.Pid -Force -ErrorAction SilentlyContinue }
}

# Excel's colors are BGR.
function Get-Bgr([int]$rgb) { (($rgb -band 0xFF) -shl 16) -bor ($rgb -band 0xFF00) -bor (($rgb -shr 16) -band 0xFF) }

function Add-ExcelOnly($wb, [string]$name) {
  switch ($name) {
    "formulas.xlsx" {
      $ws = $wb.Worksheets.Item("Shared")
      $ws.Range("A2").Value2 = "B2:B20 =E2*2, C2:C20 =B2+`$M`$2, D2:D10 ='Other sheet'!`$A`$1*ROW(), filled at once (shared)"
      $ws.Range("B2:B20").Formula = '=E2*2'
      $ws.Range("C2:C20").Formula = '=B2+$M$2'
      $ws.Range("D2:D10").Formula = "='Other sheet'!`$A`$1*ROW()"
      $ws = $wb.Worksheets.Item("Dynamic arrays")
      $cases = @(
        @("A2", "B2", "=SEQUENCE(5)"), @("A8", "B8", "=SORT(F2:F9)"), @("A17", "B17", "=UNIQUE(F2:F9)"),
        @("A25", "B25", "=FILTER(E2:E9,E2:E9>10)"), @("A30", "B30", "=E2:E5*2"), @("A35", "B35", "=SEQUENCE(3)"),
        @("A38", "B38", '=XLOOKUP("c",L2:L6,L2:N6)'), @("A40", "B40", "=SEQUENCE(2,3)"), @("A43", "B43", "=@E2:E9"),
        @("A45", "B45", "=SUM(B2#)"), @("A47", "B47", "=LET(x,2,y,x*3,x+y)"), @("A49", "B49", '=FILTER(E2:E9,E2:E9>1E9,"none")'),
        @("A51", "B51", "=FILTER(E2:E9,E2:E9>1E9)"))
      foreach ($c in $cases) {
        $ws.Range($c[0]).Value2 = "'" + $c[2]
        $ws.Range($c[1]).Formula2 = $c[2]
      }
      $ws.Range("B36").Value2 = "in the way"  # B35's three results can't spill: #SPILL!
    }
    "pictures-shapes.xlsx" {
      $ws = $wb.Worksheets.Item("Shapes")
      $r = $ws.Shapes.AddShape(1, 20, 40, 140, 70)  # msoShapeRectangle
      $r.Name = "Rectangle"
      $r.Fill.ForeColor.RGB = Get-Bgr 0x4472C4
      $rr = $ws.Shapes.AddShape(5, 190, 40, 160, 70)  # msoShapeRoundedRectangle
      $rr.Name = "Rounded rectangle"
      $rr.TextFrame2.TextRange.Text = "Text in a shape"
      $rr.Fill.ForeColor.RGB = Get-Bgr 0xFFC000
      $o = $ws.Shapes.AddShape(9, 20, 140, 120, 80)  # msoShapeOval
      $o.Name = "Ellipse"
      $a = $ws.Shapes.AddShape(33, 190, 150, 140, 60)  # msoShapeRightArrow
      $a.Name = "Arrow"
      $a.Rotation = 20
      $t = $ws.Shapes.AddTextbox(1, 380, 40, 180, 90)  # msoTextOrientationHorizontal
      $t.Name = "Text box"
      $t.TextFrame2.TextRange.Text = "A text box, with a second paragraph.`rSecond paragraph."
      $l = $ws.Shapes.AddLine(20, 250, 330, 300)
      $l.Name = "Line"
      $l.Line.Weight = 2.5
      $g1 = $ws.Shapes.AddShape(1, 380, 160, 60, 40); $g1.Name = "Group part 1"
      $g2 = $ws.Shapes.AddShape(9, 450, 170, 60, 40); $g2.Name = "Group part 2"
      $grp = $ws.Shapes.Range([object[]]@("Group part 1", "Group part 2")).Group()
      $grp.Name = "Group"
    }
    "charts.xlsx" {
      $ws = $wb.Worksheets.Item("Sparklines")
      $ws.Range("A1").Value2 = "Sparklines: line (H2:H4), column (I2:I4), win/loss (J2:J4)"
      $rows = @(@("North", 10, 13, 16, 19, 22, 25), @("South", 14, 13, 12, 11, 10, 9), @("Change", 3, -2, 5, -1, -4, 2))
      $grid = New-Object 'object[,]' 3, 7
      for ($i = 0; $i -lt 3; $i++) { for ($j = 0; $j -lt 7; $j++) { $grid[$i, $j] = $rows[$i][$j] } }
      $ws.Range("A2:G4").Value2 = $grid
      [void]$ws.Range("H2:H4").SparklineGroups.Add(1, "Sparklines!B2:G4")  # xlSparkLine
      [void]$ws.Range("I2:I4").SparklineGroups.Add(2, "Sparklines!B2:G4")  # xlSparkColumn
      [void]$ws.Range("J2:J4").SparklineGroups.Add(3, "Sparklines!B2:G4")  # xlSparkColumnStacked100 (win/loss)
    }
  }
}

$files = @(Get-ChildItem $from -Filter *.xlsx | Sort-Object Name)
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # powershell -File passes "a,b" as one string
if ($Only) { $files = @($files | Where-Object { $Only -contains $_.Name }) }
if (-not $files.Count) { throw "No workbooks in $from. Run tests\office\make_sheet_tests.py first." }
if (-not $ExcelPrinter) {
  $refManifest = Join-Path $root "tests\office\reference\manifest.json"
  if (Test-Path $refManifest) { $ExcelPrinter = [string](Get-Content $refManifest -Raw -Encoding UTF8 | ConvertFrom-Json).excel.printer }
}
if ($ExcelPrinter) {
  $device = @(([string](Get-ItemProperty 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Windows').Device) -split ',')
  $default = if ($device.Count -ge 3) { "$($device[0]) on $($device[2])" } else { "" }
  if ($default -ne $ExcelPrinter) {
    throw "Windows' default printer is '$default', but the copies are made for the reference printer '$ExcelPrinter'. Make that the default again (with 'Let Windows manage my default printer' on, print something on it), or pass -ExcelPrinter."
  }
}

$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
$excel = $null
$made = @(); $failed = @()
try {
  $excel = Start-OwnExcel
  $version = "Excel $($excel.App.Version) (build $($excel.App.Build)), for $($excel.App.ActivePrinter)"
  if ($ExcelPrinter -and [string]$excel.App.ActivePrinter -ne $ExcelPrinter) { throw "Excel lays out for $($excel.App.ActivePrinter), not $ExcelPrinter" }
  # Excel also lays out (and saves row heights) by the display: a disconnected Remote Desktop session
  # differs from a connected one. A blank sheet's standard width must be the references' (8.43).
  $refManifest = Join-Path $root "tests\office\reference\manifest.json"
  $expected = if (Test-Path $refManifest) { [double](Get-Content $refManifest -Raw -Encoding UTF8 | ConvertFrom-Json).excel.standardWidth } else { 0 }
  $blank = $excel.App.Workbooks.Add()
  $width = [Math]::Round([double]$blank.Worksheets.Item(1).StandardWidth, 4)
  $blank.Close($false)
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($blank)
  if ($expected -gt 0 -and [Math]::Abs($width - $expected) -gt 0.0001) {
    throw "Excel lays out differently here: a blank sheet's standard width is $width, not $expected. Make the copies with the reference printer the default and the Remote Desktop session connected."
  }
  foreach ($f in $files) {
    $dest = Join-Path $to $f.Name
    $wb = $null
    try {
      try { $wb = $excel.App.Workbooks.Open($f.FullName, 0, $true) }  # never update links; read-only
      catch { throw "Excel refused it (it would need repair): $($_.Exception.Message)" }
      Add-ExcelOnly $wb $f.Name
      $excel.App.CalculateFull()
      if (Test-Path $dest) { Remove-Item $dest }
      $wb.SaveAs($dest, 51)  # xlOpenXMLWorkbook
      $made += $dest
      "  {0} -> {1}" -f $f.Name, (Resolve-Path -Relative $dest)
    } catch {
      $failed += "$($f.Name): $($_.Exception.Message)"
      "  {0} FAILED: {1} (line {2})" -f $f.Name, $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber
    } finally {
      if ($wb) { try { $wb.Close($false) } catch {}; [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wb) }
    }
  }
} finally {
  Stop-OwnExcel $excel
  $mutex.ReleaseMutex()
}
if ($made.Count) {
  & $python (Join-Path $root "tests\office\make_sheet_tests.py") --scrub @made
  if ($LASTEXITCODE -ne 0) { throw "scrubbing the copies failed" }
}
"$($made.Count) of $($files.Count) saved by $version in $To."
if ($failed.Count) { $failed | ForEach-Object { "FAILED $_" }; exit 1 }
