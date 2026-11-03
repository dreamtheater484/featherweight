<#
.SYNOPSIS
  Checks what Featherweight shows UI Automation (screen readers, Narrator, test tools).
  Starts the app with the widget gallery and a PDF (settings are neither read nor saved), walks
  the whole element tree of both tabs and checks every element's control type, name, state,
  bounds and patterns, then works each kind of control through its patterns: expand, select,
  set value, toggle, invoke, scroll and focus. It checks the side panels, the status bar and
  command search the same way, and by keyboard (F6, Tab, Esc, Alt+Q) when the window can come to
  the foreground. A window shows on screen while it runs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\uia-checks.ps1
  # the trees are written to build\uia\*.txt; the exit code is the number of failed checks
#>
param([string]$Exe = "build\Release\Featherweight.exe")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root "build\uia"
New-Item -ItemType Directory -Force $out | Out-Null
$Exe = (Resolve-Path $Exe).Path

Add-Type -AssemblyName WindowsBase, UIAutomationClient, UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]
$CT = [System.Windows.Automation.ControlType]
$Scope = [System.Windows.Automation.TreeScope]
$Invoke = [System.Windows.Automation.InvokePattern]
$Toggle = [System.Windows.Automation.TogglePattern]
$Expand = [System.Windows.Automation.ExpandCollapsePattern]
$Value = [System.Windows.Automation.ValuePattern]
$Range = [System.Windows.Automation.RangeValuePattern]
$Selection = [System.Windows.Automation.SelectionPattern]
$SelItem = [System.Windows.Automation.SelectionItemPattern]
$Scroll = [System.Windows.Automation.ScrollPattern]
$ScrollItem = [System.Windows.Automation.ScrollItemPattern]

# Focus events arrive on another thread; this collects the ones from the app.
Add-Type -ReferencedAssemblies @($AE.Assembly.Location, $CT.Assembly.Location, [System.Windows.Rect].Assembly.Location) -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows.Automation;
public static class FocusWatch {
  static readonly List<string> seen = new List<string>();
  static AutomationFocusChangedEventHandler handler;
  public static void Start(int pid) {
    handler = (s, e) => {
      try {
        var el = s as AutomationElement;
        if (el == null || el.Current.ProcessId != pid) return;
        lock (seen) seen.Add(el.Current.ControlType.ProgrammaticName.Replace("ControlType.", "") + " '" + el.Current.Name + "'");
      } catch (Exception) {}
    };
    Automation.AddAutomationFocusChangedEventHandler(handler);
  }
  public static void Stop() { if (handler != null) Automation.RemoveAutomationFocusChangedEventHandler(handler); handler = null; }
  public static void Clear() { lock (seen) seen.Clear(); }
  public static bool Saw(string what) { lock (seen) return seen.Contains(what); }
  public static bool SawStart(string what) { lock (seen) return seen.Exists(s => s.StartsWith(what)); }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
"@

# ---- Reporting

$script:passed = 0
$script:failed = 0
function Pass([string]$what) { $script:passed++; "  ok    $what" }
function Fail([string]$what, [string]$why) { $script:failed++; "  FAIL  $what$(if ($why) { " - $why" })" }

# Passes when $test returns true, retrying until $waitMs is over (the app acts after a message).
function Check([string]$what, [scriptblock]$test, [int]$waitMs = 2000) {
  $deadline = [DateTime]::Now.AddMilliseconds($waitMs)
  while ($true) {
    $err = $null
    try { $ok = [bool](& $test) } catch { $ok = $false; $err = $_.Exception.Message }
    if ($ok -or [DateTime]::Now -ge $deadline) { break }
    Start-Sleep -Milliseconds 50
  }
  if ($ok) { Pass $what } else { Fail $what $err }
}

# Runs $action once, then checks $until.
function Step([string]$what, [scriptblock]$action, [scriptblock]$until, [int]$waitMs = 3000) {
  try { & $action | Out-Null } catch { Fail $what $_.Exception.Message; return }
  Check $what $until $waitMs
}

function Throws([string]$what, [scriptblock]$action) {
  try { & $action | Out-Null; Fail $what "no error" } catch { Pass $what }
}

# ---- Finding elements

function Cond($type, $name) {
  $c = @()
  if ($type) { $c += [System.Windows.Automation.PropertyCondition]::new($AE::ControlTypeProperty, $type) }
  if ($null -ne $name) { $c += [System.Windows.Automation.PropertyCondition]::new($AE::NameProperty, $name) }
  if ($c.Count -eq 1) { return $c[0] }
  return [System.Windows.Automation.AndCondition]::new([System.Windows.Automation.Condition[]]$c)
}
function Find($parent, $type, $name) { $parent.FindFirst($Scope::Descendants, (Cond $type $name)) }
function FindChild($parent, $type, $name) { $parent.FindFirst($Scope::Children, (Cond $type $name)) }
function FindAll($parent, $type) { @($parent.FindAll($Scope::Descendants, (Cond $type $null))) }
function Pat($el, $pattern) { $el.GetCurrentPattern($pattern::Pattern) }
function State($el) { (Pat $el $Expand).Current.ExpandCollapseState.ToString() }
function ToggleOf($el) { (Pat $el $Toggle).Current.ToggleState.ToString() }
function ValueOf($el) { (Pat $el $Value).Current.Value }
function RangeOf($el) { (Pat $el $Range).Current.Value }

function Start-App([string[]]$appArgs) {
  $quoted = $appArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
  $p = Start-Process -FilePath $Exe -ArgumentList $quoted -PassThru
  $cond = [System.Windows.Automation.PropertyCondition]::new($AE::ProcessIdProperty, $p.Id)
  for ($i = 0; $i -lt 100; $i++) {
    $w = $AE::RootElement.FindFirst($Scope::Children, $cond)
    if ($w -and (Find $w $CT::Pane $null)) { return $p, $w }
    Start-Sleep -Milliseconds 100
  }
  throw "The window didn't show up"
}

# ---- The whole tree: written to a file and checked element by element

function PatternNames($el) {
  @($el.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName -replace 'Identifiers\.Pattern$', '' -replace 'Pattern$', '' })
}

