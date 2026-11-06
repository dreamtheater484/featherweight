<#
.SYNOPSIS
  D5's test files, made by Word on this PC (docs/d5-plan.md, "Quality"):
  - .doc (Word 97-2003) and .odt copies of Word documents, for the .doc and .odt importers;
  - password-protected .docx (agile encryption) and .doc (RC4 CryptoAPI), password "featherweight";
  - .dotx templates;
  - documents Word writes from scratch: charts (column, bar, line, pie, area, scatter, and radar
    for a type that's drawn from its fallback), equations (OMML: fractions, scripts, radicals,
    n-ary, matrices, cases, equation arrays, accents, boxes, limits, functions, an inline one),
    right-to-left text (Arabic and Hebrew paragraphs, mixed runs, an RTL table and list) and East
    Asian text (Japanese with a document grid and a vertical section, Chinese, Korean).

  -Public (the default) writes into the repository, from tests\office\generated and
  tests\office\torture: tests\office\doc, odt, dotx, protected and script.
  -Private writes Word's copies of the private corpus ($env:FW_CORPUS) next to its docx folder:
  <corpus>\doc, odt, dotx and protected, by category. They stay outside the repository.
  Then scripts\reference-renders.ps1 makes their reference PDFs (it opens "protected" folders with
  the test password).

  Word: a new instance of its own, in safe mode (/safe), hidden, with alerts off and macros off.
  COM attaches to it by a caption only this script knows, and only that Word (and an Excel it
  started for a chart's data) is ever ended. One Office automation at a time on this PC (a global
  mutex shared with reference-renders.ps1 and word-open-check.ps1). Sources open read-only and
  are never saved over.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-test-files.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-test-files.ps1 -Private
#>
param(
  [switch]$Private,
  [string]$Corpus = $env:FW_CORPUS,
  [string]$Password = "featherweight"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$missing = [Type]::Missing

# Word's SaveAs2 formats.
$fmtDocx = 12; $fmtDoc = 0; $fmtOdt = 23; $fmtDotx = 14

$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
$started = New-Object Collections.Generic.List[int]
$since = Get-Date
$word = $null
$made = 0; $failed = @()

function Stop-Ours {
  if ($script:word) {
    try { $script:word.Quit([ref]0) } catch {}
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($script:word)
    $script:word = $null
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
  }
  foreach ($id in $started) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "WINWORD" -and -not $p.WaitForExit(15000)) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
  # An Excel that Word started for a chart's data: COM-launched (-Embedding), after this script began.
  Get-CimInstance Win32_Process -Filter "Name='EXCEL.EXE'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match '-Embedding' -and $_.CreationDate -gt $since } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Open-Read([string]$path) {
  # FileName, ConfirmConversions, ReadOnly, AddToRecentFiles, PasswordDocument, ..., NoEncodingDialog.
  $word.Documents.Open([ref]$path, [ref]$false, [ref]$true, [ref]$false, [ref]"-", [ref]"-", [ref]$false,
                       [ref]"-", [ref]"-", [ref]0, [ref]$missing, [ref]$false, [ref]$false, [ref]$missing, [ref]$true)
}

function Save-As($doc, [string]$path, [int]$format, [string]$pw = "") {
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  if (Test-Path $path) { Remove-Item $path }
  if ($pw) { $doc.SaveAs2([ref]$path, [ref]$format, [ref]$false, [ref]$pw) }
  else { $doc.SaveAs2([ref]$path, [ref]$format) }
}

# A copy of `src` in each of `targets` (path, format, password); the source opens anew for each.
function Convert-File([string]$src, $targets) {
  foreach ($t in $targets) {
    try {
      $doc = Open-Read $src
      try { Save-As $doc $t[0] $t[1] $t[2] } finally { $doc.Close([ref]0) }
      $script:made++
    } catch { $script:failed += "$src -> $($t[0]): $($_.Exception.Message)" }
  }
}

