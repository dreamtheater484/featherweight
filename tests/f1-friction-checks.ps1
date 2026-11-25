<#
.SYNOPSIS
  F1's friction flows (docs/friction-review.md, "F1 (release 4.1)") run through Featherweight's --script
  mode as checks, the way tests\friction-checks.ps1 runs D5's: RF2 and RF7 within Word's steps, and the
  new flows (merge, sign, OCR, language, share) within Acrobat's.

.DESCRIPTION
  Each flow is its steps as "script step|count" (C a click, K a key, G a drag, D a decision; typing
  the content isn't counted), a budget (the target's clicks, keys and drags, and its decisions), and
  a check of what it made (fwtool on the saved file). A flow whose steps aren't written yet is "not
  built" and names the piece whose UI it waits for: the steps are written when that UI lands, with
  its coordinates for the flow's window size.
  -Flows picks some (RF2,RF7,merge,sign,certificate,ocr,language,share). Writes <Out>\summary.json:
  { pass, flows: [{ id, owner, built, steps, decisions, target, ok, why }] }. Exit 1 when one fails
  or isn't built.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\f1-friction-checks.ps1 -Flows RF2,RF7
#>
param(
  [string]$Exe = "build\Release\Featherweight.exe",
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Out = "build\f1-friction",
  [string[]]$Flows = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$enc = New-Object Text.UTF8Encoding $false
$Flows = @($Flows | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# The flows. `size` is the client area the coordinates are for. `open` is the file it starts with (a
# copy), `out` the file it saves ({out} in a step). `verify` gets the saved file and returns problems.
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Read-ZipText([string]$zip, [string]$entry) {
  $z = [IO.Compression.ZipFile]::OpenRead($zip)
  try {
    $e = $z.GetEntry($entry)
    if (-not $e) { return "" }
    $r = New-Object IO.StreamReader($e.Open())
    try { $r.ReadToEnd() } finally { $r.Dispose() }
  } finally { $z.Dispose() }
}
function Test-Checks([string]$f) { $v = & $Fwtool check $f 2>&1; if ($LASTEXITCODE) { "fwtool check: $(@($v)[-1])" } }

$all = @(
  # RF2, D5's flow 8 (docs/friction-review.md): the contract opens with the Review pane (f1-word, DocView.cpp).
  # Next, Accept (it moves on), Reject, the open thread's card (the box takes the keyboard), the reply, Enter;
  # its card and Resolve (the contract's other thread is resolved already), Ctrl+S. At 1366x768 the card list
  # starts at the pane's foot: the first card's head shows at y 728.
  @{ id = "RF2"; owner = "f1-word"; target = "Word: 7 C + 2 K"; budget = 9; decisions = 0; size = "1366x768"
     open = "tests\office\torture\review-contract.docx"; out = "rf2.docx"
     steps = @("click:1169,381|1C", "click:1242,381|1C", "click:1317,381|1C", "click:1200,728|1C", "type:Thanks, confirmed.|", "key:enter|1K",
               "click:1200,728|1C", "click:1205,655|1C", "save:{out}|1K")
     verify = { param($f)
       Test-Checks $f
       if ((Read-ZipText $f "word/comments.xml") -notmatch 'Thanks, confirmed\.') { "the reply isn't in comments.xml" }
       if (([regex]::Matches((Read-ZipText $f "word/commentsExtended.xml"), 'w15:done="1"')).Count -lt 2) { "the open thread isn't resolved" }
       $marks = ([regex]::Matches((Read-ZipText $f "word/document.xml"), '<w:(ins|del)\b')).Count
       if ($marks -gt 4) { "$marks change marks left of 6: Accept and Reject didn't both take one" } } },
  # RF7: a new document at 1366 px; the gallery's first row is Normal, No Spacing, Heading 1, Heading 2, Title
  # (f1-word). Making the document and saving it are the check's, not the flow's.
  @{ id = "RF7"; owner = "f1-word"; target = "Word: 1 C"; budget = 1; decisions = 0; size = "1366x768"; out = "rf7.docx"
     steps = @("new|", "click:1086,110|1C", "type:Jane Doe|", "save:{out}|")
     verify = { param($f)
       Test-Checks $f
       if (-not (@(& $Fwtool dump --model $f 2>&1 | Where-Object { "$_" -match '^\s*Title\s+\S+pt \| Jane Doe$' }).Count)) { "no paragraph in the Title style" } } },
  @{ id = "merge"; owner = "f1-pdf"; target = "Acrobat: 6 C + 1 K, 2 D"; budget = 7; decisions = 2; size = "1072x686"
     open = "build\pdf-corpus\plain-text.pdf"; out = "merged.pdf"
     # f1-pdf: Pages, the last page's thumbnail, Insert (+) and B in the Open dialog (B, Open), Ctrl+S.
     steps = @("click:171,64|1C", "click:835,485|1C", "pdftool:insert:$root\build\pdf-corpus\images.pdf|3|3C", "|1D", "save:{out}|1K")
     verify = { param($f) $v = & $Fwtool pdf-valid $f 2>&1; if ($LASTEXITCODE) { "pdf-valid: $($v -join ' ')" } } },
  @{ id = "sign"; owner = "f1-pdf"; target = "Acrobat: 5 C + 1 drag + 1 K, 1 D"; budget = 7; decisions = 1; size = "1072x686"
     open = "build\pdf-corpus\plain-text.pdf"; out = "signed-drawn.pdf"
     # f1-pdf: Sign ▾, Draw signature…, the drawing (pdftool:stamp draws and places it), Place on a
     # page and the click on the page (where: 1 D), Ctrl+S.
     steps = @("cmd:4071|2C", "pdftool:stamp|1G", "|2C", "|1D", "save:{out}|1K")
     verify = { param($f) $v = & $Fwtool pdf-valid $f 2>&1; if ($LASTEXITCODE) { "pdf-valid: $($v -join ' ')" } } },
  @{ id = "certificate"; owner = "f1-pdf"; target = "Acrobat: 6 C + 1 drag, 2 D"; budget = 7; decisions = 2; size = "1072x686"
     open = "build\pdf-corpus\plain-text.pdf"; out = "signed-certificate.pdf"
     # f1-pdf: Sign ▾, Sign with a certificate…, the certificate in its menu (1 D), Enter in the Save
     # dialog with "<name> (signed)" suggested (1 D). The test PFX stands in for the store's certificate.
     steps = @("pdftool:sign:$root\tests\pdfs\signing\test-signer.pfx|featherweight|{out}|3C", "|1K", "|2D")
     verify = { param($f) $j = Join-Path $Out "cert.json"; & $Fwtool pdf-valid --json $j $f | Out-Null; if (-not @((Get-Content $j -Raw | ConvertFrom-Json).files[0].signatures).Count) { "no signature" } } },
  @{ id = "ocr"; owner = "f1-pdf"; target = "Acrobat: 5 C + 1 K"; budget = 6; decisions = 0; size = "1072x686"
     open = "build\pdf-ocr\scans\scan-01.pdf"; out = "ocr.pdf"
     # f1-pdf: Recognize text on the toolbar (pdftool:ocr runs it to the end), Ctrl+S.
     steps = @("pdftool:ocr|1C", "save:{out}|1K")
     verify = { param($f) $t = Join-Path $Out "ocr-text"; & $Fwtool pdf-valid --out $t $f | Out-Null; if ((Get-Content (Join-Path $t "ocr.txt") -Raw) -notmatch '\w{4,}') { "no text layer" } } },
  # f1-lang: File, Options, the language (1 D), then close and start again (2 C, counted, not run: a snapshot
  # run doesn't save its settings). The check starts it as the saved setting would, with --lang de, and looks
  # for German in command search.
  @{ id = "language"; owner = "f1-lang"; target = "Acrobat: 8 C, 1 D"; budget = 8; decisions = 1; size = "1072x686"; out = $null
     steps = @("new|", "click:27,55|1C", "click:48,655|1C", "click:309,460|1C", "|1D", "|2C")
     verify = { param($f)
       $err = Join-Path $Out "language-de.err"
       $p = Start-Process -FilePath $Exe -ArgumentList @("--lang", "de", "--snapshot", "`"$(Join-Path $Out 'language-de.png')`"", "--size", (Get-WindowSize "1072x686"),
         "--theme", "light", "--script", "`"new;dumpcommands:Drucken`"") -Wait -PassThru -NoNewWindow -RedirectStandardError $err
       if ($p.ExitCode -or -not (Select-String -Path $err -Pattern '^commands\[Drucken\]: Drucken\|' -Quiet)) { "German doesn't load at the next start (--lang de: no Drucken in command search)" } } },
  @{ id = "share"; owner = "f1-pdf"; target = "Word: 4 C (to the share sheet)"; budget = 4; decisions = 0; size = "1072x686"
     open = "tests\office\generated\simple.docx"; out = $null
     # f1-pdf: File, Share, Share as PDF. The last click opens Windows' share sheet, which a snapshot
     # run doesn't show (it's counted, not run); the PDF is made as Export PDF makes it.
     steps = @("click:27,55|1C", "click:41,373|1C", "|1C")
     verify = { param($f) @() } }
)
$selected = @($all | Where-Object { -not $Flows.Count -or $Flows -contains $_.id })

# The window for a client area: its frame depends on the display's scale, so a probe measures it.
Add-Type -AssemblyName System.Drawing
$frames = @{}
function Get-WindowSize([string]$client) {
  if ($frames.ContainsKey($client)) { return $frames[$client] }
  $w, $h = $client -split 'x' | ForEach-Object { [int]$_ }
  $probe = Join-Path $Out "probe.png"
  Start-Process -FilePath $Exe -ArgumentList @("--snapshot", "`"$probe`"", "--size", "$($w + 28)x$($h + 14)", "--theme", "light") -Wait -NoNewWindow
  $img = [Drawing.Image]::FromFile($probe)
  $size = "{0}x{1}" -f ($w + 28 + $w - $img.Width), ($h + 14 + $h - $img.Height)
  $img.Dispose(); Remove-Item $probe
  $frames[$client] = $size
  $size
}

$rows = @()
foreach ($f in $selected) {
  $row = [ordered]@{ id = $f.id; owner = $f.owner; built = $f.steps.Count -gt 0; steps = $null; decisions = $null; target = $f.target; ok = $false; why = "" }
  if (-not $row.built) { $row.why = "not built yet ($($f.owner)): its steps are written when the UI lands"; $rows += [pscustomobject]$row; continue }
  $script = @(); $clicks = 0; $keys = 0; $drags = 0; $dec = 0
  $outFile = if ($f.out) { Join-Path $Out $f.out } else { $null }
  if ($outFile) { Remove-Item $outFile -ErrorAction SilentlyContinue }
  foreach ($s in $f.steps) {
    $bar = $s.LastIndexOf('|')
    $step = $s.Substring(0, $bar).Replace("{out}", "$outFile")
    switch -regex ($s.Substring($bar + 1)) {
      '^(\d+)C$' { $clicks += [int]$Matches[1] } '^(\d+)K$' { $keys += [int]$Matches[1] }
      '^(\d+)G$' { $drags += [int]$Matches[1] } '^(\d+)D$' { $dec += [int]$Matches[1] }
    }
    if ($step) { $script += $step }
  }
  $files = @()
  if ($f.open) { $copy = Join-Path $Out (Split-Path -Leaf $f.open); Copy-Item (Join-Path $root $f.open) $copy -Force; $files = @("`"$copy`"") }
  $scriptFile = Join-Path $Out "$($f.id).script"
  [IO.File]::WriteAllText($scriptFile, ($script -join ";"), $enc)
  $p = Start-Process -FilePath $Exe -ArgumentList ($files + @("--snapshot", "`"$(Join-Path $Out "$($f.id).png")`"", "--size", (Get-WindowSize $f.size),
    "--theme", "light", "--script", "`"@$scriptFile`"")) -Wait -PassThru -NoNewWindow
  $problems = @()
  if ($p.ExitCode -ne 0) { $problems += "Featherweight exited with $($p.ExitCode)" }
  $row.steps = $clicks + $keys + $drags; $row.decisions = $dec
  if ($row.steps -gt $f.budget) { $problems += "$($row.steps) steps, more than $($f.budget)" }
  if ($dec -gt $f.decisions) { $problems += "$dec decisions, more than $($f.decisions)" }
  if ($outFile -and -not (Test-Path $outFile)) { $problems += "nothing was saved" }
  else { $problems += @(& $f.verify $outFile | Where-Object { $_ }) }  # no file: the flow's own check
  $row.ok = -not $problems.Count
  $row.why = $problems -join "; "
  $rows += [pscustomobject]$row
}
$pass = -not @($rows | Where-Object { -not $_.ok }).Count
[IO.File]::WriteAllText((Join-Path $Out "summary.json"), (ConvertTo-Json ([ordered]@{ pass = $pass; flows = $rows }) -Depth 4), $enc)
foreach ($r in $rows) { "{0,-12} {1,-5} {2}" -f $r.id, $(if ($r.ok) { "ok" } elseif ($r.built) { "FAIL" } else { "-" }), $(if ($r.built) { "$($r.steps) steps, $($r.decisions) decisions (target $($r.target))$(if ($r.why) { ': ' + $r.why })" } else { $r.why }) }
if ($pass) { exit 0 } else { exit 1 }
