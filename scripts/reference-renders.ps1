<#
.SYNOPSIS
  Makes the reference renders (plan section 10): Office exports each corpus file to PDF, on the
  Office reference PC (decision 19). The manifest records:
  - the Office build
  - the page count
  - the fonts Office actually drew with, from each PDF
  - the source file's hash
  - for a format copy (D5: doc\, odt\, dotx\, protected\, made by scripts\make-test-files.ps1),
    "copyOf": the original's source in the same manifest, so fidelity counts each document once
    and measures the copies as a set of their own
  - for a password-protected copy, "password": its test password (Get-TestPassword)
  `fwtool compare` (D1) measures Featherweight against these PDFs.

  Office is driven by automation and doesn't show a window:
  - Files are opened read-only, with macros off, links not updated, and nothing added to Office's
    recent files. Nothing is saved.
  - Word runs in safe mode (/safe), so no add-ins load and the session's settings (links not
    updated at open) aren't saved.
  - Each file gets its own Office process, which always ends before the next file starts, also
    when the file fails or hangs it (for example with a repair prompt). The manifest then says
    why there is no PDF.

  The public corpus: the PDFs and manifest.json go in tests\office\reference\. Each folder that is
  rendered replaces its PDFs and its manifest entries; the other folders' entries are kept. Run it
  again whenever a file or Office changes noticeably.

  The private corpus (-Corpus, usually $env:FW_CORPUS): every Office file in its docx, xlsx and
  pptx folders, which hold only the files that passed the safety screen (scripts\fetch-corpus.ps1);
  never its quarantine folder. The PDFs and manifest.json go in <corpus>\reference\, or in -Out,
  which must be outside the repository too. Files whose hash hasn't changed since the last run
  keep their PDF, so adding documents only renders the new ones; -Force renders them all again.
  Nothing in the corpus or the output folder is deleted.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Folders tests\office\layout
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Corpus $env:FW_CORPUS
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1 -Folders tests\office\sheet,tests\office\excel -PrintPdf conditional-formats.xlsx,charts.xlsx
#>
param(
  [string[]]$Folders = @("tests\office\generated", "tests\office\edge", "tests\office\layout"),
  [string]$Out = "tests\office\reference",
  [string]$Corpus,
  [switch]$Force,
  [int]$TimeoutSeconds = 120,
  # Workbooks take longer: two opens, and each non-empty cell's text is read one by one.
  [int]$ExcelTimeoutSeconds = 900,
  # At most this many cells' text per workbook (about 1 ms each); the rest is left out and the
  # cells file says so.
  [int]$MaxCells = 100000,
  # Workbooks (file names, or sources such as excel/charts.xlsx) that Excel also prints through
  # Microsoft Print to PDF, into <name>.print.pdf beside the export. On this PC Excel's PDF export
  # stops drawing a page at a data bar, an icon set or a sparkline (the rest of the page, or all of
  # it, is left empty); printing draws them. With "Let Windows manage my default printer" on,
  # printing makes Microsoft Print to PDF Windows' default printer until the next print elsewhere,
  # and workbooks can't be rendered again until the reference printer is the default (-ExcelPrinter).
  # So only with that setting off, or with whoever owns the PC agreeing.
  [string[]]$PrintPdf = @(),
  # The reference printer. Excel lays out for Windows' default printer as it is when Excel starts
  # (page breaks, column widths, row heights) and doesn't lay out again for another one, so
  # workbooks are rendered only when that default is this printer. By default the one
  # tests\office\reference\manifest.json records.
  [string]$ExcelPrinter,
  # Excel's layout also moves with the display: a disconnected Remote Desktop session lays out
  # differently from a connected one (a blank sheet's standard width 8.29 instead of 8.43). Before
  # anything is rendered, a blank sheet's standard width must be this; by default the manifest's
  # excel.standardWidth (8.43 for the references made on 16 Nov 2026, the session connected).
  [double]$ExcelStandardWidth = 0,
  # Internal: export one file in this process (the parent runs one child per file).
  [string]$Child, [string]$Source, [string]$SourceName, [string]$Pdf, [string]$ResultFile, [string]$Cells, [switch]$CellFormats,
  [string]$PrintTo, [switch]$Probe
)

