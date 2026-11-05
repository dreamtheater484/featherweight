<#
.SYNOPSIS
  The contract review (D4's exit check): a contract with tracked changes and comments is reviewed
  in Featherweight's editor and saved, then Word opens the saved file. Word must not offer a
  repair, and must show what the review left: the changes accepted or rejected gone (their text as
  decided), the others still tracked, a reply under the comment it answers, and that thread
  resolved.

  The review, on tests\office\torture\review-contract.docx (two reviewers' insertions, deletions, a
  deleted paragraph mark, formatting changes; three comments, one a reply, one resolved):
  - accept "Bolt Industries" -> "Bolt Industries B.V."
  - reject the inserted ", on the dates agreed in writing"
  - accept 60 -> 30 days
  - reject the deleted paragraph mark (the two paragraphs stay apart)
  - reply to "Our standard is 30 days. Please confirm.", and resolve that thread
  - save
  fwtool torture --replay runs the editor operations the Review pane runs (Editor::ReviewChanges,
  edit::ReplyToComment, edit::ResolveComment) and saves as the app does, checking the saved file
  reads back as edited. fwvalidate compares it with the original, and scripts\word-open-check.ps1
  -Review opens it in Word (the Office reference PC) and reads Word's paragraphs, revisions and
  comments.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\contract-review-check.ps1
  # the reviewed file and Word's report in build\contract-review
#>
param(
  [string]$Fwtool = "build\tools\fwtool\Release\fwtool.exe",
  [string]$Validator = "build\validate\fwvalidate.exe",
  [string]$Contract = "tests\office\torture\review-contract.docx",
  [string]$Out = "build\contract-review"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$doc = (Resolve-Path $Contract).Path
$utf8 = [Text.UTF8Encoding]::new($false)

# The review as editor operations at the model's positions (paragraph:offset; the model's text
# holds deleted text too, so "Bolt IndustriesBolt Industries B.V." is one range).
$steps = @(
  'command 1:59 1:94 "review=accept"  # Bolt Industries -> Bolt Industries B.V.',
  'command 3:58 3:90 "review=reject"  # the inserted ", on the dates agreed in writing"',
  'command 5:36 5:40 "review=accept"  # 60 -> 30 days',
  'command 6:43 6:43 "review=reject"  # the deleted paragraph mark',
  'command 0:0 0:0 "reply=20"  # a reply to "Our standard is 30 days. Please confirm."',
  'command 0:0 0:0 "resolve=20:1"  # its thread resolved'
)
$txt = Join-Path $Out "review.txt"
$sha = (Get-FileHash -LiteralPath $doc -Algorithm SHA256).Hash.ToLowerInvariant()
$header = @("# contract review (scripts\contract-review-check.ps1)", "file: $($doc -replace '\\', '/')", "sha256: $sha", "seed: 0", "round: 0")
[IO.File]::WriteAllLines($txt, [string[]]($header + $steps), $utf8)
$saved = Join-Path $Out "review-replay.docx"
if (Test-Path $saved) { Remove-Item $saved }

"== Featherweight: the review, saved"
& $Fwtool torture --replay $txt
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $saved)) { throw "The review didn't go through in fwtool (see above)." }
$problems = @()
if (Test-Path $Validator) {
  $v = & $Validator --compare $doc $saved
  if ($LASTEXITCODE -ne 0) { $problems += "new validator errors: " + (($v | Where-Object { "$_" -match '^\s{4}' }) -join "; ") }
}

"== Word: open the saved file, read its changes and comments"
$json = Join-Path $Out "word.json"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "word-open-check.ps1") $saved -Review -Json $json
$r = (Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json).files | Select-Object -First 1
if ($r.status -ne "ok") { $problems += "Word: $($r.status) $($r.error) $(@($r.dialogs | ForEach-Object { $_.text }) -join ' ')" }

if ($r.review) {
  # Word's text without its marks for comments and fields (control characters).
  $clean = { param($s) ("$s" -replace '[\x00-\x08\x0B-\x1F]', '').Trim() }
  $paras = @($r.review.paragraphs | ForEach-Object { & $clean $_ })
  $want = @{  # by paragraph index (an [ordered] dictionary's [int] is a position, not a key)
    1 = "This agreement is made between Acme Ltd (the Supplier) and Bolt Industries B.V. (the Client)."
    3 = "The Supplier provides the services described in Schedule 1."
    5 = "The Client pays each invoice within 30 days."
    6 = "Late payments carry interest at 2% a month."
    7 = "Interest is charged from the due date."
  }
  if ($paras.Count -ne 10) { $problems += "Word shows $($paras.Count) paragraphs, not 10" }
  foreach ($i in @($want.Keys | Sort-Object)) {
    if ($paras[$i] -ne $want[$i]) { $problems += "paragraph ${i}: Word shows '$($paras[$i])', not '$($want[$i])'" }
  }
  # What's left tracked: Sam Okafor's formatting of the term clause only (no insertion or deletion).
  $revs = @($r.review.revisions)
  if (-not $revs.Count) { $problems += "Word shows no tracked changes; the term clause's formatting changes should still be there" }
  foreach ($v in $revs) {
    if ($v.type -in 1, 2) { $problems += "an insertion or deletion is still tracked: '$(& $clean $v.text)' by $($v.author)" }
    if ($v.author -ne "Sam Okafor") { $problems += "a tracked change by $($v.author): '$(& $clean $v.text)'" }
  }
  # Comments: the three, and the reply, under "Our standard is 30 days", whose thread is resolved.
  $notes = @($r.review.comments)
  $first = $notes | Where-Object { (& $clean $_.text) -eq "Our standard is 30 days. Please confirm." } | Select-Object -First 1
  $reply = $notes | Where-Object { $_.author -eq "Torture" } | Select-Object -First 1
  $agreed = $notes | Where-Object { (& $clean $_.text) -eq "Agreed, 30 days works for us." } | Select-Object -First 1
  $months = $notes | Where-Object { (& $clean $_.text) -eq "Should this say calendar months?" } | Select-Object -First 1
  if ($notes.Count -ne 4) { $problems += "Word shows $($notes.Count) comments, not 4" }
  if (-not $first) { $problems += "the comment 'Our standard is 30 days' is gone" }
  elseif (-not $first.done) { $problems += "the thread 'Our standard is 30 days' isn't resolved" }
  if (-not $reply) { $problems += "the new reply isn't there" }
  elseif ($first -and $reply.parent -ne $first.index) { $problems += "the new reply answers comment $($reply.parent), not $($first.index)" }
  elseif ((& $clean $reply.text) -ne "A reply from the torture test") { $problems += "the reply says '$(& $clean $reply.text)'" }
  if ($agreed -and $first -and $agreed.parent -ne $first.index) { $problems += "'Agreed, 30 days' no longer answers 'Our standard is 30 days'" }
  if (-not $months) { $problems += "the comment 'Should this say calendar months?' is gone" }
  elseif (-not $months.done) { $problems += "'Should this say calendar months?' isn't resolved any more" }
} elseif ($r.status -eq "ok") {
  $problems += "Word opened the file but its changes and comments couldn't be read"
}

""
if ($problems.Count) {
  "FAIL: the contract review"
  $problems | ForEach-Object { "  - $_" }
  exit 1
}
"ok: Word opens the reviewed contract without a prompt and shows the review: $($want.Count) paragraphs as decided, $(@($r.review.revisions).Count) formatting change(s) still tracked, 4 comments with the reply under its thread, resolved."
exit 0