function Get-Tree($win) {
  $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
  $items = [System.Collections.Generic.List[object]]::new()
  $stack = [System.Collections.Generic.Stack[object]]::new()
  $stack.Push(@($win, 0, $null))
  while ($stack.Count) {
    $e, $depth, $parent = $stack.Pop()
    $c = $e.Current
    $info = [pscustomobject]@{
      El = $e; Depth = $depth; Parent = $parent; Type = $c.ControlType.ProgrammaticName -replace '^ControlType\.', ''
      Name = $c.Name; Enabled = $c.IsEnabled; Focusable = $c.IsKeyboardFocusable; Focused = $c.HasKeyboardFocus
      Offscreen = $c.IsOffscreen; Keys = $c.AcceleratorKey; Status = $c.ItemStatus; Rect = $c.BoundingRectangle
      Patterns = PatternNames $e
    }
    $items.Add($info)
    $children = @()
    $ch = $walker.GetFirstChild($e)
    while ($ch) { $children += $ch; $ch = $walker.GetNextSibling($ch) }
    for ($k = $children.Count - 1; $k -ge 0; $k--) { $stack.Push(@($children[$k], ($depth + 1), $info)) }
  }
  return $items
}

function Write-Tree($items, [string]$file) {
  $items | ForEach-Object {
    $r = $_.Rect
    $rect = if ($r.IsEmpty) { "none" } else { "{0},{1} {2}x{3}" -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height }
    $flags = @()
    if (-not $_.Enabled) { $flags += "disabled" }
    if ($_.Focusable) { $flags += "focusable" }
    if ($_.Focused) { $flags += "focused" }
    if ($_.Offscreen) { $flags += "offscreen" }
    if ($_.Keys) { $flags += "keys=$($_.Keys)" }
    if ($_.Status) { $flags += "status=$($_.Status)" }
    ('  ' * $_.Depth) + "$($_.Type) '$($_.Name)' [$($_.Patterns -join ',')] $($flags -join ' ') @ $rect"
  } | Set-Content -Encoding utf8 (Join-Path $out $file)
}

# Every element: a name; the patterns its control type needs; selectable items in a container
# that has Selection; bounds inside the window when on screen; a runtime id of its own.
function Check-Tree($items, [string]$label) {
  $win = $items[0].Rect
  $ids = @{}
  $before = $script:failed
  foreach ($i in ($items | Select-Object -Skip 1)) {
    $where = "${label}: $($i.Type) '$($i.Name)' in $($i.Parent.Type) '$($i.Parent.Name)'"
    $p = $i.Patterns
    $need = switch ($i.Type) {
      "Button" { @() }
      "SplitButton" { @("Invoke", "ExpandCollapse") }
      "ComboBox" { @("ExpandCollapse", "Value") }
      "Edit" { @("Value") }
      "Spinner" { @("RangeValue") }
      "Tab" { @("Selection") }
      "TabItem" { @("SelectionItem") }
      default { @() }
    }
    if (-not $i.Name) { Fail $where "no name" }
    foreach ($n in $need) { if ($p -notcontains $n) { Fail $where "no $n pattern" } }
    if ($i.Type -eq "Button") {
      if (-not ($p -contains "Invoke" -or $p -contains "Toggle" -or $p -contains "ExpandCollapse")) { Fail $where "a button that does nothing" }
      if ($p -contains "Invoke" -and $p -contains "Toggle") { Fail $where "both Invoke and Toggle" }
    }
    if ($i.Type -eq "ListItem" -and -not ($p -contains "SelectionItem" -or $p -contains "Invoke")) { Fail $where "neither SelectionItem nor Invoke" }
    # A list whose items can be selected says what is selected (a list of links needn't).
    if ($i.Type -eq "List" -and $p -notcontains "Selection" -and @($items | Where-Object { $_.Parent -eq $i -and $_.Patterns -contains "SelectionItem" }).Count) {
      Fail $where "selectable items but no Selection pattern"
    }
    if ($p -contains "SelectionItem") {
      $box = (Pat $i.El $SelItem).Current.SelectionContainer
      if (-not $box) { Fail $where "no selection container" }
      elseif ((PatternNames $box) -notcontains "Selection") { Fail $where "its container has no Selection pattern" }
    }
    if ($p -contains "Value" -and $i.Type -ne "ComboBox" -and $i.Type -ne "Edit") { Fail $where "Value on a $($i.Type)" }
    if (-not $i.Offscreen) {
      $r = $i.Rect
      if ($r.IsEmpty -or $r.Width -le 0 -or $r.Height -le 0) { Fail $where "on screen without bounds" }
      elseif ($r.Left -lt $win.Left - 1 -or $r.Top -lt $win.Top - 1 -or $r.Right -gt $win.Right + 1 -or $r.Bottom -gt $win.Bottom + 1) {
        Fail $where "bounds $r outside the window $win"
      }
    }
    $id = $i.El.GetRuntimeId() -join '.'
    if ($ids.ContainsKey($id)) { Fail $where "runtime id $id also used by $($ids[$id])" } else { $ids[$id] = "$($i.Type) '$($i.Name)'" }
  }
  if ($script:failed -eq $before) { Pass "${label}: $($items.Count) elements have names, patterns, bounds and ids" }
}

# ---- Run

