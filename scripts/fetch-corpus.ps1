<#
.SYNOPSIS
  Builds the real-world Word corpus (plan section 10) from tests\office\real-world.json: downloads
  every file, checks its hash and screens it, so anyone can rebuild the corpus exactly. The
  documents are never committed (their licences vary); only the manifest is.

  Corpus layout (point FW_CORPUS at the folder):
    docx\<category>\<id>.docx           the documents that passed the screen
    reference\docx\<category>\<id>.docx.pdf, reference\manifest.json
                                        Word's PDFs (scripts\reference-renders.ps1 -Corpus)
    screen.json                         this run's results: every file, passed or rejected, and why

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
  5. Antivirus: Microsoft Defender scans the quarantine folder (no setting is changed).
  Only files that pass everything move into the corpus. Rejected files are deleted; screen.json
  keeps the reasons. A site that asks for a browser challenge is skipped, never worked around.

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
  # Screen these local files and print the results; nothing is downloaded, moved or deleted.
  [string[]]$Screen
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
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
function Get-Download([string]$url, [string]$dest) {
  $headers = "$dest.headers"; $errors = "$dest.errors"
  try {
    for ($hop = 0; $hop -le 5; $hop++) {
      $u = [Uri]$url
      if ($u.Scheme -ne "https") { throw "not HTTPS: $url" }
      if (-not (Find-Source $u)) { throw "not on the sources list: $($u.Host)" }
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
      # The served name must be a .docx too when the server or the URL gives one.
      $m = [regex]::Match($head, "(?im)^content-disposition:.*?filename\*?=(?:UTF-8'')?""?([^"";\r\n]+)")
      $name = if ($m.Success) { [Uri]::UnescapeDataString($m.Groups[1].Value) } else { [Uri]::UnescapeDataString($u.Segments[-1]) }
      if ($name -match '(?i)\.(docm|dotx|dotm|doc|rtf|odt|zip|7z|rar|exe|pdf|htm|html)$') { throw "served as $name, not a .docx" }
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

# fwtool check refuses malformed packages and zip bombs, and checks every reference.
function Test-Fwtool([string]$path) {
  $ErrorActionPreference = "Continue"  # stderr lines from a native tool aren't exceptions
  $output = & $Fwtool check $path 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -eq 0) { return $null }
  $errors = @($output | Where-Object { $_ -match '^\s*(error|refused)' -or $_ -match 'FAIL' } | Select-Object -First 3)
  "fwtool check failed: " + (($errors | ForEach-Object { $_.Trim() }) -join " | ")
}

# Defender scans a folder (custom scan, Defender's own settings). Returns $true when it found nothing.
function Test-Defender([string]$folder) {
  $ErrorActionPreference = "Continue"
  $output = & $mpcmdrun -Scan -ScanType 3 -File $folder 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  @{ clean = ($code -eq 0); code = $code; output = (($output | Where-Object { $_.Trim() }) -join " / ") }
}

# ---------------------------------------------------------------- Run

if ($Screen) {
  foreach ($p in $Screen) {
    $t = Test-Docx ([IO.Path]::GetFullPath($p))
    $reasons = @($t.reasons)
    if (-not $reasons.Count) { $fw = Test-Fwtool $p; if ($fw) { $reasons = @($fw) } }
    if ($reasons.Count) { "REJECTED {0}: {1}" -f $p, ($reasons -join "; ") }
    else { "passed   {0}: {1}" -f $p, (ConvertTo-PrettyJson $t.facts).Replace("`n", " ") }
  }
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

$docxRoot = Join-Path $Out "docx"
$quarantine = Join-Path $Out "quarantine"
New-Item -ItemType Directory -Force $Out, $docxRoot | Out-Null
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
    $work += [pscustomobject]@{ id = $id; candidate = $c }
  }
} else {
  $work = @($files | ForEach-Object { [pscustomobject]@{ id = $_.id; entry = $_ } })
}
if ($Only) { $work = @($work | Where-Object { $Only -contains $_.id }) }
if ($work.Count -eq 0) { "Nothing to do."; return }

# 1. Into quarantine: one new, empty folder per file.
if (Test-Path $quarantine) { Remove-Item -Recurse -Force $quarantine }
New-Item -ItemType Directory -Force $quarantine | Out-Null
$results = @()
$i = 0
foreach ($w in $work) {
  $i++
  $folder = Join-Path $quarantine ("{0:d4}" -f $i)
  New-Item -ItemType Directory $folder | Out-Null
  $q = Join-Path $folder "$($w.id).docx"
  $e = if ($w.entry) { $w.entry } else { $w.candidate }
  $res = [ordered]@{ id = $w.id; url = $e.url; finalUrl = $null; quarantine = $q; sha256 = $null; size = $null; reasons = @(); facts = $null }
  try {
    if ($e.category -notmatch '^[a-z]+$') { throw "no valid category" }
    if ($Rescreen) {
      $existing = Join-Path $Out $e.file
      if (-not (Test-Path -LiteralPath $existing)) { throw "missing from the corpus" }
      Copy-Item -LiteralPath $existing $q
    } else {
      $res.finalUrl = Get-Download $e.url $q
      Start-Sleep -Milliseconds 300
    }
    $res.sha256 = (Get-FileHash -LiteralPath $q -Algorithm SHA256).Hash.ToLowerInvariant()
    $res.size = (Get-Item -LiteralPath $q).Length
    if ($w.entry -and $w.entry.sha256 -and $w.entry.sha256 -ne $res.sha256) { $res.reasons = @("hash differs from the manifest (the file changed)") }
  } catch {
    $x = $_.Exception
    while ($x.InnerException) { $x = $x.InnerException }
    $res.reasons = @("download: $($x.Message)")
  }
  "{0,4}/{1} {2,-60} {3}" -f $i, $work.Count, $w.id, $(if ($res.reasons.Count) { $res.reasons[0] } else { "$($res.size) bytes" })
  $results += [pscustomobject]$res
}

# 2. Defender scans the whole quarantine; if it finds anything, each folder again to see which.
"Defender: scanning $quarantine"
$scan = Test-Defender $quarantine
$defender = [ordered]@{ folder = $quarantine; exitCode = $scan.code; clean = $scan.clean; output = $scan.output }
if (-not $scan.clean) {
  foreach ($res in $results) {
    if (-not (Test-Path -LiteralPath $res.quarantine)) { if (-not $res.reasons.Count) { $res.reasons = @("removed by Defender") }; continue }
    $one = Test-Defender (Split-Path -Parent $res.quarantine)
    if (-not $one.clean) { $res.reasons = @($res.reasons) + "Defender: $($one.output)" }
  }
}
"Defender: exit code $($scan.code). $($scan.output)"

# 3-4. The content screen, then fwtool check.
$seen = @{}
if ($Candidates) { foreach ($f in $files) { $seen[$f.sha256] = $f.id } }  # a new URL for a file we have
foreach ($res in $results) {
  if ($res.reasons.Count -or -not (Test-Path -LiteralPath $res.quarantine)) { continue }
  if ($seen[$res.sha256]) { $res.reasons = @("same file as $($seen[$res.sha256])"); continue }
  $seen[$res.sha256] = $res.id
  $t = Test-Docx $res.quarantine
  $res.facts = $t.facts
  $res.reasons = @($t.reasons)
  if (-not $res.reasons.Count) { $fw = Test-Fwtool $res.quarantine; if ($fw) { $res.reasons = @($fw) } }
}

# 5. Passing files move into the corpus; the rest are deleted. New candidates (and, with -Rescreen,
# every file) get their facts and tags recorded in the manifest.
function Set-Facts($entry, $facts) {
  $values = [ordered]@{
    modified = $facts.modified
    producer = (@($facts.producer, $facts.appVersion) | Where-Object { $_ }) -join " "
    compatibilityMode = $facts.compatibilityMode
    savedPages = $facts.savedPages
    tags = $facts.tags
  }
  foreach ($k in $values.Keys) { $entry | Add-Member -NotePropertyName $k -NotePropertyValue $values[$k] -Force }
}
$byId = [ordered]@{}
foreach ($f in $files) { $byId[$f.id] = $f }
foreach ($res in $results) {
  $w = $work | Where-Object { $_.id -eq $res.id } | Select-Object -First 1
  $e = if ($w.entry) { $w.entry } else { $w.candidate }
  $dest = Join-Path $docxRoot "$($e.category)\$($res.id).docx"
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
  if ($Candidates) {
    $c = $e
    $e = [pscustomobject][ordered]@{
      id = $res.id
      file = "docx/$($c.category)/$($res.id).docx"
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
  if ($Candidates -or $Rescreen) { Set-Facts $e $res.facts }
}
Remove-Item -Recurse -Force $quarantine

# The run's results beside the corpus, and the updated manifest.
Write-PrettyJson ([ordered]@{
  date = $today
  mode = $(if ($Candidates) { "candidates" } elseif ($Rescreen) { "rescreen" } else { "fetch" })
  defender = $defender
  passed = @($results | Where-Object { -not $_.reasons.Count } | ForEach-Object { [ordered]@{ id = $_.id; sha256 = $_.sha256; facts = $_.facts } })
  rejected = @($results | Where-Object { $_.reasons.Count } | ForEach-Object { [ordered]@{ id = $_.id; url = $_.url; reasons = $_.reasons } })
}) (Join-Path $Out "screen.json")
if ($Candidates -or $Rescreen) {
  $manifestData.files = @($byId.Values | Sort-Object category, id)
  $manifestData | Add-Member -NotePropertyName rejected -NotePropertyValue @($rejected) -Force
  Write-PrettyJson $manifestData $Manifest
}
$passCount = @($results | Where-Object { -not $_.reasons.Count }).Count
"$passCount of $($results.Count) passed. Corpus: $docxRoot. Results: $(Join-Path $Out 'screen.json')"
