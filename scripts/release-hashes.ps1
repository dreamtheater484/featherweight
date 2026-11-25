# SHA-256 of every file in a folder (not its subfolders), one "<hash>  <name>" line each, sorted by name (f1-ship).
# release.yml uses it to compare two builds and, with -Sums, to write SHA256SUMS.txt, which the updater reads.
param([Parameter(Mandatory)][string]$Dir, [Parameter(Mandatory)][string]$Out, [switch]$Sums)
$ErrorActionPreference = "Stop"
$outName = Split-Path -Leaf $Out
$lines = Get-ChildItem $Dir -File | Where-Object { $_.Name -ne $outName -and -not ($Sums -and $_.Extension -eq ".cer") } |
  Sort-Object Name | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name }
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