$p, $win = Start-App @("--widget-gallery", "--theme", "light", (Join-Path $root "tests\pdfs\text.pdf"))
$hwnd = [IntPtr]$win.Current.NativeWindowHandle
[FocusWatch]::Start($p.Id)
try {
  "Window"
  $titleBar = FindChild $win $CT::TitleBar $null
  $tabList = Find $titleBar $CT::Tab "Open files"
  $tabs = FindAll $tabList $CT::TabItem
  Check "the title bar lists both tabs" { $tabs.Count -eq 2 }
  $galleryTab = $tabs | Where-Object { $_.Current.Name -eq "Widget gallery" }
  $pdfTab = $tabs | Where-Object { $_.Current.Name -ne "Widget gallery" }
  Check "the gallery tab is selected" { (Pat $galleryTab $SelItem).Current.IsSelected -and -not (Pat $pdfTab $SelItem).Current.IsSelected }
  Check "each tab has a close button" { (FindChild $galleryTab $CT::Button "Close tab").Current.AcceleratorKey -eq "Ctrl+W" }
  foreach ($n in "Minimize", "Maximize", "Close") { Check "title bar button $n" { (PatternNames (FindChild $titleBar $CT::Button $n)) -contains "Invoke" } }

  # ---- The gallery
  "Widget gallery"
  $tree = Get-Tree $win
  Write-Tree $tree "gallery.txt"
  Check-Tree $tree "gallery"
  $doc = FindChild $win $CT::Pane "Widget gallery"
  $lastChange = { (Find $doc $CT::Text $null).Current.Name }
  Check "the last change is shown as text" { (& $lastChange) -eq "Last change: Nothing yet" }
  foreach ($n in "Dropdown", "Combo box", "Split button", "Color picker", "Spin box", "List") {
    Check "card '$n' is a group" { FindChild $doc $CT::Group $n }
  }

  # Dropdown
  $card = FindChild $doc $CT::Group "Dropdown"
  $style = FindChild $card $CT::ComboBox "Style"
  Check "Style: shows Normal, read-only, collapsed" { (ValueOf $style) -eq "Normal" -and (Pat $style $Value).Current.IsReadOnly -and (State $style) -eq "Collapsed" }
  $list = FindChild $style $CT::List "Style"
  Check "Style: its list is off screen while closed" { $list.Current.IsOffscreen }
  Step "Style: expands" { (Pat $style $Expand).Expand() } { (State $style) -eq "Expanded" -and -not $list.Current.IsOffscreen }
  Check "Style: the open list's items are on screen" { -not (FindChild $list $CT::ListItem "Heading 2").Current.IsOffscreen }
  Step "Style: selecting Heading 2 picks it and closes the list" { (Pat (FindChild $list $CT::ListItem "Heading 2") $SelItem).Select() } {
    (ValueOf $style) -eq "Heading 2" -and (State $style) -eq "Collapsed" -and (& $lastChange) -eq "Last change: Style set to Heading 2"
  }
  Check "Style: the list's selection is Heading 2" { $s = (Pat $list $Selection).Current.GetSelection(); $s.Count -eq 1 -and $s[0].Current.Name -eq "Heading 2" }
  $disabled = FindChild $card $CT::ComboBox "Disabled example"
  Check "Disabled example: not enabled, not focusable" { -not $disabled.Current.IsEnabled -and -not $disabled.Current.IsKeyboardFocusable }
  Throws "Disabled example: refuses to expand" { (Pat $disabled $Expand).Expand() }

  # Combo box
  $card = FindChild $doc $CT::Group "Combo box"
  $font = FindChild $card $CT::ComboBox "Font"
  Check "Font: has its shortcut and an editable value" { $font.Current.AcceleratorKey -eq "Ctrl+Shift+F" -and -not (Pat $font $Value).Current.IsReadOnly }
  Step "Font: set to Georgia" { (Pat $font $Value).SetValue("Georgia") } { (ValueOf $font) -eq "Georgia" -and (& $lastChange) -eq "Last change: Font set to Georgia" }
  Check "Font: its edit shows Georgia" { (ValueOf (FindChild $font $CT::Edit "Font")) -eq "Georgia" }
  Step "Font: expands" { (Pat $font $Expand).Expand() } { (State $font) -eq "Expanded" -and (FindChild $font $CT::Button "Close") }
  Check "Font: the open list selects Georgia" { (Pat (Find $font $CT::ListItem "Georgia") $SelItem).Current.IsSelected }
  Step "Font: collapses" { (Pat $font $Expand).Collapse() } { (State $font) -eq "Collapsed" -and (FindChild $font $CT::Button "Open") }
  $size = FindChild $card $CT::ComboBox "Font size"
  Step "Font size: 13.3 is tidied to 13.5" { (Pat $size $Value).SetValue("13.3") } { (ValueOf $size) -eq "13.5" -and (& $lastChange) -eq "Last change: Font size set to 13.5" }
  Step "Font size: a word is refused" { (Pat $size $Value).SetValue("big") } { (ValueOf $size) -eq "13.5" -and (& $lastChange) -eq "Last change: Font size set to 13.5" }

  # Split buttons
  $card = FindChild $doc $CT::Group "Split button"
  $color = FindChild $card $CT::SplitButton "Font color"
  Check "Font color: shows Dark Red as its status" { $color.Current.ItemStatus -eq "Dark Red" }
  Step "Font color: expands to a palette" { (Pat $color $Expand).Expand() } { (State $color) -eq "Expanded" -and (Find $color $CT::List "Standard colors") }
  $palette = Find $color $CT::Pane $null
  Check "Font color: the palette has theme and standard colors, Automatic and More colors" {
    (FindChild $palette $CT::List "Theme colors") -and (FindChild $palette $CT::List "Standard colors") -and
    (FindChild $palette $CT::Button "Automatic") -and (FindChild $palette $CT::Button "More colors$([char]0x2026)")
  }
  Check "Font color: Dark Red is the selected standard color" { (Pat (Find $palette $CT::ListItem "Dark Red") $SelItem).Current.IsSelected }
  Check "Font color: 60 theme colors" { (FindAll (FindChild $palette $CT::List "Theme colors") $CT::ListItem).Count -eq 60 }
  $blue = (FindChild $palette $CT::List "Standard colors").FindFirst($Scope::Children, (Cond $CT::ListItem "Blue"))
  Step "Font color: selecting Blue picks it" { (Pat $blue $SelItem).Select() } {
    $color.Current.ItemStatus -eq "Blue" -and (State $color) -eq "Collapsed" -and (& $lastChange) -eq "Last change: Font color set to Blue"
  }
  Step "Font color: invoking applies Blue" { (Pat $color $Invoke).Invoke() } { (& $lastChange) -eq "Last change: Applied font color Blue" }
  Step "Font color: its arrow expands it too" { (Pat (FindChild $color $CT::Button "More options") $Expand).Expand() } { (State $color) -eq "Expanded" }
  Step "Font color: collapses" { (Pat $color $Expand).Collapse() } { (State $color) -eq "Collapsed" -and -not (Find $color $CT::Pane $null) }
  $bullets = FindChild $card $CT::SplitButton "Bullets"
  Step "Bullets: expands to a list" { (Pat $bullets $Expand).Expand() } { (State $bullets) -eq "Expanded" -and -not (FindChild $bullets $CT::List "Bullets").Current.IsOffscreen }
  Step "Bullets: selecting Square picks it" { (Pat (Find $bullets $CT::ListItem "Square") $SelItem).Select() } {
    (State $bullets) -eq "Collapsed" -and (& $lastChange) -eq "Last change: Bullet style set to Square"
  }
  Step "Bullets: its main part applies Square" { (Pat (FindChild $bullets $CT::Button "Bullets") $Invoke).Invoke() } { (& $lastChange) -eq "Last change: Applied bullets: Square" }

  # Color pickers
  $card = FindChild $doc $CT::Group "Color picker"
  $highlight = FindChild $card $CT::Button "Highlight"
  Check "Highlight: a button that expands, showing Yellow" { (PatternNames $highlight) -contains "ExpandCollapse" -and $highlight.Current.ItemStatus -eq "Yellow" }
  Step "Highlight: expands" { (Pat $highlight $Expand).Expand() } { (State $highlight) -eq "Expanded" -and (Find $highlight $CT::Button "No color") }
  Step "Highlight: No color picks it" { (Pat (Find $highlight $CT::Button "No color") $Invoke).Invoke() } {
    $highlight.Current.ItemStatus -eq "No color" -and (State $highlight) -eq "Collapsed" -and (& $lastChange) -eq "Last change: Highlight set to No color"
  }
  Check "Page color: shows Automatic" { (FindChild $card $CT::Button "Page color").Current.ItemStatus -eq "Automatic" }

  # Spin boxes
  $card = FindChild $doc $CT::Group "Spin box"
  $before = FindChild $card $CT::Spinner "Spacing before"
  Check "Spacing before: 0 to 1584 in steps of 6" {
    $r = (Pat $before $Range).Current; $r.Value -eq 0 -and $r.Minimum -eq 0 -and $r.Maximum -eq 1584 -and $r.SmallChange -eq 6
  }
  Check "Spacing before: Decrease is off at the minimum" { -not (FindChild $before $CT::Button "Decrease").Current.IsEnabled }
  Step "Spacing before: set to 18" { (Pat $before $Range).SetValue(18) } { (RangeOf $before) -eq 18 -and (ValueOf (FindChild $before $CT::Edit $null)) -eq "18 pt" }
  Step "Spacing before: Increase adds a step" { (Pat (FindChild $before $CT::Button "Increase") $Invoke).Invoke() } { (RangeOf $before) -eq 24 }
  Check "Spacing before: Decrease is on again" { (FindChild $before $CT::Button "Decrease").Current.IsEnabled }
  Throws "Spacing before: refuses 5000" { (Pat $before $Range).SetValue(5000) }
  Step "Spacing before: its edit takes text" { (Pat (FindChild $before $CT::Edit $null) $Value).SetValue("30") } { (RangeOf $before) -eq 30 }
  Check "Line spacing: 1.15" { [math]::Abs((RangeOf (FindChild $card $CT::Spinner "Line spacing")) - 1.15) -lt 1e-9 }

  # List
  $files = FindChild (FindChild $doc $CT::Group "List") $CT::List "Files"
  $sp = Pat $files $Scroll
  Check "Files: scrolls vertically, at the top" { $sp.Current.VerticallyScrollable -and $sp.Current.VerticalScrollPercent -eq 0 -and $sp.Current.VerticalViewSize -lt 100 }
  $invoice = FindChild $files $CT::ListItem "Invoice 1042.pdf"
  Check "Files: Invoice 1042.pdf starts off screen" { $invoice.Current.IsOffscreen }
  Step "Files: scrolls Invoice 1042.pdf into view" { (Pat $invoice $ScrollItem).ScrollIntoView() } { -not $invoice.Current.IsOffscreen -and $sp.Current.VerticalScrollPercent -gt 0 }
  Step "Files: selects it" { (Pat $invoice $SelItem).Select() } { (Pat $invoice $SelItem).Current.IsSelected -and (& $lastChange) -eq "Last change: Selected Invoice 1042.pdf" }
  Step "Files: scrolls to the end" { $sp.SetScrollPercent($Scroll::NoScroll, 100) } { $sp.Current.VerticalScrollPercent -eq 100 -and -not (FindChild $files $CT::ListItem "Year in review.pptx").Current.IsOffscreen }

  # The gallery's toolbar
  $bar = FindChild $win $CT::ToolBar "Commands"
  $tbStyle = FindChild $bar $CT::ComboBox "Style"
  Step "Toolbar Style: expands" { (Pat $tbStyle $Expand).Expand() } { (State $tbStyle) -eq "Expanded" }
  Check "Toolbar Style: the open list is inside the window" {
    $r = (FindChild $tbStyle $CT::List $null).Current.BoundingRectangle; $w = $win.Current.BoundingRectangle
    -not $r.IsEmpty -and $r.Top -ge $w.Top -and $r.Bottom -le $w.Bottom
  }
  Step "Toolbar Style: selecting Title picks it" { (Pat (Find $tbStyle $CT::ListItem "Title") $SelItem).Select() } {
    (ValueOf $tbStyle) -eq "Title" -and (& $lastChange) -eq "Last change: Toolbar style set to Title"
  }
  Step "Toolbar Font: set to Verdana" { (Pat (FindChild $bar $CT::ComboBox "Font") $Value).SetValue("Verdana") } { (& $lastChange) -eq "Last change: Toolbar font set to Verdana" }
  $bold = FindChild $bar $CT::Button "Bold"
  Check "Toolbar Bold: a toggle with its shortcut" { (ToggleOf $bold) -eq "Off" -and $bold.Current.AcceleratorKey -eq "Ctrl+B" }
  Step "Toolbar Bold: toggles on" { (Pat $bold $Toggle).Toggle() } { (ToggleOf $bold) -eq "On" }
  Step "Toolbar Bold: toggles off" { (Pat $bold $Toggle).Toggle() } { (ToggleOf $bold) -eq "Off" }
  $after = FindChild $bar $CT::Spinner "Spacing after"
  Step "Toolbar Spacing after: set to 12" { (Pat $after $Range).SetValue(12) } { (RangeOf $after) -eq 12 -and (& $lastChange) -eq "Last change: Toolbar spacing after set to 12 pt" }
  $tbColor = FindChild $bar $CT::SplitButton "Font color"
  Step "Toolbar Font color: picks Green from its palette" {
    (Pat $tbColor $Expand).Expand()
    $deadline = [DateTime]::Now.AddSeconds(3)
    while (-not ($g = Find $tbColor $CT::ListItem "Green") -and [DateTime]::Now -lt $deadline) { Start-Sleep -Milliseconds 50 }
    (Pat $g $SelItem).Select()
  } { $tbColor.Current.ItemStatus -eq "Green" -and (& $lastChange) -eq "Last change: Toolbar font color set to Green" }
  Step "Toolbar Bullets: invokes" { (Pat (FindChild $bar $CT::SplitButton "Bullets") $Invoke).Invoke() } { (& $lastChange) -like "Last change: Applied bullets: *" }

  # Side panels: Format is made of controls, Comments is drawn by the gallery
  "Side panel"
  $fmtButton = FindChild $bar $CT::Button "Format panel"
  $comButton = FindChild $bar $CT::Button "Comments panel"
  Check "Format panel: a toggle, off, and no panel open" { (ToggleOf $fmtButton) -eq "Off" -and -not (FindChild $win $CT::Pane "Format") }
  Step "Format panel: toggling opens it" { (Pat $fmtButton $Toggle).Toggle() } { (ToggleOf $fmtButton) -eq "On" -and (FindChild $win $CT::Pane "Format") }
  $panel = FindChild $win $CT::Pane "Format"
  Check "Format panel: at the right, with the document area narrowed beside it" {
    $p = $panel.Current.BoundingRectangle; $d = $doc.Current.BoundingRectangle
    $p.Width -ge 300 -and [math]::Abs($d.Right - $p.Left) -le 1 -and $p.Top -eq $d.Top
  }
  $tree = Get-Tree $win
  Write-Tree $tree "gallery-panel.txt"
  Check-Tree $tree "gallery with the Format panel"
  Check "Format panel: a close button with its key" { $c = FindChild $panel $CT::Button "Close"; $c -and $c.Current.AcceleratorKey -eq "Esc" }
  Check "Format panel: its controls show their values" {
    (ValueOf (FindChild $panel $CT::ComboBox "Font")) -eq "Calibri" -and (RangeOf (FindChild $panel $CT::Spinner "Spacing after")) -eq 8
  }
  Step "Format panel: Font size set to 14" { (Pat (FindChild $panel $CT::ComboBox "Font size") $Value).SetValue("14") } { (& $lastChange) -eq "Last change: Format: font size set to 14" }
  $align = FindChild $panel $CT::ComboBox "Alignment"
  Step "Format panel: Alignment's list opens inside the panel" { (Pat $align $Expand).Expand() } {
    $r = (FindChild $align $CT::List $null).Current.BoundingRectangle; $p = $panel.Current.BoundingRectangle
    (State $align) -eq "Expanded" -and $r.Left -ge $p.Left -and $r.Right -le $p.Right + 1
  }
  Step "Format panel: selecting Center picks it" { (Pat (Find $align $CT::ListItem "Center") $SelItem).Select() } {
    (ValueOf $align) -eq "Center" -and (& $lastChange) -eq "Last change: Format: alignment set to Center"
  }
  Step "Comments panel: takes the Format panel's place" { (Pat $comButton $Toggle).Toggle() } {
    (FindChild $win $CT::Pane "Comments") -and -not (FindChild $win $CT::Pane "Format") -and (ToggleOf $fmtButton) -eq "Off" -and (ToggleOf $comButton) -eq "On"
  }
  $cpanel = FindChild $win $CT::Pane "Comments"
  $clist = FindChild $cpanel $CT::List "Comments"
  $comments = FindAll $clist $CT::ListItem
  Check "Comments panel: three comments, the first selected" {
    $comments.Count -eq 3 -and (Pat $comments[0] $SelItem).Current.IsSelected -and $comments[0].Current.Name -like "Ana Lima: *" -and $comments[0].Current.ItemStatus -eq "2 hours ago"
  }
  $second = $comments[1]
  Step "Comments panel: selecting the second comment" { (Pat $second $SelItem).Select() } {
    (Pat $second $SelItem).Current.IsSelected -and (& $lastChange) -eq "Last change: Selected the comment by Sam Okafor"
  }
  Step "Comments panel: its close button closes it" { (Pat (FindChild $cpanel $CT::Button "Close") $Invoke).Invoke() } {
    -not (FindChild $win $CT::Pane "Comments") -and (ToggleOf $comButton) -eq "Off"
  }
  Check "Comments panel: the document area is as wide as before" { [math]::Abs($doc.Current.BoundingRectangle.Right - $bar.Current.BoundingRectangle.Right) -le 1 }

  # Status bar
  "Status bar"
  $sb = FindChild $win $CT::StatusBar $null
  Check "status bar: its segments and the zoom are text" { (@(FindAll $sb $CT::Text | ForEach-Object { $_.Current.Name }) -join "|") -eq "6 cards|12 controls|English (UK)|100%" }
  Check "status bar: below the document area" { [math]::Abs($sb.Current.BoundingRectangle.Top - $doc.Current.BoundingRectangle.Bottom) -le 1 }
  $slider = FindChild $sb $CT::Slider "Zoom"
  Check "Zoom: a slider from 10 to 500 in steps of 10, at 100" {
    $r = (Pat $slider $Range).Current; $r.Value -eq 100 -and $r.Minimum -eq 10 -and $r.Maximum -eq 500 -and $r.SmallChange -eq 10 -and $slider.Current.IsKeyboardFocusable
  }
  Step "Zoom: set to 150" { (Pat $slider $Range).SetValue(150) } { (RangeOf $slider) -eq 150 -and (& $lastChange) -eq "Last change: Zoom set to 150%" -and (FindChild $sb $CT::Text "150%") }
  Step "Zoom in: adds 10" { (Pat (FindChild $sb $CT::Button "Zoom in") $Invoke).Invoke() } { (RangeOf $slider) -eq 160 }
  Step "Zoom out: takes 10 off" { (Pat (FindChild $sb $CT::Button "Zoom out") $Invoke).Invoke() } { (RangeOf $slider) -eq 150 }
  Throws "Zoom: refuses 1000" { (Pat $slider $Range).SetValue(1000) }

  # Keyboard focus: needs the window in the foreground.
  if ([FocusWatch]::GetForegroundWindow() -ne $hwnd) { try { $win.SetFocus() } catch {} ; Start-Sleep -Milliseconds 300 }
  if ([FocusWatch]::GetForegroundWindow() -eq $hwnd) {
    $fontEdit = FindChild (FindChild (FindChild $doc $CT::Group "Combo box") $CT::ComboBox "Font") $CT::Edit "Font"
    [FocusWatch]::Clear()
    Step "focus: the Font edit takes the keyboard" { $fontEdit.SetFocus() } { $fontEdit.Current.HasKeyboardFocus -and $AE::FocusedElement.Current.Name -eq "Font" }
    Check "focus: a focus event for the Font edit" { [FocusWatch]::Saw("Edit 'Font'") }
    $cols = FindChild (FindChild $doc $CT::Group "Spin box") $CT::Spinner "Columns"
    Step "focus: the Columns spin box takes it next" { $cols.SetFocus() } { $AE::FocusedElement.Current.Name -eq "Columns" -and -not $fontEdit.Current.HasKeyboardFocus }
    Check "focus: a focus event for Columns" { [FocusWatch]::Saw("Edit 'Columns'") -or [FocusWatch]::Saw("Spinner 'Columns'") }
    Step "focus: the toolbar's Font size box takes it" { (FindChild (FindChild $bar $CT::ComboBox "Font size") $CT::Edit $null).SetFocus() } {
      $f = $AE::FocusedElement.Current; $f.Name -eq "Font size" -and $f.BoundingRectangle.Top -lt (FindChild $win $CT::Pane "Widget gallery").Current.BoundingRectangle.Top
    }
    Step "focus: back to the document" { $doc.SetFocus() } { $doc.Current.HasKeyboardFocus }

    Add-Type -AssemblyName System.Windows.Forms
    $keys = { param([string]$k) [System.Windows.Forms.SendKeys]::SendWait($k) }
    $inside = { param($el, $outer) $r = $el.Current.BoundingRectangle; $o = $outer.Current.BoundingRectangle; $r.Left -ge $o.Left -and $r.Right -le $o.Right + 1 }
    # The keyboard is on `$outer` or on one of its parts (a list's selected item).
    $focusIn = {
      param($outer)
      $walker = [System.Windows.Automation.TreeWalker]::RawViewWalker
      for ($e = $AE::FocusedElement; $e; $e = $walker.GetParent($e)) { if ([System.Windows.Automation.Automation]::Compare($e, $outer)) { return $true } }
      $false
    }

    # The status bar's slider by keyboard
    [FocusWatch]::Clear()
    Step "keys: F6 moves to the status bar's zoom" { & $keys "{F6}" } { $AE::FocusedElement.Current.Name -eq "Zoom" -and $slider.Current.HasKeyboardFocus }
    Check "keys: a focus event for the zoom slider" { [FocusWatch]::Saw("Slider 'Zoom'") }
    Step "keys: Right zooms in by 10" { & $keys "{RIGHT}" } { (RangeOf $slider) -eq 160 }
    Step "keys: Home goes to 10" { & $keys "{HOME}" } { (RangeOf $slider) -eq 10 }
    Step "keys: Esc gives the keyboard back to the document" { & $keys "{ESC}" } { $doc.Current.HasKeyboardFocus }

    # The side panel by keyboard: F6 and Tab go in, Esc closes it and goes back
    $cols = FindChild (FindChild $doc $CT::Group "Spin box") $CT::Spinner "Columns"
    Step "keys: the Columns spin box has the keyboard" { $cols.SetFocus() } { $AE::FocusedElement.Current.Name -eq "Columns" }
    Step "keys: the Comments panel opens" { (Pat $comButton $Toggle).Toggle() } { FindChild $win $CT::Pane "Comments" }
    $cpanel = FindChild $win $CT::Pane "Comments"
    [FocusWatch]::Clear()
    Step "keys: F6 moves into the panel, to the selected comment" { & $keys "{F6}" } {
      $f = $AE::FocusedElement; $f.Current.ControlType -eq $CT::ListItem -and $f.Current.Name -like "Sam Okafor: *"
    }
    Check "keys: a focus event for the comment" { [FocusWatch]::SawStart("ListItem 'Sam Okafor: ") }
    Step "keys: Down selects the next comment" { & $keys "{DOWN}" } { (& $lastChange) -eq "Last change: Selected the comment by Mei Chen" -and $AE::FocusedElement.Current.Name -like "Mei Chen: *" }
    Step "keys: Esc closes the panel and gives Columns the keyboard back" { & $keys "{ESC}" } {
      -not (FindChild $win $CT::Pane "Comments") -and $AE::FocusedElement.Current.Name -eq "Columns"
    }
    $files = FindChild (FindChild $doc $CT::Group "List") $CT::List "Files"
    Step "keys: the Format panel opens, the Files list has the keyboard" { (Pat $fmtButton $Toggle).Toggle(); $files.SetFocus() } {
      (FindChild $win $CT::Pane "Format") -and (& $focusIn $files)
    }
    $panel = FindChild $win $CT::Pane "Format"
    Step "keys: Tab past the gallery's last control goes into the panel" { & $keys "{TAB}" } {
      $f = $AE::FocusedElement; $f.Current.Name -eq "Font" -and (& $inside $f $panel)
    }
    Step "keys: Shift+Tab goes round to the panel's last control" { & $keys "+{TAB}" } {
      $f = $AE::FocusedElement; $f.Current.Name -eq "Line spacing" -and (& $inside $f $panel)
    }
    Step "keys: Esc closes the panel and gives Files the keyboard back" { & $keys "{ESC}" } {
      -not (FindChild $win $CT::Pane "Format") -and (& $focusIn $files)
    }

    # Command search
    [FocusWatch]::Clear()
    Step "Alt+Q: command search opens with the keyboard in its box" { & $keys "%q" } {
      $cs = FindChild $win $CT::Pane "Command search"; $cs -and (FindChild $cs $CT::Edit "Search for a command").Current.HasKeyboardFocus
    }
    Check "command search: a focus event for its box" { [FocusWatch]::Saw("Edit 'Search for a command'") }
    $cs = FindChild $win $CT::Pane "Command search"
    Check "command search: over the toolbar, with its shortcut" {
      $r = $cs.Current.BoundingRectangle; $t = $bar.Current.BoundingRectangle
      (FindChild $cs $CT::Edit "Search for a command").Current.AcceleratorKey -eq "Alt+Q" -and $r.Top -ge $t.Top -and $r.Top -lt $t.Bottom
    }
    Step "command search: typing fi finds Find" { & $keys "fi" } { $l = FindChild $cs $CT::List "Commands"; $l -and (FindChild $l $CT::ListItem "Find") }
    $results = FindChild $cs $CT::List "Commands"
    Check "command search: results show their shortcut, and the first is selected" {
      $f = FindChild $results $CT::ListItem "Find"
      $f.Current.AcceleratorKey -eq "Ctrl+F" -and (Pat $f $SelItem).Current.IsSelected -and (FindChild $results $CT::ListItem "Find next").Current.AcceleratorKey -eq "F3"
    }
    $tree = Get-Tree $win
    Write-Tree $tree "gallery-commands.txt"
    Check-Tree $tree "gallery with command search"
    [FocusWatch]::Clear()
    Step "command search: Down moves the keyboard to the next result" { & $keys "{DOWN}" } {
      (Pat (FindChild $results $CT::ListItem "Find next") $SelItem).Current.IsSelected -and $AE::FocusedElement.Current.Name -eq "Find next"
    }
    Check "command search: a focus event for Find next" { [FocusWatch]::Saw("ListItem 'Find next'") }
    Step "command search: a command that can't run here is disabled" { (Pat (FindChild $cs $CT::Edit "Search for a command") $Value).SetValue("print") } {
      $i = Find $cs $CT::ListItem "Print"; $i -and -not $i.Current.IsEnabled -and $i.Current.AcceleratorKey -eq "Ctrl+P"
    }
    Step "command search: Esc closes it and gives Files the keyboard back" { & $keys "{ESC}" } {
      -not (FindChild $win $CT::Pane "Command search") -and (& $focusIn $files)
    }
    Step "command search: Enter runs the command found (Format panel)" {
      & $keys "%q"; Start-Sleep -Milliseconds 300; & $keys "format"; Start-Sleep -Milliseconds 300; & $keys "{ENTER}"
    } { (FindChild $win $CT::Pane "Format") -and -not (FindChild $win $CT::Pane "Command search") }
    Step "command search: invoking a result runs it (Comments panel)" {
      & $keys "%q"
      $deadline = [DateTime]::Now.AddSeconds(3)
      while (-not ($c = FindChild $win $CT::Pane "Command search") -and [DateTime]::Now -lt $deadline) { Start-Sleep -Milliseconds 50 }
      (Pat (FindChild $c $CT::Edit "Search for a command") $Value).SetValue("comm")
      while (-not ($i = Find $c $CT::ListItem "Comments panel") -and [DateTime]::Now -lt $deadline) { Start-Sleep -Milliseconds 50 }
      (Pat $i $Invoke).Invoke()
    } { (FindChild $win $CT::Pane "Comments") -and -not (FindChild $win $CT::Pane "Format") -and -not (FindChild $win $CT::Pane "Command search") }
    Step "keys: Esc in the document closes the panel" { $doc.SetFocus(); & $keys "{ESC}" } { -not (FindChild $win $CT::Pane "Comments") }
  } else {
    "  skip  focus checks: the window couldn't come to the foreground"
  }

  # ---- The PDF tab
  "PDF tab"
  Step "selecting the PDF's tab shows it" { (Pat $pdfTab $SelItem).Select() } { $win.Current.Name -like "$($pdfTab.Current.Name)*" -and (FindChild $win $CT::Pane $pdfTab.Current.Name) }
  Start-Sleep -Milliseconds 500
  $tree = Get-Tree $win
  Write-Tree $tree "pdf.txt"
  Check-Tree $tree "pdf"
  Check "the PDF tab has no status bar" { -not (FindChild $win $CT::StatusBar $null) }
  $bar = FindChild $win $CT::ToolBar "Commands"
  Check "Save: off while nothing changed" { $b = FindChild $bar $CT::Button "Save"; -not $b.Current.IsEnabled -and $b.Current.AcceleratorKey -eq "Ctrl+S" }
  Check "Save as: a menu button" { (PatternNames (FindChild $bar $CT::Button "Save as$([char]0x2026)")) -contains "ExpandCollapse" }
  Check "Print: has its shortcut" { (FindChild $bar $CT::Button "Print").Current.AcceleratorKey -eq "Ctrl+P" }
  Check "Zoom: a menu button showing the zoom" { $z = FindChild $bar $CT::Button "Zoom"; (PatternNames $z) -contains "ExpandCollapse" -and $z.Current.ItemStatus -match '%' }
  Check "Page layout: a menu button" { (State (FindChild $bar $CT::Button "Page layout")) -eq "Collapsed" }
  Check "Rotate: has its shortcut" { (FindChild $bar $CT::Button "Rotate").Current.AcceleratorKey -eq "Ctrl+Shift+=" }
  $pageBox = FindChild $bar $CT::Edit "Go to page"
  Check "Go to page: shows 1, with its shortcut" { (ValueOf $pageBox) -eq "1" -and $pageBox.Current.AcceleratorKey -eq "Ctrl+G" }
  Check "the page count is text" { (FindChild $bar $CT::Text $null).Current.Name -match '^of \d+$' }
  Step "Go to page: 2 goes to page 2" { (Pat $pageBox $Value).SetValue("2") } { (ValueOf (FindChild $bar $CT::Edit "Go to page")) -eq "2" }
  $night = FindChild $bar $CT::Button "Night reading: dark pages"
  Step "Night reading: toggles on" { (Pat $night $Toggle).Toggle() } { (ToggleOf $night) -eq "On" }
  Step "Night reading: toggles off" { (Pat $night $Toggle).Toggle() } { (ToggleOf $night) -eq "Off" }
  $find = FindChild $bar $CT::Button "Find"
  Check "Find: a toggle with its shortcut, off" { (ToggleOf $find) -eq "Off" -and $find.Current.AcceleratorKey -eq "Ctrl+F" }
  Step "Find: toggling opens the find panel" { (Pat $find $Toggle).Toggle() } { (ToggleOf $find) -eq "On" -and (FindChild $win $CT::Pane "Find") }
  $panel = FindChild $win $CT::Pane "Find"
  $box = FindChild $panel $CT::Edit "Find in document"
  Check "Find panel: its box has the shortcut" { $box.Current.AcceleratorKey -eq "Ctrl+F" }
  $match = { if ((FindChild $panel $CT::Text $null).Current.Name -match '^(\d+) of \d+$') { [int]$Matches[1] } else { 0 } }
  Step "Find panel: searching for fox finds matches" { (Pat $box $Value).SetValue("fox") } { (& $match) -gt 0 } 10000
  $first = & $match
  Step "Find panel: Next match moves on" { (Pat (FindChild $panel $CT::Button "Next match") $Invoke).Invoke() } { (& $match) -eq $first + 1 }
  $case = FindChild $panel $CT::Button "Match case"
  Step "Find panel: Match case toggles on" { (Pat $case $Toggle).Toggle() } { (ToggleOf $case) -eq "On" }
  Step "Find panel: Match case toggles off" { (Pat $case $Toggle).Toggle() } { (ToggleOf $case) -eq "Off" }
  Step "Find panel: Close closes it" { (Pat (FindChild $panel $CT::Button "Close") $Invoke).Invoke() } { -not (FindChild $win $CT::Pane "Find") -and (ToggleOf (FindChild $bar $CT::Button "Find")) -eq "Off" }

  # ---- The start page, once both tabs are closed
  "Start page"
  for ($k = 0; $k -lt 2; $k++) {
    $t = FindChild $tabList $CT::TabItem $null
    $n = $t.Current.Name
    Step "closing tab '$n'" { (Pat (FindChild $t $CT::Button "Close tab") $Invoke).Invoke() } { -not (FindChild $tabList $CT::TabItem $n) }
  }
  Check "the start page shows" { FindChild $win $CT::Pane "Start" }
  $tree = Get-Tree $win
  Write-Tree $tree "start.txt"
  Check-Tree $tree "start"
} finally {
  [FocusWatch]::Stop()
  if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(3000)) { $p.Kill() } }
}

"$($script:passed) passed, $($script:failed) failed. Trees are in $out"
exit $script:failed