$ErrorActionPreference = "Stop"
# powershell -File passes "a,b" as one string.
$Folders = @($Folders | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$PrintPdf = @($PrintPdf | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$apps = @{
  Word = @{ Exe = "WINWORD"; ProgId = "Word.Application"; Ext = @(".docx", ".docm", ".dotx", ".dotm", ".doc", ".odt") }
  Excel = @{ Exe = "EXCEL"; ProgId = "Excel.Application"; Ext = @(".xlsx", ".xlsm", ".xltx", ".xltm") }
  PowerPoint = @{ Exe = "POWERPNT"; ProgId = "PowerPoint.Application"; Ext = @(".pptx", ".pptm", ".potx", ".potm", ".ppsx", ".ppsm") }
}

# The test password of a password-protected copy (D5): files in a protected\ folder, made by
# scripts\make-test-files.ps1 with "featherweight", except msoffcrypto-tool's ECMA-376 standard
# encryption file and its decrypted twin (tests\office\README.md). Null when it has none. These are
# test values, not secrets; the manifest records them so fwtool compare and the torture test can open
# the copies.
function Get-TestPassword([string]$path) {
  if ($path -notmatch '(^|[\\/])protected[\\/]') { return $null }
  $leaf = Split-Path -Leaf $path
  if ($leaf -eq "ecma376-standard.docx") { return "Password1234_" }
  if ($leaf -eq "ecma376-standard-plain.docx") { return $null }
  "featherweight"
}

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# Ends the Office processes a child started, once they've had $waitMs to quit on their own. Only
# processes with the app's name are touched, so a reused process id is left alone.
function Stop-Started($ids, [string]$exe, [int]$waitMs = 15000) {
  foreach ($id in @($ids)) {
    $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $proc -or $proc.ProcessName -ne $exe) { continue }
    if (-not $proc.WaitForExit($waitMs)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
}

# Whether a window of this process, shown or hidden, has `text` in its title: how a Word child knows
# that COM attached to the Word it started (see the child).
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class FwOfficeWindows {
  delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
  public static bool HasTitle(int pid, string text) {
    bool found = false;
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      var s = new StringBuilder(512); GetWindowText(h, s, 512);
      if ((int)owner == pid && s.ToString().Contains(text)) found = true;
      return !found;
    }, IntPtr.Zero);
    return found;
  }
  public static int ProcessOf(int hwnd) { uint pid; GetWindowThreadProcessId(new IntPtr(hwnd), out pid); return (int)pid; }
}
"@

# ---- Excel's view of a workbook (S1), written beside its PDF as <file>.cells.json for
# `fwtool compare`. Per sheet: the used range, hidden rows and columns, column widths, every
# non-empty cell's displayed text (Range.Text, "####" included) and raw value (Value2), and every
# formula (Formula2) with three values: the cached one in the file (read with calculation manual,
# so nothing is recalculated), the one Excel shows after opening it normally (what it recalculates
# on open), and Excel's own full recalculation (CalculateFull). Also the locale Excel formats with.

$xlErrors = @{ 2000 = "#NULL!"; 2007 = "#DIV/0!"; 2015 = "#VALUE!"; 2023 = "#REF!"; 2029 = "#NAME?"; 2036 = "#NUM!"; 2042 = "#N/A";
  2043 = "#GETTING_DATA"; 2045 = "#SPILL!"; 2046 = "#CONNECT!"; 2047 = "#BLOCKED!"; 2048 = "#UNKNOWN!"; 2049 = "#FIELD!"; 2050 = "#CALC!" }
$invariant = [Globalization.CultureInfo]::InvariantCulture

function ConvertTo-JsonString([string]$s) {
  $b = New-Object Text.StringBuilder ($s.Length + 2)
  [void]$b.Append('"')
  foreach ($ch in $s.ToCharArray()) {
    switch ($ch) {
      '"' { [void]$b.Append('\"') } '\' { [void]$b.Append('\\') } "`n" { [void]$b.Append('\n') } "`r" { [void]$b.Append('\r') } "`t" { [void]$b.Append('\t') }
      default { if ([int]$ch -lt 0x20) { [void]$b.Append(('\u{0:x4}' -f [int]$ch)) } else { [void]$b.Append($ch) } }
    }
  }
  [void]$b.Append('"')
  $b.ToString()
}

# A cell value from Value2 as JSON: numbers (round-trip), text, true/false, null, and errors
# (COM gives them as an Int32 code) as {"e": "#N/A"}.
function ConvertTo-JsonValue($v) {
  if ($null -eq $v) { return "null" }
  if ($v -is [bool]) { if ($v) { return "true" } else { return "false" } }
  if ($v -is [double]) { return $v.ToString("R", $invariant) }
  if ($v -is [int]) { $code = $v -band 0xFFFF; $name = $xlErrors[$code]; if (-not $name) { $name = "#ERR$code" }; return '{"e": "' + $name + '"}' }
  if ($v -is [string]) { return ConvertTo-JsonString $v }
  ConvertTo-JsonString ([string]$v)
}

function Get-ColumnName([int]$c) {
  $s = ""
  while ($c -gt 0) { $m = ($c - 1) % 26; $s = [char](65 + $m) + $s; $c = [math]::Floor(($c - 1) / 26) }
  $s
}

# A range's Value2 or Formula2 as a 1-based grid, also for a single cell (COM gives a scalar).
function Get-Grid($range, [string]$property) {
  $v = $range.$property
  if ($v -is [Array]) { return , $v }
  $g = [Array]::CreateInstance([object], [int[]]@(1, 1), [int[]]@(1, 1))
  $g.SetValue($v, 1, 1)
  , $g
}

# Runs of numbers as "a" or "a:b" (rows), or of columns as "C" or "C:E".
function Format-Runs([int[]]$items, [switch]$Columns) {
  $out = New-Object Collections.Generic.List[string]
  $i = 0
  while ($i -lt $items.Count) {
    $j = $i
    while ($j + 1 -lt $items.Count -and $items[$j + 1] -eq $items[$j] + 1) { $j++ }
    $a = if ($Columns) { Get-ColumnName $items[$i] } else { [string]$items[$i] }
    $b = if ($Columns) { Get-ColumnName $items[$j] } else { [string]$items[$j] }
    $out.Add($(if ($i -eq $j) { $a } else { "${a}:$b" }))
    $i = $j + 1
  }
  $out.ToArray()
}

# The formula areas of each worksheet: @{ <sheet> = @( @{ Range; Row; Col; Formulas; Values } ) }.
function Read-Formulas($book) {
  $all = @{}
  foreach ($ws in $book.Worksheets) {
    $areas = New-Object Collections.Generic.List[object]
    $found = $null
    try { $found = $ws.UsedRange.SpecialCells(-4123) } catch {}  # xlCellTypeFormulas; throws when there are none
    if ($found) {
      foreach ($a in $found.Areas) {
        $areas.Add(@{ Address = $a.Address($false, $false); Row = $a.Row; Col = $a.Column; Rows = $a.Rows.Count; Cols = $a.Columns.Count
                      Formulas = (Get-Grid $a "Formula2"); Values = (Get-Grid $a "Value2") })
      }
    }
    $all[$ws.Name] = $areas
  }
  $all
}

function Get-ExcelLocale($xl) {
  # Application.International: separators, date order and the like (the Windows regional format,
  # unless Excel's own separators are set), and the Windows formats Excel's locale-dependent
  # built-in number formats use.
  $intl = [ordered]@{
    decimalSeparator = $xl.International(3); thousandsSeparator = $xl.International(4); listSeparator = $xl.International(5)
    dateOrder = @("MDY", "DMY", "YMD")[[int]$xl.International(32)]; dateSeparator = $xl.International(17); timeSeparator = $xl.International(18)
    clock24 = [bool]$xl.International(33); dayLeadingZero = [bool]$xl.International(42); monthLeadingZero = [bool]$xl.International(41)
    fourDigitYears = [bool]$xl.International(43); timeLeadingZero = [bool]$xl.International(45); currencySymbol = $xl.International(25)
    currencyBefore = [bool]$xl.International(37); currencySpace = [bool]$xl.International(36); currencyDigits = [int]$xl.International(27)
    currencyNegative = [int]$xl.International(28); countryCode = [int]$xl.International(1); countrySetting = [int]$xl.International(2)
    useSystemSeparators = [bool]$xl.UseSystemSeparators; generalFormatName = [string]$xl.International(26)
  }
  $reg = Get-ItemProperty "HKCU:\Control Panel\International" -ErrorAction SilentlyContinue
  if ($reg) {
    foreach ($k in "LocaleName", "sShortDate", "sLongDate", "sTimeFormat", "sShortTime", "s1159", "s2359", "sYearMonth") { $intl["windows$k"] = $reg.$k }
  }
  $intl
}


function ConvertTo-JsonObject($map) {
  '{' + (@($map.Keys | ForEach-Object {
    $v = $map[$_]
    $j = if ($v -is [bool]) { if ($v) { "true" } else { "false" } } elseif ($v -is [int]) { [string]$v } else { ConvertTo-JsonString ([string]$v) }
    '"' + $_ + '": ' + $j
  }) -join ", ") + '}'
}

# Adds to $seen the rows (or columns) from $first to $last that aren't hidden. For protected
# sheets, where SpecialCells fails: Hidden of a span is true or false when all its rows agree and
# null when they don't, so mixed spans are split in two.
function Add-ShownLines($sh, [int]$first, [int]$last, [bool]$columns, $seen) {
  $span = if ($columns) { $sh.Range($sh.Cells.Item(1, $first), $sh.Cells.Item(1, $last)).EntireColumn }
          else { $sh.Range($sh.Cells.Item($first, 1), $sh.Cells.Item($last, 1)).EntireRow }
  $h = $span.Hidden
  if ($first -ge $last -and -not ($h -is [bool])) { $h = $false }
  if ($h -is [bool]) {
    if (-not $h) { for ($i = $first; $i -le $last; $i++) { [void]$seen.Add($i) } }
    return
  }
  $mid = [int][math]::Floor(($first + $last) / 2)
  Add-ShownLines $sh $first $mid $columns $seen
  Add-ShownLines $sh ($mid + 1) $last $columns $seen
}

# Reads what Excel shows of the workbook opened normally (cell text, values, formula values as
# opened), then has Excel recalculate everything and reads the formulas' values again. $formulas
# comes from Read-Formulas on the same file opened with calculation manual. Writes the cells file;
# returns the number of cells read.
function Write-WorkbookCells($xl, $book, $formulas, [string]$path, [string]$source, $result, [int]$maxCells, [bool]$formats) {
  $total = 0
  $sheets = New-Object Collections.Generic.List[object]
  $charts = @{}
  foreach ($c in $book.Charts) { $charts[$c.Name] = $true }
  $count = $book.Sheets.Count
  for ($si = 1; $si -le $count; $si++) {
    $sh = $book.Sheets.Item($si)
    $name = $sh.Name
    $vis = switch ([int]$sh.Visible) { -1 { "visible" } 0 { "hidden" } 2 { "veryHidden" } default { "$_" } }
    $info = @{ Head = '{"name": ' + (ConvertTo-JsonString $name) + ', "visible": "' + $vis + '"'; Name = $name }
    $sheets.Add($info)
    if ($charts[$name] -or -not $formulas.ContainsKey($name)) {
      $info.Head += ', "kind": "' + $(if ($charts[$name]) { "chart" } else { "other" }) + '"'
      continue
    }
    $used = $sh.UsedRange
    $r0 = $used.Row; $c0 = $used.Column; $nr = $used.Rows.Count; $nc = $used.Columns.Count
    $info.Head += ', "kind": "worksheet", "usedRange": "' + $used.Address($false, $false) + '", "standardWidth": ' + ([double]$sh.StandardWidth).ToString("R", $invariant)
    # Hidden rows and columns: those of the used range outside every visible area.
    $rowsSeen = New-Object 'Collections.Generic.HashSet[int]'
    $colsSeen = New-Object 'Collections.Generic.HashSet[int]'
    $visible = $null
    try { $visible = $used.SpecialCells(12) } catch {}  # xlCellTypeVisible; throws when nothing is visible
    if ($visible) {
      foreach ($a in $visible.Areas) {
        for ($r = $a.Row; $r -lt $a.Row + $a.Rows.Count; $r++) { [void]$rowsSeen.Add($r) }
        for ($c = $a.Column; $c -lt $a.Column + $a.Columns.Count; $c++) { [void]$colsSeen.Add($c) }
      }
    } elseif ($sh.ProtectContents) {
      # SpecialCells fails on protected sheets: ask the rows and columns themselves.
      Add-ShownLines $sh $r0 ($r0 + $nr - 1) $false $rowsSeen
      Add-ShownLines $sh $c0 ($c0 + $nc - 1) $true $colsSeen
    }
    $hiddenRows = [int[]]@(for ($r = $r0; $r -lt $r0 + $nr; $r++) { if (-not $rowsSeen.Contains($r)) { $r } })
    $hiddenCols = [int[]]@(for ($c = $c0; $c -lt $c0 + $nc; $c++) { if (-not $colsSeen.Contains($c)) { $c } })
    $info.Head += ",`n     " + '"hiddenRows": [' + ((Format-Runs $hiddenRows | ForEach-Object { '"' + $_ + '"' }) -join ", ") + '], "hiddenColumns": [' +
                  ((Format-Runs $hiddenCols -Columns | ForEach-Object { '"' + $_ + '"' }) -join ", ") + ']'
    $widths = @(for ($c = $c0; $c -lt [math]::Min($c0 + $nc, $c0 + 512); $c++) { '"' + (Get-ColumnName $c) + '": ' + ([double]$sh.Columns.Item($c).ColumnWidth).ToString("R", $invariant) })
    $info.Head += ",`n     " + '"columnWidths": {' + ($widths -join ", ") + '}'
    # Cells: their text (one call each) and their values (all at once).
    $values = Get-Grid $used "Value2"
    $cellsCom = $sh.Cells
    $lines = New-Object Collections.Generic.List[string]
    $truncated = $false
    for ($i = 1; $i -le $nr -and -not $truncated; $i++) {
      for ($j = 1; $j -le $nc; $j++) {
        $v = $values[$i, $j]
        if ($null -eq $v) { continue }
        if ($total -ge $maxCells) { $truncated = $true; break }
        $total++
        $cell = $cellsCom.Item($r0 + $i - 1, $c0 + $j - 1)
        $line = '["' + (Get-ColumnName ($c0 + $j - 1)) + ($r0 + $i - 1) + '", ' + (ConvertTo-JsonString ([string]$cell.Text)) + ', ' + (ConvertTo-JsonValue $v)
        # On the reference PC this comes in local form ("#.##0,00", "dd/mm/jjjj", "Standaard"),
        # not in the file's form: it shows how Excel reads the code here, nothing more.
        if ($formats) { $line += ', ' + (ConvertTo-JsonString ([string]$cell.NumberFormat)) }
        $lines.Add($line + ']')
      }
    }
    $info.Head += ",`n     " + '"truncated": ' + $(if ($truncated) { "true" } else { "false" })
    $info.Cells = $lines
    foreach ($a in $formulas[$name]) {
      $a.Com = $sh.Range($a.Address)
      $a.Opened = Get-Grid $a.Com "Value2"
    }
  }

  # Excel's own full recalculation.
  $xl.CalculateFull()

  $w = New-Object IO.StreamWriter($path, $false, (New-Object Text.UTF8Encoding($false)))
  try {
    $w.WriteLine("{")
    $w.WriteLine('  "source": ' + (ConvertTo-JsonString $source) + ',')
    $w.WriteLine('  "app": ' + (ConvertTo-JsonString $result.app) + ',')
    $w.WriteLine('  "locale": ' + (ConvertTo-JsonObject $result.locale) + ',')
    $w.WriteLine('  "date1904": ' + $(if ($book.Date1904) { "true" } else { "false" }) + ',')
    $w.WriteLine('  "cellCount": ' + $total + ',')
    $w.WriteLine('  "cellFields": ["ref", "text", "value"' + $(if ($formats) { ', "formatLocal"' } else { "" }) + '],')
    $w.WriteLine('  "formulaFields": ["ref", "formula", "cached", "opened", "recalculated"],')
    $w.WriteLine('  "sheets": [')
    for ($k = 0; $k -lt $sheets.Count; $k++) {
      $info = $sheets[$k]
      $last = $(if ($k -lt $sheets.Count - 1) { "," } else { "" })
      if ($null -eq $info.Cells) { $w.WriteLine('    ' + $info.Head + '}' + $last); continue }
      $w.WriteLine('    ' + $info.Head + ',')
      $w.WriteLine('     "cells": [')
      if ($info.Cells.Count) { $w.WriteLine('      ' + ($info.Cells -join ",`n      ")) }
      $w.WriteLine('     ],')
      $flines = New-Object Collections.Generic.List[string]
      foreach ($a in $formulas[$info.Name]) {
        $recalc = Get-Grid $a.Com "Value2"
        for ($i = 1; $i -le $a.Rows; $i++) {
          for ($j = 1; $j -le $a.Cols; $j++) {
            $ref = (Get-ColumnName ($a.Col + $j - 1)) + ($a.Row + $i - 1)
            $flines.Add('["' + $ref + '", ' + (ConvertTo-JsonString ([string]$a.Formulas[$i, $j])) + ', ' + (ConvertTo-JsonValue $a.Values[$i, $j]) + ', ' +
                        (ConvertTo-JsonValue $a.Opened[$i, $j]) + ', ' + (ConvertTo-JsonValue $recalc[$i, $j]) + ']')
          }
        }
      }
      $w.WriteLine('     "formulas": [')
      if ($flines.Count) { $w.WriteLine('      ' + ($flines -join ",`n      ")) }
      $w.WriteLine('     ]}' + $last)
    }
    $w.WriteLine('  ]')
    $w.WriteLine("}")
  } finally { $w.Dispose() }
  $total
}

# ---- Child: open one file in a new Office process, export it, quit.
if ($Child) {
  $app = $apps[$Child]
  $missing = [Type]::Missing
  # Every Office process this child starts. They all end before it does, whatever happens, and
  # the parent reads them from the result file to end them if the child hangs or dies. An Office
  # this child didn't start (someone's PowerPoint, another program's Word) is never touched.
  $started = New-Object Collections.Generic.List[int]
  $result = [ordered]@{ pid = $started }
  $office = $null; $doc = $null
  try {
    $safe = $null
    if ($Child -eq "Word") {
      # Word starts in safe mode, and COM attaches to that instance instead of starting another.
      $exe = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Winword.exe").'(default)'
      $safe = Start-Process -FilePath $exe -ArgumentList "/safe", "/automation", "-Embedding" -PassThru -WindowStyle Hidden
      $started.Add($safe.Id)
      Write-Json $result $ResultFile
      [void]$safe.WaitForInputIdle(30000)
    }
    foreach ($wait in 2, 4, 8, 0) {
      if ($safe) { Start-Sleep -Seconds $wait }  # until it has registered for COM
      $before = @(Get-Process $app.Exe -ErrorAction SilentlyContinue | ForEach-Object Id)
      $office = New-Object -ComObject $app.ProgId
      $new = @(Get-Process $app.Exe -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object Id)
      if (-not $safe) {
        # Excel and PowerPoint: COM started a process for this call, or attached to one already
        # running (PowerPoint runs once per user), which isn't this script's to use or close.
        if (-not $new.Count) {
          [void][Runtime.InteropServices.Marshal]::ReleaseComObject($office)
          $office = $null
          throw "$Child is already running on this PC: close it first (this script only uses an Office it starts)"
        }
        if ($Child -eq "Excel") {
          # Excel starts a new instance for every COM call, so someone may be using Excel at the same
          # time. Only the process behind this Excel's own window is this child's (and is ever ended).
          $xlPid = [FwOfficeWindows]::ProcessOf([int]$office.Hwnd)
          if ($new -notcontains $xlPid) {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($office)
            $office = $null
            throw "COM attached to an Excel this script didn't start (process $xlPid)"
          }
          $new = @($xlPid)
        }
        foreach ($id in $new) { $started.Add($id) }
        Write-Json $result $ResultFile
        break
      }
      # Word: is it the safe-mode Word this child started? Its main window takes a caption only this
      # child knows. (Another program's Word waiting for its own automation could have answered.)
      $mark = "fw-reference-renders $PID $wait"
      $caption = $office.Caption
      $office.Caption = $mark
      $ours = $false
      for ($i = 0; $i -lt 30 -and -not $ours; $i++) {
        $ours = [FwOfficeWindows]::HasTitle($safe.Id, $mark)
        if (-not $ours) { Start-Sleep -Milliseconds 100 }
      }
      if ($ours) { break }
      # Not ours. A Word this call started is closed through COM; another program's is left as it was.
      if ($new.Count) { try { $office.Quit([ref]0) } catch {} } else { try { $office.Caption = $caption } catch {} }
      [void][Runtime.InteropServices.Marshal]::ReleaseComObject($office)
      $office = $null
    }
    if (-not $office) { throw "Word didn't connect to the safe-mode instance" }
    $result.app = "$Child $($office.Version) (build $($office.Build))"
    $office.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    switch ($Child) {
      "Word" {
        $office.Visible = $false
        $office.DisplayAlerts = 0  # wdAlertsNone
        $office.Options.UpdateLinksAtOpen = $false  # safe mode doesn't save it, so only this session
        if (@($office.COMAddIns | Where-Object { $_.Connect }).Count) { throw "add-ins are loaded" }
        $result.app += ", safe mode"
        # FileName, ConfirmConversions, ReadOnly, AddToRecentFiles, PasswordDocument, PasswordTemplate, Revert,
        # WritePasswordDocument, WritePasswordTemplate, Format, Encoding, Visible, OpenAndRepair, DocumentDirection,
        # NoEncodingDialog, all by reference. A dummy password makes a protected file fail instead of prompting.
        # The test files in a "protected" folder (scripts\make-test-files.ps1) open with the test password.
        $pw = Get-TestPassword $Source
        if (-not $pw) { $pw = "-" }  # a password-protected file we don't know fails instead of prompting
        $doc = $office.Documents.Open([ref]$Source, [ref]$false, [ref]$true, [ref]$false, [ref]$pw, [ref]"-", [ref]$false,
                                      [ref]"-", [ref]"-", [ref]0, [ref]$missing, [ref]$false, [ref]$false, [ref]$missing, [ref]$true)
        $result.pages = $doc.ComputeStatistics(2)  # wdStatisticPages
        # wdExportFormatPDF, optimized for print, the whole document without markup, tagged, missing fonts as bitmaps.
        $doc.ExportAsFixedFormat($Pdf, 17, $false, 0, 0, 1, 1, 0, $false, $true, 0, $true, $true, $false)
        $doc.Close([ref]0)  # wdDoNotSaveChanges
      }
      "Excel" {
        $office.Visible = $false
        $office.DisplayAlerts = $false  # a file that needs repair then fails to open instead of being repaired
        $office.AskToUpdateLinks = $false
        $office.EnableEvents = $false
        $office.ScreenUpdating = $false
        $result.locale = Get-ExcelLocale $office
        # Calculation can only be set with a workbook open: a blank one, kept open throughout.
        $blank = $office.Workbooks.Add()
        $result.printer = [string]$office.ActivePrinter
        if ($ExcelPrinter -and $result.printer -ne $ExcelPrinter) { throw "Excel lays out for $($result.printer), not $ExcelPrinter" }
        # How Excel lays out here: a blank sheet's standard column width, which moves with the printer
        # and the display.
        $result.standardWidth = [Math]::Round([double]$blank.Worksheets.Item(1).StandardWidth, 4)
        if ($ExcelStandardWidth -gt 0 -and [Math]::Abs($result.standardWidth - $ExcelStandardWidth) -gt 0.0001) {
          throw "Excel lays out differently here: a blank sheet's standard width is $($result.standardWidth), not $ExcelStandardWidth"
        }
        if ($Probe) {
          $blank.Close($false)
          [void][Runtime.InteropServices.Marshal]::ReleaseComObject($blank)
          break  # only the layout check (the parent's, before a run)
        }
        # 1. Calculation manual: the file's cached formula values, nothing recalculated.
        $office.Calculation = -4135  # xlCalculationManual
        # Filename, UpdateLinks (never), ReadOnly. Automation doesn't add files to Excel's recent list.
        $doc = $office.Workbooks.Open($Source, 0, $true)
        $formulas = Read-Formulas $doc
        $doc.Close($false)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($doc)
        $doc = $null
        # 2. Opened as anyone opens it (Excel recalculates what it would on opening): the PDF and
        # the cells' text; then Excel's full recalculation.
        $office.Calculation = -4105  # xlCalculationAutomatic
        $doc = $office.Workbooks.Open($Source, 0, $true)
        try {
          # xlTypePDF, standard quality, no document properties, print areas respected
          $doc.ExportAsFixedFormat(0, $Pdf, 0, $false, $false, $missing, $missing, $false)
        } catch {
          # A workbook with nothing to print has no PDF; its cells are still read.
          $result.pdfError = $_.Exception.Message
        }
        if ($Cells) { $result.cells = Write-WorkbookCells $office $doc $formulas $Cells $SourceName $result $MaxCells $CellFormats.IsPresent }
        if ($PrintTo) {
          # Printed as well: Excel's export leaves out data bars, icon sets and sparklines here. After
          # the cells: Excel's column widths follow its printer, which this switches for a moment.
          $device = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Devices' -ErrorAction SilentlyContinue).'Microsoft Print to PDF'
          if (-not $device) { throw "Microsoft Print to PDF isn't installed" }
          $was = $office.ActivePrinter  # only Excel's, and only this Excel's; set with a workbook open
          $office.ActivePrinter = "Microsoft Print to PDF on $(($device -split ',')[1])"
          try {
            # From, To, Copies, Preview, ActivePrinter, PrintToFile, Collate, PrToFileName, IgnorePrintAreas
            $doc.PrintOut($missing, $missing, 1, $false, $missing, $true, $missing, $PrintTo, $false)
          } finally {
            $office.ActivePrinter = $was
          }
          # The printer writes the file once the spooler has the job: wait until it's complete.
          $size = -1
          for ($i = 0; $i -lt 120; $i++) {
            Start-Sleep -Milliseconds 500
            if (-not (Test-Path -LiteralPath $PrintTo)) { continue }
            $now = (Get-Item -LiteralPath $PrintTo).Length
            if ($now -gt 0 -and $now -eq $size) {
              try { [IO.File]::Open($PrintTo, 'Open', 'Read', 'None').Dispose(); break } catch {}
            }
            $size = $now
          }
          if (-not (Test-Path -LiteralPath $PrintTo)) { $result.printError = "Microsoft Print to PDF wrote nothing" }
        }
        $doc.Close($false)
        $blank.Close($false)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($blank)
      }
      "PowerPoint" {
        # ReadOnly, not untitled, no window
        $doc = $office.Presentations.Open($Source, -1, 0, 0)
        $doc.SaveCopyAs($Pdf, 32)  # ppSaveAsPDF
        $doc.Close()
      }
    }
  } catch {
    $result.error = $_.Exception.Message
  } finally {
    if ($office) {
      try { if ($Child -eq "Word") { $office.Quit([ref]0) } else { $office.Quit() } } catch {}
      foreach ($o in $doc, $office) { if ($o) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($o) } }
      $doc = $null; $office = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
    # Whatever happened, the Office this child started ends with it.
    Stop-Started $started $app.Exe
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-reference-renders"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Get-PdfFacts([string]$path) {
  $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($path))
  $fonts = [regex]::Matches($text, '/BaseFont\s*/([^\s/\[\]<>()]+)') | ForEach-Object { $_.Groups[1].Value -replace '^[A-Z]{6}\+', '' -replace '#20', ' ' }
  @{ pages = [regex]::Matches($text, '/Type\s*/Page(?![A-Za-z])').Count; fonts = @($fonts | Sort-Object -Unique) }
}
function Test-Inside([string]$path, [string]$folder) {
  $path.TrimEnd('\').Equals($folder, 'OrdinalIgnoreCase') -or $path.StartsWith($folder + '\', 'OrdinalIgnoreCase')
}
function Get-Kind([string]$ext) {
  $apps.Keys | Where-Object { $apps[$_].Ext -contains $ext.ToLower() } | Select-Object -First 1
}

# A format copy's original: the .docx it was made from (private: docx\<category>\, the same name;
# public: generated\ or torture\, the name without "-protected"). Null for anything else.
function Get-CopyOf([string]$source, [string]$sourceRoot) {
  $parts = $source -split '/'
  if ($parts.Count -lt 2 -or @("doc", "odt", "dotx", "protected") -notcontains $parts[0]) { return $null }
  $stem = (($parts[1..($parts.Count - 1)] -join '/') -replace '\.[^./]+$', '') -replace '-protected$', ''
  $candidates = if ($Corpus) { @("docx/$stem.docx") } else { @("generated/$stem.docx", "torture/$stem.docx") }
  foreach ($c in $candidates) { if (Test-Path -LiteralPath (Join-Path $sourceRoot $c)) { return $c } }
  $null
}

$refManifest = Join-Path $root "tests\office\reference\manifest.json"
if (-not $ExcelPrinter -and (Test-Path $refManifest)) {
  $ExcelPrinter = [string](Get-Content $refManifest -Raw | ConvertFrom-Json).excel.printer
}

# Each job: the source file, its name in the manifest, and its PDF relative to the output folder.
$jobs = @()
$clear = @()
$kept = @()
$old = @()
$previousFiles = @()
if ($Corpus) {
  $corpusRoot = (Resolve-Path $Corpus).Path.TrimEnd('\')
  $outRoot = if ($PSBoundParameters.ContainsKey("Out")) { [IO.Path]::GetFullPath($Out) } else { Join-Path $corpusRoot "reference" }
  foreach ($p in @($corpusRoot, $outRoot)) {
    if (Test-Inside $p $root) { throw "$p is inside the repository. The private corpus and its renders stay outside it." }
  }
  New-Item -ItemType Directory -Force $outRoot | Out-Null
  $manifestPath = Join-Path $outRoot "manifest.json"
  if ((Test-Path $manifestPath) -and -not $Force) { $old = @((Get-Content $manifestPath -Raw | ConvertFrom-Json).files) }
  # docx, xlsx, pptx: screened downloads; doc, odt, dotx, protected: Word's copies of them (D5,
  # scripts\make-test-files.ps1).
  $screened = @("docx", "xlsx", "pptx", "doc", "odt", "dotx", "protected") | ForEach-Object { Join-Path $corpusRoot $_ } | Where-Object { Test-Path $_ }
  if (-not $screened) { throw "$corpusRoot has no docx, xlsx or pptx folder (see scripts\fetch-corpus.ps1)." }
  foreach ($f in $screened | ForEach-Object { Get-ChildItem $_ -File -Recurse } | Sort-Object FullName) {
    if ((Test-Inside $f.FullName $outRoot) -or -not (Get-Kind $f.Extension)) { continue }
    $rel = $f.FullName.Substring($corpusRoot.Length + 1) -replace '\\', '/'
    $jobs += @{ File = $f; Source = $rel; Pdf = "$rel.pdf" }
  }
} else {
  $outRoot = Join-Path $root $Out
  New-Item -ItemType Directory -Force $outRoot | Out-Null
  $manifestPath = Join-Path $outRoot "manifest.json"
  $names = @($Folders | ForEach-Object { Split-Path -Leaf $_ })
  if (Test-Path $manifestPath) {
    # Entries of folders that aren't rendered this time stay as they are.
    $previousFiles = @((Get-Content $manifestPath -Raw | ConvertFrom-Json).files)
    $kept = @($previousFiles | Where-Object { $names -notcontains ($_.source -split '/')[0] })
  }
  foreach ($folder in $Folders) {
    $name = Split-Path -Leaf $folder
    $clear += Join-Path $outRoot $name  # emptied once the run is sure to go ahead
    foreach ($f in Get-ChildItem (Join-Path $root $folder) -File | Sort-Object Name) {
      if (Get-Kind $f.Extension) { $jobs += @{ File = $f; Source = "$name/$($f.Name)"; Pdf = "$name/$($f.Name).pdf" } }
    }
  }
}

# Workbooks only for the reference printer (-ExcelPrinter): stop before anything starts otherwise.
if ($ExcelPrinter -and @($jobs | Where-Object { (Get-Kind $_.File.Extension) -eq "Excel" }).Count) {
  $device = @(([string](Get-ItemProperty 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Windows').Device) -split ',')
  $default = if ($device.Count -ge 3) { "$($device[0]) on $($device[2])" } else { "" }
  if ($default -ne $ExcelPrinter) {
    throw "Windows' default printer is '$default', but workbooks are rendered for the reference printer '$ExcelPrinter'. Make that the default again (with 'Let Windows manage my default printer' on, print something on it), or pass -ExcelPrinter."
  }
}
# Printing while Windows manages the default printer would make Microsoft Print to PDF the default
# halfway through the run, and every workbook after it would be laid out for that printer.
if ($PrintPdf.Count -and [int](Get-ItemProperty 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Windows').LegacyDefaultPrinterMode -eq 0) {
  throw "-PrintPdf needs 'Let Windows manage my default printer' off (Settings > Bluetooth & devices > Printers & scanners): with it on, printing changes the default printer."
}

# One Office automation at a time on this PC (word-open-check.ps1 takes the same mutex): a second
# one's COM could attach to the first one's Word.
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}

# Excel must lay out as it did for the references (-ExcelStandardWidth): one Excel, a blank sheet.
if (-not $ExcelStandardWidth -and (Test-Path $refManifest)) {
  $ExcelStandardWidth = [double](Get-Content $refManifest -Raw | ConvertFrom-Json).excel.standardWidth
}
if (@($jobs | Where-Object { (Get-Kind $_.File.Extension) -eq "Excel" }).Count) {
  $probeFile = Join-Path $tmp "probe.json"
  if (Test-Path $probeFile) { Remove-Item $probeFile }
  $probeArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", "Excel", "-Probe",
    "-ResultFile", "`"$probeFile`"", "-ExcelStandardWidth", $ExcelStandardWidth)
  if ($ExcelPrinter) { $probeArgs += @("-ExcelPrinter", "`"$ExcelPrinter`"") }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $probeArgs
  $done = $p.WaitForExit(120000)
  if (-not $done) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  $probed = if (Test-Path $probeFile) { Get-Content $probeFile -Raw | ConvertFrom-Json } else { $null }
  if ($probed) { Stop-Started @($probed.pid) $apps["Excel"].Exe $(if ($done) { 15000 } else { 0 }) }
  if (-not $probed -or $probed.error) {
    $mutex.ReleaseMutex()
    $why = if ($probed) { $probed.error } else { "Excel didn't answer" }
    throw "Nothing was rendered: $why. Excel lays out with the default printer and the display as they are when it starts; render with the reference printer the default and the Remote Desktop session connected, as for the references, or pass -ExcelStandardWidth to start new references."
  }
}
foreach ($dir in $clear) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } }
$entries = @()
$excelFacts = $null
$n = 0
$reused = 0
foreach ($job in $jobs) {
  $n++
  $f = $job.File
  $kind = Get-Kind $f.Extension
  $pdfPath = Join-Path $outRoot ($job.Pdf -replace '/', '\')
  $entry = [ordered]@{ source = $job.Source; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower() }
  # A workbook's cells file sits beside its PDF.
  $cellsRel = if ($kind -eq "Excel") { $job.Pdf -replace '\.pdf$', '.cells.json' } else { $null }
  $cellsPath = if ($cellsRel) { Join-Path $outRoot ($cellsRel -replace '/', '\') } else { $null }
  # Printed through Microsoft Print to PDF too (-PrintPdf)?
  $printRel = if ($kind -eq "Excel" -and ($PrintPdf -contains $f.Name -or $PrintPdf -contains $job.Source)) { $job.Pdf -replace '\.pdf$', '.print.pdf' } else { $null }
  $printPath = if ($printRel) { Join-Path $outRoot ($printRel -replace '/', '\') } else { $null }
  $previous = $old | Where-Object { $_.source -eq $job.Source -and $_.sha256 -eq $entry.sha256 -and ($_.pdf -or $_.cells) -and -not $_.error -and ($kind -ne "Excel" -or $_.cells) } | Select-Object -First 1
  if ($previous -and (-not $previous.pdf -or (Test-Path $pdfPath)) -and (-not $cellsPath -or (Test-Path $cellsPath)) -and
      (-not $printPath -or ($previous.printPdf -and (Test-Path $printPath)))) {
    $entries += $previous
    $reused++
    continue
  }
  New-Item -ItemType Directory -Force (Split-Path -Parent $pdfPath) | Out-Null
  $resultPath = Join-Path $tmp "result.json"
  if (Test-Path $resultPath) { Remove-Item $resultPath }
  $childArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
    "-Child", $kind, "-Source", "`"$($f.FullName)`"", "-SourceName", "`"$($job.Source)`"", "-Pdf", "`"$pdfPath`"", "-ResultFile", "`"$resultPath`"")
  if ($cellsPath) {
    $childArgs += @("-Cells", "`"$cellsPath`"", "-MaxCells", $MaxCells)
    if (-not $Corpus) { $childArgs += "-CellFormats" }  # the public files also record each cell's number format
  }
  if ($kind -eq "Excel" -and $ExcelPrinter) { $childArgs += @("-ExcelPrinter", "`"$ExcelPrinter`"") }
  if ($printPath) {
    if (Test-Path $printPath) { Remove-Item $printPath }
    $childArgs += @("-PrintTo", "`"$printPath`"")
  }
  $limit = if ($kind -eq "Excel") { $ExcelTimeoutSeconds } else { $TimeoutSeconds }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $childArgs
  $finished = $p.WaitForExit($limit * 1000)
  # Office is stuck, usually on a prompt: end the child.
  if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw | ConvertFrom-Json } else { $null }
  # The child ends the Office it started; if it hung or died, that's done here, so no hidden Office
  # is left to block the next file.
  if ($r) { Stop-Started @($r.pid) $apps[$kind].Exe $(if ($finished) { 15000 } else { 0 }) }
  if (-not $finished) {
    $entry.error = "$kind didn't finish within $limit s"
  } elseif ($r) {
    $entry.app = $r.app
    if ($r.error) { $entry.error = $r.error }
    if ($r.pages) { $entry.pages = $r.pages }
    if ($r.locale -and -not $excelFacts) { $excelFacts = [ordered]@{ locale = $r.locale; printer = $r.printer; standardWidth = $r.standardWidth } }
    if (-not $r.error -and $cellsPath -and (Test-Path $cellsPath)) {
      $entry.cells = $cellsRel
      $entry.cellCount = [int]$r.cells
    }
  } else {
    $entry.error = "$kind didn't start"
  }
  if (-not $entry.error -and (Test-Path $pdfPath)) {
    $facts = Get-PdfFacts $pdfPath
    $entry.pdf = $job.Pdf
    if (-not $entry.pages) { $entry.pages = $facts.pages }
    $entry.fonts = $facts.fonts
    if ($printPath -and (Test-Path $printPath)) {
      $entry.printPdf = $printRel
      $entry.printPages = (Get-PdfFacts $printPath).pages
    } elseif ($printPath) {
      $entry.printError = if ($r.printError) { $r.printError } else { "not printed" }
    }
    $more = if ($entry.cells) { "  $($entry.cellCount) cells" } else { "" }
    if ($entry.printPdf) { $more += "  printed: $($entry.printPages) pages" }
    "[{0}/{1}] {2,-45} {3,3} pages  {4}{5}" -f $n, $jobs.Count, $entry.source, $entry.pages, ($facts.fonts -join ", "), $more
  } elseif (-not $entry.error -and $entry.cells -and $r.pdfError) {
    # Nothing to print (Excel exports no PDF), but the cells are recorded.
    $entry.noPdf = $r.pdfError
    "[{0}/{1}] {2,-45} no PDF ({3})  {4} cells" -f $n, $jobs.Count, $entry.source, $r.pdfError, $entry.cellCount
  } else {
    if (-not $entry.error) { $entry.error = "no PDF was written" }
    "[{0}/{1}] {2,-45} FAILED: {3}" -f $n, $jobs.Count, $entry.source, $entry.error
  }
  $entries += [pscustomobject]$entry
}

$manifest = [ordered]@{
  made = (Get-Date).ToString("yyyy-MM-dd")
  computer = "Office reference PC (decision 19)"
  export = "Word (safe mode, no add-ins, macros forced off, links not updated, read-only): the document without markup, for print, tagged; Excel (macros forced off, links not updated, read-only): print areas, and a cells file per workbook (each non-empty cell's text and value, formulas with their cached, opened and recalculated values); PowerPoint: slides"
}
# The locale and printer Excel formatted and laid out with; kept from the last run that rendered workbooks.
if (-not $excelFacts -and (Test-Path $manifestPath)) { $excelFacts = (Get-Content $manifestPath -Raw | ConvertFrom-Json).excel }
if ($excelFacts) { $manifest.excel = $excelFacts }
if ($Corpus) { $manifest.corpus = $corpusRoot }  # fwtool compare finds the documents here
# Folders stay in the order the manifest had them, new folders after them; files by name.
$groups = @(@($previousFiles) + @($entries) | ForEach-Object { ($_.source -split '/')[0] } | Select-Object -Unique)
$manifest.files = @(@($kept) + @($entries) | Sort-Object { if ($Corpus) { 0 } else { [array]::IndexOf($groups, ($_.source -split '/')[0]) } }, { $_.source })
# Every entry, kept ones too, says which original it copies (the coordinator's D5 decision).
$sourceRoot = if ($Corpus) { $corpusRoot } else { Join-Path $root "tests\office" }
foreach ($e in $manifest.files) {
  $c = Get-CopyOf $e.source $sourceRoot
  if ($c) { $e | Add-Member -NotePropertyName copyOf -NotePropertyValue $c -Force }
  $p = Get-TestPassword $e.source
  if ($p) { $e | Add-Member -NotePropertyName password -NotePropertyValue $p -Force }
}
Write-Json $manifest $manifestPath
$mutex.ReleaseMutex()
$unchanged = if ($reused) { ", $reused of them unchanged since the last run" } else { "" }
"$(@($entries | Where-Object { -not $_.error }).Count) of $($entries.Count) exported$unchanged. The PDFs and manifest.json are in $outRoot"
