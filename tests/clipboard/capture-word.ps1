# Captures what Word puts on the clipboard, for the clipboard tests (test_clipboard.cpp): builds a
# small document through Word's COM interface (a heading, formatted text with a link, a numbered
# and a bulleted list, a table, a line break, a picture), copies five selections, and saves each
# one's "Rich Text Format", "HTML Format" and text. The clipboard's text is put back afterwards.
#   powershell -NoProfile -File tests\clipboard\capture-word.ps1 [-Out tests\clipboard]
param([string]$Out = (Split-Path -Parent $MyInvocation.MyCommand.Path))
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Clip {
  [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
  [DllImport("user32.dll")] static extern bool CloseClipboard();
  [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint f);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RegisterClipboardFormat(string s);
  [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
  [DllImport("kernel32.dll")] static extern UIntPtr GlobalSize(IntPtr h);
  public static byte[] Get(string name) {
    uint f = name == "text" ? 13u : RegisterClipboardFormat(name);
    for (int i = 0; i < 40 && !OpenClipboard(IntPtr.Zero); i++) System.Threading.Thread.Sleep(50);
    try {
      IntPtr h = GetClipboardData(f);
      if (h == IntPtr.Zero) return null;
      IntPtr p = GlobalLock(h);
      int n = (int)GlobalSize(h).ToUInt64();
      byte[] b = new byte[n];
      Marshal.Copy(p, b, 0, n);
      GlobalUnlock(h);
      return b;
    } finally { CloseClipboard(); }
  }
}
"@

function Save-Formats([string]$name) {
  Start-Sleep -Milliseconds 300
  foreach ($f in @(@("Rich Text Format", "rtf"), @("HTML Format", "html"), @("text", "txt"))) {
    $bytes = [Clip]::Get($f[0])
    if ($bytes -eq $null) { Write-Host "  $name.$($f[1]): not on the clipboard"; continue }
    if ($f[1] -eq "txt") {
      $s = [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char]0)
      $bytes = [Text.Encoding]::UTF8.GetBytes($s)
    } else {
      $n = $bytes.Length; while ($n -gt 0 -and $bytes[$n - 1] -eq 0) { $n-- }
      $bytes = $bytes[0..($n - 1)]
      # The user's name in temporary picture paths, replaced (same length: the offsets stay right).
      $s = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
      $s = $s.Replace("/Users/$env:USERNAME/", "/Users/" + ("x" * $env:USERNAME.Length) + "/")
      $s = $s.Replace("\Users\$env:USERNAME\", "\Users\" + ("x" * $env:USERNAME.Length) + "\")
      $bytes = [Text.Encoding]::GetEncoding(28591).GetBytes($s)
    }
    [IO.File]::WriteAllBytes((Join-Path $Out "word-$name.$($f[1])"), $bytes)
    Write-Host "  word-$name.$($f[1]): $($bytes.Length) bytes"
  }
}

$saved = Get-Clipboard -Raw -ErrorAction SilentlyContinue
$word = New-Object -ComObject Word.Application
try {
  $word.Visible = $false
  $word.DisplayAlerts = 0
  $doc = $word.Documents.Add()
  $r = $doc.Range()
  $r.InsertAfter("Quarterly report`r")
  $r.InsertAfter("Sales rose by 12% in the third quarter; the full figures are online.`r")
  $r.InsertAfter("First point`rSecond point`rThird point`r")
  $r.InsertAfter("Apples`rPears`r")
  $r.InsertAfter("`r")
  $r.InsertAfter("After the table" + [char]11 + "on two lines, with a picture: ")
  $doc.Paragraphs.Item(1).Style = $doc.Styles.Item(-2)              # Heading 1
  $p2 = $doc.Paragraphs.Item(2).Range
  $doc.Paragraphs.Item(2).Alignment = 1                              # centered
  $find = { param($text) $x = $p2.Duplicate; [void]$x.Find.Execute($text); $x }
  (& $find "12%").Bold = $true
  $q = & $find "third quarter"; $q.Italic = $true; $q.Font.Color = 255  # red (BGR)
  $q = & $find "full figures"; $q.Font.Size = 14; $q.Font.Name = "Arial"
  [void]$doc.Hyperlinks.Add((& $find "online"), "https://example.com/figures")
  $doc.Range($doc.Paragraphs.Item(3).Range.Start, $doc.Paragraphs.Item(5).Range.End).ListFormat.ApplyNumberDefault()
  $doc.Range($doc.Paragraphs.Item(6).Range.Start, $doc.Paragraphs.Item(7).Range.End).ListFormat.ApplyBulletDefault()
  $t = $doc.Tables.Add($doc.Paragraphs.Item(8).Range, 2, 3)
  $t.Style = "Table Grid"
  $cells = @("Region", "Q2", "Q3", "North", "1,200", "1,344")
  for ($i = 0; $i -lt 6; $i++) { $t.Cell([math]::Floor($i / 3) + 1, ($i % 3) + 1).Range.Text = $cells[$i] }
  $t.Cell(1, 1).Range.Bold = $true
  # A small picture at the end.
  Add-Type -AssemblyName System.Drawing
  $png = Join-Path $env:TEMP "fw-clip-capture.png"
  $bmp = New-Object System.Drawing.Bitmap 16, 16
  for ($x = 0; $x -lt 16; $x++) { for ($y = 0; $y -lt 16; $y++) { $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $x * 16, $y * 16, 128)) } }
  $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  $last = $doc.Paragraphs.Item($doc.Paragraphs.Count).Range
  [void]$doc.InlineShapes.AddPicture($png, $false, $true, $doc.Range($last.End - 1, $last.End - 1))
  Remove-Item $png

  $doc.Content.Copy(); Save-Formats "all"
  $doc.Range($p2.Start + 6, $p2.Start + 33).Copy(); Save-Formats "partial"   # "rose by 12% in the third qu"
  $doc.Range($doc.Paragraphs.Item(1).Range.Start, $doc.Paragraphs.Item(2).Range.End).Copy(); Save-Formats "paragraphs"
  $doc.Range($doc.Paragraphs.Item(3).Range.Start, $doc.Paragraphs.Item(7).Range.End).Copy(); Save-Formats "lists"
  $t.Range.Copy(); Save-Formats "table"
  $doc.Close(0)
} finally {
  if ($saved) { Set-Clipboard -Value $saved }
  $word.Quit([ref]0)
  [void][Runtime.InteropServices.Marshal]::ReleaseComObject($word)
}
