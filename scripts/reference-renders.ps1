<#
.SYNOPSIS
  Makes the reference renders (plan section 10): Office exports each public corpus file to PDF,
  on the Office reference PC (decision 19). The PDFs and a manifest go in tests\office\reference\.
  The manifest records:
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

  Run it again whenever Office changes noticeably. It replaces every PDF and the manifest.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\reference-renders.ps1
#>
param(
  [string[]]$Folders = @("tests\office\generated", "tests\office\edge"),
  [string]$Out = "tests\office\reference",
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
$outRoot = Join-Path $root $Out
if (Test-Path $outRoot) { Remove-Item -Recurse -Force $outRoot }
New-Item -ItemType Directory -Force $outRoot | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-reference-renders"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Get-PdfFacts([string]$path) {
  $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($path))
  $fonts = [regex]::Matches($text, '/BaseFont\s*/([^\s/\[\]<>()]+)') | ForEach-Object { $_.Groups[1].Value -replace '^[A-Z]{6}\+', '' -replace '#20', ' ' }
  @{ pages = [regex]::Matches($text, '/Type\s*/Page(?![A-Za-z])').Count; fonts = @($fonts | Sort-Object -Unique) }
}

$entries = @()
foreach ($folder in $Folders) {
  $name = Split-Path -Leaf $folder
  New-Item -ItemType Directory -Force (Join-Path $outRoot $name) | Out-Null
  foreach ($f in Get-ChildItem (Join-Path $root $folder) -File | Sort-Object Name) {
    $kind = $apps.Keys | Where-Object { $apps[$_].Ext -contains $f.Extension.ToLower() } | Select-Object -First 1
    if (-not $kind) { continue }
    $pdfPath = Join-Path $outRoot "$name\$($f.Name).pdf"
    $resultPath = Join-Path $tmp "result.json"
    if (Test-Path $resultPath) { Remove-Item $resultPath }
    $entry = [ordered]@{ source = "$name/$($f.Name)"; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower() }
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
      $entry.pdf = "$name/$($f.Name).pdf"
      if (-not $entry.pages) { $entry.pages = $facts.pages }
      $entry.fonts = $facts.fonts
      "{0,-45} {1,3} pages  {2}" -f $entry.source, $entry.pages, ($facts.fonts -join ", ")
    } else {
      if (-not $entry.error) { $entry.error = "no PDF was written" }
      "{0,-45} FAILED: {1}" -f $entry.source, $entry.error
    }
    $entries += [pscustomobject]$entry
  }
}

Write-Json ([ordered]@{
  made = (Get-Date).ToString("yyyy-MM-dd")
  computer = "Office reference PC (decision 19)"
  export = "Word: the document without markup, for print, tagged; Excel: print areas; PowerPoint: slides"
  files = $entries
}) (Join-Path $outRoot "manifest.json")
"$(@($entries | Where-Object { -not $_.error }).Count) of $($entries.Count) exported. The PDFs and manifest.json are in $outRoot"
