<#
.SYNOPSIS
  P3's hand-made decks (docs/p3-plan.md, "Reference PC"): PowerPoint 2021 builds, from a blank
  presentation, the decks the screened corpus has too few of, into <corpus>\made\:
  - transitions.pptx: one slide per transition, §8.2's common ones (fade, push, wipe, split, cover,
    uncover, zoom) and others PowerPoint plays (vortex, curtains, prestige, honeycomb, ripple...), with
    durations, advance on click or after a time, and a transition sound; and the same deck as a show
    (transitions-show.ppsx) and a template (transitions-template.potx), for the .pps and .pot copies;
  - animations.pptx: entrance, emphasis and exit effects (§8.2's common ones and others), triggers on
    click, with previous and after previous, durations, delays, directions, text by paragraph, motion
    paths, repeats, and an interactive trigger sequence;
  - comments.pptx: comments by two authors on two slides, one with a thread of replies;
  - media.pptx: an embedded video (made by PowerPoint itself: CreateVideo of a 3-slide deck, into
    made\media\clip.mp4), an embedded sound that plays on its own (a Windows sound), a linked video;
  - protected\animations.pptx: animations.pptx encrypted (agile) with the test password "featherweight".
  And in <corpus>\ppt\protected\: animations.ppt (made\) and one screened deck as .ppt, both
  encrypted by PowerPoint's .ppt writer (RC4 CryptoAPI) with the test password.
  Then scripts\make-deck-copies.ps1 -From made makes their .ppt/.pps/.pot and .odp copies, and
  scripts\reference-renders.ps1 -CorpusFolders made,ppt,odp their PDFs. made\manifest.json lists
  every file made, its SHA-256 and size, and what went wrong.

  Nothing is downloaded: the content is PowerPoint's own and Windows' sounds. PowerPoint runs as in
  scripts\make-deck-copies.ps1: only from Roy's active, unlocked session (exit 3), never with a
  PowerPoint this script didn't start or another job's FeatherweightNotes\quiet-pc.lock (exit 4), its
  own instance, hidden, macros forced off, alerts off, always quit or ended at the end.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make-deck-edge-cases.ps1 -Corpus D:\Documents\Codex\FeatherweightCorpus
#>
param(
  [string]$Corpus = $(if ($env:FW_CORPUS) { $env:FW_CORPUS } else { "D:\Documents\Codex\FeatherweightCorpus" }),
  # The screened deck (its path under pptx\) also saved as an encrypted .ppt.
  [string]$Encrypt = "lectures/edu-cmsc-201-computer-science-i-lecture-1.pptx",
  [int]$TimeoutSeconds = 900,
  # Internal: the child builds everything in one PowerPoint.
  [string]$Child, [string]$Made, [string]$PptOut, [string]$EncryptSource, [string]$EncryptRel, [string]$ResultFile
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$quietLock = "D:\Documents\Codex\FeatherweightNotes\quiet-pc.lock"
$testPassword = "featherweight"  # a test value (scripts\reference-renders.ps1 Get-TestPassword), not a secret

function Write-Json($value, [string]$path) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

if ($Child) {
  [void][Reflection.Assembly]::LoadWithPartialName("Microsoft.Office.Interop.PowerPoint")
  function E([string]$type, [string]$name) { [int][Enum]::Parse([type]"Microsoft.Office.Interop.PowerPoint.$type", $name) }
  $result = [ordered]@{ pid = @(); powerpoint = $null; files = @(); notes = @() }
  $pp = $null; $open = New-Object Collections.ArrayList
  function Add-File([string]$path, [string]$what) { $script:result.files += [pscustomobject]@{ path = $path; what = $what } }
  function Note([string]$s) { $script:result.notes += $s }
  function New-Deck { $d = $pp.Presentations.Add(0); [void]$open.Add($d); $d }  # no window
  function Close-Deck($d) { try { $d.Close() } catch {}; $open.Remove($d); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($d) }
  function Add-Slide($d, [string]$layout, [string]$title) {
    $s = $d.Slides.Add($d.Slides.Count + 1, (E PpSlideLayout $layout))
    if ($title -and $s.Shapes.HasTitle) { $s.Shapes.Title.TextFrame.TextRange.Text = $title }
    $s
  }
  function Add-Box($s, [double]$l, [double]$t, [double]$w, [double]$h, [string]$text, [int]$rgb) {
    $b = $s.Shapes.AddShape(1, $l, $t, $w, $h)  # msoShapeRectangle
    $b.Fill.ForeColor.RGB = $rgb
    $b.TextFrame.TextRange.Text = $text
    $b.TextFrame.TextRange.Font.Size = 14
    $b
  }
  try {
    if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { throw "a PowerPoint this script didn't start is running" }
    $since = Get-Date
    $pp = New-Object -ComObject PowerPoint.Application
    $new = @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $since.AddSeconds(-5) } | ForEach-Object Id)
    if (-not $new.Count) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp); $pp = $null; throw "COM attached to a PowerPoint this script didn't start; nothing was done to it" }
    $result.pid = $new
    Write-Json $result $ResultFile
    $pp.AutomationSecurity = 3  # macros never run
    $pp.DisplayAlerts = 1       # ppAlertsNone
    $result.powerpoint = "PowerPoint $($pp.Version) (build $($pp.Build))"
    New-Item -ItemType Directory -Force $Made, (Join-Path $Made "media"), (Join-Path $Made "protected") | Out-Null
    $pptx = E PpSaveAsFileType ppSaveAsOpenXMLPresentation
    $ppt = E PpSaveAsFileType ppSaveAsPresentation
    $onClick = E MsoAnimTriggerType msoAnimTriggerOnPageClick
    $withPrev = E MsoAnimTriggerType msoAnimTriggerWithPrevious
    $afterPrev = E MsoAnimTriggerType msoAnimTriggerAfterPrevious
    $sound = Get-ChildItem C:\Windows\Media -File -Filter "Windows Ding.wav" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $sound) { $sound = Get-ChildItem C:\Windows\Media -File -Filter *.wav | Sort-Object Length | Select-Object -First 1 }

    # ---- Transitions: one slide each. [effect, duration s, advance after s (0: on click only)]
    $d = New-Deck
    $transitions = @(
      @("ppEffectFadeSmoothly", 0.7, 0), @("ppEffectFade", 1.0, 0), @("ppEffectPushUp", 1.0, 0), @("ppEffectPushLeft", 0.5, 2),
      @("ppEffectWipeRight", 1.0, 0), @("ppEffectWipeDown", 2.0, 0), @("ppEffectSplitVerticalOut", 1.25, 0), @("ppEffectSplitHorizontalIn", 1.0, 3),
      @("ppEffectCoverLeft", 1.0, 0), @("ppEffectCoverRightDown", 0.75, 0), @("ppEffectUncoverUp", 1.0, 0), @("ppEffectUncoverLeftDown", 1.0, 0),
      @("ppEffectZoomIn", 0.5, 0), @("ppEffectZoomOut", 1.0, 0), @("ppEffectCut", 0, 0), @("ppEffectRandomBarsHorizontal", 1.0, 0),
      @("ppEffectVortexLeft", 3.0, 0), @("ppEffectCurtains", 2.0, 0), @("ppEffectPrestige", 2.0, 0), @("ppEffectHoneycomb", 3.0, 0),
      @("ppEffectRippleCenter", 1.4, 0), @("ppEffectDoorsVertical", 1.4, 0)
    )
    foreach ($t in $transitions) {
      $s = Add-Slide $d "ppLayoutText" "Transition: $($t[0] -replace '^ppEffect', '')"
      $s.Shapes.Item(2).TextFrame.TextRange.Text = "Duration $($t[1]) s`rAdvance: " + $(if ($t[2]) { "on click or after $($t[2]) s" } else { "on click" })
      $st = $s.SlideShowTransition
      $st.EntryEffect = (E PpEntryEffect $t[0])
      try { if ($t[1] -gt 0) { $st.Duration = $t[1] } } catch { Note "transitions: $($t[0]) takes no duration: $($_.Exception.Message)" }
      $st.AdvanceOnClick = -1
      if ($t[2]) { $st.AdvanceOnTime = -1; $st.AdvanceTime = $t[2] }
    }
    # Slide 2 plays a sound with its transition; slide 3 advances only after its time, not on click.
    try { [void]$d.Slides.Item(2).SlideShowTransition.SoundEffect.ImportFromFile($sound.FullName) } catch { Note "transitions: sound: $($_.Exception.Message)" }
    $d.Slides.Item(3).SlideShowTransition.AdvanceOnClick = 0
    $d.Slides.Item(3).SlideShowTransition.AdvanceOnTime = -1
    $d.Slides.Item(3).SlideShowTransition.AdvanceTime = 4
    $d.Slides.Item(3).Shapes.Item(2).TextFrame.TextRange.Text = "Duration 1 s`rAdvance: after 4 s only (not on click)"
    $f = Join-Path $Made "transitions.pptx"; $d.SaveAs($f, $pptx); Add-File $f "one slide per transition: §8.2's common ones and others; durations; advance on click or after a time; a transition sound (slide 2)"
    $f = Join-Path $Made "transitions-show.ppsx"; $d.SaveCopyAs($f, (E PpSaveAsFileType ppSaveAsOpenXMLShow)); Add-File $f "transitions.pptx as a show (for the .pps copy)"
    $f = Join-Path $Made "transitions-template.potx"; $d.SaveCopyAs($f, (E PpSaveAsFileType ppSaveAsOpenXMLTemplate)); Add-File $f "transitions.pptx as a template (for the .pot copy)"
    Close-Deck $d

    # ---- Animations
    $d = New-Deck
    function Add-Effect($s, $shape, [string]$effect, [int]$trigger, [double]$duration, [double]$delay, [string]$direction, [switch]$Exit, [int]$level = 0) {
      $e = $s.TimeLine.MainSequence.AddEffect($shape, (E MsoAnimEffect $effect), $level, $trigger)
      if ($Exit) { $e.Exit = -1 }
      if ($duration -gt 0) { $e.Timing.Duration = $duration }
      if ($delay -gt 0) { $e.Timing.TriggerDelayTime = $delay }
      if ($direction) { try { $e.EffectParameters.Direction = (E MsoAnimDirection $direction) } catch { Note "animations: $effect has no direction $direction" } }
      $e
    }
    # Slide 1: entrances, §8.2's common ones.
    $s = Add-Slide $d "ppLayoutTitleOnly" "Entrance effects"
    $entr = @(
      @("msoAnimEffectAppear", $onClick, 0, 0, ""), @("msoAnimEffectFade", $withPrev, 0.5, 0, ""), @("msoAnimEffectFly", $afterPrev, 1.0, 0.25, "msoAnimDirectionBottom"),
      @("msoAnimEffectWipe", $onClick, 0.75, 0, "msoAnimDirectionLeft"), @("msoAnimEffectZoom", $afterPrev, 0.5, 0, ""), @("msoAnimEffectFloat", $onClick, 1.0, 0, ""),
      @("msoAnimEffectSplit", $withPrev, 0.5, 1.0, ""), @("msoAnimEffectWheel", $onClick, 2.0, 0, ""), @("msoAnimEffectRiseUp", $afterPrev, 1.0, 0, "")
    )
    for ($i = 0; $i -lt $entr.Count; $i++) {
      $x = $entr[$i]
      $b = Add-Box $s (40 + ($i % 3) * 300) (130 + [Math]::Floor($i / 3) * 120) 260 90 ($x[0] -replace '^msoAnimEffect', '') (0xD08040 + $i * 0x101010)
      [void](Add-Effect $s $b $x[0] $x[1] $x[2] $x[3] $x[4])
    }
    # Slide 2: emphasis and exits.
    $s = Add-Slide $d "ppLayoutTitleOnly" "Emphasis and exit effects"
    $emph = @(@("msoAnimEffectGrowShrink", $onClick, 1.0), @("msoAnimEffectSpin", $afterPrev, 2.0), @("msoAnimEffectTransparency", $onClick, 0.5), @("msoAnimEffectTeeter", $withPrev, 1.0))
    for ($i = 0; $i -lt $emph.Count; $i++) {
      $b = Add-Box $s (40 + $i * 225) 140 200 90 ($emph[$i][0] -replace '^msoAnimEffect', '') 0x40A060
      [void](Add-Effect $s $b $emph[$i][0] $emph[$i][1] $emph[$i][2] 0 "")
    }
    $exits = @(@("msoAnimEffectFade", $onClick, 0.5, ""), @("msoAnimEffectFly", $afterPrev, 0.75, "msoAnimDirectionRight"), @("msoAnimEffectZoom", $onClick, 0.5, ""))
    for ($i = 0; $i -lt $exits.Count; $i++) {
      $b = Add-Box $s (40 + $i * 300) 300 260 90 ("Exit " + ($exits[$i][0] -replace '^msoAnimEffect', '')) 0x6060C0
      [void](Add-Effect $s $b $exits[$i][0] $exits[$i][1] $exits[$i][2] 0 $exits[$i][3] -Exit)
    }
    # Slide 3: text by paragraph.
    $s = Add-Slide $d "ppLayoutText" "Text by paragraph"
    $s.Shapes.Item(2).TextFrame.TextRange.Text = "First point`rSecond point`rThird point`rFourth point"
    [void](Add-Effect $s $s.Shapes.Item(2) "msoAnimEffectFade" $onClick 0.5 0 "" -level (E MsoAnimateByLevel msoAnimateTextByFirstLevel))
    # Slide 4: effects Featherweight plays as appear, motion paths, a repeat, an interactive trigger.
    $s = Add-Slide $d "ppLayoutTitleOnly" "Other effects, paths and a trigger"
    $other = @("msoAnimEffectBounce", "msoAnimEffectSwivel", "msoAnimEffectPinwheel", "msoAnimEffectPathCircle", "msoAnimEffectPathDown")
    for ($i = 0; $i -lt $other.Count; $i++) {
      $b = Add-Box $s (40 + $i * 180) 140 160 80 ($other[$i] -replace '^msoAnimEffect', '') 0x3090C0
      $e = Add-Effect $s $b $other[$i] $(if ($i -eq 0) { $onClick } else { $afterPrev }) 1.0 0 ""
      if ($other[$i] -eq "msoAnimEffectPathCircle") { $e.Timing.RepeatCount = 2 }
    }
    $trigger = Add-Box $s 40 330 200 70 "Click me" 0x2020A0
    $target = Add-Box $s 300 330 300 70 "Appears when 'Click me' is clicked" 0x20A0A0
    try {
      $iseq = $s.TimeLine.InteractiveSequences.Add()
      [void]$iseq.AddTriggerEffect($target, (E MsoAnimEffect msoAnimEffectFade), (E MsoAnimTriggerType msoAnimTriggerOnShapeClick), $trigger)
    } catch { Note "animations: interactive trigger: $($_.Exception.Message)" }
    # Slide 5: a transition and animations together.
    $s = Add-Slide $d "ppLayoutTitleOnly" "A transition and a chain of effects"
    $s.SlideShowTransition.EntryEffect = (E PpEntryEffect ppEffectPushUp)
    $s.SlideShowTransition.Duration = 0.8
    for ($i = 0; $i -lt 3; $i++) {
      $b = Add-Box $s (60 + $i * 280) 180 240 100 "Step $($i + 1)" 0xA04060
      [void](Add-Effect $s $b "msoAnimEffectFly" $(if ($i -eq 0) { $withPrev } else { $afterPrev }) 0.6 0.2 "msoAnimDirectionLeft")
    }
    $f = Join-Path $Made "animations.pptx"; $d.SaveAs($f, $pptx); Add-File $f "entrance, emphasis and exit effects; triggers; durations; delays; directions; text by paragraph; paths; a repeat; an interactive trigger; a transition with a chain"
    # Encrypted copies (the test password), then the password is taken off again.
    $d.Password = $testPassword
    $f = Join-Path $Made "protected\animations.pptx"; $d.SaveCopyAs($f, $pptx); Add-File $f "animations.pptx encrypted by PowerPoint (agile), password '$testPassword'"
    $f = Join-Path $PptOut "protected\made\animations.ppt"; New-Item -ItemType Directory -Force (Split-Path -Parent $f) | Out-Null
    $d.SaveCopyAs($f, $ppt); Add-File $f "animations.pptx as .ppt encrypted by PowerPoint's .ppt writer (RC4 CryptoAPI), password '$testPassword'"
    $d.Password = ""
    Close-Deck $d

    # ---- Comments
    $d = New-Deck
    $s1 = Add-Slide $d "ppLayoutText" "Comments on this slide"
    $s1.Shapes.Item(2).TextFrame.TextRange.Text = "A slide with one comment and a thread of two replies"
    $s2 = Add-Slide $d "ppLayoutText" "Two authors"
    $s2.Shapes.Item(2).TextFrame.TextRange.Text = "Two comments by different authors at different places"
    [void](Add-Slide $d "ppLayoutText" "No comments here")
    function Add-Comment($target, [double]$l, [double]$t, [string]$author, [string]$initials, [string]$text) {
      try { return $target.Add2($l, $t, $author, $initials, $text, "None", $author) } catch { $script:add2 = $_.Exception.Message }
      $target.Add($l, $t, $author, $initials, $text)
    }
    $c = Add-Comment $s1.Comments 120 90 "Ada Lovelace" "AL" "Is this title right?"
    try {
      [void](Add-Comment $c.Replies 0 0 "Charles Babbage" "CB" "Yes, it matches the outline.")
      [void](Add-Comment $c.Replies 0 0 "Ada Lovelace" "AL" "Thanks, resolved.")
    } catch { Note "comments: replies: $($_.Exception.Message)" }
    [void](Add-Comment $s2.Comments 40 200 "Ada Lovelace" "AL" "Left comment.")
    [void](Add-Comment $s2.Comments 600 300 "Grace Hopper" "GH" "Right comment, with ümlauts and € signs.")
    if ($script:add2) { Note "comments: Comments.Add2 failed ($script:add2); Comments.Add used" }
    $f = Join-Path $Made "comments.pptx"; $d.SaveAs($f, $pptx); Add-File $f "comments by three authors on two slides, a thread of two replies on slide 1"
    Close-Deck $d

    # ---- Media: a video PowerPoint makes, a Windows sound, a linked video.
    $clip = Join-Path $Made "media\clip.mp4"
    $d = New-Deck
    foreach ($i in 1..3) {
      $s = Add-Slide $d "ppLayoutBlank" ""
      $s.FollowMasterBackground = 0
      $s.Background.Fill.ForeColor.RGB = @(0x3050C0, 0x30A050, 0xC05030)[$i - 1]
      $b = $s.Shapes.AddTextbox(1, 300, 150, 360, 200); $b.TextFrame.TextRange.Text = "$i"; $b.TextFrame.TextRange.Font.Size = 120
    }
    if (Test-Path $clip) { Remove-Item $clip }
    # FileName, UseTimingsAndNarrations, DefaultSlideDuration, VertResolution, FramesPerSecond, Quality
    $d.CreateVideo($clip, 0, 1, 240, 15, 60)
    $deadline = (Get-Date).AddSeconds(240)
    while ((Get-Date) -lt $deadline -and @(1, 2) -contains [int]$d.CreateVideoStatus) { Start-Sleep -Seconds 1 }  # in progress, queued
    $status = [int]$d.CreateVideoStatus
    Close-Deck $d
    if ($status -ne (E PpMediaTaskStatus ppMediaTaskStatusDone) -or -not (Test-Path $clip)) { throw "CreateVideo didn't make the clip (status $status)" }
    Add-File $clip "a 3-second H.264 video PowerPoint made (CreateVideo, 240 lines, 15 fps), embedded and linked in media.pptx"
    $d = New-Deck
    $s = Add-Slide $d "ppLayoutTitleOnly" "Embedded video (plays on click)"
    [void]$s.Shapes.AddMediaObject2($clip, 0, -1, 200, 140, 427, 240)
    $s = Add-Slide $d "ppLayoutTitleOnly" "Embedded sound (plays on its own)"
    $a = $s.Shapes.AddMediaObject2($sound.FullName, 0, -1, 420, 220, 48, 48)
    try { $a.AnimationSettings.PlaySettings.PlayOnEntry = -1; $a.AnimationSettings.PlaySettings.HideWhileNotPlaying = -1 } catch { Note "media: autoplay: $($_.Exception.Message)" }
    $s = Add-Slide $d "ppLayoutTitleOnly" "Linked video (made\media\clip.mp4)"
    [void]$s.Shapes.AddMediaObject2($clip, -1, 0, 200, 140, 427, 240)
    $f = Join-Path $Made "media.pptx"; $d.SaveAs($f, $pptx); Add-File $f "an embedded video with its poster frame, an embedded sound that plays automatically ($($sound.Name)), a video linked to made\media\clip.mp4"
    Close-Deck $d

    # ---- A screened deck as an encrypted .ppt
    if ($EncryptSource) {
      $d = $pp.Presentations.Open($EncryptSource, -1, 0, 0); [void]$open.Add($d)
      $d.Password = $testPassword
      $f = Join-Path $PptOut ("protected\" + ($EncryptRel -replace '/', '\' -replace '\.pptx$', '.ppt'))
      New-Item -ItemType Directory -Force (Split-Path -Parent $f) | Out-Null
      $d.SaveCopyAs($f, $ppt); Add-File $f "pptx/$EncryptRel as .ppt encrypted by PowerPoint's .ppt writer (RC4 CryptoAPI), password '$testPassword'"
      Close-Deck $d
    }

    # ---- Each encrypted file opens with the test password and is what it should be.
    foreach ($x in @($result.files | Where-Object { $_.what -match 'encrypted' })) {
      $bytes = [IO.File]::ReadAllBytes($x.path)
      $cfb = $bytes.Length -gt 8 -and $bytes[0] -eq 0xD0 -and $bytes[1] -eq 0xCF -and $bytes[2] -eq 0x11 -and $bytes[3] -eq 0xE0
      $text = [Text.Encoding]::Unicode.GetString($bytes)
      $kind = if ($x.path -like "*.ppt") { if ($text.Contains("EncryptedSummary")) { "an EncryptedSummary stream (CryptoAPI)" } else { "NO EncryptedSummary stream" } }
              elseif ($text.Contains("EncryptionInfo") -and $text.Contains("EncryptedPackage")) { "EncryptionInfo and EncryptedPackage streams" } else { "NO EncryptionInfo stream" }
      $d = $pp.Presentations.Open($x.path + "::" + $testPassword + "::", -1, 0, 0); [void]$open.Add($d)
      Note ("{0}: compound file {1}, {2}; opens with the test password: {3} slides" -f (Split-Path -Leaf $x.path), $cfb, $kind, $d.Slides.Count)
      Close-Deck $d
    }
  } catch {
    $result.error = $_.Exception.Message + " (line " + $_.InvocationInfo.ScriptLineNumber + ")"
  } finally {
    if ($pp) {
      foreach ($d in @($open)) { try { $d.Close() } catch {} }
      $others = 0
      try { $others = [int]$pp.Presentations.Count } catch {}
      if ($others -gt 0) { $result.error = "someone opened a presentation in the PowerPoint this script started; it's left running"; $result.pid = @() }
      else { try { $pp.Quit() } catch {} }
      try { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pp) } catch {}
      $pp = $null
      [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
  }
  Write-Json $result $ResultFile
  exit 0
}

