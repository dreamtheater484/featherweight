<#
.SYNOPSIS
  Makes the reference renders (plan section 10): Office exports each corpus file to PDF, on the
  Office reference PC (decision 19). The manifest records:
  - the Office build
  - the page count
  - the fonts Office actually drew with, from each PDF
  - the source file's hash
  `fwtool compare` (D1) measures Featherweight against these PDFs.

  Office is driven by automation and doesn't show a window:
  - Files are opened read-only, with macros off, links not updated, and nothing added to Office's
    recent files. Nothing is saved.
  - Each file gets its own Office process, which is ended if the file hangs it (for example with
    a repair prompt). The manifest then says why there is no PDF.

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
#>
param(
  [string[]]$Folders = @("tests\office\generated", "tests\office\edge", "tests\office\layout"),
  [string]$Out = "tests\office\reference",
  [string]$Corpus,
  [switch]$Force,
  [int]$TimeoutSeconds = 120,
  # Internal: export one file in this process (the parent runs one child per file).
  [string]$Child, [string]$Source, [string]$Pdf, [string]$ResultFile
)

$ErrorActionPreference = "Stop"
$apps = @{
  Word = @{ Exe = "WINWORD"; ProgId = "Word.Application"; Ext = @(".docx", ".docm", ".dotx", ".dotm") }
  Excel = @{ Exe = "EXCEL"; ProgId = "Excel.Application"; Ext = @(".xlsx", ".xlsm", ".xltx", ".xltm") }
  PowerPoint = @{ Exe = "POWERPNT"; ProgId = "PowerPoint.Application"; Ext = @(".pptx", ".pptm", ".potx", ".potm", ".ppsx", ".ppsm") }
}

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# ---- Child: open one file in a new Office process, export it, quit.
if ($Child) {
  $app = $apps[$Child]
  $missing = [Type]::Missing
  $before = @(Get-Process $app.Exe -ErrorAction SilentlyContinue | ForEach-Object Id)
  $office = New-Object -ComObject $app.ProgId
  $officePid = @(Get-Process $app.Exe -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | ForEach-Object Id)
  $result = [ordered]@{ pid = $officePid; app = "$Child $($office.Version) (build $($office.Build))" }
  Write-Json $result $ResultFile  # the parent reads the process id from here if it has to end it
  try {
    $office.AutomationSecurity = 3  # msoAutomationSecurityForceDisable: macros never run
    switch ($Child) {
      "Word" {
        $office.Visible = $false
        $office.DisplayAlerts = 0  # wdAlertsNone
        # FileName, ConfirmConversions, ReadOnly, AddToRecentFiles, PasswordDocument, PasswordTemplate, Revert,
        # WritePasswordDocument, WritePasswordTemplate, Format, Encoding, Visible, OpenAndRepair, DocumentDirection,
        # NoEncodingDialog, all by reference. A dummy password makes a protected file fail instead of prompting.
        $doc = $office.Documents.Open([ref]$Source, [ref]$false, [ref]$true, [ref]$false, [ref]"-", [ref]"-", [ref]$false,
                                      [ref]"-", [ref]"-", [ref]0, [ref]$missing, [ref]$false, [ref]$false, [ref]$missing, [ref]$true)
        $result.pages = $doc.ComputeStatistics(2)  # wdStatisticPages
        # wdExportFormatPDF, optimized for print, the whole document without markup, tagged, missing fonts as bitmaps.
        $doc.ExportAsFixedFormat($Pdf, 17, $false, 0, 0, 1, 1, 0, $false, $true, 0, $true, $true, $false)
        $doc.Close([ref]0)  # wdDoNotSaveChanges
        $office.Quit([ref]0)
      }
      "Excel" {
        $office.Visible = $false
        $office.DisplayAlerts = $false
        $office.AskToUpdateLinks = $false
        $office.EnableEvents = $false
        # Filename, UpdateLinks (never), ReadOnly. Automation doesn't add files to Excel's recent list.
        $wb = $office.Workbooks.Open($Source, 0, $true)
        # xlTypePDF, standard quality, print areas respected
        $wb.ExportAsFixedFormat(0, $Pdf, 0, $false, $false, $missing, $missing, $false)
        $wb.Close($false)
        $office.Quit()
      }
      "PowerPoint" {
        # ReadOnly, not untitled, no window
        $pres = $office.Presentations.Open($Source, -1, 0, 0)
        $pres.SaveCopyAs($Pdf, 32)  # ppSaveAsPDF
        $pres.Close()
        $office.Quit()
      }
    }
  } catch {
    $result.error = $_.Exception.Message
    try { if ($Child -eq "Word") { $office.Quit([ref]0) } else { $office.Quit() } } catch {}
  } finally {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($office)
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

# Each job: the source file, its name in the manifest, and its PDF relative to the output folder.
$jobs = @()
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
  $screened = @("docx", "xlsx", "pptx") | ForEach-Object { Join-Path $corpusRoot $_ } | Where-Object { Test-Path $_ }
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
    $dir = Join-Path $outRoot $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    foreach ($f in Get-ChildItem (Join-Path $root $folder) -File | Sort-Object Name) {
      if (Get-Kind $f.Extension) { $jobs += @{ File = $f; Source = "$name/$($f.Name)"; Pdf = "$name/$($f.Name).pdf" } }
    }
  }
}

