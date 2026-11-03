<#
.SYNOPSIS
  Checks the window's Direct3D 11 rendering (plan decision 20): renders the same states with each
  renderer and compares them.
    wic     Direct2D's software renderer, as tests\visual-checks.ps1 uses
    gpu     the shared Direct3D device, off-screen
    warp    the same on WARP, the fallback when there is no usable graphics adapter
    window  the real window and swap chain, captured from the compositor
  "window" must equal "gpu" pixel for pixel; "gpu" differs from "wic" by ClearType and
  anti-aliasing, and "warp" from "gpu" by anti-aliasing only.

  The window cases show a window at the top left of the screen for a moment (it doesn't take focus).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tests\renderer-checks.ps1
  powershell -ExecutionPolicy Bypass -File tests\renderer-checks.ps1 -Lifecycle
  # -Lifecycle: the window resized and after a lost device, compared with an off-screen frame
#>
param(
  [string]$Exe = "build\Release\Featherweight.exe",
  [switch]$Lifecycle
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root "build\renderers"
New-Item -ItemType Directory -Force $out | Out-Null
$pdfs = Join-Path $root "tests\pdfs"

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class PixDelta {
  // "changed-pixels largest-channel-difference", or the two sizes if they differ.
  public static string Compare(string a, string b) {
    using (var A = new Bitmap(a)) using (var B = new Bitmap(b)) {
      if (A.Width != B.Width || A.Height != B.Height) return "size " + A.Width + "x" + A.Height + " vs " + B.Width + "x" + B.Height;
      var r = new Rectangle(0, 0, A.Width, A.Height);
      var da = A.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      var db = B.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      int n = da.Stride * A.Height; var ba = new byte[n]; var bb = new byte[n];
      Marshal.Copy(da.Scan0, ba, 0, n); Marshal.Copy(db.Scan0, bb, 0, n);
      A.UnlockBits(da); B.UnlockBits(db);
      int diff = 0, max = 0;
      for (int i = 0; i < n; i += 4) {
        int m = 0;
        for (int k = 0; k < 3; k++) m = Math.Max(m, Math.Abs(ba[i + k] - bb[i + k]));
        if (m > 0) diff++;
        max = Math.Max(max, m);
      }
      return diff + " " + max;
    }
  }
}
"@

# name, theme, extra args, script, files[, final size: off-screen frames are drawn at this size,
# without the steps only a window can do]
if ($Lifecycle) {
  $modes = @("gpu", "window")
  $cases = @(
    @("resize-smaller", "light", "", "resize:900x560;cmd:fit-page", @("text.pdf"), "900x560"),
    @("resize-larger", "dark", "", "view:facing;resize:1300x820", @("text.pdf"), "1300x820"),
    @("device-lost", "light", "", "cmd:find;type:fox;wait:400;devicelost;wait:500", @("text.pdf"), "1100x700"),
    @("device-lost-night", "dark", "--invert", "devicelost;page:3;wait:500", @("text.pdf"), "1100x700")
  )
} else {
  $modes = @("wic", "gpu", "warp", "window")
  $cases = @(
    @("welcome-dark", "dark", "", "", @()),
    @("text-light", "light", "", "", @("text.pdf")),
    @("night-reading", "dark", "--invert", "view:facing;cmd:fit-page", @("text.pdf")),
    @("search", "light", "", "cmd:find;type:fox;wait:400", @("text.pdf")),
    @("form-typing", "light", "", "click:560,285;type:Jane Doe;key:tab;type:jane@example.com", @("form.pdf")),
    # The note's text box is a native child window drawn over the swap chain; its blinking caret is
    # the only expected difference.
    @("note-edit-existing", "dark", "", "note:760,210,Existing note|with two lines;click:772,222;type: and more", @("text.pdf"))
  )
}

foreach ($c in $cases) {
  $name, $theme, $extra, $script, $files, $final = $c
  foreach ($mode in $modes) {
    $png = Join-Path $out "$name-$mode.png"
    if (Test-Path $png) { Remove-Item $png }
    $size = "1100x700"
    $s = $script
    if ($final -and $mode -ne "window") {
      $size = $final
      $s = (($script -split ';') | Where-Object { $_ -notmatch '^(resize|devicelost)' }) -join ';'
    }
    $a = @("--snapshot", $png, "--size", $size, "--theme", $theme)
    switch ($mode) { "gpu" { $a += "--gpu" } "warp" { $a += @("--gpu", "--warp") } "window" { $a += "--window" } }
    if ($extra) { $a += $extra }
    if ($s) { $a += @("--script", $s) }
    foreach ($f in $files) { $a += (Join-Path $pdfs $f) }
    $p = Start-Process -FilePath $Exe -ArgumentList ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0 -or -not (Test-Path $png)) { "{0,-20} {1,-7} FAILED ({2})" -f $name, $mode, $p.ExitCode }
  }
  $cmp = { param($x, $y) $px = Join-Path $out "$name-$x.png"; $py = Join-Path $out "$name-$y.png"
    if ((Test-Path $px) -and (Test-Path $py)) { [PixDelta]::Compare($px, $py) } else { "-" } }
  if ($Lifecycle) { "{0,-20} window/gpu: {1}" -f $name, (& $cmp "window" "gpu") }
  else { "{0,-20} window/gpu: {1,-12} gpu/wic: {2,-12} warp/gpu: {3}" -f $name, (& $cmp "window" "gpu"), (& $cmp "gpu" "wic"), (& $cmp "warp" "gpu") }
}
"Images are in $out"
