# Builds the slide show's test decks with PowerPoint (P1, p1-show): every transition and animation the
# show plays, hyperlinks and actions, and a 50-slide deck for the frame-rate bench.
#
#   powershell -NoProfile -File tests\office\show\make_show_decks.ps1 [-OutDir <folder>]
#
# PowerPoint runs as a single instance, so this refuses to run while PowerPoint is open (it would
# attach to that window), and it quits only the PowerPoint it started, with alerts off.
param([string]$OutDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'

if (Get-Process POWERPNT -ErrorAction SilentlyContinue) {
  throw "PowerPoint is open: close it first (this script never automates a PowerPoint someone is using)."
}
$OutDir = (Resolve-Path $OutDir).Path
$app = New-Object -ComObject PowerPoint.Application
$app.DisplayAlerts = 1  # ppAlertsNone
$true_ = -1; $false_ = 0

$made = New-Object System.Collections.ArrayList
function New-Deck {
  $p = $app.Presentations.Add($false_)  # no window
  [void]$made.Add($p)
  # PowerPoint's default size: widescreen 16:9, 13.33 × 7.5 in (960 × 540 pt)
  return $p
}

function Add-Slide($pres, [int]$layout, [string]$title) {
  $l = $pres.SlideMaster.CustomLayouts.Item($layout)
  $s = $pres.Slides.AddSlide($pres.Slides.Count + 1, $l)
  if ($title -and $s.Shapes.HasTitle) { $s.Shapes.Title.TextFrame.TextRange.Text = $title }
  return $s
}

# A filled rectangle with its name as text; colors are 0xBBGGRR.
function Add-Box($slide, [double]$x, [double]$y, [double]$w, [double]$h, [string]$text, [int]$rgb = 0xC07030) {
  $b = $slide.Shapes.AddShape(1, $x, $y, $w, $h)
  $b.Fill.ForeColor.RGB = $rgb
  $b.Line.Visible = $false_
  $b.TextFrame.TextRange.Text = $text
  $b.TextFrame.TextRange.Font.Size = 18
  $b.Name = $text
  return $b
}

function Add-Effect($slide, $shape, [int]$effect, [int]$trigger = 1, [double]$duration = 0.5, [int]$direction = 0,
                    [double]$delay = 0, [bool]$exit = $false, [int]$level = 0) {
  $e = $slide.TimeLine.MainSequence.AddEffect($shape, $effect, $level, $trigger, -1)
  if ($direction) { $e.EffectParameters.Direction = $direction }
  if ($exit) { $e.Exit = $true_ }
  $e.Timing.Duration = $duration
  if ($delay) { $e.Timing.TriggerDelayTime = $delay }
  return $e
}

function Save-Deck($pres, [string]$name, [int]$format = 24) {
  $path = Join-Path $OutDir $name
  if (Test-Path $path) { Remove-Item $path }
  $pres.SaveAs($path, $format)
  Write-Host "  $name"
}

try {
  # ---- Transitions: one slide each, with its name and a few shapes to see it move ----
  $p = New-Deck
  $list = @(
    @('Fade (smoothly)', 3849), @('Fade (ppEffectFade)', 1793), @('Cut through black', 258),
    @('Push down', 3852), @('Push left', 3853), @('Push right', 3854), @('Push up', 3855),
    @('Wipe left', 2817), @('Wipe up', 2818), @('Wipe right', 2819), @('Wipe down', 2820),
    @('Split horizontal out', 3585), @('Split horizontal in', 3586), @('Split vertical out', 3587), @('Split vertical in', 3588),
    @('Cover left', 1281), @('Cover up', 1282), @('Cover right', 1283), @('Cover down', 1284), @('Cover right-down', 1288),
    @('Uncover left', 2049), @('Uncover up', 2050), @('Uncover right', 2051), @('Uncover down', 2052), @('Uncover left-up', 2053),
    # Box in and out are what PowerPoint writes as <p:zoom> (its ppEffectZoom* values aren't transitions).
    @('Zoom in', 3074), @('Zoom out', 3073),
    @('Vortex (plays as a fade)', 3863), @('Ripple (plays as a fade)', 3867), @('Gallery (plays as a fade)', 3880),
    @('Dissolve (plays as a fade)', 1537), @('Cut', 257))
  $i = 0
  foreach ($t in $list) {
    $s = Add-Slide $p 2 $t[0]
    $s.Shapes.Item(2).TextFrame.TextRange.Text = "Slide $($i + 1) of $($list.Count + 2)`rThe transition into this slide: $($t[0])"
    [void](Add-Box $s 700 330 200 120 'A' (0x3050C0 + 0x10 * $i))
    [void](Add-Box $s 760 400 160 90 'B' 0x40A040)
    $s.SlideShowTransition.EntryEffect = $t[1]
    $s.SlideShowTransition.Duration = 1.0
    $i++
  }
  $s = Add-Slide $p 2 'Slow push (2 s), advances after 3 s'
  $s.SlideShowTransition.EntryEffect = 3855
  $s.SlideShowTransition.Duration = 2.0
  $s.SlideShowTransition.AdvanceOnTime = $true_
  $s.SlideShowTransition.AdvanceTime = 3
  $s.SlideShowTransition.AdvanceOnClick = $false_
  $s = Add-Slide $p 1 'The end'
  $s.SlideShowTransition.EntryEffect = 3849
  Save-Deck $p 'transitions.pptx'
  $p.Close()

  # ---- Animations ----
  $p = New-Deck
  $s = Add-Slide $p 6 'Entrance: appear, fade, fly in (on click)'
  [void](Add-Effect $s (Add-Box $s 60 150 250 150 'Appear') 1 1 0.5)
  [void](Add-Effect $s (Add-Box $s 360 150 250 150 'Fade') 10 1 1.0)
  [void](Add-Effect $s (Add-Box $s 660 150 250 150 'Fly in from bottom') 2 1 0.5 11)

  $s = Add-Slide $p 6 'Fly in from each side (one click, with previous)'
  [void](Add-Effect $s (Add-Box $s 60 200 200 120 'From left') 2 1 0.75 4)
  [void](Add-Effect $s (Add-Box $s 290 200 200 120 'From right') 2 2 0.75 2)
  [void](Add-Effect $s (Add-Box $s 520 200 200 120 'From top') 2 2 0.75 10)
  [void](Add-Effect $s (Add-Box $s 750 200 200 120 'From top-left') 2 2 0.75 12)

  $s = Add-Slide $p 6 'Wipe, split, zoom'
  [void](Add-Effect $s (Add-Box $s 60 150 250 200 'Wipe from bottom') 22 1 1.0 11)
  [void](Add-Effect $s (Add-Box $s 360 150 250 200 'Wipe from left') 22 1 1.0 4)
  [void](Add-Effect $s (Add-Box $s 660 150 250 200 'Split vertical out') 16 1 1.0 26)
  [void](Add-Effect $s (Add-Box $s 360 380 250 120 'Zoom') 23 1 1.0)

  $s = Add-Slide $p 6 'Float in, faded zoom; grow and turn plays as appear'
  [void](Add-Effect $s (Add-Box $s 60 200 250 150 'Float in (ascend)') 39 1 1.0)
  [void](Add-Effect $s (Add-Box $s 360 200 250 150 'Faded zoom') 48 1 1.0)
  [void](Add-Effect $s (Add-Box $s 660 200 250 150 'Grow and turn') 31 1 1.0)
  [void](Add-Effect $s (Add-Box $s 360 400 250 100 'Float out (descend)') 42 1 1.0)

  $s = Add-Slide $p 6 'Emphasis: grow/shrink, spin; teeter does nothing'
  [void](Add-Effect $s (Add-Box $s 100 200 220 150 'Grow 150%') 59 1 2.0)
  [void](Add-Effect $s (Add-Box $s 400 200 220 150 'Spin') 61 1 2.0)
  [void](Add-Effect $s (Add-Box $s 700 200 220 150 'Teeter') 80 1 1.0)

  $s = Add-Slide $p 6 'Exit: fade, fly out, wipe'
  [void](Add-Effect $s (Add-Box $s 60 200 250 150 'Fade out') 10 1 1.0 0 0 $true)
  [void](Add-Effect $s (Add-Box $s 360 200 250 150 'Fly out to right') 2 1 0.75 2 0 $true)
  [void](Add-Effect $s (Add-Box $s 660 200 250 150 'Wipe out to top') 22 1 1.0 10 0 $true)

  $s = Add-Slide $p 2 'By paragraph: fade, then wipe'
  $s.Shapes.Item(2).TextFrame.TextRange.Text = "First paragraph`rSecond paragraph`rThird paragraph`rFourth paragraph"
  [void](Add-Effect $s $s.Shapes.Item(2) 10 1 0.75 0 0 $false 2)

  $s = Add-Slide $p 6 'With previous, after previous, delays'
  [void](Add-Effect $s (Add-Box $s 60 200 200 120 'A: click, fade 1 s') 10 1 1.0)
  [void](Add-Effect $s (Add-Box $s 290 200 200 120 'B: with A, delay 0.5 s') 10 2 1.0 0 0.5)
  [void](Add-Effect $s (Add-Box $s 520 200 200 120 'C: after B') 2 3 0.5 11)
  [void](Add-Effect $s (Add-Box $s 750 200 200 120 'D: after C, delay 1 s') 22 3 0.5 4 1.0)

  $s = Add-Slide $p 6 'Starts by itself (after previous)'
  [void](Add-Effect $s (Add-Box $s 200 200 250 150 'After the slide, 1 s') 10 3 1.0 0 1.0)
  [void](Add-Effect $s (Add-Box $s 500 200 250 150 'Then on click') 2 1 0.5 11)

  $s = Add-Slide $p 6 'Trigger: click the button'
  $button = Add-Box $s 60 400 200 80 'Button' 0x2080E0
  $target = Add-Box $s 400 200 300 150 'Shown by the button'
  [void]$s.TimeLine.InteractiveSequences.Add().AddTriggerEffect($target, 10, 4, $button)

  $s = Add-Slide $p 6 'Motion paths'
  [void](Add-Effect $s (Add-Box $s 60 150 150 100 'Right') 149 1 2.0)
  [void](Add-Effect $s (Add-Box $s 400 250 150 100 'Circle') 86 1 2.0)
  [void](Add-Effect $s (Add-Box $s 700 150 150 100 'Arc down') 122 1 2.0)

  $s = Add-Slide $p 6 'Repeat twice, auto-reverse'
  $e = Add-Effect $s (Add-Box $s 350 200 250 150 'Spin back and forth') 61 1 1.0
  $e.Timing.RepeatCount = 2
  $e.Timing.AutoReverse = $true_

  $s = Add-Slide $p 6 'Others play as appear: wheel, blinds, checkerboard'
  [void](Add-Effect $s (Add-Box $s 60 200 250 150 'Wheel') 21 1 1.0)
  [void](Add-Effect $s (Add-Box $s 360 200 250 150 'Blinds') 3 1 1.0)
  [void](Add-Effect $s (Add-Box $s 660 200 250 150 'Checkerboard') 5 1 1.0)
  $s = Add-Slide $p 1 'The end'
  Save-Deck $p 'animations.pptx'
  $p.Close()

  # ---- Hyperlinks and actions ----
  $p = New-Deck
  $s1 = Add-Slide $p 6 'Links: 1'
  $s2 = Add-Slide $p 6 'Links: 2'
  $s3 = Add-Slide $p 6 'Links: 3 (a link goes here)'
  $s4 = Add-Slide $p 6 'Links: 4 (hidden, skipped)'
  $s4.SlideShowTransition.Hidden = $true_
  $s5 = Add-Slide $p 6 'Links: 5 (the last)'
  $acts = @(@('Next slide', 1), @('Previous slide', 2), @('First slide', 3), @('Last slide', 4), @('End show', 6))
  $x = 40
  foreach ($a in $acts) {
    $b = Add-Box $s1 $x 150 160 70 $a[0] 0x2080E0
    $b.ActionSettings.Item(1).Action = $a[1]
    $x += 175
  }
  $b = Add-Box $s1 40 260 200 70 'Slide 3' 0x40A040
  $b.ActionSettings.Item(1).Action = 7
  $b.ActionSettings.Item(1).Hyperlink.SubAddress = "$($s3.SlideID),$($s3.SlideIndex),Links: 3"
  $b = Add-Box $s1 260 260 200 70 'example.com' 0x40A040
  $b.ActionSettings.Item(1).Hyperlink.Address = 'https://example.com/'
  $b = Add-Box $s1 480 260 200 70 'A file' 0x40A040
  $b.ActionSettings.Item(1).Hyperlink.Address = 'transitions.pptx'
  $tb = $s1.Shapes.AddTextbox(1, 40, 380, 600, 40)
  $tb.TextFrame.TextRange.Text = 'A link in text: example.org, and on.'
  $tb.TextFrame.TextRange.Characters(18, 11).ActionSettings.Item(1).Hyperlink.Address = 'https://example.org/'
  foreach ($s in @($s2, $s3, $s5)) {
    $b = Add-Box $s 40 150 160 70 'Previous slide' 0x2080E0
    $b.ActionSettings.Item(1).Action = 2
    $b = Add-Box $s 220 150 160 70 'First slide' 0x2080E0
    $b.ActionSettings.Item(1).Action = 3
  }
  Save-Deck $p 'links.pptx'
  Save-Deck $p 'links.ppsx' 28
  $p.Close()

  # ---- 50 slides for the frame-rate bench: a transition into every slide ----
  $p = New-Deck
  $cycle = @(3849, 3853, 2820, 3587, 1283, 2050, 3074)
  for ($n = 1; $n -le 50; $n++) {
    $s = Add-Slide $p 2 "Bench slide $n"
    $s.Shapes.Item(2).TextFrame.TextRange.Text = "A paragraph of body text on slide $n, long enough to wrap onto a second line in the placeholder.`rSecond point`rThird point with more words"
    $b = Add-Box $s 650 300 260 180 "Shape $n" (0x204060 + 0x0303 * $n)
    $b.Fill.TwoColorGradient(1, 1)
    $b.Fill.ForeColor.RGB = 0xE0A040
    $b.Fill.BackColor.RGB = 0x603010
    $b.Shadow.Visible = $true_
    $o = $s.Shapes.AddShape(9, 80, 400, 120, 120)  # oval
    $o.Fill.ForeColor.RGB = 0x3080F0
    $s.SlideShowTransition.EntryEffect = $cycle[($n - 1) % $cycle.Count]
    $s.SlideShowTransition.Duration = 1.0
  }
  Save-Deck $p 'bench50.pptx'
  $p.Close()
} finally {
  # Close what this script made (after an error too), unsaved.
  foreach ($d in $made) { try { $d.Saved = $true_; $d.Close() } catch {} }
  # Quit only the PowerPoint this script started, and only if nothing else is open in it.
  if ($app.Presentations.Count -eq 0) { $app.Quit() }
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($app)
}
