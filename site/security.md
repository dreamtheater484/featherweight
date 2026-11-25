---
title: Security
description: How to report a vulnerability in Featherweight, and how it keeps documents from doing harm.
---

# Security

## Report a vulnerability privately

On GitHub, open the repository's **Security** tab and choose
[**Report a vulnerability**]({{ site.repo }}/security/advisories/new). Please don't open a public issue for a
vulnerability. The full policy, with response times and what's in scope, is in
[SECURITY.md]({{ site.repo }}/blob/main/SECURITY.md).

Only the latest release is supported: fixes ship in a new release, and Featherweight's updater installs it.

## How Featherweight limits what a document can do

- Documents are treated as untrusted. Macros are kept but never run, embedded objects are never started, fields that
  could run code are never updated, and nothing a document links to on the web is fetched.
- Only web and mail links open; links to files or programs are blocked.
- The readers cap sizes, ratios, entries and nesting, so a malicious file fails with a message instead of hanging.
- The only network access is the update check, and each update is checked against the release's SHA-256 sums.

## How it's checked

- Every file reader is fuzzed with libFuzzer and AddressSanitizer, on every change.
- The code is scanned by [CodeQL]({{ site.repo }}/security/code-scanning) and the
  [OpenSSF Scorecard](https://securityscorecards.dev/viewer/?uri=github.com/dreamtheater484/featherweight).
- Each release's files are scanned by VirusTotal and run in a sandbox; the reports are linked from the
  [release notes]({{ site.repo }}/releases).
- Each release lists its components in a CycloneDX SBOM, and the PDF engine (PDFium) is checked weekly for updates.
