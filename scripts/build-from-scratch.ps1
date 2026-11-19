<#
.SYNOPSIS
  S2's "build from scratch" check (docs\s2-plan.md): the household budget and the small invoice
  (tests\office\build\*.steps) are typed into a blank workbook, step by step, by Excel and by
  Featherweight, and Excel must show the same in both.
  1. Excel plays the steps in a new workbook (typing through FormulaLocal, which converts what's
     typed as typing in the cell does: numbers, dates, percentages, currency, formulas in the
     region's form), saves it (<out>\excel\<name>.xlsx) and dumps what it shows (<name>.json).
  2. With -Featherweight <exe>: Featherweight plays the same steps through the grid's commands, as
     the keyboard does them (a script for `Featherweight.exe --snapshot --script @file`: Go to,
     typing, Enter, AutoSum, Ctrl+D/R, the fill handle, the format keys and commands, Column width,
     Freeze panes, Rename sheet, Save As), into <out>\featherweight\<name>.xlsx (with a picture of
     the end, <name>.png, and what it printed, <name>.log). Then as -Saved on that folder.
  3. With -Saved: Featherweight's saved copies (<saved>\<name>.xlsx) are opened in Excel and dumped
     the same way, and compared with Excel's own: per cell its formula, value, text (Range.Text),
     number format, font, fill, alignment and borders; per sheet its name, column widths, merges and
     frozen panes. They must also open without a repair (scripts\excel-open-check.ps1).
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
  powershell -ExecutionPolicy Bypass -File scripts\build-from-scratch.ps1 -Featherweight build\Release\Featherweight.exe
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-from-scratch.ps1 -Saved build\from-scratch\featherweight
#>
param(
  [string[]]$Steps,
  [string]$Out = "build\from-scratch",
  [string]$Saved,
  [string]$Featherweight,
  # S3's "created as Excel shows it": tests\office\fixtures\*.steps, played in Featherweight only
  # (-Featherweight is needed), then Excel reads the objects back and exports the active sheet.
  [switch]$Fixtures,
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
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
        spill = if ($formula) { try { [bool]$cell.HasSpill } catch { $false } } else { $false }  # a dynamic array's anchor
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

# ---- What Excel reads back of a fixture, one fact per line, for the fixtures' "expect <fact>"
# lines: cells (from the dump), names, charts and their series, tables, conditional formats,
# validations, notes, threaded comments, hyperlinks and page setup. Formulas as Excel's English
# form (Formula2, RefersTo); colors as RRGGBB.
function Get-Facts($wb, $sheets) {
  $hex = { param($bgr) if ($null -eq $bgr) { "" } else { "{0:X2}{1:X2}{2:X2}" -f ($bgr -band 0xFF), (($bgr -shr 8) -band 0xFF), (($bgr -shr 16) -band 0xFF) } }
  foreach ($s in @($sheets)) {
    foreach ($c in @($s.cells)) {
      $at = "$($s.name)!$($c.ref)"
      "cell $at text $($c.text)"
      if ($c.formula -like "=*") { "cell $at formula $($c.formula)" }
      if ($c.spill) { "cell $at spills" }  # a dynamic array (not a legacy Ctrl+Shift+Enter one)
      "cell $at numfmt $($c.numfmt)"
      "cell $at font $($c.font) $($c.size)$(if ($c.bold) { ' bold' })$(if ($c.italic) { ' italic' })$(if ($c.underline -ne -4142) { ' underline' })"
      if ($null -ne $c.fill) { "cell $at fill $(& $hex $c.fill)" }
      if ($c.align -ne 1) { "cell $at align $($c.align)" }
      if ($c.wrap) { "cell $at wrap" }
      if ($c.top -notlike "-4142/*") { "cell $at top $($c.top)" }
      if ($c.bottom -notlike "-4142/*") { "cell $at bottom $($c.bottom)" }
    }
    $widths = if ($s.widths -is [Collections.IDictionary]) { $s.widths.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" } }
              else { $s.widths.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" } }
    "sheet $($s.name) widths $(@($widths) -join ' ')"
    if ($s.freeze) { "sheet $($s.name) freeze $($s.freeze)" }
    foreach ($m in @($s.merges)) { "sheet $($s.name) merge $m" }
  }
  foreach ($n in $wb.Names) { "name $($n.Name) $($n.RefersTo)" }
  if ($wb.ProtectStructure) { "protect-book structure" }
  foreach ($cs in $wb.Charts) {  # chart sheets
    "chartsheet $($cs.Name) type $($cs.ChartType) series $($cs.SeriesCollection().Count)"
    $j = 0
    foreach ($se in $cs.SeriesCollection()) { $j++; "chartsheet $($cs.Name) series $j $($se.Formula)" }
  }
  foreach ($ws in $wb.Worksheets) {
    $sn = $ws.Name
    $i = 0
    foreach ($co in $ws.ChartObjects()) {
      $i++
      $ch = $co.Chart
      "chart $sn $i type $($ch.ChartType) series $($ch.SeriesCollection().Count)"
      "chart $sn $i title $(if ($ch.HasTitle) { $ch.ChartTitle.Text } else { '-' }) legend $(if ($ch.HasLegend) { $ch.Legend.Position } else { '-' })"
      $j = 0
      foreach ($se in $ch.SeriesCollection()) {
        $j++
        "chart $sn $i series $j $($se.Formula)"
        if ($se.HasDataLabels) { "chart $sn $i series $j labels" }
      }
    }
    foreach ($lo in $ws.ListObjects) {
      $style = try { $lo.TableStyle.Name } catch { "" }
      "table $($lo.Name) $sn!$($lo.Range.Address($false, $false)) style $style totals $($lo.ShowTotals)"
      # s3-data: the style options (Filter Button is ShowAutoFilterDropDown), each column's name and
      # its total (TotalsCalculation: 0 none, 1 sum, 2 average, 3 count, 4 count numbers, 5 min, 6 max...).
      $buttons = try { $lo.ShowAutoFilterDropDown } catch { "" }
      "table $($lo.Name) options headers $($lo.ShowHeaders) rows $($lo.ShowTableStyleRowStripes) cols $($lo.ShowTableStyleColumnStripes) first $($lo.ShowTableStyleFirstColumn) last $($lo.ShowTableStyleLastColumn) filter $buttons"
      $ci = 0
      foreach ($col in $lo.ListColumns) { $ci++; "table $($lo.Name) column $ci $($col.Name)$(if ($lo.ShowTotals) { " total $($col.TotalsCalculation)" })" }
    }
    $fcs = $ws.Cells.FormatConditions
    for ($k = 1; $k -le $fcs.Count; $k++) {
      $fc = $fcs.Item($k)
      $op = try { " operator $($fc.Operator)" } catch { "" }
      "rule $sn!$($fc.AppliesTo.Address($false, $false)) type $($fc.Type)$op"
      foreach ($f in @(Get-RuleFacts $fc)) { "rule $sn!$($fc.AppliesTo.Address($false, $false)) $f" }
    }
    # Assigned inside the try: `$x = try { range }` would unroll the range into its cells.
    $valid = $null
    try { $valid = $ws.Cells.SpecialCells(-4174) } catch { }  # xlCellTypeAllValidation
    if ($valid) {
      # Each area by its address ("B2:B5,D2:D5"). Formula1 comes in the region's form (a list
      # "Yes;No" in en-NL), whatever culture the call carries.
      foreach ($a in $valid.Address($false, $false) -split ',') {
        $area = $ws.Range($a)
        $v = $area.Cells.Item(1, 1).Validation
        $f1 = try { [string][System.__ComObject].InvokeMember("Formula1", [Reflection.BindingFlags]::GetProperty, $null, $v, $null, $null, $enUS, $null) } catch { "" }
        "validation $sn!$($area.Address($false, $false)) type $($v.Type) $f1"
        # s3-data: the comparison of a number, date, time or length (Operator: 1 between ... 8 <=)
        # and its second value.
        if ($v.Type -in 1, 2, 4, 5, 6) {
          $f2 = try { [string][System.__ComObject].InvokeMember("Formula2", [Reflection.BindingFlags]::GetProperty, $null, $v, $null, $null, $enUS, $null) } catch { "" }
          "validation $sn!$($area.Address($false, $false)) operator $($v.Operator) $f2".TrimEnd()
        }
      }
    }
    # Texts on one line: a line break as \n. A thread's older-versions note is its own fact.
    $line = { param($t) ([string]$t) -replace "`r`n|`r|`n", '\n' }
    foreach ($cm in $ws.Comments) { "note $sn!$($cm.Parent.Address($false, $false)) $(& $line $cm.Text())" }
    $threads = try { $ws.CommentsThreaded } catch { $null }
    if ($threads) { foreach ($t in $threads) { "comment $sn!$($t.Parent.Address($false, $false)) $(& $line $t.Text()) replies $($t.Replies.Count)" } }
    foreach ($h in $ws.Hyperlinks) { "link $sn!$($h.Range.Address($false, $false)) address $($h.Address) place $($h.SubAddress) text $($h.TextToDisplay)".TrimEnd() }
    # Pictures and shapes (msoShapeType: 1 a shape, 9 a line, 13 a picture, 17 a text box), not
    # charts (3, above) or notes (4): where, fill, outline and its weight, text, alt text.
    foreach ($sp in $ws.Shapes) {
      $type = $sp.Type
      if ($type -eq 3 -or $type -eq 4) { continue }
      $fill = try { if ($sp.Fill.Visible) { & $hex $sp.Fill.ForeColor.RGB } else { "none" } } catch { "-" }
      $outline = try { if ($sp.Line.Visible) { "$(& $hex $sp.Line.ForeColor.RGB) $($sp.Line.Weight)" } else { "none" } } catch { "-" }
      $text = try { if ($sp.TextFrame2.HasText) { & $line $sp.TextFrame2.TextRange.Text } else { "" } } catch { "" }
      $kind = try { $sp.AutoShapeType } catch { "-" }
      "shape $sn $($sp.Name) type $type autoshape $kind at $($sp.TopLeftCell.Address($false, $false)) fill $fill line $outline text $text alt $(& $line $sp.AlternativeText)".TrimEnd()
    }
    if ($ws.ProtectContents) { "protect $sn contents formatcells $($ws.Protection.AllowFormattingCells) insertrows $($ws.Protection.AllowInsertingRows)" }
    $ps = $ws.PageSetup
    "pagesetup $sn orientation $($ps.Orientation) zoom $($ps.Zoom) wide $($ps.FitToPagesWide) tall $($ps.FitToPagesTall) titlerows $($ps.PrintTitleRows) titlecols $($ps.PrintTitleColumns) area $($ps.PrintArea)"
    # s3-data: pictures and shapes (charts and notes are above): Type (1 a shape or line, 13 a
    # picture, 17 a text box), Placement (1 move and size with cells, 2 move only), cells, size in
    # points; fill, outline, text and alt text.
    $inv = [Globalization.CultureInfo]::InvariantCulture
    foreach ($sh in $ws.Shapes) {
      if ($sh.Type -in 3, 4) { continue }
      $at = "shape $sn $($sh.Name)"
      "$at type $($sh.Type) placement $($sh.Placement) at $($sh.TopLeftCell.Address($false, $false)):$($sh.BottomRightCell.Address($false, $false)) size $([string]::Format($inv, '{0:0.##}x{1:0.##}', $sh.Width, $sh.Height))"
      try { if ($sh.Fill.Visible) { "$at fill $(& $hex $sh.Fill.ForeColor.RGB)" } else { "$at fill none" } } catch { }
      try { if ($sh.Line.Visible) { "$at line $(& $hex $sh.Line.ForeColor.RGB) $([string]::Format($inv, '{0:0.##}', $sh.Line.Weight))" } else { "$at line none" } } catch { }
      try { if ($sh.TextFrame2.HasText) { "$at text $(& $line $sh.TextFrame2.TextRange.Text)" } } catch { }
      if ($sh.AlternativeText) { "$at alt $(& $line $sh.AlternativeText)" }
    }
  }
}

# s3-data: a conditional format's details by its type (Type: 1 cell value, 2 formula, 3 color scale,
# 4 data bar, 5 top/bottom, 6 icon set, 8 duplicate/unique, 9 text, 11 dates, 12 above/below
# average). Properties a type hasn't throw, so each is tried; formulas as Excel gives them.
function Get-RuleFacts($fc) {
  $hex = { param($bgr) if ($null -eq $bgr) { "" } else { "{0:X2}{1:X2}{2:X2}" -f ($bgr -band 0xFF), (($bgr -shr 8) -band 0xFF), (($bgr -shr 16) -band 0xFF) } }
  $t = [int]$fc.Type
  try {
    switch ($t) {
      1 { "formula $($fc.Formula1)"; if ([int]$fc.Operator -in 1, 2) { "formula2 $($fc.Formula2)" } }
      2 { "formula $($fc.Formula1)" }
      3 { "scale $($fc.ColorScaleCriteria.Count)" }
      4 { "bar $(& $hex $fc.BarColor.Color)" }
      5 { "top $($fc.TopBottom) rank $($fc.Rank) percent $($fc.Percent)" }
      6 { "icons $($fc.IconSet.ID)" }
      8 { "unique $($fc.DupeUnique)" }
      9 { "text $($fc.TextOperator) $($fc.Text)" }
      11 { "period $($fc.DateOperator)" }
      12 { "average $($fc.AboveBelow)" }
    }
  } catch { }
  # The format a rule applies: its fill and font color (none set: no line). A rule's Interior.Pattern
  # is empty even when it fills (Excel's own rules too); ColorIndex is a number only when it does.
  if ($t -in 1, 2, 5, 8, 9, 11, 12) {
    try { if ($fc.Interior.ColorIndex -is [ValueType]) { "fill $(& $hex ([int]$fc.Interior.Color))" } } catch { }
    try { if ($fc.Font.Color -is [ValueType]) { "font $(& $hex ([int]$fc.Font.Color))" } } catch { }
  }
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
      if ($Target) {
        # Fixtures: opened without recalculating (manual calculation, set on a blank workbook first),
        # so the cells show the results Featherweight saved; then Excel recalculates, and a cell
        # whose text changes is one Featherweight calculated differently.
        $blank = $xl.Workbooks.Add()
        $xl.Calculation = -4135  # xlCalculationManual
      }
      $wb = $xl.Workbooks.Open($Source, 0, $true)
      $active = $wb.ActiveSheet
      $result.sheets = Get-Dump $xl $wb
      if ($Target) {
        $ours = @{}
        foreach ($s in @($result.sheets)) { foreach ($c in @($s.cells)) { $ours["$($s.name)!$($c.ref)"] = $c.text } }
        $xl.CalculateFull()
        $result.sheets = Get-Dump $xl $wb
        $result.recalc = @(foreach ($s in @($result.sheets)) {
          foreach ($c in @($s.cells)) {
            $was = $ours["$($s.name)!$($c.ref)"]
            if ($null -ne $was -and $was -ne $c.text) { "$($s.name)!$($c.ref): Featherweight '$was', Excel '$($c.text)'" }
          }
        })
        # The objects as Excel reads them, and its PDF of the sheet the file opens on.
        $result.facts = @(Get-Facts $wb $result.sheets)
        $active.Activate()
        $active.ExportAsFixedFormat(0, $Target)  # xlTypePDF
      }
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
            "name" { [void]$wb.Names.Add($first, $quoted) }  # name Rate "=Sheet!$B$2"
            "delete-name" { $wb.Names.Item($first).Delete() }
            "names-create" {  # names-create A1:B5 top,left: Create from Selection
              $sides = (($rest -split '\s+')[1]) -split ','
              [void]$ws.Range($first).CreateNames($sides -contains 'top', $sides -contains 'left', $sides -contains 'bottom', $sides -contains 'right')
            }
            "freeze" {
              $win = $wb.Windows.Item(1)
              $win.FreezePanes = $false
              $cell = $ws.Range($first)
              $win.SplitRow = $cell.Row - 1
              $win.SplitColumn = $cell.Column - 1
              $win.FreezePanes = $true
            }
            "save" { $wb.SaveAs($Target, 51) }  # xlOpenXMLWorkbook
            { $_ -in "table", "table-total", "rule", "validation", "note", "comment", "link", "link-place", "protect", "chart", "pagesetup",
                     "table-style", "table-option", "table-name", "table-resize", "table-convert", "picture", "shape", "object-fill",
                     "object-outline", "object-text", "object-alt", "row-height" } {
              throw "line ${n}: '$op' is a fixture step (played in Featherweight only: -Fixtures)"
            }
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
$Steps = @($Steps | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # -File passes "a,b" as one
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
    # Widths within a pixel at 100 % (1/7 of a character): Excel's ColumnWidth depends on how it
    # measures the digits on this screen. Over Remote Desktop it stores 22.64453125 for 22
    # characters where it stores 22.7109375 at the console, and shows the console's value as 22.06.
    foreach ($p in $x.widths.PSObject.Properties) {
      $w = $y.widths.($p.Name)
      if ($null -eq $w -or [Math]::Abs([double]$w - [double]$p.Value) -gt 0.15) { $diffs += "$($x.name) column $($p.Name) width: Excel $($p.Value), Featherweight $w" }
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

# ---- Featherweight plays the steps as the keyboard does them: one script for
# `Featherweight.exe --snapshot <png> --script @file` (App::RunScript). In a script ';' separates
# the ops, so a typed ';' is written "\;". Command ids are src\Commands.h's: 2009 Rename sheet,
# 2013 Go to, 2014/2015 Fill down/right, 2020 AutoSum, 2004 Column width, 2026 Merge cells,
# 2028 Freeze panes. Number format codes go as the steps have them, in the English form files store
# (format:code=, as Excel's player sets Range.NumberFormat); format:numfmt= would read them in the
# region's form, as typed into Format Cells here (Dutch date letters, "#.##0,00").
function ConvertTo-AppScript([string]$stepsPath, [string]$target, [string]$pdf) {
  $ops = New-Object Collections.Generic.List[string]
  $ops.Add("new:workbook")
  function Select-Ref([string]$ref) { $ops.Add("cmd:2013"); $ops.Add("type:$ref"); $ops.Add("key:enter") }
  function Add-Typed([string]$text) { $ops.Add("type:" + ($text -replace ';', '\;')) }
  $n = 0
  foreach ($line in Get-Content -LiteralPath $stepsPath -Encoding UTF8) {
    $n++
    $line = $line.Trim()
    if (-not $line -or $line.StartsWith("#")) { continue }
    if ($line -notmatch '^(\S+)\s*(.*)$') { throw "line ${n}: can't read '$line'" }
    $op = $Matches[1]; $rest = $Matches[2]
    $quoted = if ($rest -match '"((?:[^"\\]|\\.)*)"') { $Matches[1] -replace '\\(.)', '$1' } else { $null }
    $quotes = @([regex]::Matches($rest, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value -replace '\\(.)', '$1' })
    $first = ($rest -split '\s+')[0]
    switch ($op) {
      "sheet-name" { $ops.Add("cmd:2009"); Add-Typed $quoted; $ops.Add("key:enter") }
      "enter" { Select-Ref $first; Add-Typed $quoted; $ops.Add("key:enter") }
      # AutoSum proposes the formula; Enter takes it. The comparison checks it's the step's.
      "autosum" { Select-Ref $first; $ops.Add("cmd:2020"); $ops.Add("key:enter") }
      "fill-right" { Select-Ref $first; $ops.Add("cmd:2015") }
      "fill-down" { Select-Ref $first; $ops.Add("cmd:2014") }
      # The fill handle of the range's first cell, dragged over the range.
      "fill-series" { Select-Ref ($first -replace ':.*$', ''); $ops.Add("fill:$first") }
      "format" {
        Select-Ref $first
        $what = $rest.Substring($first.Length).Trim()
        switch -regex ($what) {
          # (switch -regex runs every clause that matches: each ends with break)
          '^bold$' { $ops.Add("key:ctrl+b"); break }
          '^italic$' { $ops.Add("key:ctrl+i"); break }
          '^underline$' { $ops.Add("key:ctrl+u"); break }
          '^numfmt=#,##0\.00$' { $ops.Add("key:ctrl+shift+1"); break }  # Excel's Number format key
          '^numfmt=0%$' { $ops.Add("key:ctrl+shift+5"); break }
          '^numfmt=(.+)$' { $ops.Add("format:code=" + ($Matches[1] -replace ';', '\;')); break }
          default { $ops.Add("format:" + ($what -replace ';', '\;')) }  # the toolbar's and Format Cells' choices
        }
      }
      "width" {
        $cols = $first; if ($cols -notmatch ':') { $cols = "${cols}:$cols" }
        Select-Ref $cols
        $ops.Add("cmd:2004"); $ops.Add("key:ctrl+a"); $ops.Add("type:" + ($rest -split '\s+')[1]); $ops.Add("key:enter")
      }
      "merge" { Select-Ref $first; $ops.Add("cmd:2026") }
      # Names (s3-calc's script ops, WorkbookNames.h): define or save, delete, Create from Selection.
      "name" { $ops.Add("name:$first=" + (($quoted -replace '^=', '') -replace ';', '\;')) }
      "delete-name" { $ops.Add("name:-$first") }
      "names-create" { Select-Ref $first; $ops.Add("names:create=" + ($rest -split '\s+')[1]) }
      "freeze" { Select-Ref $first; $ops.Add("cmd:2028") }
      # S3's objects (fixtures). Tables, rules and validation: s3-data's script ops on the selection
      # (WorkbookData.cpp: table:, rule:, valid:), values in the region's form.
      "table" {  # table A1:C5 [noheaders] [TableStyleMedium9]: Insert > Table, the first row the headers
        Select-Ref $first
        $ops.Add("table:create" + ((@($rest -split '\s+' | Select-Object -Skip 1 | Where-Object { $_ }) | ForEach-Object { ",$_" }) -join ''))
      }
      "table-total" { Select-Ref $first; $ops.Add("table:total") }  # the Total Row of the table at the cell
      # Table Design on the table at the cell: table-style A1 TableStyleLight9, table-option A1
      # <rows|cols|first|last|filter> (toggled; filter: the Filter Button), table-name A1 Sales,
      # table-resize A1 A1:E9, table-convert A1 (Convert to Range).
      "table-style" { Select-Ref $first; $ops.Add("table:style," + ($rest -split '\s+')[1]) }
      "table-option" { Select-Ref $first; $ops.Add("table:option," + ($rest -split '\s+')[1]) }
      "table-name" { Select-Ref $first; $ops.Add("table:name," + ($rest -split '\s+')[1]) }
      "table-resize" { Select-Ref $first; $ops.Add("table:resize," + ($rest -split '\s+')[1]) }
      "table-convert" { Select-Ref $first; $ops.Add("table:convert") }
      "rule" {  # rule D2:D6 greater "200" / rule D2:D6 bar / rule A2:C6 formula "=$C2>100" [choice=N] [format=N]
        Select-Ref $first
        $v = @($quotes) + @("", "")
        $unquoted = $rest -replace '"((?:[^"\\]|\\.)*)"', ''
        $choice = if ($unquoted -match '\bchoice=(\d+)') { $Matches[1] } else { "0" }  # a gallery's (bars, scales, icons, dates)
        $format = if ($unquoted -match '\bformat=(\d+)') { $Matches[1] } else { "0" }  # the preset format (0 light red fill...)
        $ops.Add("rule:" + ($rest -split '\s+')[1] + "," + ($v[0] -replace ';', '\;') + "," + ($v[1] -replace ';', '\;') + ",$choice,$format")
      }
      "validation" {  # validation E2:E6 list "Yes|No" / validation F2:F6 whole between "1" "10"
        Select-Ref $first
        $words = $rest -split '\s+'
        $v = @($quotes) + @("", "")
        if ($words[1] -eq "list" -or $words[1] -eq "custom") { $ops.Add("valid:$($words[1])," + ($v[0] -replace ';', '\;')) }
        else { $ops.Add("valid:$($words[1]),$($words[2]),$($v[0]),$($v[1])") }
      }
      # Notes, comments, links and protection: s3-review's panels, as typed (WorkbookReview.cpp).
      "note" { Select-Ref $first; $ops.Add("cmd:2902"); Add-Typed $quoted; $ops.Add("key:enter") }  # New Note (Shift+F2)
      "comment" { Select-Ref $first; $ops.Add("key:ctrl+alt+m"); Add-Typed $quoted; $ops.Add("key:enter") }  # New Comment, Post
      "link" { Select-Ref $first; $ops.Add("key:ctrl+k"); Add-Typed $quoted; $ops.Add("key:enter") }  # an address
      "link-place" { Select-Ref $first; $ops.Add("key:ctrl+k"); $ops.Add("key:tab"); Add-Typed $quoted; $ops.Add("key:enter") }
      "protect" { $ops.Add("cmd:2900"); $ops.Add("key:enter") }  # Protect Sheet, no password, Excel's permissions
      # Charts: s3-charts' Insert > Chart commands on the selection (2800 Alt+F1, 2801-2807 by type).
      "chart" {  # chart A1:C5 [column|bar|line|pie|doughnut|area|scatter]
        Select-Ref $first
        $kinds = @{ column = 2801; bar = 2802; line = 2803; pie = 2804; doughnut = 2805; area = 2806; scatter = 2807 }
        $kind = ($rest -split '\s+')[1]
        $ops.Add("cmd:" + $(if ($kind) { $kinds[$kind] } else { 2800 }))
      }
      # Page setup: s3-print's script op (WorkbookPageSetup.cpp), key=value as it takes them.
      "pagesetup" { $ops.Add("pagesetup:" + ($rest.Trim() -replace ';', '\;')) }
      # Pictures and shapes: s3-data's Insert > Pictures and Shapes at the cell (WorkbookShapes.cpp,
      # object:), then the Format panel on the object just put in (it stays selected):
      #   picture A1 "file.png" (beside the steps file), shape C2 <rect|roundrect|oval|line|arrow|textbox> ["text"],
      #   object-fill <RRGGBB|none>, object-outline <RRGGBB|none> [pt], object-text "..", object-alt "..";
      #   row-height A1 60 (Row height on the cell's row, in points)
      "picture" { Select-Ref $first; $ops.Add("object:picture," + (Join-Path (Split-Path -Parent $stepsPath) $quoted)) }
      "shape" { Select-Ref $first; $ops.Add("object:" + ($rest -split '\s+')[1] + $(if ($null -ne $quoted) { "," + ($quoted -replace ';', '\;') } else { "" })) }
      "object-fill" { $ops.Add("object:fill,$first") }
      "object-outline" { $ops.Add("object:outline," + ((@($rest -split '\s+') | Where-Object { $_ }) -join ',')) }
      "object-text" { $ops.Add("object:text," + ($quoted -replace ';', '\;')) }
      "object-alt" { $ops.Add("object:alt," + ($quoted -replace ';', '\;')) }
      "row-height" { Select-Ref $first; $ops.Add("cmd:2003"); $ops.Add("key:ctrl+a"); $ops.Add("type:" + ($rest -split '\s+')[1]); $ops.Add("key:enter") }
      # Anything else through the app's script (App::RunScript): Go to a range, typing, a key, a
      # command id (src\Commands.h), or a raw script op as the app takes it (`app object:rect`;
      # a ';' inside written "\;"). A new object is selected after it's inserted.
      "select" { Select-Ref $first }
      "type" { Add-Typed $quoted }
      "key" { $ops.Add("key:" + $rest.Trim()) }
      "cmd" { $ops.Add("cmd:$first") }
      "app" { $ops.Add($rest.Trim()) }
      "save" { $ops.Add("saveas:$target") }
      "expect" { }  # a fixture's fact, checked in Excel's read-back
      default { throw "line ${n}: unknown step '$op'" }
    }
  }
  if ($pdf) { $ops.Add("exportpdf:$pdf") }  # File > Export PDF: the active sheet
  $ops -join ";"
}

# Plays one steps file in a Featherweight of its own (a --snapshot run: hidden, its own settings
# and clipboard, never Roy's instance). "" when it saved, else why not.
function Invoke-Featherweight([string]$stepsPath, [string]$name, [string]$dir, [string]$pdf) {
  $target = Join-Path $dir "$name.xlsx"
  if (Test-Path $target) { Remove-Item $target }
  if ($pdf -and (Test-Path $pdf)) { Remove-Item $pdf }
  $scriptPath = Join-Path $dir "$name.script.txt"
  [IO.File]::WriteAllText($scriptPath, (ConvertTo-AppScript $stepsPath $target $pdf), [Text.UTF8Encoding]::new($false))
  $log = Join-Path $dir "$name.log"
  $p = Start-Process -FilePath $Featherweight -PassThru -RedirectStandardError $log -ArgumentList @(
    "--snapshot", "`"$(Join-Path $dir "$name.png")`"", "--size", "1400x900", "--script", "`"@$scriptPath`"")
  $null = $p.Handle  # so ExitCode is kept
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; return "timeout" }
  $printed = @(Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ })
  if ($p.ExitCode) { return "exit code $($p.ExitCode)" + $(if ($printed) { ": " + ($printed -join "; ") } else { "" }) }
  if (-not (Test-Path $target)) { return "no saved copy" + $(if ($printed) { ": " + ($printed -join "; ") } else { "" }) }
  ""
}

if ($Featherweight) {
  if (-not [IO.Path]::IsPathRooted($Featherweight)) { $Featherweight = Join-Path $root $Featherweight }
  if (-not (Test-Path $Featherweight)) { throw "Featherweight.exe not found at $Featherweight. Build it (target Featherweight)." }
  $fwDir = Join-Path $Out "featherweight"
  New-Item -ItemType Directory -Force $fwDir | Out-Null
  if (-not $Saved) { $Saved = $fwDir }
}

$bad = 0
$report = @()
if ($Fixtures) {
  # Each fixture: played in Featherweight (saved, and its active sheet exported as PDF), opened in
  # Excel, which reads back the objects (every "expect <fact>" line must be among its facts) and
  # exports the same sheet; then the two PDFs page by page (fwtool pdf-compare, SSIM >= 0.95).
  if (-not $Featherweight) { throw "-Fixtures needs -Featherweight <exe>" }
  if (-not [IO.Path]::IsPathRooted($Fwtool)) { $Fwtool = Join-Path $root $Fwtool }
  if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
  if (-not $PSBoundParameters.ContainsKey("Steps")) { $Steps = @(Get-ChildItem (Join-Path $root "tests\office\fixtures") -Filter *.steps | ForEach-Object FullName) }
  New-Item -ItemType Directory -Force (Join-Path $Out "pdf") | Out-Null
  foreach ($s in $Steps) {
    $name = [IO.Path]::GetFileNameWithoutExtension($s)
    $ourPdf = Join-Path $Out "pdf\$name.featherweight.pdf"
    $excelPdf = Join-Path $Out "pdf\$name.excel.pdf"
    $why = Invoke-Featherweight (Resolve-Path $s).Path $name $fwDir $ourPdf
    if ($why) { "[FAIL] $name in Featherweight: $why"; $bad++; continue }
    if (-not (Test-Path $ourPdf)) { "[FAIL] $($name): Featherweight's Export PDF wrote nothing"; $bad++; continue }
    "[ ok ] $name in Featherweight: $(Join-Path $fwDir "$name.xlsx"), $ourPdf"
    if (Test-Path $excelPdf) { Remove-Item $excelPdf }
    $d = Invoke-Child "dump" (Resolve-Path (Join-Path $fwDir "$name.xlsx")).Path $excelPdf
    if ($d.error) { "[FAIL] $($name): Excel didn't read Featherweight's copy: $($d.error)"; $bad++; continue }
    $facts = @($d.facts | ForEach-Object { "$_".TrimEnd() })
    [IO.File]::WriteAllLines((Join-Path $Out "$name.facts.txt"), [string[]]$facts, [Text.UTF8Encoding]::new($false))
    $expected = @(Get-Content -LiteralPath $s -Encoding UTF8 | ForEach-Object { $_.Trim() } | Where-Object { $_ -like "expect *" } | ForEach-Object { $_.Substring(7) })
    $have = New-Object 'Collections.Generic.HashSet[string]' (, [string[]]$facts)
    # An expect with * matches as a wildcard (a note's author is the Windows user's name).
    $missing = @($expected | Where-Object { $want = $_; if ($want.Contains('*')) { -not ($facts | Where-Object { $_ -like $want }) } else { -not $have.Contains($want) } })
    if ($missing.Count) {
      $bad++
      "[FAIL] $($name): $($missing.Count) of $($expected.Count) expected facts not in Excel's read-back ($name.facts.txt)"
      foreach ($m in $missing | Select-Object -First 15) {
        $key = ($m -split ' ')[0..1] -join ' '
        "       want: $m"
        $near = @($facts | Where-Object { $_ -like "$key *" } | Select-Object -First 2)
        foreach ($x in $near) { "       Excel: $x" }
      }
    } else { "[ ok ] $($name): Excel reads back all $($expected.Count) expected facts" }
    $recalc = @($d.recalc | Where-Object { $_ })
    if ($recalc.Count) {
      $bad++
      "[FAIL] $($name): $($recalc.Count) cells show another result once Excel recalculates"
      $recalc | Select-Object -First 15 | ForEach-Object { "       $_" }
    } else { "[ ok ] $($name): Excel's recalculation gives Featherweight's results" }
    $pdfOut = & $Fwtool pdf-compare --out (Join-Path $Out "pdf") --json (Join-Path $Out "pdf\$name.json") $excelPdf $ourPdf 2>&1
    $pdfOk = $LASTEXITCODE -eq 0
    if (-not $pdfOk) { $bad++ }
    "[$(if ($pdfOk) { ' ok ' } else { 'FAIL' })] $($name) PDF: $(@($pdfOut)[-1])"
    if (-not $pdfOk) { @($pdfOut) | Where-Object { $_ -match 'FAILS' } | Select-Object -First 5 | ForEach-Object { "       $_" } }
    $report += [ordered]@{ name = $name; missing = $missing; recalc = $recalc; pdf = "$(@($pdfOut)[-1])" }
  }
  $Steps = @()  # the build-from-scratch loop below has nothing left to do
}
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
  if ($Featherweight) {
    $why = Invoke-Featherweight (Resolve-Path $s).Path $name $fwDir
    if ($why) { "[FAIL] $name in Featherweight: $why"; $bad++; continue }
    "[ ok ] $name in Featherweight: saved as $(Join-Path $fwDir "$name.xlsx")"
    foreach ($line in @(Get-Content (Join-Path $fwDir "$name.log") -ErrorAction SilentlyContinue | Where-Object { $_ })) { "       $line" }
  }
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
