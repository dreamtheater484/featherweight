# F1: refinement, and release 4.1

The work plan for F1 (IMPLEMENTATION_PLAN.md §9, "F1. Refinement, after 4.0 (release 4.1)", and decision 29). Each session reads this first. Then it reads:
- §9's F1 table, and §3, §4.7, §10, §11 and §12 of the plan;
- section 9 of the product analysis (`Featherweight product analysis.docx` in `D:\Documents\Codex\Featherweight-business`), and its section 7 for the legal items;
- the work plan of the milestone that built the code it changes.

F1 closes the gaps the analysis found, within the lightweight contract:
- legal hygiene first;
- Word fidelity;
- the interface in four more languages;
- PDF page tools, OCR and signing;
- the product basics: signing, the stores, a safer updater, business deployment, support, a security process and sharing;
- outside review of the claims Featherweight markets;
- V8 as an option, and a Windows on ARM build;
- what 4.0 left in speed and friction.

It ends with release **4.1**.

Roy, 2026-11-24: F1 starts right after release 4.0 is published, and 4.1 is published in one go when F1's automated gate passes, without a manual check by Roy (decision 29). Roy, 2026-11-22: no questions unless unavoidable. Screen-reader accessibility stays out (decision 21). If business buyers become a goal, it gets a milestone of its own.

## Exit criteria

- **Legal hygiene:**
  - **Cloud fonts:** Featherweight never reads Office's cloud fonts (`%LOCALAPPDATA%\Microsoft\FontCache\4\CloudFonts`, read today by `src/draw/CloudFonts.cpp`): not to draw, and not to embed in Export PDF. Nothing derived from those files ships.
  - **Substitutes:** each cloud font gets a metric-matched substitute from the fonts Windows installs. That is the family plus a horizontal scale chosen so that line breaks match Word's (decision 11's method). The missing-font banner shows as today.
  - **Pages still match:** fidelity (below) is measured with the cloud fonts gone. The slide and print SSIM numbers are no lower than 4.0's.
  - **The analysis's other legal items:**
    - The Swedish (LPPL) and Finnish hyphenation patterns ship byte for byte, or are dropped.
    - Word, Excel and PowerPoint appear only as descriptions ("Word documents"), never in a product or feature name.
    - The README says the MIT licence doesn't cover the name and logo.
    - THIRD_PARTY_NOTICES is shown in About and matches the SBOM.
- **Word fidelity:**
  - Pages match Word for ≥ 97 % of the private corpus's distinct documents: 148 of 152. D5 reached 143; see `docs/d5-fidelity.md` for the misses and their causes.
  - Lines stay at 96.8 % or more.
  - Both are measured with `fwtool compare`, without cloud fonts.
- **Languages:** the interface in Dutch, German, French and Spanish, besides English.
  - Every string goes through one catalog (`T()`).
  - The language follows Windows' display language unless one is chosen in File > Options.
  - Only the chosen language is loaded, and none for English. Each language is ≤ 40 KB in the exe, compressed.
  - Every string is translated, with placeholders and access keys consistent.
  - An automated check finds no label clipped at 1366 × 768 (100 %) in any ribbon tab, panel or dialog.
  - Launch to window in German is no more than 5 ms slower than in English.
- **PDF tools:**
  - **Page tools:** in the PDF tab, page thumbnails to reorder, rotate and delete pages; insert pages from another PDF (merge); extract or split pages into new files. All through PDFium's own functions.
  - **OCR:** scanned pages are read through Windows' offline OCR (`Windows.Media.Ocr`) and written as an invisible text layer, so the text can be found, selected and copied.
  - **Signing:**
    - A drawn signature (pen, mouse or a picture) placed as a stamp.
    - Certificate signatures (PAdES baseline B) with a certificate from the Windows store, through CNG.
    - A signed file shows as signed, and a change after signing shows as invalid.
  - **Each output:**
    - passes `fwtool pdfcheck`;
    - reopens without errors in PDFium, in Windows' own PDF parser (`Windows.Data.Pdf`, not PDFium) and in Word;
    - also opens in Acrobat Reader once it's on the reference PC (Roy's step 9).
  - **Accuracy:** OCR gets ≥ 95 % of words right on the generated scan set (English, 300 dpi). Signatures pass an independent validator.