$entries = @()
$n = 0
$reused = 0
foreach ($job in $jobs) {
  $n++
  $f = $job.File
  $kind = Get-Kind $f.Extension
  $pdfPath = Join-Path $outRoot ($job.Pdf -replace '/', '\')
  $entry = [ordered]@{ source = $job.Source; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower() }
  $previous = $old | Where-Object { $_.source -eq $job.Source -and $_.sha256 -eq $entry.sha256 -and $_.pdf -and -not $_.error } | Select-Object -First 1
  if ($previous -and (Test-Path $pdfPath)) {
    $entries += $previous
    $reused++
    continue
  }
  New-Item -ItemType Directory -Force (Split-Path -Parent $pdfPath) | Out-Null
  $resultPath = Join-Path $tmp "result.json"
  if (Test-Path $resultPath) { Remove-Item $resultPath }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
    "-Child", $kind, "-Source", "`"$($f.FullName)`"", "-Pdf", "`"$pdfPath`"", "-ResultFile", "`"$resultPath`"")
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    # Office is stuck, usually on a prompt: end the child and the Office process it started.
    $stuck = if (Test-Path $resultPath) { (Get-Content $resultPath -Raw | ConvertFrom-Json).pid } else { @() }
    foreach ($id in $stuck) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $entry.error = "$kind didn't finish within $TimeoutSeconds s"
  } elseif (Test-Path $resultPath) {
    $r = Get-Content $resultPath -Raw | ConvertFrom-Json
    $entry.app = $r.app
    if ($r.error) { $entry.error = $r.error }
    if ($r.pages) { $entry.pages = $r.pages }
  } else {
    $entry.error = "$kind didn't start"
  }
  if (-not $entry.error -and (Test-Path $pdfPath)) {
    $facts = Get-PdfFacts $pdfPath
    $entry.pdf = $job.Pdf
    if (-not $entry.pages) { $entry.pages = $facts.pages }
    $entry.fonts = $facts.fonts
    "[{0}/{1}] {2,-45} {3,3} pages  {4}" -f $n, $jobs.Count, $entry.source, $entry.pages, ($facts.fonts -join ", ")
  } else {
    if (-not $entry.error) { $entry.error = "no PDF was written" }
    "[{0}/{1}] {2,-45} FAILED: {3}" -f $n, $jobs.Count, $entry.source, $entry.error
  }
  $entries += [pscustomobject]$entry
}

$manifest = [ordered]@{
  made = (Get-Date).ToString("yyyy-MM-dd")
  computer = "Office reference PC (decision 19)"
  export = "Word: the document without markup, for print, tagged; Excel: print areas; PowerPoint: slides"
}
if ($Corpus) { $manifest.corpus = $corpusRoot }  # fwtool compare finds the documents here
# Folders stay in the order the manifest had them, new folders after them; files by name.
$groups = @(@($previousFiles) + @($entries) | ForEach-Object { ($_.source -split '/')[0] } | Select-Object -Unique)
$manifest.files = @(@($kept) + @($entries) | Sort-Object { if ($Corpus) { 0 } else { [array]::IndexOf($groups, ($_.source -split '/')[0]) } }, { $_.source })
Write-Json $manifest $manifestPath
$unchanged = if ($reused) { ", $reused of them unchanged since the last run" } else { "" }
"$(@($entries | Where-Object { -not $_.error }).Count) of $($entries.Count) exported$unchanged. The PDFs and manifest.json are in $outRoot"