try {
  $exe = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Winword.exe").'(default)'
  $safe = Start-Process -FilePath $exe -ArgumentList "/safe", "/automation", "-Embedding" -PassThru -WindowStyle Hidden
  $started.Add($safe.Id)
  [void]$safe.WaitForInputIdle(30000)
  Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class FwWin {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public static bool HasTitle(int pid, string text) {
    bool found = false;
    EnumWindows((h, l) => { uint o; GetWindowThreadProcessId(h, out o); var s = new StringBuilder(512); GetWindowText(h, s, 512);
      if ((int)o == pid && s.ToString().Contains(text)) found = true; return !found; }, IntPtr.Zero);
    return found;
  }
}
"@
  foreach ($wait in 2, 4, 8, 0) {
    Start-Sleep -Seconds $wait
    $before = @(Get-Process WINWORD -ErrorAction SilentlyContinue | ForEach-Object Id)
    $word = New-Object -ComObject Word.Application
    $new = @(Get-Process WINWORD -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object Id)
    $mark = "fw-make-test-files $PID $wait"
    $caption = $word.Caption
    $word.Caption = $mark
    $ours = $false
    for ($i = 0; $i -lt 30 -and -not $ours; $i++) { $ours = [FwWin]::HasTitle($safe.Id, $mark); if (-not $ours) { Start-Sleep -Milliseconds 100 } }
    if ($ours) { break }
    # Another program's Word answered: leave it as it was.
    if ($new.Count) { try { $word.Quit([ref]0) } catch {} } else { try { $word.Caption = $caption } catch {} }
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word)
    $word = $null
  }
  if (-not $word) { throw "Word didn't connect to the safe-mode instance" }
  "Word $($word.Version) (build $($word.Build)), safe mode"
  $word.AutomationSecurity = 3   # macros never run
  $word.Visible = $false
  $word.DisplayAlerts = 0        # wdAlertsNone: no compatibility or format-loss prompts
  $word.Options.UpdateLinksAtOpen = $false

  if ($Private) {
    if (-not $Corpus -or -not (Test-Path (Join-Path $Corpus "docx"))) { throw "Set FW_CORPUS (or -Corpus) to the private corpus." }
    $docx = (Resolve-Path (Join-Path $Corpus "docx")).Path
    $all = Get-ChildItem $docx -Recurse -File -Filter *.docx | Sort-Object FullName
    foreach ($f in $all) {
      $rel = $f.FullName.Substring($docx.Length + 1)
      $base = [IO.Path]::ChangeExtension($rel, $null).TrimEnd('.')
      $targets = @(, @((Join-Path $Corpus "doc\$base.doc"), $fmtDoc, ""))
      $targets += , @((Join-Path $Corpus "odt\$base.odt"), $fmtOdt, "")
      $cat = Split-Path -Parent $rel
      if ($cat -in "letters", "cv", "reports") { $targets += , @((Join-Path $Corpus "dotx\$base.dotx"), $fmtDotx, "") }
      Convert-File $f.FullName $targets
    }
    # Password-protected: the first two of each of three categories.
    foreach ($cat in "letters", "reports", "forms") {
      foreach ($f in @($all | Where-Object { (Split-Path -Leaf (Split-Path -Parent $_.FullName)) -eq $cat } | Select-Object -First 2)) {
        $base = "$cat\" + [IO.Path]::GetFileNameWithoutExtension($f.Name)
        Convert-File $f.FullName @(@((Join-Path $Corpus "protected\$base.docx"), $fmtDocx, $Password),
                                   @((Join-Path $Corpus "protected\$base.doc"), $fmtDoc, $Password))
      }
    }
  } else {
    $out = Join-Path $root "tests\office"
    $sources = @(Get-ChildItem "tests\office\generated", "tests\office\torture" -File -Filter *.docx | Sort-Object Name)
    foreach ($f in $sources) {
      $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
      Convert-File $f.FullName @(@("$out\doc\$base.doc", $fmtDoc, ""), @("$out\odt\$base.odt", $fmtOdt, ""))
    }
    foreach ($name in "simple", "styles-unicode-comments") {
      Convert-File "$root\tests\office\generated\$name.docx" @(, @("$out\dotx\$name.dotx", $fmtDotx, ""))
    }
    foreach ($name in "simple", "tables-images-links") {
      Convert-File "$root\tests\office\generated\$name.docx" @(@("$out\protected\$name-protected.docx", $fmtDocx, $Password),
                                                             @("$out\protected\$name-protected.doc", $fmtDoc, $Password))
    }

    # Documents Word writes from scratch.
    $c = Get-Content "tests\office\make_test_files.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    $script = "$out\script"

    # Charts: the types fwchart draws, and radar for one it shows from its fallback.
    try {
      $doc = $word.Documents.Add()
      $doc.Content.Text = "Charts"
      $doc.Paragraphs.Item(1).Style = -2  # wdStyleHeading1
      $types = @(@("Clustered column", 51), @("Clustered bar", 57), @("Line with markers", 65), @("Pie", 5),
                 @("Area", 1), @("Scatter", -4169), @("Radar", -4151))
      foreach ($t in $types) {
        $doc.Content.InsertParagraphAfter()
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $t[0]
        $r.Style = -3  # wdStyleHeading2
        $doc.Content.InsertParagraphAfter()
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Style = -1  # wdStyleNormal
        $shape = $doc.InlineShapes.AddChart2(-1, $t[1], $r)
        $shape.Chart.HasTitle = $true
        $shape.Chart.ChartTitle.Text = "$($t[0]) chart"
        # Word opens the chart's data grid with a new chart; close it before the next one.
        try { $shape.Chart.ChartData.Workbook.Close() } catch {}
      }
      Save-As $doc "$script\charts.docx" $fmtDocx
      $doc.Close([ref]0)
      $made++
    } catch { $failed += "charts.docx: $($_.Exception.Message)" }

    # Equations: display ones under headings, and one inline.
    try {
      $doc = $word.Documents.Add()
      $doc.Content.Text = "Equations"
      $doc.Paragraphs.Item(1).Style = -2
      foreach ($eq in $c.equations) {
        $doc.Content.InsertParagraphAfter()
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $eq[0]
        $r.Style = -3
        $doc.Content.InsertParagraphAfter()
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $eq[1]
        $r.Style = -1
        $m = $doc.OMaths.Add($r)
        $m.OMaths.Item(1).BuildUp()
      }
      $doc.Content.InsertParagraphAfter()
      $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
      $r.Text = ($c.inline -join "")
      $r.Style = -1
      $start = $r.Start + $c.inline[0].Length
      $m = $doc.OMaths.Add($doc.Range($start, $start + $c.inline[1].Length))
      $m.OMaths.Item(1).BuildUp()
      Save-As $doc "$script\equations.docx" $fmtDocx
      $doc.Close([ref]0)
      $made++
    } catch { $failed += "equations.docx: $($_.Exception.Message)" }

    # Right to left: Arabic and Hebrew paragraphs, a mixed left-to-right one, an RTL list and table.
    try {
      $doc = $word.Documents.Add()
      $first = $true
      $paras = @() + @($c.arabic | ForEach-Object { , @($_, 1025) }) + @($c.hebrew | ForEach-Object { , @($_, 1037) })
      foreach ($p in $paras) {
        if (-not $first) { $doc.Content.InsertParagraphAfter() }
        $first = $false
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $p[0]
        $r.ParagraphFormat.ReadingOrder = 0  # wdReadingOrderRtl
        $r.LanguageID = $p[1]
        $r.Font.NameBi = "Arial"
      }
      $doc.Content.InsertParagraphAfter()
      $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
      $r.Text = $c.mixed
      $r.ParagraphFormat.ReadingOrder = 1  # left to right
      # A numbered list, right to left.
      $listStart = $doc.Paragraphs.Count + 1
      foreach ($t in $c.rtlTable | ForEach-Object { $_[0] }) {
        $doc.Content.InsertParagraphAfter()
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $t
        $r.ParagraphFormat.ReadingOrder = 0
        $r.LanguageID = 1025
      }
      $doc.Range($doc.Paragraphs.Item($listStart).Range.Start, $doc.Content.End).ListFormat.ApplyNumberDefault()
      $doc.Content.InsertParagraphAfter()
      $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
      $r.ListFormat.RemoveNumbers()
      $tbl = $doc.Tables.Add($r, $c.rtlTable.Count, $c.rtlTable[0].Count)
      $tbl.TableDirection = 0  # wdTableDirectionRtl
      $tbl.Borders.Enable = $true
      for ($i = 0; $i -lt $c.rtlTable.Count; $i++) {
        for ($j = 0; $j -lt $c.rtlTable[$i].Count; $j++) { $tbl.Cell($i + 1, $j + 1).Range.Text = $c.rtlTable[$i][$j] }
      }
      $tbl.Range.ParagraphFormat.ReadingOrder = 0
      $tbl.Range.LanguageID = 1025
      Save-As $doc "$script\rtl.docx" $fmtDocx
      $doc.Close([ref]0)
      $made++
    } catch { $failed += "rtl.docx: $($_.Exception.Message)" }

    # East Asian: Japanese with a document grid, Chinese and Korean, then a vertical section.
    try {
      $doc = $word.Documents.Add()
      $doc.Sections.Item(1).PageSetup.LayoutMode = 1  # wdLayoutModeGrid: characters and lines
      $first = $true
      $paras = @() + @($c.japanese | ForEach-Object { , @($_, 1041, "Yu Mincho") }) +
               @($c.chineseSimplified | ForEach-Object { , @($_, 2052, "Microsoft YaHei") }) +
               @($c.chineseTraditional | ForEach-Object { , @($_, 1028, "Microsoft JhengHei") }) +
               @($c.korean | ForEach-Object { , @($_, 1042, "Malgun Gothic") })
      foreach ($p in $paras) {
        if (-not $first) { $doc.Content.InsertParagraphAfter() }
        $first = $false
        $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
        $r.Text = $p[0]
        $r.LanguageIDFarEast = $p[1]
        $r.Font.NameFarEast = $p[2]
      }
      # A section of vertical text (wdSectionBreakNextPage, wdTextOrientationVerticalFarEast).
      $doc.Content.InsertParagraphAfter()
      $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
      $r.Collapse(1)  # wdCollapseStart: the break goes before the empty paragraph
      $r.InsertBreak(2)
      $r = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
      $r.Text = $c.vertical
      $r.LanguageIDFarEast = 1041
      $r.Font.NameFarEast = "Yu Mincho"
      try { $doc.Sections.Item($doc.Sections.Count).Range.Orientation = 1 } catch { $failed += "eastasia.docx: vertical text: $($_.Exception.Message)" }
      Save-As $doc "$script\eastasia.docx" $fmtDocx
      $doc.Close([ref]0)
      $made++
    } catch { $failed += "eastasia.docx: $($_.Exception.Message)" }
  }
} finally {
  Stop-Ours
  $mutex.ReleaseMutex()
}
""
"$made files made, $($failed.Count) failed"
$failed | ForEach-Object { "  FAILED: $_" }
exit $(if ($failed.Count) { 1 } else { 0 })