- **Product basics:**
  - **Code signing:** releases are built in GitHub Actions and signed through the SignPath Foundation (free for open-source projects): the exe, the DLL and the installers. Until SignPath approves (Roy's step 1), the same workflow signs with a test certificate, so nothing else waits.
  - **Distribution:** `release.ps1` generates these packages:
    - an MSIX package for the Microsoft Store, whose build is updated by the Store, with its own updater off;
    - the winget, Chocolatey and Scoop manifests.
  - **Updater:**
    - Once the running exe is signed, every download must carry a valid signature from the same publisher (`WinVerifyTrust`). Anything else is refused.
    - Stable and beta channels; the beta channel follows GitHub pre-releases.
    - The previous version is kept for one rollback.
    - An admin can turn it off by policy.
  - **Business deployment:**
    - a per-machine MSI, and silent installs (the MSI and the existing installer);
    - ADMX and ADML policies for updates off, the update channel and the interface language, read from `HKLM\SOFTWARE\Policies\Featherweight` at start (one registry read).
  - **Support:**
    - Help pages and known issues on the project's website (GitHub Pages, from `site/`), opened from Help.
    - A crash writes a local minidump, with at most 5 kept. The next start offers to open the folder and a prefilled issue. The app itself never uploads anything.
  - **Security process:**
    - `SECURITY.md`: private reporting, response times, and the supported versions (the latest release).
    - An SBOM (CycloneDX) for each release, from the pinned dependencies.
    - A PDFium and V8 update routine: a weekly CI check for new PDFium builds that opens an issue, and `scripts/update-pdfium.ps1`.
  - **Sharing:** Send as attachment and Share as PDF through Windows' share sheet, in the three editors and the PDF tab.
  - **Done when:** a signed build installs from winget without warnings, and from the Store once Roy's steps 1 and 2 are done. The updater refuses an unsigned file and a file from another publisher (ctest, offline).
- **External review:**
  - **CodeQL code scanning and the OpenSSF Scorecard** run in CI, weekly and on every release tag. No CodeQL alert of high or critical severity is open at release.
  - **Coverity Scan** runs weekly once its token is set (step 6).
  - **The OpenSSF Best Practices answers** are prepared, for Roy to submit.
  - **Scans:** each release's files are uploaded to VirusTotal and run in Hybrid Analysis. The sandbox report must show network traffic only to GitHub, for the update check.
  - **Reproducible builds:** two CI builds of the release commit on fresh runners give byte-identical files once the signatures are removed. `docs/reproducible-builds.md` says how anyone can reproduce one.
  - **Published:** the release notes carry the size and timing ledger lines. Badges go in the README and on the website. Each release's notes link its scan and sandbox reports.
- **Options:**
  - **V8 optional:** the default download carries PDFium without JavaScript and XFA, about 7.5 MB instead of 32 MB.
    - A PDF with form scripts offers the script component. It's downloaded from the same release only after the user agrees, and checked like an update.
    - Installs that already have V8 keep it.
    - The download is ≤ 10 MB (18.2 MB at 4.0).
  - **Windows on ARM:** an arm64 build is released beside x64, and ctest passes on GitHub's arm64 runner.
- **Speed left from 4.0:** `budgets.ps1 -Timings` puts the worst key in `long-300.docx` under 50 ms. It was 233 ms at 4.0's gate: an Enter in the middle of the document, known since D5, cause not yet found.
- **Friction left from 4.0:**
  - RF2 (a document with comments opens with the Review pane shown, as Word shows its comments) and RF7 (Title in the Styles gallery in one click at 1366 px) take Word's steps or fewer (`docs/friction-review.md`).
  - The new flows are written down there with their steps: merge two PDFs, sign a PDF, OCR a scan, change the language, share as PDF. None takes more steps than Acrobat for the same job.
- **Never damage a file:**
  - The document, workbook and deck torture gates pass on the final head, as at 4.0.
  - A PDF torture run on the PDF corpus (random page operations, OCR and a drawn signature, then save and reopen): no failures, and every output passes the checks above.
  - Saving a PDF without edits stays byte-identical.
  - The new parsers have fuzzers (`fuzz_pdfpages`, `fuzz_catalog`), run 60 s each in CI.
- **Lightweight (§11):**
  - **Exe size:** the exe grows by ≤ 1,024 KB in F1. At 4.0 it's 13,167,616 bytes, which is 12.56 MB as `budgets.ps1` counts (1024-based). So 4.1's exe is ≤ 13.56 MB, under the 15 MB ceiling.
  - **No regressions:** launch time, memory after launch and idle no worse than 4.0's.
  - **Nothing loads before it's used:** `fwtool bench-app`'s module list after launch shows none of OCR, the share sheet, CNG signing, dbghelp, or a language other than the chosen one.
  - **Network:** the only network access stays the update check, plus the V8 component download when the user agrees.
  - **Ledger:** each size step has a ledger line with its reason.
- **Release 4.1:** the gate below passes on the final head, and then the coordinator runs `release.ps1 -Publish`. Roy does no manual check.

## Roy's steps

These need Roy: accounts, identity, money, submissions, and work only a person can do.
- The coordinator sends them to Roy in one message on F1's first day, so their lead times run alongside the work.
- Nothing in F1 waits for them. Each piece builds and tests up to the step with test credentials.
- A step still open at the gate is reported as waiting, not failing.
- Sessions prepare everything a step needs (texts, files, answers) in `FeatherweightNotes\f1\roy-steps\`.

| # | Step for Roy | Prepared by | Until it's done |
|---|---|---|---|
| 1 | Apply to the SignPath Foundation for free open-source code signing. The form asks for the project and its owner; the code signing policy page is in the repository. | f1-ship | 4.1 is built and test-signed in CI, and released unsigned. The first signed build follows as 4.1.x with no other change. |
| 2 | A Microsoft Partner Center developer account, the app name reserved, and the Store submission, which needs Roy's identity and consent. Certification takes days. | f1-ship: the MSIX, listing texts, screenshots, privacy statement | 4.1 ships without the Store. The package waits in the release. |
| 3 | Chocolatey: an account, with its API key as a repository secret. Scoop: an OK for the pull request to the Extras bucket from Roy's GitHub account. winget runs as today (`release.ps1 -Winget`). | f1-ship | Those two manifests wait in `packaging/`. |
| 4 | Two repository settings: private vulnerability reporting on, and GitHub Pages published from Actions. | f1-trust | The site is built but not published. SECURITY.md's reporting link stays inactive. |
| 5 | VirusTotal and Hybrid Analysis: free accounts, with API keys as repository secrets. Hybrid Analysis vets API users. | f1-trust: the upload script | Releases go out, and the scan and sandbox links are added to their notes when the keys come. |
| 6 | Coverity Scan: register the project and set its token as a secret. OpenSSF Best Practices: sign in with GitHub and submit the prepared answers. | f1-trust | The workflow and the answers wait. CodeQL and Scorecard run anyway. |
| 7 | NLnet (NGI Zero): submit the application in Roy's name. A session drafts it. The grant can include a security audit by Radically Open Security. | f1-trust | No effect on 4.1. |
| 8 | Translators: a native speaker reviews each language, as volunteers or through a Weblate project. | f1-lang: drafts, a glossary per language, review files | 4.1 ships the machine-checked translations. Native fixes land in 4.1.x. |
| 9 | Optional: a yes to installing Adobe Acrobat Reader on the reference PC, as a second check of the PDF tools. | f1-gate | Windows' PDF parser and Word stand in. |
| 10 | The business side, not in F1's gate (analysis §10): trade mark clearance and an EU filing; a lawyer's look at the ribbon and the terms; a registered business; the paid Store edition and a sponsor page. | — | No effect on 4.1. |

**What can't be automated, and isn't in the gate:**
- a native speaker's judgement of the translations (step 8);
- Store certification (step 2);
- SmartScreen reputation for direct downloads, which grows with downloads even when a file is signed;
- outside decisions (SignPath, NLnet);
- legal judgement (step 10).

## Reference PC

As in P3 ("Reference PC"):
- Word, Excel and PowerPoint 2021 run as single instances, only in Roy's active session (at the console or over Remote Desktop), under `quiet-pc.lock`, with AutomationSecurity = 3. Never automate or close Roy's own instances.
- The gate's Office open checks run whenever Roy is signed in. That isn't a check by Roy.
- Quiet-PC timings run once, at the end, through the coordinator: 3 runs per scenario, with input tolerated.

Also:
- No downloads without Roy's yes, test-only tools included. CI runners may install build tools (WiX, pyHanko) pinned by version and hash.
- Test certificates are PFX files loaded in memory (`PFXImportCertStore`), never added to a Windows certificate store.
- No changes to system settings. OCR uses the languages Windows already has.

## Pieces

**Integration branch:** `claude/f1-int`, made from the commit released as 4.0. Each session works on its own branch from f1-int, pushes, and says in its turn-end report what's ready to merge.

**The coordinator integrates,** as in P3: merges into `claude/f1-int`, builds Release, runs ctest, pushes, and checks CI. The coordinator also decides which sessions take the pieces, fresh or reused.

| Piece | Session | Weight | Size budget | Command ids |
|---|---|---|---|---|
| Legal hygiene in the engines: cloud fonts out, substitutes, hyphenation files. Word fidelity to 97 %, the Enter in `long-300.docx`, RF2 and RF7 | f1-word | 22 % | 120 KB | 4000–4019 |
| Languages: the catalog (`T()`, extraction, a pseudo-language, compressed resources per language, the language setting), the marking pass over the UI, NL/DE/FR/ES, the clipping check | f1-lang | 15 % | 200 KB (4 × ≤ 40 KB, plus code) | 4020–4039 |
| PDF page tools, OCR, signing (drawn and certificate), the share sheet in every tab | f1-pdf | 20 % | 400 KB | 4040–4119 |
| Shipping: the release workflow in CI with SignPath and test signing, reproducible builds, the updater (signatures, channels, rollback, policy), MSIX, MSI, ADMX, winget, Chocolatey and Scoop, V8 as a component, the PDFium update routine, the arm64 build | f1-ship | 20 % | 80 KB | 4120–4149 |
| Trust and support: CodeQL, Scorecard, Coverity, Best Practices, the VirusTotal and sandbox uploads, SECURITY.md, the SBOM, the website with help and known issues, crash dumps with the opt-in report, the README's legal wording, the NLnet draft | f1-trust | 10 % | 40 KB | 4150–4169 |
| The gate: `scripts/f1-gate.ps1` and its report, the PDF torture run and corpus, the OCR scan set, the signature validator run, the friction scripts for the new flows, budgets | f1-gate | 8 % | 0 (tools only) | — |
| Final gate, release 4.1 | coordinator | 5 % | 0 | — |

**Size:**
- The budgets add up to 840 KB, with 184 KB in reserve (1,024 KB in all).
- Each piece measures with the linker-map method (`$env:LINK='/MAP'` from PowerShell, then p1-show's `mapsplit.py`) and records a ledger line with its reason.

**Command ids:**
- Ids 4170–4199 are in reserve.
- In `src/Commands.h`, each piece adds explicit ids in its own block, under a comment naming the piece.

### File ownership

- **f1-word:**
  - `src/draw/CloudFonts.*` (removed), `src/text/`, `src/word/`, `src/doc/`;
  - the hyphenation data and `tools/notices/`.
- **f1-lang:**
  - `src/core/Lang.*` (new), `tools/strings/`, `strings/` (new), and the language key in `src/Settings.*`.
  - Every UI file, but only in the marking pass (round 1). After that, each piece marks its own new strings.
- **f1-pdf:**
  - `src/pdf/`, with new `src/pdf/tools/`, `src/pdf/ocr/` and `src/pdf/sign/`;
  - `src/Share.*` (new);
  - each editor's File page, only to hook in Share.
- **f1-ship:**
  - `src/Updater.*`, plus `src/Policy.*` and `src/Components.*` (new);
  - `cmake/FetchPdfium.cmake` (arm64, the V8 variant) and the build options in `CMakeLists.txt`;
  - `installer/`, `packaging/`, `scripts/release.ps1`, `scripts/update-pdfium.ps1`;
  - `.github/workflows/release.yml` (new) and the arm64 job in `build.yml`;
  - `docs/reproducible-builds.md`.
- **f1-trust:**
  - `.github/workflows/codeql.yml`, `scorecard.yml` and `coverity.yml`;
  - `SECURITY.md`, `site/`, `src/CrashReport.*` (new), `README.md`, `THIRD_PARTY_NOTICES.md`;
  - `tools/sbom/` and `scripts/scan-release.ps1`.
- **f1-gate:**
  - `scripts/f1-gate.ps1`, new `fwtool` subcommands, `tests/pdf/`;
  - the F1 part of `docs/friction-review.md`.
- **Shared files** (`src/Commands.h`, `src/App*.cpp`, the ribbons, `build.yml`): hook-in edits only, in the piece's own block.

### Order

1. **Round 1: legal hygiene and the catalog.** The coordinator merges round 1 before any other piece's first merge, so every piece builds without cloud fonts and on marked files.
   - f1-word removes the cloud-font reading and puts the substitutes in, with fidelity numbers before and after.
   - f1-lang's marking pass wraps every UI string in `T()`. The pass is scripted and changes no behaviour: ctest and the renders stay identical, and the English launch time is unchanged.
   - f1-trust fixes the wording in the README and the notices.
2. **Roy's steps** go to Roy on day 1, in one message.
3. **Early:**
   - f1-ship's release workflow with test signing, and the updater's signature check: the trust rule the rest relies on.
   - f1-gate's gate script, with every row in it (failing or waiting) from the first week.
4. **Everything else in parallel.**
5. **Translations last within f1-lang.** When the coordinator says the features are in, the strings freeze; then f1-lang translates and runs the clipping check.
6. **The final gate** on the final head, then release 4.1.

### Reuse (share, don't copy)

- **Fonts:** `src/text/Fonts.cpp` (decision 11's substitution and the font banner).
- **PDF:**
  - PDFium's `fpdf_ppo.h` (importing pages), `fpdf_edit.h`, `fpdf_save.h`, `fpdf_annot.h` and `fpdf_signature.h`;
  - the PDF tab's annotation saving (`src/pdf/NoteEditor`, `FormFill`);
  - FileSession's safe save and backups.
- **Sharing:** each editor's Export PDF, for Share as PDF. The side panel and the ribbon come from R1.
- **Hashing and crypto:** the CNG code in `src/pkg/OfficeCrypto`, and the updater's `util::Sha256File`.
- **The updater's test URL** (`g_testUrl` in `src/Updater.cpp`), for offline tests of channels, signatures and rollback.
- **Checks:** `budgets.ps1`, `fwtool bench-app`, `fwtool compare`, `fwtool pdfcheck`, the torture gates, the Office open checks and `quiet-bench.ps1`.

## Source layout

- `src/core/Lang.*`, plus `strings/<lang>.txt`: the catalog sources, compiled at build time into one compressed resource per language.
- `src/pdf/tools/`, `src/pdf/ocr/` and `src/pdf/sign/`: the PDF features.
- `src/Share.*`, `src/Policy.*`, `src/Components.*`, `src/CrashReport.*`.
- `packaging/msix/`, `packaging/wix/`, `packaging/admx/`, `packaging/chocolatey/` and `packaging/scoop/`.
- `site/`: the website, with help, known issues, the privacy statement and the release reports.
- Tests:
  - unit tests in `tests/unit/test_f1_<piece>.cpp`;
  - fuzzers in `tests/fuzz/fuzz_pdfpages.cpp` and `tests/fuzz/fuzz_catalog.cpp`.

## The 4.1 gate (instead of Roy's manual check)

`scripts/f1-gate.ps1` runs on the final head's shipped build and writes `FeatherweightNotes\f1\gate-4.1.md` and `gate-4.1.json`. Each row is passed, failed, or waiting for a Roy step, with its numbers.
- `release.ps1 -Publish` refuses unless that report is for the commit being released and has no failed row. f1-gate adds that check.
- Waiting rows don't block. The release notes list them.

| Row | Check | Pass |
|---|---|---|
| Build and tests | The shipped configuration from a clean tree; ctest (x64 locally, arm64 in CI); CI green on the commit (build, fuzz, CodeQL, Scorecard) | All pass; no open CodeQL alert of high or critical severity |
| Never damage | The torture gates for documents, workbooks and decks, with the Word, Excel and PowerPoint open checks; the PDF torture run; unedited saves | 0 failures, 0 new validator errors, 0 repair prompts; unedited saves byte-identical |
| Fidelity | `fwtool compare` without cloud fonts; the slide and print SSIM sets; S1's cell text | Pages ≥ 148 of 152, lines ≥ 96.8 %; the rest no lower than 4.0's |
| Legal | A file-open hook in `fwtool` during the corpus runs; the source; the fonts embedded by Export PDF on the corpus; notices against the SBOM; the hyphenation files | No path under `FontCache\4\CloudFonts` opened or named; no cloud-font family embedded; notices match; hyphenation as decided |
| Languages | The catalog check for NL/DE/FR/ES (placeholders, access keys); the clipping check over every tab, panel and dialog at 1366 × 768; each catalog's size | 100 % translated, 0 clipped, ≤ 40 KB each |
| PDF tools | Round trips through PDFium, `Windows.Data.Pdf`, Word and `fwtool pdfcheck` (and Acrobat Reader if present); OCR on the scan set; signatures through pyHanko on a CI runner | All open without errors; ≥ 95 % of words; every signature valid, every change after signing caught |
| Product basics | The release workflow's dry run (test-signed, or signed by SignPath after step 1): `signtool verify /pa` on every file; the updater tests (unsigned and wrong-publisher files refused, channels, rollback, the policy); the MSIX through the Windows App Certification Kit; silent per-machine install and uninstall of the MSI and the installer on a CI runner; the winget, Chocolatey and Scoop manifests; the ADMX keys against what the app reads | All pass. SignPath and the Store wait for steps 1 and 2 |
| External review | Two CI builds without their signatures; VirusTotal and Hybrid Analysis (step 5); Coverity (step 6) | Byte-identical; Microsoft Defender clean, and other detections listed in the notes; sandbox traffic only to GitHub |
| Lightweight | `budgets.ps1` on the shipped build; `-Timings -Runs 3 -Record` on the final head; the module list after launch | Exe ≤ 4.0's + 1,024 KB; download ≤ 10 MB; launch, memory and idle no worse than 4.0's; `long-300.docx`'s worst key < 50 ms; no new module at launch |
| Friction | The friction scripts for RF2, RF7 and the new flows | Word's steps or fewer; the new flows within their written targets |
| Docs | The README's badges and wording, SECURITY.md, the site, and the release notes with the ledger lines and report links | Present and linked |

## Rules

As in P3 (its "Rules"), plus:

- **Every new UI string goes through `T()`** from round 1 on. A string added without it fails the catalog check.
- **Every new command id is explicit,** in the piece's own block.
- **Never damage a file** comes first.
  - PDF edits save through FileSession's safe save and backups, as documents do.
  - Editing a signed PDF warns that it breaks the signature.
  - A PDF the page tools can't change safely opens with those tools off, and says why. That covers encrypted PDFs with restrictions, and XFA forms.
- **The network promise is a gate.** Nothing new talks to the network, except the update check and, when the user agrees, the V8 component download, both from the project's GitHub releases.
  - Help and the issue link open the user's browser when the user clicks them.
  - The wording stays exact: "no telemetry, no account, no subscription; the only network access is the update check, which can be turned off".
- **Roy's steps stay Roy's.** Sessions create no accounts or keys, make no submissions or purchases, and change no repository settings. A piece builds up to the step and records "waiting for step N" in its state note.
- **No real keys on the PC.** SignPath signs in its own service. Secrets go into the repository's secrets only by Roy.
- **Lightweight:**
  - A ledger line with a reason for every size step.
  - Windows components (OCR, the share sheet, CNG, dbghelp) load only when used.
  - No new third-party library without the coordinator's OK.
- **Notes:**
  - each piece's state in `D:\Documents\Codex\FeatherweightNotes\f1\<piece>-state.md`, current at every turn end;
  - the material for Roy's steps in `FeatherweightNotes\f1\roy-steps\`.
- **Questions:** decide within this plan and record the decision below. Ask the coordinator, not Roy.

## Decisions during F1

- **Plan, 2026-11-25: cloud fonts give way to Windows' own fonts, with no data taken from Office's files.**
  - The substitutes are fonts Windows installs, scaled to match Word's line breaks on the corpus (decision 11's method).
  - Widths and other metrics taken from the cloud-font files aren't used either. Their licence (fonts for use within Office) is the problem, not only reading them at run time.
  - Fonts embedded in a document are still used within their embedding permissions.
- **Plan, 2026-11-25: one catalog, keyed by the English text.**
  - `T(L"Save")` looks the English text up in the chosen language's table, gettext's model. Strings need no ids, and English costs nothing.
  - A context (`T(L"Open", L"verb")`) separates words that translate two ways.
  - Test builds have a pseudo-language (accented and 40 % longer). It finds unmarked strings and clipping before any translation exists.
- **Plan, 2026-11-25: translations are drafted by f1-lang, checked by machine, and reviewed by people later.**
  - The drafts follow a glossary per language: one term for each Office concept, the words Windows users know.
  - The catalog check, the clipping check and a back-translation check gate them.
  - Native review (step 8) improves them in 4.1.x. It doesn't hold 4.1.
- **Plan, 2026-11-25: releases are built in CI.**
  - SignPath signs only builds whose origin it can verify.
  - `release.yml` builds from the tag, signs, makes the checksums and the SBOM, and drafts the GitHub release.
  - `release.ps1 -Publish`, run locally, checks the gate report and tags. It then waits for the workflow, compares the files with a second CI build, and publishes the draft.
  - The local build stays for testing.
- **Plan, 2026-11-25: the updater trusts what it is.**
  - A signed exe requires the same publisher on every update.
  - An unsigned one (4.0, or 4.1 before step 1) keeps the SHA-256 check.
  - So signing can start at any release, and a compromised GitHub account alone can't push code to signed installs.
- **Plan, 2026-11-25: the Store build is the same exe.**
  - The MSIX carries the same files. Inside a package (`GetCurrentPackageFullName`), the updater is off and the Store updates the app.
  - The paid Store edition (analysis §4.3) is a listing and a price, a business step for Roy, with no code.
- **Plan, 2026-11-25: the app never sends crash reports.** The minidump stays on disk, and the user may attach it to a prefilled issue in their browser. That keeps "no telemetry" exact.
- **Plan, 2026-11-25: no beta release during F1.** Decision 29 publishes 4.1 in one go. The beta channel and rollback are tested against the updater's test URL, not by publishing.
- **f1-word, 2026-11-25: the substitutes live in `src/draw/FontSubstitutes.*`, each with a width scale and a line height.**
  - fwtext's layout, fwdraw's outlined text and the sheet's fast paths share one table. `Fonts::FaceFor` gives a family drawn by a substitute an adjusted face, whoever picked the substitute (a presentation's own substitute picks from the table too). The layout multiplies its width scale into the run's, as `w:w` does.
  - The numbers are measured on Office's renders, never read from the families they replace: line breaks against Word's PDFs for the families documents use, slide SSIM against PowerPoint's for the ones decks use. Line heights come from line pitches in Word's PDFs. `fwtool` takes rows to try from `FW_SUBSTITUTES`.
  - So far: Aptos → Arial at 0.96 and 1.21 em, measured on Word's own PDF of `long-300.docx` (made with `scriptseference-renders.ps1` under quiet-pc.lock: 252 pages; ours 251, 85.0 % of lines, against 278 pages and 44 % with Segoe UI as designed, and 263 pages and 99.2 % with Office's Aptos itself, whose line height our metrics overstated); Aptos Display → Segoe UI at 0.935 and 1.22 em; Open Sans → Segoe UI at 1.06 and 1.36 em; Montserrat → Arial at 1.12; Poppins → Segoe UI at 1.1; Open Sans Light → Calibri as designed (Word draws that name as a missing family: the UW letterhead's lines; slide SSIM almost the same as with Segoe UI Semilight); Open Sans SemiBold → Segoe UI Semibold at 0.9. The other families keep the substitute's own widths until the corpus has them.
  - Word corpus without cloud fonts: 143 of 152 pages, 110,722 of 113,990 lines (4.0 with them: 143, 110,718).
- **f1-word, 2026-11-25: RF7.** A new document's Heading 3 is hidden until Heading 2 is used, as in Word's template, and applying a heading shows the next level in the gallery. The Styles gallery has seven columns, so its middle width (1366 px) shows five; its opened grid keeps six. Together they put Title in the gallery's first row at 1366 px: Normal, No Spacing, Heading 1, Heading 2, Title (either alone doesn't). `tests/friction-checks.ps1`'s CV flow clicks Title one cell to the left in the grid.
- **f1-word, 2026-11-25: the slides that lose their fonts.** On the 18 corpus decks that use families Office downloads, 309 of 338 slides pass SSIM 0.95 without them, against 329 with them (mean SSIM 0.9779 against 0.9843). Layout matches (line breaks, positions); the glyphs' shapes differ, which SSIM at 100 dpi counts on text-heavy slides: World Bank's Open Sans Light deck 7 → 0 of 8, two Montserrat decks 38 → 26 of 38. The other decks don't use those families, so the corpus total drops by the same 20 slides. Workbooks: 3 → 4 of 257 pages pass on the 15 that use them.
- **f1-word, 2026-11-25: RF2.** A document with comments opens with the Review pane, once, when its tab is first shown (a tab's panel closes when another tab is switched to).
- **f1-word, 2026-11-25: Swedish and Finnish hyphenation are dropped.** Their patterns' licences ask for the files as they are (LPPL 1.3 for Swedish; Finnish's "may be freely distributed"), and what was packed were hyph-utf8's comment-free derivatives. Shipping the originals would mean a new download in `cmake/FetchDeps.cmake`; no answer came, so the two languages go: their documents aren't hyphenated automatically (no corpus document has Swedish or Finnish text with automatic hyphenation on). THIRD_PARTY_NOTICES.md and `tools/notices/hyphenation-patterns.md` lose their two sections and the LPPL text. The exe loses the two compressed sets, 15.7 KB (14,972 and 693 bytes).
- **f1-word, 2026-11-25: the Enter in long-300.docx.** An Enter in a numbered paragraph is a structural edit (list labels after it change), and `Flow::Edited` then went back to the first page of the edit's section, for an edit that might change how a section starts. In a one-section document that is page 1: the caret's page could only be shown after laying out everything before it again (233 ms in the middle at 4.0's gate, 546 ms near the end here). It now goes back only for an edit at a section's first item, around a section break, or one that leaves a section break in what it changed; other structural edits restart one page before the change, as plain edits do. `fwtool bench --typing` on long-300: worst key 7.6 ms (Enter 7.6 at the end, 3.1 in the middle), typical 1.3. `fwtool edit-check` (60 edits on the corpus's legal and report folders and long-300) finds the layouts the same as from scratch, except the EPA QAPP's table number (088 against 087 after edit 30, seed 24), which 4.0 has too.
- **f1-trust, 2026-11-25: crash reports hold stacks, not memory.** A minidump of threads, stacks and modules (no heap), so a report holds as little of the open documents as possible; it's written from a fresh thread, with a note the next start reads, and offered after the recovery banner.
- **f1-trust, 2026-11-25: the website is Markdown built by GitHub's own Jekyll action** (`site/`, `pages.yml`), published only from main and only when Pages is on; Help (F1) opens `/featherweight/help/`, and `privacy/` is the Store's privacy statement.
- **f1-trust, 2026-11-25: CodeQL scans what ships.** It traces the shipped exe's build, and results under `third_party/` are dropped before the upload; Scorecard runs on main only, since its action refuses other branches.
- **f1-trust, 2026-11-25: a release scan passes on Defender and the network.** Microsoft Defender clean and sandbox traffic only to GitHub (Windows' own services listed apart); other engines' detections are listed in the notes, not failed.
- **f1-trust, 2026-11-25: the SBOM names PDFium's bundled libraries,** each with an SPDX expression that `make_sbom.py` checks against its licence text, so a PDFium update that changes a licence fails until it's reviewed.
- **f1-trust, 2026-11-25: NLnet gets material, not a proposal.** NLnet asks for proposals "made by humans, not AI" and about AI-written code, so Roy writes it himself from `roy-steps/step-7-nlnet.md`; the next deadline is 3 January 2027.
- **f1-pdf, 2026-11-25: the page tools undo by whole-document snapshots.** Each change keeps the PDF's bytes before it: 30 steps, at most 256 MB. PDFium has no undo of its own, and a snapshot can't get a reordering or a merge subtly wrong. An annotation or form edit clears the page tools' undo, since the snapshots would bring the old values back.
- **f1-pdf, 2026-11-25: PDF saves go through FileSession's safe save, with a backup of the original on the first save over it**, as documents do. An unedited Save as copies the file byte for byte.
- **f1-pdf, 2026-11-25: an edited PDF is written whole; a signed one gets an incremental update** (f1-gate's findings 2-4).
  - Whole, PDFium writes a classic cross-reference table. Its incremental update after a cross-reference stream would lack `/Type /XRef`, and 4.0's always-incremental save appended nearly the whole file even unedited.
  - A signed PDF must be updated after the file, or its `/ByteRange` no longer frames its `/Contents`. Its update gets the missing `/Type /XRef` added, which touches only bytes after the signed revision (`pdftools::SaveDocument`).
- **f1-pdf, 2026-11-25: moving pages pins what they inherit.** `FPDF_MovePages` gives a page its new parent's inherited `/Resources`, `/MediaBox`, `/CropBox` and `/Rotate`. When the page tree has more than one branch, the moved pages get their own copies first, as an incremental update written by `src/pdf/sign`'s object writer (`pdftools::PinInherited`). Pages keep their objects, so links, bookmarks and form widgets still point at them. Where that writer can't read the file (encrypted, or damaged and repaired by PDFium), the rotation and visible area are pinned through PDFium's functions.
  - Inserted pages get the rotation and visible area they had in their source, written on them: an imported page without `/Rotate` would take its new branch's.
  - After an insert the document is written and opened again before the source PDFs close. PDFium's imported pages go on reading streams from their source until then; closing it first crashed a later save (f1-gate's round 2). If that fails, the insert is undone.
- **f1-pdf, 2026-11-25: XFA forms are refused by the page tools and signing in every build**, by the catalog's form type (`FPDF_GetFormType`), not only when PDFium has loaded the form (the V8-less build never does). Their pages are the form's; the page tree is only a stand-in.
- **f1-pdf, 2026-11-25: the tools without a window are `pdftools::Editor`** (`src/pdf/tools/Editor.h`, for fwtool and f1-gate): open, rotate, delete, move, insert, extract, OCR, drawn and certificate signatures, and save. It follows the tab's rules and saves through `pkg::SafeWrite` (FileSession's backups are the app's).
- **f1-pdf, 2026-11-25: extract and split keep how pages look, not their form fields.** PDFium's page import brings the widgets' appearances but not the AcroForm, so fields in the parts are drawn but no longer fillable. Merge (Insert pages) works the same way for the inserted file.
- **f1-pdf, 2026-11-25: recognized text is invisible Helvetica, one text object per word, fitted to the word's box.** The standard font needs nothing embedded. Characters outside Windows-1252 are lost, so OCR in Greek, Cyrillic or Asian languages finds the words' places but not all their letters. OCR uses the languages Windows already has and says so when there's none.
- **f1-pdf, 2026-11-25: certificate signatures are invisible, on the first page, written by our own incremental update.**
  - PDFium can read signatures but not write them, so `src/pdf/sign` has a small strict object reader (xref tables and streams, object streams) and writes the signature field, its widget and the signature dictionary after the file, PAdES B-B (ETSI.CAdES.detached, ESS signing-certificate-v2). The CMS is crypt32's, with the key through CNG.
  - Encrypted and XFA PDFs can't be signed; a banner says why.
  - The certificate picker is a menu of the user's certificates that can sign, not cryptui's dialog, so cryptui never loads.
  - Chains are built from what's on the PC, with no network: a valid signature from a certificate Windows doesn't trust shows as "untrusted", not as invalid.
- **f1-pdf, 2026-11-25: any update after a signature counts as a change.** A signed PDF saved again with anything appended, annotations included, shows "changed after signing"; a signature whose bytes don't match shows "invalid". Acrobat allows some changes (form filling, comments) depending on the signature's permissions; we don't judge which are allowed.
- **f1-pdf, 2026-11-25: a drawn signature is a stamp annotation.** It's drawn with a pen or the mouse, or from a picture, and placed with a click. Removing it is Delete signature on its right-click menu. It isn't a certificate signature, and the panel says so.
- **f1-pdf, 2026-11-25: Share is a section of the File page (KeyTip H), where Word has it, and a button on the PDF toolbar.** Send as attachment shares the saved file; Share as PDF exports a copy to `%TEMP%\Featherweight share` (copies older than a day are removed). A PDF with unsaved changes is shared as a copy with them; an unsaved document asks to be saved first.
- **f1-pdf, 2026-11-25: the PDF strings go through `T()` and `Fmt()` before f1-lang's catalog lands.** `src/pdf/PdfText.h` stands in for both, with core/Lang.h's signatures, until that header is in the branch; then the stand-ins drop out. f1-lang's `mark.py --check` passes on f1-pdf's new files. The PDF syntax files are `// lang: skip-file`.
