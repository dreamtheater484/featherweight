<#
.SYNOPSIS
  Builds the real-world corpus (plan section 10) of Word documents, workbooks and CSV files from
  tests\office\real-world.json: downloads every file, checks its hash and screens it, so anyone
  can rebuild the corpus exactly. The files are never committed (their licences vary); only the
  manifest is. An entry's format follows its "file" (docx/, xlsx/, csv/, pptx/), or for a candidate
  its "format" or its URL's extension: .docx; .xlsx or .xltx; .csv, .tsv or .txt; .pptx, .ppsx or .potx.

  Decks (P1) get the same screen (steps 1, 2, 4 and 5) with their own content rules (step 3): a plain
  (macro-free) presentation, show or template; no VBA, ActiveX, OLE objects or embedded packages
  (nothing in ppt/embeddings/ but a chart's own workbook, screened inside), web extensions or ribbon
  customizations; no external relationship but an http, https or mailto hyperlink (linked pictures,
  sounds and videos would reach out during the show); and no click or hover action that leaves the
  show (ppaction://program, macro, ole, hlinkfile, hlinkpres): only slide jumps, show jumps (next,
  previous, first, last, end) and custom shows. On top of that deny-list, an allow-list (Roy,
  2026-11-20): every part's content type must be PresentationML, DrawingML, a theme, the package's
  relationships or docProps, a picture (PNG, JPEG, GIF, BMP, TIFF, EMF, WMF, SVG), a font
  (fntdata, odttf, ttf), a chart or chart style, or audio or video; anything else (custom XML,
  VML, ink, JPEG XR, sensitivity labels, unknown binaries) refuses the deck, and so does a part
  without a content type. Every part's first bytes are sniffed: an executable (MZ, ELF, Mach-O),
  a shortcut (.lnk), a compound (OLE) file or a nested zip (but a chart's workbook) refuses the
  deck, and so does text (a script, HTML, an HTA) where a picture, sound, video or font should be,
  a picture that isn't one, or an SVG that scripts, embeds or links out. The facts recorded: the producer, the slides, hidden
  slides, the slide size, and tags (pictures, photo-heavy, charts, tables, smartart, groups, wordart,
  3d, autofit, svg, transitions, animations, sounds, video, audio, hyperlinks, notes, comments,
  sections, custom-shows, embedded-fonts, ink, rtl, east-asian, hidden-slides).

  Corpus layout (point FW_CORPUS at the folder):
    docx\<category>\<id>.docx           the documents that passed the screen
    xlsx\<category>\<id>.xlsx           the workbooks (and .xltx templates) that passed it
    csv\<category>\<id>.csv             the CSV and TSV files that passed it
    pptx\<category>\<id>.pptx           the decks (and .ppsx shows, .potx templates) that passed it
    reference\docx\<category>\<id>.docx.pdf, reference\xlsx\...\<id>.xlsx.pdf and .cells.json,
    reference\manifest.json             Office's PDFs (scripts\reference-renders.ps1 -Corpus)
    screen.json                         this run's results: every file, passed or rejected, and why
    functions.json                      the functions the screened workbooks' formulas use, counted

  Workbooks get the same screen as documents (steps 1, 2, 4 and 5), with their own content rules
  (step 3): a plain (macro-free) workbook or template; no VBA or macro sheets, no external links
  (externalLink parts), data connections, query tables, Power Query (DataMashup), pivot caches
  from external sources, OLE objects, ActiveX, embeddings, web extensions or form controls with
  macros; no formulas (cells, names, conditional formats, validation) with DDE (a "|" outside
  text) or WEBSERVICE, FILTERXML, RTD, CALL, REGISTER, EXEC, EVALUATE and other XLM functions;
  hyperlinks only to http, https and mailto. CSV files: text (no zip, no web page, no binary
  data), and no formula-like field that would reach out (DDE or the functions above); then
  Defender. The screen records each file's facts: a workbook's producer, sheets, cells, formulas,
  tags (charts, tables, conditional formats, ...) and the functions it uses; a CSV's encoding,
  byte-order mark, delimiter, line ends, rows and columns.

  Every file passes the safety screen before Word or Featherweight opens it:
  1. Handling: each download goes into its own new, empty quarantine folder. Nothing from a
     download is ever run. Only HTTPS, and every redirect must stay on an allowed host (the
     "sources" list in the manifest). A source with "paths" allows only URLs under those paths
     (a shared host such as raw.githubusercontent.com, limited to one project's repository).
  2. Type: a .docx under 20 MB, a zip whose [Content_Types].xml declares the normal (not
     macro-enabled) WordprocessingML main document.
  3. Content. Rejected:
     - VBA, macro-enabled content types, ActiveX, OLE objects, anything in word/embeddings/,
       altChunk parts, web extensions and task panes, ribbon customizations, mail merge sources.
       One exception: a chart's own workbook, an .xlsx in word/embeddings/ that only a chart part
       refers to. It must be a plain workbook and pass the same kind of screen inside: no
       macros, ActiveX, OLE, external links, data connections or web functions in formulas.
     - an attached template
     - an external relationship other than a hyperlink, and a hyperlink (relationship, field or
       VML href) that isn't plain http, https or mailto
     - DDE, DDEAUTO, LINK, IMPORT, INCLUDE, INCLUDETEXT and INCLUDEPICTURE fields, and field
       codes that start with a nested field (so the keyword can't be computed)
     - ms-msdt, mhtml: or !x-usc: anywhere in the XML; a DTD in any part
     - a part or all parts together expanding past 64 MB or 256 MB while they're read
  4. Structure: `fwtool check` passes (it also refuses zip bombs and malformed packages).
  5. Antivirus (Microsoft Defender; no Defender or security setting, exclusion or policy is ever
     changed). Before anything is downloaded: its service, antivirus and real-time protection must
     be on and its signatures at most 24 h old (else MpCmdRun -SignatureUpdate, which changes no
     setting); and the standard EICAR test file, written into a new, empty quarantine subfolder,
     must be caught (so no exclusion skips the quarantine), then deleted. Each file is scanned on
     its own right after its download, before the content screen opens it, and again in the
     corpus folder after the screen. A scan passes only with exit 0 and "found no threats"
     (MpCmdRun exits 0 when it found a threat and removed it). A detection stops downloads from
     that file's hosts; Defender's own detection record for the run is checked as well.
  Only files that pass everything move into the corpus. Rejected files are deleted at once;
  screen.json keeps the reasons and the Defender status, the EICAR proof and any detection. A site
  that asks for a browser challenge is skipped, never worked around. Certificates are always
  checked (curl's default; never -k), downloads are capped at 20 MB and their SHA-256 recorded, and
  nothing from a download is run, unpacked to disk or opened by Explorer.

  The screen also records facts for the manifest: the producer (Word version, LibreOffice, or
  Google Docs, inferred when there are no docProps), Word's compatibility mode, the page count
  Word last saved, and tags for the structures each file contains (plan section 10).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\fetch-corpus.ps1 -Out D:\FeatherweightCorpus
.EXAMPLE
  # Screen the files already in the corpus again, without downloading (after the rules change)
  powershell -ExecutionPolicy Bypass -File scripts\fetch-corpus.ps1 -Out D:\FeatherweightCorpus -Rescreen
.EXAMPLE
  # Add candidates (url, publisher, terms, terms_url, category, language, title, landing): the
  # ones that pass are added to the manifest with their hash and facts, the others to its
  # "rejected" list. Known URLs are skipped; -Retry tries rejected ones again.
  powershell -ExecutionPolicy Bypass -File scripts\fetch-corpus.ps1 -Out D:\FeatherweightCorpus -Candidates new.json
.EXAMPLE
  # Screen local files and print what the screen found; nothing is moved
  powershell -ExecutionPolicy Bypass -File scripts\fetch-corpus.ps1 -Screen a.docx, b.docx
#>
param(
  [string]$Out = $env:FW_CORPUS,
  [string]$Manifest,
  [string]$Fwtool,
  [string]$Candidates,
  [switch]$Rescreen,
  [switch]$Retry,
  [string[]]$Only,
  # A JSON list of host names: every download hop (redirects too) must be one of them, on top of
  # the sources list, so a download stays on the hosts a request named.
  [string]$Hosts,
  # Only the manifest's files of these formats (docx, xlsx, csv, pptx), for example -Rescreen -Formats pptx.
  [string[]]$Formats,
  # Check Defender on these folders and the files in them (status, the EICAR proof in each, every
  # file scanned on its own, then each folder); nothing is downloaded, screened or deleted.
  [string[]]$DefenderCheck,
  # Screen these local files and print the results; nothing is downloaded, moved or deleted.
  [string[]]$Screen
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
# powershell -File passes "a,b" as one string.
$Screen = @($Screen | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$DefenderCheck = @($DefenderCheck | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Out -and $Screen) { $Out = [IO.Path]::GetTempPath() }
if (-not $Out) { throw "Give the corpus folder with -Out, or set FW_CORPUS." }
if (-not $Manifest) { $Manifest = Join-Path $root "tests\office\real-world.json" }
if (-not $Fwtool) { $Fwtool = Join-Path $root "build\tools\fwtool\Release\fwtool.exe" }
if (-not (Test-Path $Fwtool)) { throw "fwtool.exe not found at $Fwtool. Build it, or pass -Fwtool." }
$Out = [IO.Path]::GetFullPath($Out)
$MaxBytes = 20MB
$PartCap = 64MB      # no single part may expand past this while it's screened
$TotalCap = 256MB    # nor all parts together
$mpcmdrun = Join-Path $env:ProgramFiles "Windows Defender\MpCmdRun.exe"
$today = (Get-Date).ToString("yyyy-MM-dd")

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

# ---------------------------------------------------------------- JSON

# PowerShell 5.1's ConvertTo-Json indents oddly and escapes quotes; this keeps the manifest diffable.
function ConvertTo-PrettyJson($v, [string]$indent = "") {
  $next = "$indent  "
  if ($null -eq $v) { return "null" }
  if ($v -is [bool]) { if ($v) { return "true" } else { return "false" } }
  if ($v -is [int] -or $v -is [long] -or $v -is [double] -or $v -is [decimal]) { return [string]$v }
  if ($v -is [string]) {
    $s = $v -replace '\\', '\\' -replace '"', '\"' -replace "`r", '\r' -replace "`n", '\n' -replace "`t", '\t'
    return '"' + [regex]::Replace($s, '[\x00-\x1f]', { param($m) '\u{0:x4}' -f [int][char]$m.Value }) + '"'
  }
  if ($v -is [Collections.IDictionary] -or $v -is [Management.Automation.PSCustomObject]) {
    $props = New-Object Collections.Generic.List[object]
    if ($v -is [Collections.IDictionary]) { foreach ($k in $v.Keys) { $props.Add(@($k, $v[$k])) } }
    else { foreach ($p in $v.PSObject.Properties) { $props.Add(@($p.Name, $p.Value)) } }
    if ($props.Count -eq 0) { return "{}" }
    # Small objects of plain values stay on one line.
    $flat = @($props | Where-Object { $_[1] -is [Collections.IEnumerable] -and $_[1] -isnot [string] }).Count -eq 0
    $items = @($props | ForEach-Object { '"' + $_[0] + '": ' + (ConvertTo-PrettyJson $_[1] $next) })
    if ($flat -and ($items -join ", ").Length -lt 100) { return "{ " + ($items -join ", ") + " }" }
    return "{`n$next" + ($items -join ",`n$next") + "`n$indent}"
  }
  if ($v -is [Collections.IEnumerable]) {
    $items = @($v | ForEach-Object { ConvertTo-PrettyJson $_ $next })
    if ($items.Count -eq 0) { return "[]" }
    if (($items -join ", ").Length -lt 100 -and -not ($items -match "`n")) { return "[" + ($items -join ", ") + "]" }
    return "[`n$next" + ($items -join ",`n$next") + "`n$indent]"
  }
  return ConvertTo-PrettyJson ([string]$v) $indent
}

function Write-PrettyJson($value, [string]$path) {
  [IO.File]::WriteAllText($path, (ConvertTo-PrettyJson $value) + "`n", [Text.UTF8Encoding]::new($false))
}

function Read-Json([string]$path) {
  [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8) | ConvertFrom-Json
}

# ---------------------------------------------------------------- Sources and download

$manifestData = if (Test-Path $Manifest) { Read-Json $Manifest } else { $null }
if (-not $manifestData -or -not $manifestData.sources) { throw "$Manifest has no sources list." }
$sources = @($manifestData.sources)
$allowedHosts = $null
if ($Hosts) {
  $allowedHosts = @{}
  foreach ($h in @((Read-Json $Hosts) | ForEach-Object { $_ })) { $allowedHosts[([string]$h).ToLowerInvariant().TrimEnd('.')] = $true }
  if (-not $allowedHosts.Count) { throw "$Hosts lists no hosts." }
}

# The source whose domain the URL's host is, or ends with ("gov" allows every *.gov host). A
# source with "paths" matches only URLs whose path starts with one of them (case-sensitive).
function Find-Source([Uri]$u) {
  $h = $u.Host.ToLowerInvariant().TrimEnd('.')
  $best = $null
  foreach ($s in $sources) {
    $d = $s.domain.ToLowerInvariant()
    if ($h -ne $d -and -not $h.EndsWith(".$d")) { continue }
    if ($s.paths -and -not @($s.paths | Where-Object { $u.AbsolutePath.StartsWith($_, [StringComparison]::Ordinal) })) { continue }
    if (-not $best -or $d.Length -gt $best.domain.Length) { $best = $s }
  }
  $best
}

# Windows' own curl.exe downloads, one hop at a time, so every redirect is checked before it's followed.
$curl = Join-Path $env:SystemRoot "System32\curl.exe"

# Downloads $url to $dest. Returns the final URL; throws with the reason when it can't.
function Get-Download([string]$url, [string]$dest, [string]$format = "docx") {
  $headers = "$dest.headers"; $errors = "$dest.errors"
  try {
    for ($hop = 0; $hop -le 5; $hop++) {
      $u = [Uri]$url
      if ($u.Scheme -ne "https") { throw "not HTTPS: $url" }
      if (-not (Find-Source $u)) { throw "not on the sources list: $($u.Host)" }
      if ($allowedHosts -and -not $allowedHosts.ContainsKey($u.Host.ToLowerInvariant().TrimEnd('.'))) { throw "not one of the request's hosts: $($u.Host)" }
      $w = & $curl -sS --proto =https --max-redirs 0 --max-filesize $MaxBytes --connect-timeout 30 --max-time 300 `
        --stderr $errors -D $headers -o $dest -w "%{http_code} %{redirect_url}" $u.AbsoluteUri
      if ($LASTEXITCODE -eq 63) { throw "larger than 20 MB" }
      if ($LASTEXITCODE -ne 0) { throw ("curl: " + ((Get-Content -LiteralPath $errors) -join " ").Trim()) }
      $code, $next = "$w" -split ' ', 2
      $head = [IO.File]::ReadAllText($headers)
      if ($code -match '^3' -and $next) {
        Remove-Item -LiteralPath $dest -ErrorAction SilentlyContinue
        $url = $next
        continue
      }
      # A site that wants a browser challenge solved is skipped, never worked around.
      if ($head -match '(?im)^(x-amzn-waf-action|cf-mitigated):') { throw "HTTP $code, the site asks for a browser challenge" }
      if ($code -ne "200") { throw "HTTP $code" }
      # The served name must be of the expected format too when the server or the URL gives one.
      $m = [regex]::Match($head, "(?im)^content-disposition:.*?filename\*?=(?:UTF-8'')?""?([^"";\r\n]+)")
      $name = if ($m.Success) { [Uri]::UnescapeDataString($m.Groups[1].Value) } else { [Uri]::UnescapeDataString($u.Segments[-1]) }
      $bad = switch ($format) {
        "xlsx" { '(?i)\.(xlsm|xltm|xlsb|xls|xlam|ods|csv|docx|doc|zip|7z|rar|exe|pdf|htm|html)$' }
        "csv" { '(?i)\.(xlsx|xlsm|xls|xlsb|ods|docx|doc|zip|gz|7z|rar|exe|pdf|htm|html|json|xml)$' }
        "pptx" { '(?i)\.(pptm|potm|ppsm|ppt|pps|pot|odp|key|docx|xlsx|zip|7z|rar|exe|pdf|htm|html)$' }
        default { '(?i)\.(docm|dotx|dotm|doc|rtf|odt|zip|7z|rar|exe|pdf|htm|html)$' }
      }
      if ($name -match $bad) { throw "served as $name, not a .$format" }
      if ((Get-Item -LiteralPath $dest).Length -gt $MaxBytes) { throw "larger than 20 MB" }
      return $u.AbsoluteUri
    }
    throw "more than 5 redirects"
  } finally {
    Remove-Item -LiteralPath $headers, $errors -ErrorAction SilentlyContinue
  }
}

# ---------------------------------------------------------------- The screen

$wordMain = "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
$badPath = '(?i)(^|/)(vbaProject\.bin|vbaData\.xml)$|^word/embeddings/|/activeX/|^word/activeX|webextension|taskpanes|^customUI/'
$badType = '(?i)macroEnabled|vbaProject|vbaData|activeX|oleObject|webextension|taskpane|keyMapCustomizations|attachedToolbars'
# Relationship types refused even inside the package (the last part of the type URI).
$badRel = @("vbaProject", "wordVbaData", "oleObject", "package", "control", "activeXControl", "activeXControlBinary",
  "aFChunk", "attachedTemplate", "webextension", "webextensiontaskpanes", "subDocument", "frame", "mailMergeSource",
  "mailMergeHeaderSource", "extensibility", "attachedToolbars", "keyMapCustomizations")
$badField = @("DDE", "DDEAUTO", "LINK", "IMPORT", "INCLUDE", "INCLUDETEXT", "INCLUDEPICTURE")
$badString = '(?i)ms-msdt|mhtml:|!x-usc:'
$plainLink = '^(?i)\s*(https?://|mailto:)'

# How a link target is named in a reason: its kind and host, never a full path, which often
# carries the author's user name.
function Format-Target([string]$t) {
  if ($t -match '^(?i)https?://([^/\\?#]+)') { return "$($Matches[0])/..." }
  if ($t -match '^(?i)(file:/{4,}|file://(?!/)|\\\\)') { return "a network path" }
  if ($t -match '^(?i)(file:|[a-z]:[\\/]|/)') { return "a local file path" }
  if ($t -match '^([a-zA-Z][a-zA-Z0-9+.-]*):') { return "a $($Matches[1]): link" }
  "a relative path"
}

function Read-Part($entry, [ref]$budget) {
  $s = $entry.Open()
  $ms = New-Object IO.MemoryStream
  try {
    $buf = New-Object byte[] 65536
    while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) {
      if ($ms.Length + $n -gt $PartCap) { throw "part $($entry.FullName) expands past 64 MB" }
      $budget.Value -= $n
      if ($budget.Value -lt 0) { throw "the parts expand past 256 MB together" }
      $ms.Write($buf, 0, $n)
    }
  } finally { $s.Dispose() }
  , $ms.ToArray()
}

function Get-XmlText([byte[]]$bytes) {
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($bytes) }
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) { return [Text.Encoding]::BigEndianUnicode.GetString($bytes) }
  [Text.Encoding]::UTF8.GetString($bytes)
}

function New-XmlReader([byte[]]$bytes) {
  $settings = New-Object Xml.XmlReaderSettings
  $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
  $settings.XmlResolver = $null
  $settings.IgnoreComments = $true
  [Xml.XmlReader]::Create((New-Object IO.MemoryStream(, $bytes)), $settings)
}

# A relationship's target as a part name, from the .rels part that holds it.
function Resolve-PartName([string]$relsPart, [string]$target) {
  $t = [Uri]::UnescapeDataString($target)
  if ($t.StartsWith('/')) { return $t.TrimStart('/') }
  $parts = New-Object Collections.Generic.List[string]
  $dir = ($relsPart -replace '(?i)(^|/)_rels/[^/]+\.rels$', '').TrimEnd('/')
  if ($dir) { foreach ($p in $dir.Split('/')) { $parts.Add($p) } }
  foreach ($p in $t.Split('/')) {
    if ($p -eq '..') { if ($parts.Count) { $parts.RemoveAt($parts.Count - 1) } }
    elseif ($p -and $p -ne '.') { $parts.Add($p) }
  }
  $parts -join '/'
}

# A chart's own workbook (the one exception to word/embeddings/): a plain .xlsx, screened inside.
$chartBook = '(?i)^word/embeddings/[^/]+\.xlsx$'
$chartRels = '(?i)^word/charts/_rels/chart(ex)?\d*\.xml\.rels$'
$xlsxType = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
$xlsxMain = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"
$badBookPath = '(?i)vbaProject|/activeX/|^xl/embeddings/|^xl/externalLinks/|connections\.xml$|^xl/queryTables/|^xl/pivotCache/|^customUI/|webextension|taskpanes'
$badBookType = '(?i)externalLink|connections|queryTable|pivotCache'
$badBookRel = @("externalLink", "externalLinkPath", "connections", "queryTable", "pivotCacheDefinition", "pivotCacheRecords")
$badFormula = '(?i)\b(WEBSERVICE|FILTERXML|CALL|REGISTER|REGISTER\.ID|EXEC|RTD)\s*\('
# Kana, CJK ideographs and Hangul (built from code points: the script stays ASCII).
$cjkClass = "[{0}-{1}{2}-{3}{4}-{5}{6}-{7}]" -f [char]0x3040, [char]0x30FF, [char]0x3400, [char]0x4DBF, [char]0x4E00, [char]0x9FFF, [char]0xAC00, [char]0xD7AF

# Screens the workbook in $entry. Returns the reasons it fails; none means it passed.
function Test-Workbook($entry, $budget) {
  $why = New-Object Collections.Generic.List[string]
  $inner = $null
  try {
    $inner = [IO.Compression.ZipArchive]::new([IO.MemoryStream]::new([byte[]](Read-Part $entry $budget)), [IO.Compression.ZipArchiveMode]::Read)
    $es = @($inner.Entries)
    if ($es.Count -gt 1000) { $why.Add("more than 1000 parts"); return , $why }
    $sawMain = $false
    foreach ($e in $es) {
      $n = $e.FullName.TrimStart('/')
      if ($n -match $badBookPath) { $why.Add("forbidden part $n"); continue }
      if ($n -notmatch '(?i)\.(xml|rels)$') { continue }
      $bytes = Read-Part $e $budget
      $m = [regex]::Match((Get-XmlText $bytes), $badString)
      if ($m.Success) { $why.Add("contains '$($m.Value)' ($n)") }
      $inF = $false
      $r = New-XmlReader $bytes  # refuses a DTD, as in the document
      while ($r.Read()) {
        if ($r.NodeType -eq [Xml.XmlNodeType]::Text) { if ($inF -and $r.Value -match $badFormula) { $why.Add("formula with $($Matches[1]) ($n)") }; continue }
        if ($r.NodeType -eq [Xml.XmlNodeType]::EndElement) { if ($r.LocalName -eq "f") { $inF = $false }; continue }
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        switch -CaseSensitive ($r.LocalName) {
          "f" { $inF = -not $r.IsEmptyElement }
          { $_ -in "Default", "Override" } {
            $type = $r.GetAttribute("ContentType")
            if ($type -match $badType -or $type -match $badBookType) { $why.Add("forbidden content type $type") }
            if ($type -eq $xlsxMain) { $sawMain = $true }
          }
          "Relationship" {
            $short = ($r.GetAttribute("Type") -split '/')[-1]; $target = $r.GetAttribute("Target")
            if ($badRel -contains $short -or $badBookRel -contains $short) { $why.Add("forbidden relationship $short ($n)") }
            if ($r.GetAttribute("TargetMode") -eq "External") {
              if ($short -ne "hyperlink") { $why.Add("external $short relationship to $(Format-Target $target) ($n)") }
              elseif ($target -notmatch $plainLink) { $why.Add("hyperlink that isn't http, https or mailto: $(Format-Target $target) ($n)") }
            }
          }
          { $_ -in "oleObject", "oleObjects", "control", "controls" } { $why.Add("$($_) ($n)") }
        }
      }
      $r.Dispose()
    }
    if (-not $sawMain) { $why.Add("no plain (macro-free) workbook part") }
  } catch {
    $why.Add("unreadable: $($_.Exception.Message)")
  } finally { if ($inner) { $inner.Dispose() } }
  , $why
}

# Splits a field instruction into words and quoted strings.
function Split-Instr([string]$instr) {
  @([regex]::Matches($instr, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
}

# Screens one file. Returns @{ reasons = @(...); facts = @{...} }; no reasons means it passed.
function Test-Docx([string]$path) {
  $reasons = New-Object Collections.Generic.List[string]
  $facts = [ordered]@{}
  $fi = Get-Item -LiteralPath $path
  if ($fi.Extension -ne ".docx") { $reasons.Add("not a .docx file"); return @{ reasons = $reasons; facts = $facts } }
  if ($fi.Length -ge $MaxBytes) { $reasons.Add("20 MB or larger"); return @{ reasons = $reasons; facts = $facts } }
  $head = New-Object byte[] 4
  $fs = [IO.File]::OpenRead($path); [void]$fs.Read($head, 0, 4); $fs.Dispose()
  if ($head[0] -ne 0x50 -or $head[1] -ne 0x4B -or $head[2] -ne 3 -or $head[3] -ne 4) {
    $reasons.Add("not a zip (a web page or another format)"); return @{ reasons = $reasons; facts = $facts }
  }
  $zip = $null
  $part = $null
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    $entries = @($zip.Entries)
    if ($entries.Count -gt 5000) { throw "more than 5000 parts" }
    $names = @{}
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($names.ContainsKey($n.ToLowerInvariant())) { throw "duplicate part name $n" }
      $names[$n.ToLowerInvariant()] = $e
      if ($n -match $badPath -and $n -notmatch $chartBook) { $reasons.Add("forbidden part $n") }
    }
    $books = @{}  # the workbooks chart parts refer to
    $budget = [long]$TotalCap
    $ct = $names["[content_types].xml"]
    if (-not $ct) { throw "no [Content_Types].xml" }

    # Content types: the main document must be plain WordprocessingML, nothing macro-enabled.
    $defaults = @{}; $overrides = @{}
    $r = New-XmlReader (Read-Part $ct ([ref]$budget))
    while ($r.Read()) {
      if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
      $type = $r.GetAttribute("ContentType")
      if ($r.LocalName -eq "Default") { $defaults[$r.GetAttribute("Extension").ToLowerInvariant()] = $type }
      elseif ($r.LocalName -eq "Override") { $overrides[$r.GetAttribute("PartName").TrimStart('/').ToLowerInvariant()] = $type }
      # A default for an extension no part uses is harmless, unless it's macro-enabled; parts are checked below.
      if ($type -and $type -match $badType -and ($r.LocalName -eq "Override" -or $type -match '(?i)macroEnabled|vba')) { $reasons.Add("forbidden content type $type") }
    }
    $r.Dispose()
    function Get-PartType([string]$n) {
      $k = $n.ToLowerInvariant()
      if ($overrides.ContainsKey($k)) { return $overrides[$k] }
      $ext = [IO.Path]::GetExtension($k).TrimStart('.')
      if ($defaults.ContainsKey($ext)) { return $defaults[$ext] }
      $null
    }

    # Relationships: find the main document, refuse risky types and external targets.
    $main = $null
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($n -notmatch '(?i)\.rels$') { continue }
      $part = $n
      $r = New-XmlReader (Read-Part $e ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element -or $r.LocalName -ne "Relationship") { continue }
        $type = $r.GetAttribute("Type"); $target = $r.GetAttribute("Target"); $mode = $r.GetAttribute("TargetMode")
        $short = ($type -split '/')[-1]
        if ($short -eq "package" -and $n -match $chartRels -and $mode -ne "External" -and (Resolve-PartName $n $target) -match $chartBook) {
          $books[(Resolve-PartName $n $target).ToLowerInvariant()] = $true
        } elseif ($badRel -contains $short) { $reasons.Add("forbidden relationship $short ($n)") }
        if ($n -ieq "_rels/.rels" -and $short -eq "officeDocument") { $main = $target.TrimStart('/') }
        if ($mode -eq "External") {
          if ($short -ne "hyperlink") { $reasons.Add("external $short relationship to $(Format-Target $target) ($n)") }
          elseif ($target -notmatch $plainLink) { $reasons.Add("hyperlink that isn't http, https or mailto: $(Format-Target $target) ($n)") }
        }
      }
      $r.Dispose()
    }
    if (-not $main) { throw "no main document relationship" }
    $mainType = Get-PartType $main
    if ($mainType -ne $wordMain) { $reasons.Add("main document isn't plain WordprocessingML ($mainType)") }
    foreach ($e in $entries) {
      $t = Get-PartType $e.FullName.TrimStart('/')
      if ($t -and $t -match $badType) { $reasons.Add("forbidden part type $t ($($e.FullName))") }
    }
    foreach ($k in @($names.Keys)) {
      if ($k -notmatch $chartBook) { continue }
      $n = $names[$k].FullName.TrimStart('/')
      if (-not $books[$k]) { $reasons.Add("forbidden part $n (no chart refers to it)"); continue }
      $t = Get-PartType $n
      if ($t -ne $xlsxType) { $reasons.Add("chart workbook $n isn't a plain .xlsx ($t)"); continue }
      foreach ($why in (Test-Workbook $names[$k] ([ref]$budget))) { $reasons.Add("chart workbook $($n): $why") }
    }

    # Every XML part: forbidden strings and elements, fields, and the facts the manifest records.
    $count = @{}
    function Add-Count([string]$k, [int]$n = 1) { $count[$k] = $n + [int]$count[$k] }
    $fieldKinds = @{}
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      $t = Get-PartType $n
      if ($n -notmatch '(?i)\.(xml|rels|vml)$' -and -not ($t -and $t -match '(?i)[+/]xml$')) { continue }
      $bytes = Read-Part $e ([ref]$budget)
      $text = Get-XmlText $bytes
      $m = [regex]::Match($text, $badString)
      if ($m.Success) { $reasons.Add("contains '$($m.Value)' ($n)") }
      if ($n -match '(?i)\.rels$') { continue }
      if ($n -match '(?i)^word/document\d*\.xml$') {
        # Kana, CJK ideographs and Hangul in the text (not font names): East Asian text.
        $cjk = 0
        foreach ($tm in [regex]::Matches($text, '<w:t(?:\s[^>]*)?>([^<]+)</w:t>')) {
          $cjk += [regex]::Matches($tm.Groups[1].Value, $cjkClass).Count
        }
        if ($cjk -ge 10) { Add-Count "east-asian" $cjk }
      }
      $isWordPart = $n -match '(?i)^word/'
      $stack = New-Object Collections.Generic.List[object]
      $inInstr = $false
      $part = "$n ($t)"
      $r = New-XmlReader $bytes
      while ($r.Read()) {
        if ($r.NodeType -eq [Xml.XmlNodeType]::Text -or $r.NodeType -eq [Xml.XmlNodeType]::SignificantWhitespace -or $r.NodeType -eq [Xml.XmlNodeType]::Whitespace) {
          if ($inInstr -and $stack.Count -gt 0 -and $stack[$stack.Count - 1].open) { [void]$stack[$stack.Count - 1].instr.Append($r.Value) }
          continue
        }
        if ($r.NodeType -eq [Xml.XmlNodeType]::EndElement) {
          if ($r.LocalName -eq "instrText" -or $r.LocalName -eq "delInstrText") { $inInstr = $false }
          continue
        }
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        $ln = $r.LocalName
        $w = $r.NamespaceURI -match 'wordprocessingml'
        switch -CaseSensitive ($ln) {
          "altChunk" { $reasons.Add("altChunk ($n)") }
          "OLEObject" { $reasons.Add("OLE object ($n)") }
          "objectEmbed" { $reasons.Add("OLE object ($n)") }
          "objectLink" { $reasons.Add("linked OLE object ($n)") }
          "control" { if ($w) { $reasons.Add("ActiveX control ($n)") } }
          "attachedTemplate" { if ($w) { $reasons.Add("attached template ($n)") } }
          "mailMerge" { if ($w) { $reasons.Add("mail merge data source ($n)") } }
          "subDoc" { if ($w) { $reasons.Add("subdocument ($n)") } }
          "frameset" { if ($w) { $reasons.Add("frameset ($n)") } }
          "sectPr" { if ($w -and $isWordPart) { Add-Count "sections" } }
          "pgSz" { if ($w -and $r.GetAttribute("orient", $r.NamespaceURI) -eq "landscape") { Add-Count "landscape" } }
          "cols" { if ($w) { $num = $r.GetAttribute("num", $r.NamespaceURI); if ($num -and [int]$num -gt 1) { Add-Count "columns" } } }
          "footnoteReference" { Add-Count "footnotes" }
          "endnoteReference" { Add-Count "endnotes" }
          "commentReference" { Add-Count "comments" }
          { $_ -in "ins", "del", "moveFrom", "moveTo", "rPrChange", "pPrChange", "tblPrChange", "trPrChange", "tcPrChange", "sectPrChange", "numberingChange" } {
            if ($w -and $n -notmatch '(?i)^word/(styles|numbering)') { Add-Count "revisions" }
          }
          "sdt" { if ($w) { Add-Count "content-controls" } }
          "ffData" { Add-Count "legacy-form-fields" }
          "tbl" { if ($w) { Add-Count "tables" } }
          "pic" { Add-Count "pictures" }
          "imagedata" { Add-Count "pictures" }
          "txbxContent" { Add-Count "text-boxes" }
          "oMath" { Add-Count "equations" }
          "pict" { if ($w) { Add-Count "vml" } }
          "chart" { if ($r.NamespaceURI -match 'drawingml/2006/chart') { Add-Count "charts" } }
          "relIds" { Add-Count "smartart" }
          "numPr" { if ($w -and $n -match '(?i)^word/document') { Add-Count "numbering" } }
          "hyperlink" { if ($w) { Add-Count "hyperlinks" } }
          "bidi" { if ($w) { Add-Count "rtl" } }
          "rtl" { if ($w) { Add-Count "rtl" } }
          "bidiVisual" { if ($w) { Add-Count "rtl" } }
          "embedRegular" { Add-Count "embedded-fonts" }
          "headerReference" { Add-Count "headers" }
          "footerReference" { Add-Count "footers" }
          "compatSetting" {
            if ($r.GetAttribute("name", $r.NamespaceURI) -eq "compatibilityMode") { $facts.compatibilityMode = [int]$r.GetAttribute("val", $r.NamespaceURI) }
          }
          "lang" {
            if ($n -ieq "word/styles.xml" -and -not $facts.Contains("styleLanguage")) { $v = $r.GetAttribute("val", $r.NamespaceURI); if ($v) { $facts.styleLanguage = $v } }
          }
          "fldSimple" {
            if ($w) { $stack.Add(@{ instr = New-Object Text.StringBuilder($r.GetAttribute("instr", $r.NamespaceURI)); open = $true; nestedFirst = $false; simple = $true }) }
          }
          "fldChar" {
            $kind = $r.GetAttribute("fldCharType", $r.NamespaceURI)
            if ($kind -eq "begin") {
              if ($stack.Count -gt 0) { $top = $stack[$stack.Count - 1]; if ($top.open -and $top.instr.ToString().Trim() -eq "") { $top.nestedFirst = $true } }
              $stack.Add(@{ instr = New-Object Text.StringBuilder; open = $true; nestedFirst = $false; simple = $false })
            } elseif (($kind -eq "separate" -or $kind -eq "end") -and $stack.Count -gt 0) {
              $top = $stack[$stack.Count - 1]
              $top.open = $false
              if ($kind -eq "end") { $top.ended = $true }
            }
          }
          { $_ -in "instrText", "delInstrText" } { if (-not $r.IsEmptyElement) { $inInstr = $true } }
        }
        # VML shapes can carry their own hyperlink (href); pictures' o:href is only their origin.
        if ($r.HasAttributes -and $r.NamespaceURI -eq "urn:schemas-microsoft-com:vml") {
          $href = $r.GetAttribute("href")
          if ($href -and $href -notmatch $plainLink -and -not $href.StartsWith("#")) { $reasons.Add("VML hyperlink that isn't http, https or mailto: $(Format-Target $href) ($n)") }
        }
        # Judge a field once its instruction is complete (at separate or end), or a simple field right away.
        for ($i = $stack.Count - 1; $i -ge 0; $i--) {
          $f = $stack[$i]
          if ($f.judged -or ($f.open -and -not $f.simple)) { continue }
          $f.judged = $true
          $words = @(Split-Instr $f.instr.ToString())
          $kw = if ($words.Count -gt 0) { $words[0].ToUpperInvariant() } else { "" }
          if ($f.nestedFirst) { $reasons.Add("field code that starts with a nested field ($n)"); continue }
          if ($kw) { $fieldKinds[$kw] = 1 + [int]$fieldKinds[$kw] }
          if ($badField -contains $kw) { $reasons.Add("$kw field ($n)") }
          if ($kw -eq "HYPERLINK") {
            $target = $null
            for ($j = 1; $j -lt $words.Count; $j++) {
              if ($words[$j] -match '^\\[lotLOT]$') { $j++; continue }
              if ($words[$j].StartsWith('\')) { continue }
              $target = $words[$j]; break
            }
            if ($target -and $target -notmatch $plainLink) { $reasons.Add("HYPERLINK field to $(Format-Target $target) ($n)") }
          }
        }
        while ($stack.Count -gt 0 -and $stack[$stack.Count - 1].judged -and ($stack[$stack.Count - 1].simple -or $stack[$stack.Count - 1].ended)) {
          $stack.RemoveAt($stack.Count - 1)
        }
      }
      $r.Dispose()
    }

    # Facts for the manifest.
    $app = $names["docprops/app.xml"]
    if ($app) {
      $r = New-XmlReader (Read-Part $app ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        switch ($r.LocalName) {
          "Application" { $facts.producer = $r.ReadElementContentAsString() }
          "AppVersion" { $facts.appVersion = $r.ReadElementContentAsString() }
          "Pages" { $v = $r.ReadElementContentAsString(); if ($v -match '^\d+$') { $facts.savedPages = [int]$v } }
        }
      }
      $r.Dispose()
    }
    $core = $names["docprops/core.xml"]
    if ($core) {
      $r = New-XmlReader (Read-Part $core ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -eq [Xml.XmlNodeType]::Element -and $r.LocalName -eq "modified") { $v = $r.ReadElementContentAsString(); if ($v.Length -ge 10) { $facts.modified = $v.Substring(0, 10) } }
      }
      $r.Dispose()
    }
    if (-not $facts.Contains("producer")) {
      # Google Docs writes no docProps and puts [Content_Types].xml last in the zip.
      $facts.producer = if (-not $names["docprops/core.xml"] -and $entries[$entries.Count - 1].FullName -eq "[Content_Types].xml") { "Google Docs (inferred: no docProps, [Content_Types].xml last)" } else { "unknown (no docProps/app.xml)" }
    }
    if (-not $facts.Contains("compatibilityMode")) { $facts.compatibilityMode = 11 }  # no setting: Word 2003 layout
    $tags = New-Object Collections.Generic.List[string]
    if ([int]$count["sections"] -gt 1) { $tags.Add("sections") }
    foreach ($k in "landscape", "columns", "footnotes", "endnotes", "comments", "revisions", "content-controls", "legacy-form-fields",
                   "tables", "pictures", "text-boxes", "equations", "vml", "charts", "smartart", "numbering", "hyperlinks", "rtl", "east-asian", "embedded-fonts") {
      if ($count[$k]) { $tags.Add($k) }
    }
    if ($count["headers"] -or $count["footers"]) { $tags.Add("headers-footers") }
    if ($fieldKinds.Count) { $tags.Add("fields") }
    if ($fieldKinds["TOC"]) { $tags.Add("toc") }
    if ($fieldKinds["MACROBUTTON"]) { $tags.Add("macrobutton") }
    $facts.tags = @($tags)
    $facts.counts = [ordered]@{}
    foreach ($k in @($count.Keys | Sort-Object)) { $facts.counts[$k] = $count[$k] }
    $facts.fields = @($fieldKinds.Keys | Sort-Object)
  } catch {
    $where = if ($part) { " in $part" } else { "" }
    $reasons.Add("malformed$($where): $($_.Exception.Message)")
  } finally {
    if ($zip) { $zip.Dispose() }
  }
  @{ reasons = @($reasons | Select-Object -Unique); facts = $facts }
}

# ---------------------------------------------------------------- Workbooks (S1)

$xlMainTypes = @{
  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml" = ".xlsx"
  "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml" = ".xltx"
}
$badXlPath = '(?i)(^|/)vbaProject\.bin$|vbaProjectSignature|vbaData\.xml$|^xl/externalLinks/|^xl/connections\.xml$|^xl/queryTables/|^xl/embeddings/|^xl/activeX/|^xl/macrosheets/|^xl/dialogsheets/|webextension|taskpanes|^customUI/|^xl/customUI/'
$badXlType = '(?i)macroEnabled|vbaProject|vbaData|externalLink|connections|queryTable|activeX|oleObject|webextension|taskpane|macrosheet|dialogsheet|keyMapCustomizations'
$badXlRel = @("vbaProject", "externalLink", "externalLinkPath", "connections", "queryTable", "oleObject", "package", "control",
  "activeXControl", "activeXControlBinary", "webextension", "webextensiontaskpanes", "xlMacrosheet", "xlIntlMacrosheet",
  "dialogsheet", "extensibility", "attachedToolbars", "keyMapCustomizations", "slicerCacheExternal")
# Functions that reach outside the workbook or run code (web, RTD, DLL calls, XLM macros).
$badXlFunction = '(?i)(?<![A-Za-z0-9_.])(?:_xlfn\.)?(WEBSERVICE|FILTERXML|CALL|REGISTER|REGISTER\.ID|EXEC|RTD|EVALUATE|EXECUTE|FOPEN|FWRITE|FWRITELN|FREAD|FREADLN|HALT|RUN|FORMULA|DIRECTORY|FILES)\s*\('
# Elements whose text is a formula: cells, defined names, conditional formats, data validation.
$xlFormulaElements = @("f", "definedName", "formula", "formula1", "formula2")
$xlMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
$xlTagCounts = @{
  # element local name -> tag
  "mergeCell" = "merged-cells"; "conditionalFormatting" = "conditional-formats"; "dataValidation" = "data-validation"
  "hyperlink" = "hyperlinks"; "tablePart" = "tables"; "sparklineGroup" = "sparklines"; "legacyDrawing" = "notes"
  "pageSetup" = "page-setup"; "headerFooter" = "headers-footers"; "rowBreaks" = "page-breaks"; "colBreaks" = "page-breaks"
  "autoFilter" = "filters"; "sheetProtection" = "protection"; "dataBar" = "data-bars"; "iconSet" = "icon-sets"; "colorScale" = "color-scales"
}

# Strips text literals ("..." with "" inside) from a formula, so only its code is looked at.
# A formula's code without what isn't code: text literals, bracketed parts (structured references
# such as Tb[[#This Row],[Bijdrage in natura (overige)]], external workbook numbers such as [1])
# and quoted sheet names ('Calcul-Subv-GS (2)'!C24), which otherwise read as calls.
function Remove-FormulaStrings([string]$f) {
  $code = [regex]::Replace($f, '"(?:[^"]|"")*"', '""')
  do { $before = $code; $code = [regex]::Replace($code, '\[[^\[\]]*\]', '') } while ($code -ne $before)
  [regex]::Replace($code, "'(?:[^']|'')*'", "''")
}

# The functions a formula calls: names right before "(" (with a space between, it's an
# intersection), outside the parts above, without _xlfn./_xlws.
function Get-FormulaFunctions([string]$f) {
  $code = Remove-FormulaStrings $f
  @([regex]::Matches($code, "(?<![A-Za-z0-9_.\]'!])((?:_xlfn\.|_xlws\.|_xlpm\.)*[A-Za-z][A-Za-z0-9_.]*)\(") |
    ForEach-Object { ($_.Groups[1].Value -replace '^(_xlfn\.|_xlws\.)+', '').ToUpperInvariant() } | Where-Object { $_ -notmatch '^_XLPM\.' } | Select-Object -Unique)
}

# Why a formula's code is refused, or $null.
function Test-XlFormula([string]$f) {
  $code = Remove-FormulaStrings $f
  if ($code -match '\|') { return "DDE in a formula" }
  $m = [regex]::Match($code, $badXlFunction)
  if ($m.Success) { return "$($m.Groups[1].Value.ToUpperInvariant()) in a formula" }
  $null
}

# Screens one workbook. Returns @{ reasons = @(...); facts = @{...} }; no reasons means it passed.
function Test-Xlsx([string]$path) {
  $reasons = New-Object Collections.Generic.List[string]
  $facts = [ordered]@{}
  $fi = Get-Item -LiteralPath $path
  if (@(".xlsx", ".xltx") -notcontains $fi.Extension.ToLowerInvariant()) { $reasons.Add("not a .xlsx or .xltx file"); return @{ reasons = $reasons; facts = $facts } }
  if ($fi.Length -ge $MaxBytes) { $reasons.Add("20 MB or larger"); return @{ reasons = $reasons; facts = $facts } }
  $head = New-Object byte[] 4
  $fs = [IO.File]::OpenRead($path); [void]$fs.Read($head, 0, 4); $fs.Dispose()
  if ($head[0] -ne 0x50 -or $head[1] -ne 0x4B -or $head[2] -ne 3 -or $head[3] -ne 4) {
    $why = if ($head[0] -eq 0xD0 -and $head[1] -eq 0xCF) { "an OLE file (an .xls, or an encrypted workbook)" } else { "not a zip (a web page or another format)" }
    $reasons.Add($why); return @{ reasons = $reasons; facts = $facts }
  }
  $zip = $null
  $part = $null
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    $entries = @($zip.Entries)
    if ($entries.Count -gt 5000) { throw "more than 5000 parts" }
    $names = @{}
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($names.ContainsKey($n.ToLowerInvariant())) { throw "duplicate part name $n" }
      $names[$n.ToLowerInvariant()] = $e
      if ($n -match $badXlPath) { $reasons.Add("forbidden part $n") }
    }
    $budget = [long]$TotalCap
    $ct = $names["[content_types].xml"]
    if (-not $ct) { throw "no [Content_Types].xml" }
    $defaults = @{}; $overrides = @{}
    $r = New-XmlReader (Read-Part $ct ([ref]$budget))
    while ($r.Read()) {
      if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
      $type = $r.GetAttribute("ContentType")
      if ($r.LocalName -eq "Default") { $defaults[$r.GetAttribute("Extension").ToLowerInvariant()] = $type }
      elseif ($r.LocalName -eq "Override") { $overrides[$r.GetAttribute("PartName").TrimStart('/').ToLowerInvariant()] = $type }
      if ($type -and $type -match $badXlType -and ($r.LocalName -eq "Override" -or $type -match '(?i)macroEnabled|vba')) { $reasons.Add("forbidden content type $type") }
    }
    $r.Dispose()
    function Get-PartType([string]$n) {
      $k = $n.ToLowerInvariant()
      if ($overrides.ContainsKey($k)) { return $overrides[$k] }
      $ext = [IO.Path]::GetExtension($k).TrimStart('.')
      if ($defaults.ContainsKey($ext)) { return $defaults[$ext] }
      $null
    }
    $main = $null
    $count = @{}
    function Add-Count([string]$k, [int]$n = 1) { $count[$k] = $n + [int]$count[$k] }
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($n -notmatch '(?i)\.rels$') { continue }
      $part = $n
      $r = New-XmlReader (Read-Part $e ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element -or $r.LocalName -ne "Relationship") { continue }
        $target = $r.GetAttribute("Target"); $mode = $r.GetAttribute("TargetMode")
        $short = ($r.GetAttribute("Type") -split '/')[-1]
        if ($badXlRel -contains $short) { $reasons.Add("forbidden relationship $short ($n)") }
        if ($n -ieq "_rels/.rels" -and $short -eq "officeDocument") { $main = $target.TrimStart('/') }
        if ($mode -eq "External") {
          if ($short -ne "hyperlink") { $reasons.Add("external $short relationship to $(Format-Target $target) ($n)") }
          elseif ($target -notmatch $plainLink) { $reasons.Add("hyperlink that isn't http, https or mailto: $(Format-Target $target) ($n)") }
        }
        switch ($short) { "chart" { Add-Count "charts" } "chartsheet" { Add-Count "chart-sheets" } "image" { Add-Count "pictures" }
          "pivotTable" { Add-Count "pivot-tables" } "threadedComment" { Add-Count "threaded-comments" } "slicer" { Add-Count "slicers" }
          "timeline" { Add-Count "timelines" } "ctrlProp" { Add-Count "form-controls" } }
      }
      $r.Dispose()
    }
    if (-not $main) { throw "no main workbook relationship" }
    $mainType = Get-PartType $main
    if (-not $xlMainTypes.ContainsKey([string]$mainType)) { $reasons.Add("main part isn't a plain workbook or template ($mainType)") }
    elseif ($xlMainTypes[$mainType] -ne $fi.Extension.ToLowerInvariant()) { $reasons.Add("a $($xlMainTypes[$mainType]) named $($fi.Extension)") }
    foreach ($e in $entries) {
      $t = Get-PartType $e.FullName.TrimStart('/')
      if ($t -and $t -match $badXlType) { $reasons.Add("forbidden part type $t ($($e.FullName))") }
    }

    # Every XML part: forbidden strings, formulas, controls; and the facts.
    $functions = @{}
    $sharedFns = @{}  # shared formula index -> its functions, per part
    $sheets = 0; $cells = 0; $formulas = 0
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      $t = Get-PartType $n
      if ($n -notmatch '(?i)\.(xml|rels|vml)$' -and -not ($t -and $t -match '(?i)[+/]xml$')) { continue }
      $bytes = Read-Part $e ([ref]$budget)
      $text = Get-XmlText $bytes
      $m = [regex]::Match($text, $badString)
      if ($m.Success) { $reasons.Add("contains '$($m.Value)' ($n)") }
      if ($n -match '(?i)\.rels$') { continue }
      if ($n -match '(?i)^customXml/item\d*\.xml$' -and $text -match 'DataMashup') { $reasons.Add("Power Query (DataMashup, $n)") }
      if ($n -match '(?i)\.vml$') {
        if ($text -match '(?i)<x:FmlaMacro') { $reasons.Add("a control that runs a macro ($n)") }
        continue  # VML is loose HTML-style markup, not always well-formed XML
      }
      $part = "$n ($t)"
      $isSheet = $n -match '(?i)^xl/worksheets/[^/]+\.xml$'
      if ($isSheet) { $sheets++ }
      $sharedFns.Clear()
      $inFormula = $null; $fShared = $null; $fText = New-Object Text.StringBuilder
      $r = New-XmlReader $bytes
      while ($r.Read()) {
        $nt = $r.NodeType
        if ($nt -eq [Xml.XmlNodeType]::Text -or $nt -eq [Xml.XmlNodeType]::Whitespace -or $nt -eq [Xml.XmlNodeType]::SignificantWhitespace) {
          if ($inFormula) { [void]$fText.Append($r.Value) }
          continue
        }
        if ($nt -eq [Xml.XmlNodeType]::EndElement) {
          if ($inFormula -and $r.LocalName -eq $inFormula) {
            $formulaText = $fText.ToString()
            $why = Test-XlFormula $formulaText
            if ($why) { $reasons.Add("$why ($n)") }
            if ($cellFormula) {
              $fns = Get-FormulaFunctions $formulaText
              if ($null -ne $fShared) { $sharedFns[$fShared] = $fns }
              foreach ($fn in $fns) { $functions[$fn] = 1 + [int]$functions[$fn] }
            }
            $inFormula = $null
          }
          continue
        }
        if ($nt -ne [Xml.XmlNodeType]::Element) { continue }
        $ln = $r.LocalName
        if ($xlFormulaElements -contains $ln) {
          # Cell formulas are counted (charts and sparklines have f elements too); all are screened.
          $cellFormula = $ln -eq "f" -and $isSheet -and $r.NamespaceURI -eq $xlMain
          if ($cellFormula) {
            $formulas++
            $kind = $r.GetAttribute("t")
            $si = $r.GetAttribute("si")
            if ($kind -eq "shared") { Add-Count "shared-formulas" }
            if ($kind -eq "array") { Add-Count "array-formulas" }
            $fShared = if ($kind -eq "shared" -and $si) { $si } else { $null }
            if ($r.IsEmptyElement) {
              # A shared formula's other cells: the functions of its first cell.
              if ($fShared -and $sharedFns.ContainsKey($fShared)) { foreach ($fn in $sharedFns[$fShared]) { $functions[$fn] = 1 + [int]$functions[$fn] } }
              continue
            }
          }
          if (-not $r.IsEmptyElement) { $inFormula = $ln; [void]$fText.Clear() }
          continue
        }
        switch -CaseSensitive ($ln) {
          "c" { if ($isSheet) { $cells++; if ($r.GetAttribute("cm")) { Add-Count "dynamic-arrays" } } }
          { $_ -in "oleObject", "oleObjects" } { $reasons.Add("OLE object ($n)") }
          "control" { if ($r.NamespaceURI -match 'spreadsheetml') { Add-Count "controls" } }
          "formControlPr" { if ($r.GetAttribute("macro")) { $reasons.Add("a control that runs a macro ($n)") } }
          "cacheSource" { $ty = $r.GetAttribute("type"); if ($ty -and $ty -ne "worksheet") { $reasons.Add("pivot cache from a $ty source ($n)") }; if ($r.GetAttribute("connectionId")) { $reasons.Add("pivot cache from a data connection ($n)") } }
          "pane" { if ($r.GetAttribute("state") -match 'frozen') { Add-Count "frozen-panes" } elseif ($r.GetAttribute("state") -eq "split") { Add-Count "split-panes" } }
          "row" { if ($r.GetAttribute("hidden") -match '^(1|true)$') { Add-Count "hidden-rows" }; if ($r.GetAttribute("outlineLevel")) { Add-Count "outline" } }
          "col" { if ($r.GetAttribute("hidden") -match '^(1|true)$') { Add-Count "hidden-columns" }; if ($r.GetAttribute("outlineLevel")) { Add-Count "outline" } }
          "sheetView" { if ($r.GetAttribute("rightToLeft") -match '^(1|true)$') { Add-Count "rtl" } }
          "workbookPr" { if ($r.GetAttribute("date1904") -match '^(1|true)$') { Add-Count "date1904" } }
          "sheet" { if ($n -ieq "xl/workbook.xml" -and $r.GetAttribute("state") -match '^(hidden|veryHidden)$') { Add-Count "hidden-sheets" } }
          "calcPr" { if ($r.GetAttribute("calcMode") -eq "manual") { Add-Count "manual-calculation" } }
          "r" { if ($n -match '(?i)sharedStrings') { Add-Count "rich-text" } }
          "pic" { if ($n -match '(?i)^xl/drawings/') { Add-Count "pictures" } }
          "sp" { if ($n -match '(?i)^xl/drawings/') { Add-Count "shapes" } }
          default { if ($xlTagCounts.ContainsKey($ln)) { Add-Count $xlTagCounts[$ln] } }
        }
        if ($ln -eq "definedName") { Add-Count "names" }
        if ($ln -eq "conditionalFormatting" -and $r.NamespaceURI -match '2009/9/main') { Add-Count "x14-conditional-formats" }
      }
      $r.Dispose()
    }

    $app = $names["docprops/app.xml"]
    if ($app) {
      $r = New-XmlReader (Read-Part $app ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        switch ($r.LocalName) {
          "Application" { $facts.producer = $r.ReadElementContentAsString() }
          "AppVersion" { $facts.appVersion = $r.ReadElementContentAsString() }
        }
      }
      $r.Dispose()
    }
    $core = $names["docprops/core.xml"]
    if ($core) {
      $r = New-XmlReader (Read-Part $core ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -eq [Xml.XmlNodeType]::Element -and $r.LocalName -eq "modified") { $v = $r.ReadElementContentAsString(); if ($v.Length -ge 10) { $facts.modified = $v.Substring(0, 10) } }
      }
      $r.Dispose()
    }
    if (-not $facts.Contains("producer")) { $facts.producer = "unknown (no docProps/app.xml)" }
    if ($formulas) { Add-Count "formulas" $formulas }
    $facts.sheets = $sheets
    $facts.cells = $cells
    $facts.formulas = $formulas
    $facts.tags = @($count.Keys | Sort-Object)
    $facts.counts = [ordered]@{}
    foreach ($k in @($count.Keys | Sort-Object)) { $facts.counts[$k] = $count[$k] }
    $facts.functions = [ordered]@{}
    foreach ($k in @($functions.Keys | Sort-Object)) { $facts.functions[$k] = $functions[$k] }
  } catch {
    $where = if ($part) { " in $part" } else { "" }
    $reasons.Add("malformed$($where): $($_.Exception.Message)")
  } finally {
    if ($zip) { $zip.Dispose() }
  }
  @{ reasons = @($reasons | Select-Object -Unique); facts = $facts }
}

# ---------------------------------------------------------------- CSV (S1)

# Reads a CSV the way Excel's quoting works (a field that starts with a quote runs to the closing
# quote, "" inside is a quote, anything else is literal) and counts what the screen and the
# manifest need. Compiled, because a 20 MB file is too slow for a PowerShell loop.
Add-Type @"
using System;
using System.Text;
using System.Text.RegularExpressions;
public sealed class FwCsvFacts {
  public int Rows, MaxFields, QuotedLineBreaks, LeadingZeros, LongNumbers, DecimalCommas, FormulaLike;
  public bool UnclosedQuote; public string Danger;
}
public static class FwCsv {
  // The delimiter that splits the first lines most consistently into the most fields.
  public static char Guess(string text) {
    char best = ','; double bestScore = -1;
    foreach (char d in new[] { ',', ';', '\t', '|' }) {
      var counts = new System.Collections.Generic.List<int>();
      int n = 0; bool q = false; int lines = 0;
      for (int i = 0; i < text.Length && lines < 50; i++) {
        char c = text[i];
        if (c == '"') q = !q;
        else if (!q && c == d) n++;
        else if (!q && (c == '\n' || c == '\r')) {
          if (c == '\r' && i + 1 < text.Length && text[i + 1] == '\n') i++;
          counts.Add(n); n = 0; lines++;
        }
      }
      if (n > 0) counts.Add(n);
      if (counts.Count == 0) continue;
      counts.Sort();
      int median = counts[counts.Count / 2];
      if (median == 0) continue;
      int same = 0; foreach (int c in counts) if (c == median) same++;
      double score = (double)same / counts.Count * 1000 + median;
      if (score > bestScore) { bestScore = score; best = d; }
    }
    return best;
  }
  static readonly Regex Bad = new Regex(@"(?i)(?<![A-Za-z0-9_.])(?:_xlfn\.)?(WEBSERVICE|FILTERXML|CALL|REGISTER|EXEC|RTD|EVALUATE)\s*\(");
  public static FwCsvFacts Scan(string text, char d) {
    var f = new FwCsvFacts();
    var field = new StringBuilder();
    int fields = 0; bool quoted = false, inQuotes = false, any = false;
    Action end = () => {
      string s = field.ToString();
      if (s.Length > 1 && s[0] == '0' && char.IsDigit(s[1]) && Regex.IsMatch(s, @"^\d+$")) f.LeadingZeros++;
      if (s.Length >= 16 && Regex.IsMatch(s, @"^-?\d{16,}$")) f.LongNumbers++;
      if (Regex.IsMatch(s, @"^-?\d+,\d+$")) f.DecimalCommas++;
      if (s.Length > 1 && "=+-@".IndexOf(s[0]) >= 0 && !Regex.IsMatch(s, @"^[-+]?[\d.,]+%?$")) {
        f.FormulaLike++;
        string code = Regex.Replace(s, "\"(?:[^\"]|\"\")*\"", "\"\"");
        if (f.Danger == null && (code.IndexOf('|') >= 0 || Bad.IsMatch(code))) f.Danger = s.Length > 60 ? s.Substring(0, 60) : s;
      }
      field.Clear(); fields++; quoted = false; any = true;
    };
    Action row = () => { end(); f.Rows++; if (fields > f.MaxFields) f.MaxFields = fields; fields = 0; any = false; };
    for (int i = 0; i < text.Length; i++) {
      char c = text[i];
      if (inQuotes) {
        if (c == '"') { if (i + 1 < text.Length && text[i + 1] == '"') { field.Append('"'); i++; } else inQuotes = false; }
        else { if (c == '\n' || c == '\r') f.QuotedLineBreaks++; field.Append(c); }
        continue;
      }
      if (c == '"' && field.Length == 0 && !quoted) { inQuotes = true; quoted = true; any = true; continue; }
      if (c == d) { end(); continue; }
      if (c == '\r' || c == '\n') { if (c == '\r' && i + 1 < text.Length && text[i + 1] == '\n') i++; row(); continue; }
      field.Append(c); any = true;
    }
    if (inQuotes) f.UnclosedQuote = true;
    if (any || field.Length > 0) row();
    return f;
  }
}
"@

# Screens one CSV or TSV file. Returns @{ reasons; facts } like Test-Xlsx.
function Test-Csv([string]$path) {
  $reasons = New-Object Collections.Generic.List[string]
  $facts = [ordered]@{}
  $fi = Get-Item -LiteralPath $path
  if (@(".csv", ".tsv", ".txt") -notcontains $fi.Extension.ToLowerInvariant()) { $reasons.Add("not a .csv, .tsv or .txt file"); return @{ reasons = $reasons; facts = $facts } }
  if ($fi.Length -ge $MaxBytes) { $reasons.Add("20 MB or larger"); return @{ reasons = $reasons; facts = $facts } }
  $bytes = [IO.File]::ReadAllBytes($path)
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0x50 -and $bytes[1] -eq 0x4B) { $reasons.Add("a zip, not text"); return @{ reasons = $reasons; facts = $facts } }
  $bom = $false
  if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $enc = "utf-8"; $bom = $true; $text = [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3) }
  elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { $enc = "utf-16-le"; $bom = $true; $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2) }
  elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) { $enc = "utf-16-be"; $bom = $true; $text = [Text.Encoding]::BigEndianUnicode.GetString($bytes, 2, $bytes.Length - 2) }
  else {
    if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { $reasons.Add("binary data (NUL bytes), not text"); return @{ reasons = $reasons; facts = $facts } }
    try { $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes); $enc = "utf-8" }
    catch { $text = [Text.Encoding]::GetEncoding(1252).GetString($bytes); $enc = "windows-1252" }
  }
  $start = $text.Substring(0, [math]::Min(2048, $text.Length))
  if ($start -match '(?i)<!DOCTYPE\s+html|<html[\s>]|<head[\s>]|<body[\s>]') { $reasons.Add("a web page, not CSV"); return @{ reasons = $reasons; facts = $facts } }
  $sepHint = [regex]::Match($start, '^sep=(.)\r?\n')
  $delim = if ($sepHint.Success) { [char]$sepHint.Groups[1].Value } else { [FwCsv]::Guess($text) }
  $scan = [FwCsv]::Scan($text, $delim)
  if ($scan.Danger) { $reasons.Add("formula-like field that reaches out: '$($scan.Danger)'") }
  $crlf = ([regex]::Matches($text, "`r`n")).Count
  $lf = ([regex]::Matches($text, "(?<!`r)`n")).Count
  $cr = ([regex]::Matches($text, "`r(?!`n)")).Count
  $facts.encoding = $enc
  $facts.bom = $bom
  $facts.delimiter = switch ($delim) { "`t" { "tab" } default { [string]$delim } }
  $facts.lineEnding = if (($crlf -gt 0) + ($lf -gt 0) + ($cr -gt 0) -gt 1) { "mixed" } elseif ($crlf) { "CRLF" } elseif ($cr) { "CR" } else { "LF" }
  $facts.rows = $scan.Rows
  $facts.columns = $scan.MaxFields
  $tags = New-Object Collections.Generic.List[string]
  if ($sepHint.Success) { $tags.Add("sep-line") }
  if ($scan.QuotedLineBreaks) { $tags.Add("quoted-line-breaks") }
  if ($scan.LeadingZeros) { $tags.Add("leading-zeros") }
  if ($scan.LongNumbers) { $tags.Add("long-numbers") }
  if ($scan.DecimalCommas) { $tags.Add("decimal-commas") }
  if ($scan.FormulaLike) { $tags.Add("formula-like-text") }
  if ($scan.UnclosedQuote) { $tags.Add("unclosed-quote") }
  if ($enc -ne "utf-8") { $tags.Add($enc) }
  $facts.tags = @($tags)
  @{ reasons = @($reasons); facts = $facts }
}

# ---------------------------------------------------------------- Decks (P1)

$pptMainTypes = @{
  "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml" = ".pptx"
  "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml" = ".ppsx"
  "application/vnd.openxmlformats-officedocument.presentationml.template.main+xml" = ".potx"
}
# Parts a plain deck never has: VBA, ActiveX, OLE objects and packages (everything in
# ppt/embeddings/ but a chart's own workbook), web extensions, ribbon customizations, add-ins' data.
$badPptPath = '(?i)(^|/)vbaProject\.bin$|vbaProjectSignature|vbaData\.xml$|^ppt/activeX/|^ppt/embeddings/|webextension|taskpanes|^customUI/'
$pptChartBook = '(?i)^ppt/embeddings/[^/]+\.xlsx$'
$pptChartRels = '(?i)^ppt/charts/_rels/chart(ex)?\d*\.xml\.rels$'
$badPptType = '(?i)macroEnabled|vbaProject|vbaData|activeX|oleObject|webextension|taskpane|addin'
$badPptRel = @("vbaProject", "oleObject", "package", "control", "activeXControl", "activeXControlBinary", "webextension",
  "webextensiontaskpanes", "extensibility", "attachedTemplate")
# Click and hover actions: these jump within the show or play a custom show; the rest start a program,
# run a macro, open a file, another presentation or an OLE verb, and are refused.
$plainAction = '^(?i)ppaction://(hlinksldjump|hlinkshowjump\?jump=\w+|customshow\?|noaction|media)'
# The allow-list, on top of the deny-list (Roy, 2026-11-20): every part of a deck has one of these
# content types: PresentationML, DrawingML (charts, SmartArt, theme overrides), the theme, the
# package's relationships and docProps, pictures, fonts, chart styles, audio and video. Anything
# else (custom XML, VML, ink, OLE, ...) refuses the deck, and so does a part without a content type.
# The chart workbooks are screened as workbooks and allowed only where a chart points at them.
$pptAllowedType = '(?i)^(' + (@(
  'application/vnd\.openxmlformats-officedocument\.presentationml\.[a-z.]+(\+xml)?',
  'application/vnd\.ms-powerpoint\.(comments|authors|revisioninfo|changesinfo)\+xml',
  'application/vnd\.openxmlformats-officedocument\.drawingml\.[a-z.]+\+xml',
  'application/vnd\.ms-office\.drawingml\.diagramDrawing\+xml',
  'application/vnd\.openxmlformats-officedocument\.theme(Manager|Override)?\+xml',
  'application/vnd\.ms-office\.(chartstyle|chartcolorstyle|chartex)\+xml',
  'application/vnd\.openxmlformats-package\.(relationships|core-properties)\+xml',
  'application/vnd\.openxmlformats-officedocument\.(extended|custom)-properties\+xml',
  'image/(png|x-png|jpeg|jpg|pjpeg|gif|bmp|x-bmp|tiff|x-emf|emf|x-wmf|wmf|svg\+xml)',
  'application/(x-fontdata|x-font-odttf|vnd\.openxmlformats-officedocument\.obfuscatedFont|x-font-ttf|x-font-truetype|font-sfnt)',
  'font/(ttf|sfnt)',
  '(audio|video)/[a-z0-9.+-]+') -join '|') + ')$'
# Pictures, sounds, videos and fonts are binary: text there (a script, HTML, an HTA) refuses the deck.
$pptBinaryType = '(?i)^(image/(?!svg)|audio/|video/|font/|application/(x-fontdata|x-font|vnd\.openxmlformats-officedocument\.obfuscatedFont|font-sfnt))'
# An SVG picture may not script, embed or reach out: only links within itself or to data: pictures.
$svgBad = '(?i)<script|<foreignObject|<iframe|<embed|<object|javascript:|\son[a-z]+\s*=|(xlink:)?href\s*=\s*["''](?!#|data:image/)'

# The first bytes of a part, without reading the rest.
function Get-PartHead($entry, [int]$n = 512) {
  $s = $entry.Open()
  try {
    $buf = New-Object byte[] $n
    $got = 0
    while ($got -lt $n) { $k = $s.Read($buf, $got, $n - $got); if ($k -le 0) { break }; $got += $k }
    if ($got -lt $n) { $short = New-Object byte[] $got; [Array]::Copy($buf, $short, $got); $buf = $short }
    , $buf
  } finally { $s.Dispose() }
}
function Test-Prefix([byte[]]$h, [int[]]$m) {
  if ($h.Length -lt $m.Length) { return $false }
  for ($i = 0; $i -lt $m.Length; $i++) { if ($h[$i] -ne $m[$i]) { return $false } }
  $true
}
# What a part's first bytes say it is, when that's refused anywhere in a deck.
function Get-ForbiddenMagic([byte[]]$h) {
  if (Test-Prefix $h 0x4D, 0x5A) { return "an executable (MZ)" }
  if (Test-Prefix $h 0x7F, 0x45, 0x4C, 0x46) { return "an executable (ELF)" }
  foreach ($m in @(@(0xFE, 0xED, 0xFA, 0xCE), @(0xFE, 0xED, 0xFA, 0xCF), @(0xCE, 0xFA, 0xED, 0xFE), @(0xCF, 0xFA, 0xED, 0xFE), @(0xCA, 0xFE, 0xBA, 0xBE))) {
    if (Test-Prefix $h $m) { return "an executable (Mach-O)" }
  }
  if (Test-Prefix $h 0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00) { return "a shortcut (.lnk)" }
  if (Test-Prefix $h 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1) { return "a compound (OLE) file" }
  if ((Test-Prefix $h 0x50, 0x4B, 0x03, 0x04) -or (Test-Prefix $h 0x50, 0x4B, 0x05, 0x06) -or (Test-Prefix $h 0x50, 0x4B, 0x07, 0x08)) { return "a zip" }
  $null
}
# PNG, JPEG, GIF, BMP, TIFF, EMF or WMF (Office sometimes types one picture format as another).
function Test-PictureMagic([byte[]]$h) {
  (Test-Prefix $h 0x89, 0x50, 0x4E, 0x47) -or (Test-Prefix $h 0xFF, 0xD8, 0xFF) -or (Test-Prefix $h 0x47, 0x49, 0x46, 0x38) -or
  (Test-Prefix $h 0x42, 0x4D) -or (Test-Prefix $h 0x49, 0x49, 0x2A, 0x00) -or (Test-Prefix $h 0x4D, 0x4D, 0x00, 0x2A) -or
  (Test-Prefix $h 0x01, 0x00, 0x00, 0x00) -or (Test-Prefix $h 0xD7, 0xCD, 0xC6, 0x9A) -or (Test-Prefix $h 0x01, 0x00, 0x09, 0x00) -or
  (Test-Prefix $h 0x02, 0x00, 0x09, 0x00)
}
# Text where a binary part should be: markup or a script line first, or nothing but text.
function Test-LooksLikeText([byte[]]$h) {
  if (-not $h.Length) { return $false }
  if ((Test-Prefix $h 0xFF, 0xFE) -or (Test-Prefix $h 0xFE, 0xFF)) { return $true }  # UTF-16 text
  $i = if (Test-Prefix $h 0xEF, 0xBB, 0xBF) { 3 } else { 0 }
  while ($i -lt $h.Length -and @(9, 10, 13, 32) -contains $h[$i]) { $i++ }
  if ($i -lt $h.Length -and $h[$i] -eq 0x3C) { return $true }  # '<': HTML, an HTA, XML
  if ($i + 1 -lt $h.Length -and $h[$i] -eq 0x23 -and $h[$i + 1] -eq 0x21) { return $true }  # '#!'
  foreach ($b in $h) { if ($b -lt 9 -or ($b -gt 13 -and $b -lt 32) -or $b -gt 126) { return $false } }
  $true
}

# Screens one deck. Returns @{ reasons = @(...); facts = @{...} }; no reasons means it passed.
function Test-Pptx([string]$path) {
  $reasons = New-Object Collections.Generic.List[string]
  $facts = [ordered]@{}
  $fi = Get-Item -LiteralPath $path
  if (@(".pptx", ".ppsx", ".potx") -notcontains $fi.Extension.ToLowerInvariant()) { $reasons.Add("not a .pptx, .ppsx or .potx file"); return @{ reasons = $reasons; facts = $facts } }
  if ($fi.Length -ge $MaxBytes) { $reasons.Add("20 MB or larger"); return @{ reasons = $reasons; facts = $facts } }
  $head = New-Object byte[] 4
  $fs = [IO.File]::OpenRead($path); [void]$fs.Read($head, 0, 4); $fs.Dispose()
  if ($head[0] -ne 0x50 -or $head[1] -ne 0x4B -or $head[2] -ne 3 -or $head[3] -ne 4) {
    $why = if ($head[0] -eq 0xD0 -and $head[1] -eq 0xCF) { "an OLE file (a .ppt, or an encrypted deck)" } else { "not a zip (a web page or another format)" }
    $reasons.Add($why); return @{ reasons = $reasons; facts = $facts }
  }
  $zip = $null
  $part = $null
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    # A zip whose central directory isn't where its end record says reads as one empty entry here.
    $entries = @($zip.Entries | Where-Object { $_ -and $_.FullName })
    if (-not $entries.Count) { throw "a damaged zip (no part can be read: its central directory isn't where its end record says)" }
    if ($entries.Count -gt 5000) { throw "more than 5000 parts" }
    $names = @{}
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($names.ContainsKey($n.ToLowerInvariant())) { throw "duplicate part name $n" }
      $names[$n.ToLowerInvariant()] = $e
      if ($n -match $badPptPath -and $n -notmatch $pptChartBook) { $reasons.Add("forbidden part $n") }
    }
    $budget = [long]$TotalCap
    $ct = $names["[content_types].xml"]
    if (-not $ct) { throw "no [Content_Types].xml" }
    $defaults = @{}; $overrides = @{}
    $r = New-XmlReader (Read-Part $ct ([ref]$budget))
    while ($r.Read()) {
      if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
      $type = $r.GetAttribute("ContentType")
      if ($r.LocalName -eq "Default") { $defaults[$r.GetAttribute("Extension").ToLowerInvariant()] = $type }
      # Part names are URIs here (Cort%C3%A1zar.wav), plain in the zip (Cortázar.wav).
      elseif ($r.LocalName -eq "Override") { $overrides[[Uri]::UnescapeDataString($r.GetAttribute("PartName")).TrimStart('/').ToLowerInvariant()] = $type }
      if ($type -and $type -match $badPptType -and ($r.LocalName -eq "Override" -or $type -match '(?i)macroEnabled|vba')) { $reasons.Add("forbidden content type $type") }
    }
    $r.Dispose()
    function Get-PartType([string]$n) {
      $k = $n.ToLowerInvariant()
      if ($overrides.ContainsKey($k)) { return $overrides[$k] }
      $ext = [IO.Path]::GetExtension($k).TrimStart('.')
      if ($defaults.ContainsKey($ext)) { return $defaults[$ext] }
      $null
    }
    $count = @{}
    function Add-Count([string]$k, [int]$n = 1) { $count[$k] = $n + [int]$count[$k] }
    # The allow-list and every part's first bytes: no executable, shortcut, compound (OLE) file or
    # nested zip (but the chart workbooks) anywhere, and no text where a binary part should be.
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($n.EndsWith('/') -or $n -ieq "[Content_Types].xml") { continue }
      $t = Get-PartType $n
      $isBook = $n -match $pptChartBook -and $t -eq $xlsxType
      if (-not $t) { $reasons.Add("a part with no content type ($n)"); continue }
      if (-not $isBook -and $t -notmatch $pptAllowedType) { $reasons.Add("content type not on the allow-list: $t ($n)") }
      $h = Get-PartHead $e
      $magic = Get-ForbiddenMagic $h
      if ($magic -and -not ($isBook -and $magic -eq "a zip")) { $reasons.Add("$magic in $n ($t)") }
      elseif ($t -match $pptBinaryType -and (Test-LooksLikeText $h)) { $reasons.Add("text (a script, HTML or an HTA) in $n, typed $t") }
      elseif ($t -match '(?i)^image/(?!svg)' -and $h.Length -and -not (Test-PictureMagic $h)) { $reasons.Add("$n is typed $t but isn't a PNG, JPEG, GIF, BMP, TIFF, EMF or WMF picture") }
    }
    # Relationships: the main part, refused types and external targets, and the chart workbooks.
    $main = $null
    $books = @{}
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      if ($n -notmatch '(?i)\.rels$') { continue }
      $part = $n
      $r = New-XmlReader (Read-Part $e ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element -or $r.LocalName -ne "Relationship") { continue }
        $target = $r.GetAttribute("Target"); $mode = $r.GetAttribute("TargetMode")
        $short = ($r.GetAttribute("Type") -split '/')[-1]
        if ($short -eq "package" -and $n -match $pptChartRels -and $mode -ne "External" -and (Resolve-PartName $n $target) -match $pptChartBook) {
          $books[(Resolve-PartName $n $target).ToLowerInvariant()] = $true
        } elseif ($badPptRel -contains $short) { $reasons.Add("forbidden relationship $short ($n)") }
        if ($n -ieq "_rels/.rels" -and $short -eq "officeDocument") { $main = $target.TrimStart('/') }
        if ($mode -eq "External") {
          # Linked pictures, sounds and videos reach out when the deck is shown: only hyperlinks may be external.
          if ($short -ne "hyperlink") { $reasons.Add("external $short relationship to $(Format-Target $target) ($n)") }
          elseif ($target -notmatch $plainLink) { $reasons.Add("hyperlink that isn't http, https or mailto: $(Format-Target $target) ($n)") }
        }
        switch ($short) { "video" { Add-Count "video" } "audio" { Add-Count "audio" } "media" { Add-Count "media" } "font" { Add-Count "embedded-fonts" }
          "comments" { Add-Count "comments" } "diagramData" { Add-Count "smartart" } "notesSlide" { Add-Count "notes" } }
      }
      $r.Dispose()
    }
    if (-not $main) { throw "no main document relationship" }
    $mainType = Get-PartType $main
    if (-not $pptMainTypes.ContainsKey([string]$mainType)) { $reasons.Add("main part isn't a plain (macro-free) presentation, show or template ($mainType)") }
    else { $facts.kind = $pptMainTypes[$mainType] }
    foreach ($e in $entries) {
      $t = Get-PartType $e.FullName.TrimStart('/')
      if ($t -and $t -match $badPptType) { $reasons.Add("forbidden part type $t ($($e.FullName))") }
    }
    foreach ($k in @($names.Keys)) {
      if ($k -notmatch $pptChartBook) { continue }
      $n = $names[$k].FullName.TrimStart('/')
      if (-not $books[$k]) { $reasons.Add("forbidden part $n (no chart refers to it)"); continue }
      $t = Get-PartType $n
      if ($t -ne $xlsxType) { $reasons.Add("chart workbook $n isn't a plain .xlsx ($t)"); continue }
      foreach ($why in (Test-Workbook $names[$k] ([ref]$budget))) { $reasons.Add("chart workbook $($n): $why") }
    }

    # Every XML part: forbidden strings and elements, click actions, and the facts.
    $slides = 0; $hidden = 0; $cjk = 0
    foreach ($e in $entries) {
      $n = $e.FullName.TrimStart('/')
      $t = Get-PartType $n
      if ($n -notmatch '(?i)\.(xml|rels|vml)$' -and -not ($t -and $t -match '(?i)[+/]xml$')) { continue }
      $bytes = Read-Part $e ([ref]$budget)
      $text = Get-XmlText $bytes
      $m = [regex]::Match($text, $badString)
      if ($m.Success) { $reasons.Add("contains '$($m.Value)' ($n)") }
      if ($n -match '(?i)\.rels$') { continue }
      if ($t -match '(?i)^image/svg') {
        $m = [regex]::Match($text, $svgBad)
        if ($m.Success) { $reasons.Add("an SVG picture that scripts, embeds or links out ('$($m.Value.Trim())', $n)") }
      }
      $isSlide = $n -match '(?i)^ppt/slides/slide\d+\.xml$'
      if ($isSlide) {
        $slides++
        foreach ($tm in [regex]::Matches($text, '<a:t>([^<]+)</a:t>')) { $cjk += [regex]::Matches($tm.Groups[1].Value, $cjkClass).Count }
      }
      $part = "$n ($t)"
      $r = New-XmlReader $bytes
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        $ln = $r.LocalName
        switch -CaseSensitive ($ln) {
          "sld" { if ($isSlide -and $r.GetAttribute("show") -in "0", "false") { $hidden++ } }
          { $_ -in "oleObj", "control", "embed" } { if ($r.NamespaceURI -match 'presentationml') { $reasons.Add("$ln ($n)") } }
          { $_ -in "hlinkClick", "hlinkHover", "hlinkMouseOver" } {
            $action = $r.GetAttribute("action")
            if ($action -and $action -notmatch $plainAction) {
              $what = if ($action -match '^(?i)ppaction://([a-z]+)') { $Matches[1] } else { "an action" }
              $reasons.Add("a click action that leaves the show ($what) ($n)")
            }
            if ($isSlide -or $n -match '(?i)^ppt/slide(Layout|Master)s/') { Add-Count "hyperlinks" }
          }
          "snd" { if ($r.NamespaceURI -match 'presentationml') { Add-Count "sounds" } }
          "pic" { if ($isSlide) { Add-Count "pictures" } }
          "tbl" { if ($isSlide) { Add-Count "tables" } }
          "chart" { if ($r.NamespaceURI -match 'drawingml/2006/chart' -and $isSlide) { Add-Count "charts" } }
          "grpSp" { if ($isSlide) { Add-Count "groups" } }
          "transition" { if ($isSlide) { Add-Count "transitions" } }
          "timing" { if ($isSlide) { Add-Count "animations" } }
          "prstTxWarp" { if ($isSlide -and $r.GetAttribute("prst") -ne "textNoShape") { Add-Count "wordart" } }
          "sp3d" { if ($isSlide) { Add-Count "3d" } }
          "normAutofit" { if ($isSlide -and $r.GetAttribute("fontScale")) { Add-Count "autofit" } }
          "sectionLst" { Add-Count "sections" }
          "custShow" { Add-Count "custom-shows" }
          "embeddedFont" { Add-Count "embedded-fonts" }
          "oMath" { Add-Count "equations" }
          "svgBlip" { Add-Count "svg" }
          "contentPart" { if ($isSlide) { Add-Count "ink" } }
          "sldSz" {
            $cx = [double]$r.GetAttribute("cx"); $cy = [double]$r.GetAttribute("cy")
            if ($cy -gt 0) { $facts.slideSize = if ([Math]::Abs($cx / $cy - 16 / 9) -lt 0.01) { "16:9" } elseif ([Math]::Abs($cx / $cy - 4 / 3) -lt 0.01) { "4:3" } elseif ([Math]::Abs($cx / $cy - 16 / 10) -lt 0.01) { "16:10" } elseif ($cy -gt $cx) { "portrait" } else { "other" } }
          }
          "pPr" { if ($isSlide -and $r.GetAttribute("rtl") -eq "1") { Add-Count "rtl" } }
        }
      }
      $r.Dispose()
    }
    if ($cjk -ge 10) { Add-Count "east-asian" $cjk }
    $facts.slides = $slides
    if ($hidden) { $facts.hiddenSlides = $hidden }
    # Photo-heavy: many pictures for the slides (a picture or more on every slide, on average).
    if ($slides -gt 0 -and [int]$count["pictures"] -ge $slides) { Add-Count "photo-heavy" }

    # Facts for the manifest.
    $app = $names["docprops/app.xml"]
    if ($app) {
      $r = New-XmlReader (Read-Part $app ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        switch ($r.LocalName) {
          "Application" { $facts.producer = $r.ReadElementContentAsString() }
          "AppVersion" { $facts.appVersion = $r.ReadElementContentAsString() }
        }
      }
      $r.Dispose()
    }
    $core = $names["docprops/core.xml"]
    if ($core) {
      $r = New-XmlReader (Read-Part $core ([ref]$budget))
      while ($r.Read()) {
        if ($r.NodeType -eq [Xml.XmlNodeType]::Element -and $r.LocalName -eq "modified") { $v = $r.ReadElementContentAsString(); if ($v.Length -ge 10) { $facts.modified = $v.Substring(0, 10) } }
      }
      $r.Dispose()
    }
    if (-not $facts.Contains("producer")) {
      $facts.producer = if (-not $names["docprops/core.xml"] -and $entries[$entries.Count - 1].FullName -eq "[Content_Types].xml") { "Google Slides (inferred: no docProps, [Content_Types].xml last)" } else { "unknown (no docProps/app.xml)" }
    }
    $tags = New-Object Collections.Generic.List[string]
    foreach ($k in "pictures", "photo-heavy", "charts", "tables", "smartart", "groups", "wordart", "3d", "autofit", "svg", "equations",
                   "transitions", "animations", "sounds", "video", "audio", "hyperlinks", "notes", "comments", "sections", "custom-shows",
                   "embedded-fonts", "ink", "rtl", "east-asian") {
      if ($count[$k]) { $tags.Add($k) }
    }
    if ($hidden) { $tags.Add("hidden-slides") }
    $facts.tags = @($tags)
    $facts.counts = [ordered]@{}
    foreach ($k in @($count.Keys | Sort-Object)) { $facts.counts[$k] = $count[$k] }
  } catch {
    $where = if ($part) { " in $part" } else { "" }
    $reasons.Add("malformed$($where): $($_.Exception.Message)")
  } finally {
    if ($zip) { $zip.Dispose() }
  }
  @{ reasons = @($reasons | Select-Object -Unique); facts = $facts }
}

# The format of a manifest entry or candidate: docx, xlsx, csv or pptx.
function Get-Format($e) {
  $ext = if ($e.file) { [IO.Path]::GetExtension($e.file) } elseif ($e.format) { "." + $e.format } else { [IO.Path]::GetExtension(([Uri]$e.url).AbsolutePath) }
  switch -Regex ($ext.ToLowerInvariant()) { '^\.(xlsx|xltx)$' { "xlsx" } '^\.(csv|tsv|txt)$' { "csv" } '^\.(pptx|ppsx|potx)$' { "pptx" } default { "docx" } }
}
# The file extension a format's file is kept with.
function Get-Extension($e, [string]$format) {
  if ($e.file) { return [IO.Path]::GetExtension($e.file).ToLowerInvariant() }
  $ext = [IO.Path]::GetExtension(([Uri]$e.url).AbsolutePath).ToLowerInvariant()
  switch ($format) { "xlsx" { if ($ext -eq ".xltx") { ".xltx" } else { ".xlsx" } } "csv" { if ($ext -eq ".tsv") { ".tsv" } else { ".csv" } }
    "pptx" { if ($e.format -and -not $ext) { $ext = ".$($e.format)".ToLowerInvariant() }  # a template served from a URL without an extension
      if (@(".ppsx", ".potx") -contains $ext) { $ext } else { ".pptx" } } default { ".docx" } }
}

# fwtool check refuses malformed packages and zip bombs, and checks every reference.
function Test-Fwtool([string]$path) {
  $ErrorActionPreference = "Continue"  # stderr lines from a native tool aren't exceptions
  $output = & $Fwtool check $path 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -eq 0) { return $null }
  $errors = @($output | Where-Object { $_ -match '^\s*(error|refused)' -or $_ -match 'FAIL' } | Select-Object -First 3)
  "fwtool check failed: " + (($errors | ForEach-Object { $_.Trim() }) -join " | ")
}

# Defender scans a folder (custom scan, Defender's own settings). Returns $true when it found nothing.
# MpCmdRun exits 0 also when it found a threat and removed it ("... found 1 threats.", probed with
# EICAR on 2026-11-20), so a scan is clean only with exit 0 and "found no threats".
function Test-Defender([string]$folder) {
  $ErrorActionPreference = "Continue"
  $output = & $mpcmdrun -Scan -ScanType 3 -File $folder 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $text = ($output | Where-Object { $_.Trim() }) -join " / "
  $threats = 0
  foreach ($m in [regex]::Matches($text, '(?i)found (\d+) threats?')) { $threats += [int]$m.Groups[1].Value }
  if ($code -eq 2 -and -not $threats) { $threats = 1 }
  @{ clean = ($code -eq 0 -and -not $threats -and $text -match '(?i)found no threats'); code = $code; threats = $threats; output = $text }
}

# Before anything is downloaded or screened (Roy, 2026-11-20): Defender's service, antivirus and
# real-time protection on, and its signatures at most a day old. Only its status is read; old
# signatures are updated (MpCmdRun -SignatureUpdate updates the definitions and changes no setting).
# No Defender or security setting, exclusion or policy is ever changed here.
function Assert-DefenderReady {
  $s = Get-MpComputerStatus
  $off = @(@("AMServiceEnabled", "AntivirusEnabled", "RealTimeProtectionEnabled") | Where-Object { -not $s.$_ })
  if ($off.Count) { throw "Defender isn't ready ($($off -join ', ') off): nothing is downloaded or screened. Tell Roy; no setting is changed here." }
  if (((Get-Date) - $s.AntivirusSignatureLastUpdated).TotalHours -gt 24) {
    Write-Host "Defender: signatures from $($s.AntivirusSignatureLastUpdated), more than 24 h old: MpCmdRun -SignatureUpdate"
    & $mpcmdrun -SignatureUpdate | Out-Null
    $s = Get-MpComputerStatus
    if (((Get-Date) - $s.AntivirusSignatureLastUpdated).TotalHours -gt 24) { throw "Defender's signatures are still from $($s.AntivirusSignatureLastUpdated) after an update: nothing is downloaded or screened." }
  }
  [ordered]@{ service = $s.AMServiceEnabled; antivirus = $s.AntivirusEnabled; realTime = $s.RealTimeProtectionEnabled
    signatures = $s.AntivirusSignatureVersion; signaturesUpdated = $s.AntivirusSignatureLastUpdated.ToString("yyyy-MM-dd HH:mm"); engine = $s.AMEngineVersion }
}

# The standard EICAR antivirus test file, as base64 so that this script isn't one itself.
$eicarBase64 = "WDVPIVAlQEFQWzRcUFpYNTQoUF4pN0NDKTd9JEVJQ0FSLVNUQU5EQVJELUFOVElWSVJVUy1URVNULUZJTEUhJEgrSCo="
# Proves Defender scans the quarantine (an exclusion would make it skip the folder silently): the
# EICAR file in a new, empty subfolder must be caught, by real-time protection (the write refused or
# the file removed) or by the scan (exit 2). Whatever is left of it is deleted. Throws otherwise.
function Assert-EicarCaught([string]$quarantine) {
  $dir = Join-Path $quarantine "eicar-proof"
  if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir }
  New-Item -ItemType Directory $dir | Out-Null
  $file = Join-Path $dir "eicar.com"
  $how = $null
  try {
    try { [IO.File]::WriteAllBytes($file, [Convert]::FromBase64String($eicarBase64)) }
    catch { $how = "real-time protection refused to write it" }
    if (-not $how) {
      for ($k = 0; $k -lt 20 -and (Test-Path -LiteralPath $file); $k++) { Start-Sleep -Milliseconds 250 }
      if (-not (Test-Path -LiteralPath $file)) { $how = "real-time protection removed it" }
    }
    if (-not $how) {
      $scan = Test-Defender $file
      if ($scan.threats) { $how = "the scan found it (exit $($scan.code): $($scan.output))" }
      elseif (-not (Test-Path -LiteralPath $file)) { $how = "real-time protection removed it during the scan" }
      else { throw "Defender didn't catch the EICAR test file in $dir (scan exit $($scan.code): $($scan.output)): nothing is downloaded or screened." }
    }
  } finally {
    Remove-Item -Recurse -Force -LiteralPath $dir -ErrorAction SilentlyContinue
  }
  if (Test-Path -LiteralPath $dir) { throw "couldn't delete $dir after the EICAR proof" }
  $how
}

# ---------------------------------------------------------------- Run

# Screens one file by its format; fwtool check for the packages.
function Test-File([string]$path, [string]$format) {
  $t = switch ($format) { "xlsx" { Test-Xlsx $path } "csv" { Test-Csv $path } "pptx" { Test-Pptx $path } default { Test-Docx $path } }
  $reasons = @($t.reasons)
  if (-not $reasons.Count -and $format -ne "csv") { $fw = Test-Fwtool $path; if ($fw) { $reasons = @($fw) } }
  @{ reasons = $reasons; facts = $t.facts }
}

if ($Screen) {
  foreach ($p in $Screen) {
    $t = Test-File ([IO.Path]::GetFullPath($p)) (Get-Format ([pscustomobject]@{ url = "file:///$([IO.Path]::GetFileName($p))" }))
    $reasons = @($t.reasons)
    if ($reasons.Count) { "REJECTED {0}: {1}" -f $p, ($reasons -join "; ") }
    else { "passed   {0}: {1}" -f $p, (ConvertTo-PrettyJson $t.facts).Replace("`n", " ") }
  }
  return
}

# -DefenderCheck: Defender's status, the EICAR proof in each folder (an exclusion could cover any of
# them), every file in them scanned on its own, then each folder as a whole. Nothing is deleted but
# the EICAR file (and a folder this made for the proof). Exit 1 if anything isn't clean.
if ($DefenderCheck) {
  $status = Assert-DefenderReady
  "Defender: service, antivirus and real-time protection on; signatures {0} from {1}; engine {2}" -f $status.signatures, $status.signaturesUpdated, $status.engine
  $bad = 0
  foreach ($dir in $DefenderCheck) {
    $full = [IO.Path]::GetFullPath($dir)
    $made = -not (Test-Path -LiteralPath $full)
    if ($made) { New-Item -ItemType Directory -Force $full | Out-Null }
    try { "EICAR in ${full}: caught, " + (Assert-EicarCaught $full) + " (deleted)" }
    finally { if ($made) { Remove-Item -Recurse -Force -LiteralPath $full -ErrorAction SilentlyContinue } }
    if ($made) { continue }
    $n = 0; $here = 0
    foreach ($f in @(Get-ChildItem -LiteralPath $full -File -Recurse | Sort-Object FullName)) {
      $n++
      $one = Test-Defender $f.FullName
      if (-not $one.clean -or -not (Test-Path -LiteralPath $f.FullName)) { $here++; Write-Host "NOT CLEAN $($f.FullName): exit $($one.code), $($one.output)" -ForegroundColor Red }
    }
    $all = Test-Defender $full
    if (-not $all.clean) { $here++ }
    $bad += $here
    "{0}: {1} files scanned one by one, {2} not clean; the folder as a whole: exit {3}, {4}" -f $full, $n, $here, $all.code, $all.output
  }
  if ($bad) { exit 1 }
  "Defender check: all clean."
  return
}

function Get-Slug([string]$s) {
  $s = [Uri]::UnescapeDataString($s).ToLowerInvariant().Normalize([Text.NormalizationForm]::FormD)
  $s = [regex]::Replace($s, '\p{Mn}', '')
  $s = [regex]::Replace($s, '[^a-z0-9]+', '-').Trim('-')
  if ($s.Length -gt 48) {
    $s = $s.Substring(0, 48)
    $cut = $s.LastIndexOf('-')
    if ($cut -gt 24) { $s = $s.Substring(0, $cut) }
  }
  $s.Trim('-')
}

$quarantine = Join-Path $Out "quarantine"
New-Item -ItemType Directory -Force $Out | Out-Null
$files = @($manifestData.files | Where-Object { $_ })
$rejected = @($manifestData.rejected | Where-Object { $_ })

if ($Candidates) {
  $list = @((Read-Json $Candidates) | ForEach-Object { $_ })  # Windows PowerShell returns a JSON array as one object
  $known = @{}
  foreach ($f in $files) { $known[$f.url] = $true }
  # Rejected URLs are skipped, unless -Retry: then they're tried again and their old reasons dropped.
  if ($Retry) { $retryUrls = @{}; foreach ($c in $list) { $retryUrls[$c.url] = $true }; $rejected = @($rejected | Where-Object { -not $retryUrls[$_.url] }) }
  foreach ($f in $rejected) { $known[$f.url] = $true }
  $usedIds = @{}
  foreach ($f in $files) { $usedIds[$f.id] = $true }
  $work = @()
  foreach ($c in $list) {
    if ($known[$c.url]) { continue }
    $known[$c.url] = $true
    $src = Find-Source ([Uri]$c.url)
    $tag = if ($src) { $src.tag } else { "x" }
    $name = if ($c.title) { $c.title } else { [IO.Path]::GetFileNameWithoutExtension(([Uri]$c.url).AbsolutePath) }
    $slug = Get-Slug $name
    if ($slug.StartsWith("$tag-")) { $slug = $slug.Substring($tag.Length + 1) }
    $base = "$tag-$slug"
    $id = $base; $k = 2
    while ($usedIds[$id]) { $id = "$base-$k"; $k++ }
    $usedIds[$id] = $true
    $format = Get-Format $c
    $work += [pscustomobject]@{ id = $id; candidate = $c; format = $format; ext = (Get-Extension $c $format) }
  }
} else {
  $work = @($files | ForEach-Object { $format = Get-Format $_; [pscustomobject]@{ id = $_.id; entry = $_; format = $format; ext = (Get-Extension $_ $format) } })
}
if ($Only) { $work = @($work | Where-Object { $Only -contains $_.id }) }
$Formats = @($Formats | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($Formats.Count) { $work = @($work | Where-Object { $Formats -contains $_.format }) }
if ($work.Count -eq 0) { "Nothing to do."; return }

# 0. Defender on and up to date, and proven to scan the quarantine (the EICAR test file), before
# anything is downloaded or copied there.
if (Test-Path $quarantine) { Remove-Item -Recurse -Force $quarantine }
New-Item -ItemType Directory -Force $quarantine | Out-Null
$defenderStatus = Assert-DefenderReady
"Defender: service, antivirus and real-time protection on; signatures {0} from {1}; engine {2}" -f $defenderStatus.signatures, $defenderStatus.signaturesUpdated, $defenderStatus.engine
$runStart = Get-Date
$eicar = Assert-EicarCaught $quarantine
"Defender: the EICAR test file in the quarantine was caught: $eicar (deleted)"
$detections = @()
$blockedHosts = @{}
# A file Defender doesn't pass: deleted at once and logged; a detection (exit 2, or the file removed)
# also stops downloads from its hosts.
function Add-Detection($res, $scan, [string]$where) {
  $script:detections += [pscustomobject][ordered]@{ id = $res.id; url = $res.url; where = $where; exitCode = $scan.code; output = $scan.output }
  if ($scan.threats -or $scan.removed) {
    foreach ($u in @($res.url, $res.finalUrl) | Where-Object { $_ }) { $script:blockedHosts[([Uri]$u).Host.ToLowerInvariant()] = $true }
    Write-Host ("DEFENDER DETECTION in {0} ({1}, {2}): {3}. No more downloads from {4}." -f $res.id, $where, $res.url, $scan.output, (@($script:blockedHosts.Keys) -join ", ")) -ForegroundColor Red
  }
}

# 1. Into quarantine: one new, empty folder per file, and Defender scans it on its own at once.
$results = @()
$i = 0
foreach ($w in $work) {
  $i++
  $folder = Join-Path $quarantine ("{0:d4}" -f $i)
  New-Item -ItemType Directory $folder | Out-Null
  $q = Join-Path $folder "$($w.id)$($w.ext)"
  $e = if ($w.entry) { $w.entry } else { $w.candidate }
  $res = [ordered]@{ id = $w.id; url = $e.url; finalUrl = $null; quarantine = $q; sha256 = $null; size = $null; reasons = @(); facts = $null }
  try {
    if ($e.category -notmatch '^[a-z]+$') { throw "no valid category" }
    if ($Rescreen) {
      $existing = Join-Path $Out $e.file
      if (-not (Test-Path -LiteralPath $existing)) { throw "missing from the corpus" }
      Copy-Item -LiteralPath $existing $q
    } else {
      $h = ([Uri]$e.url).Host.ToLowerInvariant()
      if ($blockedHosts[$h]) { throw "not downloaded: Defender found a threat in another file from $h" }
      $res.finalUrl = Get-Download $e.url $q $w.format
      Start-Sleep -Milliseconds 300
    }
    $res.sha256 = (Get-FileHash -LiteralPath $q -Algorithm SHA256).Hash.ToLowerInvariant()
    $res.size = (Get-Item -LiteralPath $q).Length
    if ($w.entry -and $w.entry.sha256 -and $w.entry.sha256 -ne $res.sha256) { $res.reasons = @("hash differs from the manifest (the file changed)") }
    $one = Test-Defender $q
    $one.removed = -not (Test-Path -LiteralPath $q)
    if (-not $one.clean -or $one.removed) {
      $res.reasons = @($res.reasons) + ("Defender (exit {0}{1}): {2}" -f $one.code, $(if ($one.removed) { ", removed" } else { "" }), $one.output)
      Add-Detection ([pscustomobject]$res) $one "quarantine"
    }
  } catch {
    $x = $_.Exception
    while ($x.InnerException) { $x = $x.InnerException }
    $res.reasons = @("download: $($x.Message)")
  }
  # Rejected files are deleted at once.
  if ($res.reasons.Count) { Remove-Item -Recurse -Force -LiteralPath $folder -ErrorAction SilentlyContinue }
  "{0,4}/{1} {2,-60} {3}" -f $i, $work.Count, $w.id, $(if ($res.reasons.Count) { $res.reasons[0] } else { "$($res.size) bytes" })
  $results += [pscustomobject]$res
}
# Defender's own record: a file real-time protection took during the download (before the scan saw
# it) counts as a detection too. The EICAR proof's folder isn't a file of ours.
foreach ($d in @(Get-MpThreatDetection -ErrorAction SilentlyContinue | Where-Object { $_.InitialDetectionTime -ge $runStart })) {
  $name = (@(Get-MpThreat -ErrorAction SilentlyContinue | Where-Object { $_.ThreatID -eq $d.ThreatID }) | Select-Object -First 1).ThreatName
  foreach ($r in @($d.Resources)) {
    if ([string]$r -notlike "*$quarantine*" -or [string]$r -like "*eicar-proof*") { continue }
    foreach ($res in $results) {
      if ([string]$r -notlike "*$(Split-Path -Leaf $res.quarantine)*") { continue }
      if (-not ($res.reasons -match '^Defender')) { $res.reasons = @($res.reasons) + "Defender detection: $name" }
      Add-Detection $res @{ code = 2; threats = 1; removed = $true; output = "$name ($r)" } "quarantine (real-time)"
      Remove-Item -Recurse -Force -LiteralPath (Split-Path -Parent $res.quarantine) -ErrorAction SilentlyContinue
    }
  }
}
"Defender: each file scanned on its own in the quarantine; {0} not passed" -f @($detections).Count

# 2-4. The content screen, then fwtool check; a rejected file is deleted at once.
$seen = @{}
if ($Candidates) { foreach ($f in $files) { $seen[$f.sha256] = $f.id } }  # a new URL for a file we have
foreach ($res in $results) {
  if ($res.reasons.Count -or -not (Test-Path -LiteralPath $res.quarantine)) { continue }
  if ($seen[$res.sha256]) { $res.reasons = @("same file as $($seen[$res.sha256])") }
  else {
    $seen[$res.sha256] = $res.id
    $w = $work | Where-Object { $_.id -eq $res.id } | Select-Object -First 1
    $t = Test-File $res.quarantine $w.format
    $res.facts = $t.facts
    $res.reasons = @($t.reasons)
  }
  if ($res.reasons.Count) { Remove-Item -Recurse -Force -LiteralPath (Split-Path -Parent $res.quarantine) -ErrorAction SilentlyContinue }
}

# 5. Passing files move into the corpus; the rest are deleted. New candidates (and, with -Rescreen,
# every file) get their facts and tags recorded in the manifest.
function Set-Facts($entry, $facts, [string]$format) {
  $values = switch ($format) {
    "xlsx" {
      [ordered]@{
        modified = $facts.modified
        producer = (@($facts.producer, $facts.appVersion) | Where-Object { $_ }) -join " "
        sheets = $facts.sheets; cells = $facts.cells; formulas = $facts.formulas
        tags = $facts.tags
        functions = @($facts.functions.Keys)
      }
    }
    "pptx" {
      [ordered]@{
        modified = $facts.modified
        producer = (@($facts.producer, $facts.appVersion) | Where-Object { $_ }) -join " "
        slides = $facts.slides; hiddenSlides = $facts.hiddenSlides; slideSize = $facts.slideSize
        tags = $facts.tags
      }
    }
    "csv" {
      [ordered]@{ encoding = $facts.encoding; bom = $facts.bom; delimiter = $facts.delimiter; lineEnding = $facts.lineEnding
        rows = $facts.rows; columns = $facts.columns; tags = $facts.tags }
    }
    default {
      [ordered]@{
        modified = $facts.modified
        producer = (@($facts.producer, $facts.appVersion) | Where-Object { $_ }) -join " "
        compatibilityMode = $facts.compatibilityMode
        savedPages = $facts.savedPages
        tags = $facts.tags
      }
    }
  }
  foreach ($k in $values.Keys) { $entry | Add-Member -NotePropertyName $k -NotePropertyValue $values[$k] -Force }
}
$byId = [ordered]@{}
foreach ($f in $files) { $byId[$f.id] = $f }
foreach ($res in $results) {
  $w = $work | Where-Object { $_.id -eq $res.id } | Select-Object -First 1
  $e = if ($w.entry) { $w.entry } else { $w.candidate }
  # A deck's extension follows its main part (.pptx, .ppsx or .potx), not the URL or the candidate:
  # PowerPoint refuses a presentation named .potx ("its file extension has changed").
  if ($w.format -eq "pptx" -and -not $Rescreen -and $res.facts -and $res.facts.kind) { $w.ext = $res.facts.kind }
  $dest = Join-Path $Out "$($w.format)\$($e.category)\$($res.id)$($w.ext)"
  if ($res.reasons.Count) {
    "REJECTED {0}: {1}" -f $res.id, ($res.reasons -join "; ")
    if ($Candidates -or $Rescreen) {
      $rejected += [pscustomobject][ordered]@{ url = $e.url; publisher = $e.publisher; category = $e.category; language = $e.language; reasons = @($res.reasons); date = $today }
      $byId.Remove($res.id)
    }
    if ($Rescreen -or -not $Candidates) { if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest } }
    continue
  }
  New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
  if (-not $Rescreen) { Move-Item -LiteralPath $res.quarantine $dest -Force }
  # Defender again, on the screened file where it now lies in the corpus.
  $two = Test-Defender $dest
  $two.removed = -not (Test-Path -LiteralPath $dest)
  if (-not $two.clean -or $two.removed) {
    $res.reasons = @("Defender in the corpus folder (exit {0}{1}): {2}" -f $two.code, $(if ($two.removed) { ", removed" } else { "" }), $two.output)
    Add-Detection $res $two "corpus"
    Remove-Item -LiteralPath $dest -ErrorAction SilentlyContinue
    "REJECTED {0}: {1}" -f $res.id, ($res.reasons -join "; ")
    if ($Candidates -or $Rescreen) {
      $rejected += [pscustomobject][ordered]@{ url = $e.url; publisher = $e.publisher; category = $e.category; language = $e.language; reasons = @($res.reasons); date = $today }
      $byId.Remove($res.id)
    }
    continue
  }
  if ($Candidates) {
    $c = $e
    $e = [pscustomobject][ordered]@{
      id = $res.id
      file = "$($w.format)/$($c.category)/$($res.id)$($w.ext)"
      category = $c.category
      language = $c.language
      title = $c.title
      publisher = $c.publisher
      url = $c.url
      landing = $c.landing
      terms = $c.terms
      termsUrl = $c.terms_url
      sha256 = $res.sha256
      size = $res.size
      retrieved = $today
    }
    $byId[$res.id] = $e
  }
  if ($Candidates -or $Rescreen) { Set-Facts $e $res.facts $w.format }
}
Remove-Item -Recurse -Force $quarantine

# The run's results beside the corpus, and the updated manifest.
Write-PrettyJson ([ordered]@{
  date = $today
  mode = $(if ($Candidates) { "candidates" } elseif ($Rescreen) { "rescreen" } else { "fetch" })
  defender = [ordered]@{ status = $defenderStatus; eicarProof = $eicar; scans = "each file on its own in the quarantine, then in the corpus folder"
    detections = @($detections); blockedHosts = @($blockedHosts.Keys) }
  passed =@($results | Where-Object { -not $_.reasons.Count } | ForEach-Object { [ordered]@{ id = $_.id; sha256 = $_.sha256; facts = $_.facts } })
  rejected = @($results | Where-Object { $_.reasons.Count } | ForEach-Object { [ordered]@{ id = $_.id; url = $_.url; reasons = $_.reasons } })
}) (Join-Path $Out "screen.json")
if ($Candidates -or $Rescreen) {
  $manifestData.files = @($byId.Values | Sort-Object category, id)
  $manifestData | Add-Member -NotePropertyName rejected -NotePropertyValue @($rejected) -Force
  Write-PrettyJson $manifestData $Manifest
}
# The functions the passing workbooks' formulas use: in how many formula cells, and in how many
# workbooks (s1-calc picks its functions by these counts; plan section 7.2).
$cellsUsing = @{}; $booksUsing = @{}
foreach ($res in $results) {
  if ($res.reasons.Count -or -not $res.facts -or -not $res.facts.Contains("functions")) { continue }
  foreach ($k in $res.facts.functions.Keys) { $cellsUsing[$k] = [int]$cellsUsing[$k] + [int]$res.facts.functions[$k]; $booksUsing[$k] = 1 + [int]$booksUsing[$k] }
}
if ($cellsUsing.Count) {
  Write-PrettyJson ([ordered]@{
    date = $today
    workbooks = @($results | Where-Object { -not $_.reasons.Count -and $_.facts -and $_.facts.Contains("functions") }).Count
    functions = @($cellsUsing.Keys | Sort-Object { -$cellsUsing[$_] }, { $_ } | ForEach-Object { [ordered]@{ name = $_; cells = $cellsUsing[$_]; workbooks = $booksUsing[$_] } })
  }) (Join-Path $Out "functions.json")
}
$passCount = @($results | Where-Object { -not $_.reasons.Count }).Count
"$passCount of $($results.Count) passed. Corpus: $Out. Results: $(Join-Path $Out 'screen.json')"
