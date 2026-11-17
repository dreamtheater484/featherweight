<#
.SYNOPSIS
  S2's "build from scratch" check (docs\s2-plan.md): the household budget and the small invoice
  (tests\office\build\*.steps) are typed into a blank workbook, step by step, by Excel and by
  Featherweight, and Excel must show the same in both.
  1. Excel plays the steps in a new workbook (typing through FormulaLocal, which converts what's
     typed as typing in the cell does: numbers, dates, percentages, currency, formulas in the
     region's form), saves it (<out>\excel\<name>.xlsx) and dumps what it shows (<name>.json).
  2. With -Saved: Featherweight's saved copies (<saved>\<name>.xlsx, made by playing the same
     steps through the grid's commands) are opened in Excel and dumped the same way, and compared
     with Excel's own: per cell its formula, value, text (Range.Text), number format, font, fill,
     alignment and borders; per sheet its name, column widths, merges and frozen panes. They must
     also open without a repair (scripts\excel-open-check.ps1).
  Excel automation as in scripts\excel-open-check.ps1: a new Excel per workbook, hidden, alerts,
  events and macros off; only the Excel it started is ever ended; one at a time (a global mutex).
  The exit code is 1 when anything differs or failed.

  Steps (one per line; # starts a comment; cells and ranges in A1 form; text in double quotes):
    sheet-name "Name"              rename the active sheet
    enter B5 "12,5"                select B5, type, Enter (in the region's form)
    autosum B7 "=SUM(B5:B6)"       Alt+= on B7, then Enter; the quoted formula is what Excel proposes
    fill-right B5:M5 / fill-down / fill-series B3:M3 (the fill handle: Jan, Feb, ...)
    format A1:D1 <what>            bold, italic, underline, size=<pt>, font=<name>, color=<RRGGBB>,
                                   fill=<RRGGBB>, numfmt=<code>, border=<top|bottom|double-top|none>,
                                   align=<left|center|right>, wrap
    width A 22 / width B:N 11      column widths in characters
    merge A1:D1, freeze B4, save

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-from-scratch.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-from-scratch.ps1 -Saved build\from-scratch\featherweight
#>
param(
  [string[]]$Steps,
  [string]$Out = "build\from-scratch",
  [string]$Saved,
  [int]$TimeoutSeconds = 180,
  # Internal: play one steps file (-Child play) or dump one workbook (-Child dump) in this process.
  [string]$Child, [string]$Source, [string]$Target, [string]$ResultFile
)

$ErrorActionPreference = "Stop"

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class FwBuildWindows {
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  public static int ProcessOf(int hwnd) { uint pid; GetWindowThreadProcessId((IntPtr)hwnd, out pid); return (int)pid; }
}
"@

function Stop-Started($ids, [int]$waitMs = 15000) {
  foreach ($id in @($ids)) {
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne "EXCEL") { continue }
    if (-not $proc.WaitForExit($waitMs)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
}

# ---- What Excel shows of a workbook: per sheet and per cell of its used range.
function Get-Dump($xl, $wb) {
  $sheets = @()
  foreach ($ws in $wb.Worksheets) {
    $ws.Activate()
    $win = $wb.Windows.Item(1)
    $used = $ws.UsedRange
    $widths = [ordered]@{}
    for ($c = 1; $c -le $used.Column + $used.Columns.Count - 1; $c++) {
      $name = ($ws.Cells.Item(1, $c).Address($false, $false)) -replace '\d', ''
      $widths[$name] = [Math]::Round([double]$ws.Columns.Item($c).ColumnWidth, 2)
    }
    $merges = New-Object Collections.Generic.HashSet[string]
    $cells = @()
    foreach ($cell in $used.Cells) {
      if ($cell.MergeCells) { [void]$merges.Add($cell.MergeArea.Address($false, $false)) }
      $formula = [string]$cell.Formula2
      $v = $cell.Value2
      $numfmt = Get-NumberFormat $cell
      if ($formula -eq "" -and $null -eq $v -and $numfmt -eq "General" -and -not $cell.Font.Bold -and $cell.Interior.Pattern -eq -4142 -and
          $cell.Borders.Item(8).LineStyle -eq -4142 -and $cell.Borders.Item(9).LineStyle -eq -4142) { continue }  # nothing in it
      $cells += [ordered]@{
        ref = $cell.Address($false, $false)
        formula = $formula
        value = if ($v -is [double]) { [Math]::Round($v, 10) } else { $v }
        text = [string]$cell.Text
        numfmt = $numfmt
        bold = [bool]$cell.Font.Bold; italic = [bool]$cell.Font.Italic; underline = [int]$cell.Font.Underline
        size = [double]$cell.Font.Size; font = [string]$cell.Font.Name; color = [int]$cell.Font.Color
        fill = if ($cell.Interior.Pattern -eq -4142) { $null } else { [int]$cell.Interior.Color }
        align = [int]$cell.HorizontalAlignment; wrap = [bool]$cell.WrapText
        top = "$($cell.Borders.Item(8).LineStyle)/$($cell.Borders.Item(8).Weight)"
        bottom = "$($cell.Borders.Item(9).LineStyle)/$($cell.Borders.Item(9).Weight)"
      }
    }
    $sheets += [ordered]@{
      name = $ws.Name
      freeze = if ($win.FreezePanes) { "$($win.SplitRow),$($win.SplitColumn)" } else { "" }
      widths = $widths
      merges = @($merges | Sort-Object)
      cells = $cells
    }
  }
  $sheets
}

# Range.NumberFormat through PowerShell's COM calls takes and gives the region's codes ("dd/mm/jjjj",
# "Standaard" here: the call carries the user's locale). With en-US on the call it is the invariant
# code files store, as for Featherweight's.
$enUS = [Globalization.CultureInfo]::GetCultureInfo("en-US")
function Set-NumberFormat($range, [string]$code) {
  [void][System.__ComObject].InvokeMember("NumberFormat", [Reflection.BindingFlags]::SetProperty, $null, $range, @($code), $null, $enUS, $null)
}
function Get-NumberFormat($range) {
  [string][System.__ComObject].InvokeMember("NumberFormat", [Reflection.BindingFlags]::GetProperty, $null, $range, $null, $null, $enUS, $null)
}

# The color Excel takes (BGR) for RRGGBB.
function ConvertTo-Bgr([string]$rgb) {
  $n = [Convert]::ToInt32($rgb, 16)
  (($n -band 0xFF) -shl 16) -bor ($n -band 0xFF00) -bor (($n -shr 16) -band 0xFF)
}

# ---- Child
if ($Child) {
  $started = New-Object Collections.Generic.List[int]
  $result = [ordered]@{ pid = $started }
  $xl = $null; $wb = $null
  try {
    $before = @(Get-Process EXCEL -ErrorAction SilentlyContinue | ForEach-Object Id)
    $xl = New-Object -ComObject Excel.Application
    $xlPid = [FwBuildWindows]::ProcessOf([int]$xl.Hwnd)
    if ($before -contains $xlPid) {
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($xl); $xl = $null
      throw "COM attached to an Excel this script didn't start (process $xlPid)"
    }
    $started.Add($xlPid)
    Write-Json $result $ResultFile
    $xl.AutomationSecurity = 3
    $xl.Visible = $false
    $xl.DisplayAlerts = $false
    $xl.AskToUpdateLinks = $false
    $xl.EnableEvents = $false
    $result.app = "Excel $($xl.Version) (build $($xl.Build))"
    $result.locale = "decimal '$($xl.International(3))', list '$($xl.International(5))'"
    if ($Child -eq "dump") {
      $wb = $xl.Workbooks.Open($Source, 0, $true)
      $result.sheets = Get-Dump $xl $wb
      $wb.Close($false)
    } else {
      $wb = $xl.Workbooks.Add()
      while ($wb.Worksheets.Count -gt 1) { $wb.Worksheets.Item($wb.Worksheets.Count).Delete() }
      $ws = $wb.Worksheets.Item(1)
      $ws.Activate()
      $notes = @()
      $n = 0
      foreach ($line in Get-Content -LiteralPath $Source -Encoding UTF8) {
        $n++
        $line = $line.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        if ($line -notmatch '^(\S+)\s*(.*)$') { throw "line ${n}: can't read '$line'" }
        $op = $Matches[1]; $rest = $Matches[2]
        $quoted = if ($rest -match '"((?:[^"\\]|\\.)*)"') { $Matches[1] -replace '\\(.)', '$1' } else { $null }
        $first = ($rest -split '\s+')[0]
        try {
          switch ($op) {
            "sheet-name" { $ws.Name = $quoted }
            "enter" { $ws.Range($first).FormulaLocal = $quoted }
            "autosum" {
              # Excel's AutoSum (ExecuteMso) leaves a hidden Excel editing the cell, refusing every
              # call after it; so Excel types the formula the step says AutoSum proposes, and
              # Featherweight's AutoSum must propose that formula when it plays the step.
              $ws.Range($first).FormulaLocal = $quoted
            }
            "fill-right" { [void]$ws.Range($first).FillRight() }
            "fill-down" { [void]$ws.Range($first).FillDown() }
            "fill-series" {
              $r = $ws.Range($first)
              [void]$r.Cells.Item(1, 1).AutoFill($r, 0)  # xlFillDefault: Jan, Feb, ...
            }
            "format" {
              $r = $ws.Range($first)
              $what = $rest.Substring($first.Length).Trim()
              switch -regex ($what) {
                '^bold$' { $r.Font.Bold = $true }
                '^italic$' { $r.Font.Italic = $true }
                '^underline$' { $r.Font.Underline = 2 }
                '^size=(.+)$' { $r.Font.Size = [double]$Matches[1] }
                '^font=(.+)$' { $r.Font.Name = $Matches[1] }
                '^color=([0-9A-Fa-f]{6})$' { $r.Font.Color = ConvertTo-Bgr $Matches[1] }
                '^fill=([0-9A-Fa-f]{6})$' { $r.Interior.Color = ConvertTo-Bgr $Matches[1] }
                '^numfmt=(.+)$' { Set-NumberFormat $r $Matches[1] }
                '^border=top$' { $r.Borders.Item(8).LineStyle = 1; $r.Borders.Item(8).Weight = 2 }
                '^border=bottom$' { $r.Borders.Item(9).LineStyle = 1; $r.Borders.Item(9).Weight = 2 }
                '^border=double-top$' { $r.Borders.Item(8).LineStyle = -4119 }
                '^border=none$' { foreach ($b in 7..10) { $r.Borders.Item($b).LineStyle = -4142 } }
                '^align=left$' { $r.HorizontalAlignment = -4131 }
                '^align=center$' { $r.HorizontalAlignment = -4108 }
                '^align=right$' { $r.HorizontalAlignment = -4152 }
                '^wrap$' { $r.WrapText = $true }
                default { throw "line ${n}: unknown format '$what'" }
              }
            }
            "width" {
              $cols = $first; if ($cols -notmatch ':') { $cols = "${cols}:$cols" }
              $ws.Range($cols).ColumnWidth = [double](($rest -split '\s+')[1])
            }
            "merge" { [void]$ws.Range($first).Merge() }
            "freeze" {
              $win = $wb.Windows.Item(1)
              $win.FreezePanes = $false
              $cell = $ws.Range($first)
              $win.SplitRow = $cell.Row - 1
              $win.SplitColumn = $cell.Column - 1
              $win.FreezePanes = $true
            }
            "save" { $wb.SaveAs($Target, 51) }  # xlOpenXMLWorkbook
            default { throw "line ${n}: unknown step '$op'" }
          }
        } catch {
          throw "line ${n} ($line): $($_.Exception.Message)"
        }
      }
      $result.notes = $notes
      # What Excel shows of its own saved copy, reopened, as Featherweight's copy is shown (Excel's
      # save changes some things: a custom "dd/mm/yyyy" that is the region's short date comes back
      # as the built-in date format).
      $wb.Close($false)
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($wb)
      $wb = $xl.Workbooks.Open($Target, 0, $true)
      $result.sheets = Get-Dump $xl $wb
      $wb.Close($false)
    }
    $result.ok = $true
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($xl) {
      try { $xl.Quit() } catch {}
      foreach ($o in $wb, $xl) { if ($o) { try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } catch {} } }
      $wb = $null; $xl = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
    Stop-Started $started
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
$root = Split-Path -Parent $PSScriptRoot
if (-not $Steps) { $Steps = @(Get-ChildItem (Join-Path $root "tests\office\build") -Filter *.steps | ForEach-Object FullName) }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force (Join-Path $Out "excel") | Out-Null
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-build-from-scratch-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Invoke-Child([string]$mode, [string]$source, [string]$target) {
  $resultPath = Join-Path $tmp "result.json"
  if (Test-Path $resultPath) { Remove-Item $resultPath }
  $args_ = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", $mode,
             "-Source", "`"$source`"", "-ResultFile", "`"$resultPath`"")
  if ($target) { $args_ += @("-Target", "`"$target`"") }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $args_
  $finished = $p.WaitForExit($TimeoutSeconds * 1000)
  if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
  if ($r) { Stop-Started @($r.pid) $(if ($finished) { 15000 } else { 0 }) }
  if (-not $finished) { return [pscustomobject]@{ error = "timeout" } }
  if (-not $r) { return [pscustomobject]@{ error = "no result" } }
  $r
}

# What differs between Excel's own workbook and Featherweight's, as Excel shows them.
function Compare-Dumps($a, $b) {
  $diffs = @()
  $sa = @($a.sheets); $sb = @($b.sheets)
  if ($sa.Count -ne $sb.Count) { $diffs += "sheets: Excel $($sa.Count), Featherweight $($sb.Count)" }
  for ($i = 0; $i -lt [Math]::Min($sa.Count, $sb.Count); $i++) {
    $x = $sa[$i]; $y = $sb[$i]
    foreach ($k in "name", "freeze") { if ("$($x.$k)" -ne "$($y.$k)") { $diffs += "sheet $($i + 1) ${k}: Excel '$($x.$k)', Featherweight '$($y.$k)'" } }
    if ((@($x.merges) -join ",") -ne (@($y.merges) -join ",")) { $diffs += "$($x.name) merges: Excel $(@($x.merges) -join ','), Featherweight $(@($y.merges) -join ',')" }
    foreach ($p in $x.widths.PSObject.Properties) {
      $w = $y.widths.($p.Name)
      if ($null -eq $w -or [Math]::Abs([double]$w - [double]$p.Value) -gt 0.01) { $diffs += "$($x.name) column $($p.Name) width: Excel $($p.Value), Featherweight $w" }
    }
    $cy = @{}; foreach ($c in $y.cells) { $cy[$c.ref] = $c }
    $refs = @($x.cells | ForEach-Object ref) + @($y.cells | ForEach-Object ref) | Select-Object -Unique
    $cx = @{}; foreach ($c in $x.cells) { $cx[$c.ref] = $c }
    foreach ($ref in $refs) {
      $c1 = $cx[$ref]; $c2 = $cy[$ref]
      if (-not $c1 -or -not $c2) { $diffs += "$($x.name)!${ref}: only in $(if ($c1) { 'Excel' } else { 'Featherweight' })'s"; continue }
      foreach ($k in "formula", "value", "text", "numfmt", "bold", "italic", "underline", "size", "font", "color", "fill", "align", "wrap", "top", "bottom") {
        if ("$($c1.$k)" -ne "$($c2.$k)") { $diffs += "$($x.name)!$ref ${k}: Excel '$($c1.$k)', Featherweight '$($c2.$k)'" }
      }
    }
  }
  $diffs
}

$bad = 0
$report = @()
foreach ($s in $Steps) {
  $name = [IO.Path]::GetFileNameWithoutExtension($s)
  $xlsx = Join-Path $Out "excel\$name.xlsx"
  if (Test-Path $xlsx) { Remove-Item $xlsx }
  $r = Invoke-Child "play" (Resolve-Path $s).Path $xlsx
  if ($r.error) { "[FAIL] $name in Excel: $($r.error)"; $bad++; continue }
  Write-Json ([ordered]@{ app = $r.app; locale = $r.locale; notes = $r.notes; sheets = $r.sheets }) (Join-Path $Out "excel\$name.json")
  $cellCount = (@($r.sheets) | ForEach-Object { @($_.cells).Count } | Measure-Object -Sum).Sum
  "[ ok ] $name in Excel: $cellCount cells, saved as $xlsx ($($r.locale))"
  foreach ($note in @($r.notes)) { "       $note" }
  if ($Saved) {
    $ours = Join-Path $Saved "$name.xlsx"
    if (-not (Test-Path $ours)) { "[FAIL] $($name): Featherweight's copy $ours is missing"; $bad++; continue }
    $d = Invoke-Child "dump" (Resolve-Path $ours).Path $null
    if ($d.error) { "[FAIL] $($name): Excel didn't open Featherweight's copy: $($d.error)"; $bad++; continue }
    Write-Json ([ordered]@{ sheets = $d.sheets }) (Join-Path $Out "$name.featherweight.json")
    $diffs = @(Compare-Dumps $r $d)
    $report += [ordered]@{ name = $name; differences = $diffs }
    if ($diffs.Count) { $bad++; "[FAIL] $($name): $($diffs.Count) differences"; $diffs | Select-Object -First 20 | ForEach-Object { "       $_" } }
    else { "[ ok ] $($name): Featherweight's workbook shows in Excel as Excel's own" }
  }
}
$mutex.ReleaseMutex()
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($Saved) {
  Write-Json $report (Join-Path $Out "compare.json")
  # Featherweight's copies must also open without a repair.
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "excel-open-check.ps1") $Saved -Json (Join-Path $Out "excel-open.json")
  if ($LASTEXITCODE) { $bad++ }
}
exit $(if ($bad) { 1 } else { 0 })
