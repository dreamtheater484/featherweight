# Security policy

## Supported versions

Only the latest release is supported: the latest stable release, and the latest beta when there is one.
Security fixes ship in a new release, never as a patch to an older one, and the built-in updater installs
it. If you turned automatic updates off, download it from
[the releases page](https://github.com/dreamtheater484/featherweight/releases/latest).

## Reporting a vulnerability

Please report privately: on the repository's **Security** tab, choose
[**Report a vulnerability**](https://github.com/dreamtheater484/featherweight/security/advisories/new).
Don't open a public issue for a vulnerability. If private reporting isn't available, open an issue that
asks for a private contact, without any details.

Useful to include: the version (⋯ → **About Featherweight**), Windows' version, the steps, and a file
that shows the problem. A file that crashes Featherweight is welcome; the crash dump from
`%LOCALAPPDATA%\Featherweight\crashes` helps too.

## What happens next

| | Within |
|---|---|
| Acknowledgement of your report | 7 days |
| A first assessment: whether it's a vulnerability, and how severe | 14 days |
| A release with the fix, for high or critical severity | 30 days |
| A release with the fix, for the rest | 90 days |

- You're kept up to date in the private advisory, and credited in it unless you prefer not to be.
- The advisory is published with the release that fixes it, with a CVE where one applies.
- If a fix needs more time, we agree a date with you.
- A vulnerability in PDFium, V8 or another library Featherweight uses goes to its own project too.
  Featherweight releases an update once that project has a fix.

## Scope

In scope: `Featherweight.exe`, its PDF engine as Featherweight ships and uses it, the installer, the
portable zip and the updater. For example:
- a file that makes Featherweight run code, read or write files it shouldn't, or reach the network;
- the updater accepting a file that isn't from the project's releases;
- memory corruption while opening, editing, saving or printing a file.

Out of scope: crashes that need changes to the PC first (a debugger, a changed program file), and
denial of service by a file that is just very large.

## How Featherweight limits the damage

From the design (`IMPLEMENTATION_PLAN.md` §4.7 and §12):
- Documents are untrusted input. Macros are kept but never run, embedded objects are never started,
  fields that could run code are never updated, and external content is never fetched.
- Only `http`, `https` and `mailto` links open. Links to files or programs are blocked.
- The zip and XML readers cap sizes, ratios, entries and nesting; DTDs are rejected.
- Every parser is fuzzed with libFuzzer and AddressSanitizer in CI, and the code is scanned by CodeQL,
  the OpenSSF Scorecard and Coverity Scan.
- The only network access is the update check, which can be turned off. Each update is checked
  against the release's SHA-256 sums, and against the publisher's signature once releases are signed.
- Recovery copies are encrypted for your Windows account.
- Each release lists its components in a CycloneDX SBOM. PDFium is checked weekly for new builds.