# ---- Parent
Set-Location $root
$corpusRoot = (Resolve-Path $Corpus).Path.TrimEnd('\')
if ($corpusRoot.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "$corpusRoot is inside the repository; the private corpus stays outside it." }
$made = Join-Path $corpusRoot "made"
$pptOut = Join-Path $corpusRoot "ppt"
$encSource = $null
if ($Encrypt) {
  $encSource = Join-Path $corpusRoot ("pptx\" + ($Encrypt -replace '/', '\'))
  $sha = (Get-FileHash -LiteralPath $encSource -Algorithm SHA256).Hash.ToLower()
  $screened = @((Get-Content (Join-Path $root "tests\office\real-world.json") -Raw -Encoding UTF8 | ConvertFrom-Json).files | Where-Object { $_.sha256 -eq $sha })
  if (-not $screened.Count) { throw "pptx/$Encrypt isn't a deck the screen passed (its hash isn't in tests\office\real-world.json)." }
}
$me = @(qwinsta 2>$null | Where-Object { $_ -match '^>' }) | Select-Object -First 1
if (-not $me -or $me -notmatch '^>(console|rdp-tcp#\d+)\s' -or $me -notmatch '\sActive\s*$') { "STOPPED: no active session"; exit 3 }
$sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
if (@(Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sid }).Count) { "STOPPED: the session is locked"; exit 3 }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fw-deck-edge-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$resultPath = Join-Path $tmp "result.json"
$mutex = New-Object Threading.Mutex($false, "Global\FeatherweightOfficeAutomation")
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] {}
try {
  if (@(Get-Process POWERPNT -ErrorAction SilentlyContinue).Count) { "STOPPED: a PowerPoint this script didn't start is running"; exit 4 }
  if ((Test-Path $quietLock) -and -not $env:FW_HOLDS_QUIET_PC_LOCK) { "STOPPED: another job holds FeatherweightNotes\quiet-pc.lock"; exit 4 }
  $args_ = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Child", "1", "-Made", "`"$made`"", "-PptOut", "`"$pptOut`"",
             "-ResultFile", "`"$resultPath`"")
  if ($encSource) { $args_ += @("-EncryptSource", "`"$encSource`"", "-EncryptRel", "`"$Encrypt`"") }
  $p = Start-Process -FilePath "powershell.exe" -PassThru -WindowStyle Hidden -ArgumentList $args_
  $finished = $p.WaitForExit($TimeoutSeconds * 1000)
  if (-not $finished) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
  foreach ($id in @(if ($r) { $r.pid })) {
    $x = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($x -and $x.ProcessName -eq "POWERPNT" -and -not $x.WaitForExit($(if ($finished) { 15000 } else { 0 }))) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
  }
} finally {
  $mutex.ReleaseMutex()
}
if (-not $finished) { $r = [pscustomobject]@{ error = "timeout after $TimeoutSeconds s"; files = @(); notes = @() } }
$files = @(foreach ($x in @($r.files)) {
  $rel = $x.path.Substring($corpusRoot.Length + 1) -replace '\\', '/'
  if (Test-Path -LiteralPath $x.path) {
    [ordered]@{ file = $rel; what = $x.what; sha256 = (Get-FileHash -LiteralPath $x.path -Algorithm SHA256).Hash.ToLower(); bytes = (Get-Item -LiteralPath $x.path).Length }
  } else { [ordered]@{ file = $rel; what = $x.what; error = "not written" } }
})
New-Item -ItemType Directory -Force $made | Out-Null
Write-Json ([ordered]@{
  made = (Get-Date).ToString("yyyy-MM-dd HH:mm")
  powerpoint = $r.powerpoint
  note = "Decks PowerPoint built from a blank presentation (scripts\make-deck-edge-cases.ps1), and the encrypted copies; test password '$testPassword'. No downloads: PowerPoint's own content and Windows' sounds."
  error = $r.error
  notes = @($r.notes)
  files = $files
}) (Join-Path $made "manifest.json")
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
$files | ForEach-Object { "{0}: {1}" -f $_.file, $(if ($_.error) { "FAILED ($($_.error))" } else { "$($_.bytes) bytes" }) }
@($r.notes) | ForEach-Object { "note: $_" }
if ($r.error) { "FAILED: $($r.error)"; exit 1 }
"made $($files.Count) files; made\manifest.json"
