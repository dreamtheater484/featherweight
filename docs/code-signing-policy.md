# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

## What is signed

Featherweight's releases on [GitHub](https://github.com/dreamtheater484/featherweight/releases): the program
(`Featherweight.exe`), the PDFium DLLs it ships (`pdfium-standard.dll`, `pdfium-v8.dll`, installed as
`pdfium.dll`), the installer, the MSI and the MSIX package, for x64 and ARM64.

Every signed file is built by GitHub Actions from this repository's source
([`.github/workflows/release.yml`](../.github/workflows/release.yml)), from a version tag on `main`. Nothing built
on a personal computer is signed. Each release is built twice on fresh runners and must come out byte for byte the
same before it is signed ([reproducible builds](reproducible-builds.md)). PDFium is the prebuilt library from
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), pinned by version and SHA-256 in
[`cmake/FetchPdfium.cmake`](../cmake/FetchPdfium.cmake).

## Team roles

- Committers and reviewers: [members of the repository](https://github.com/dreamtheater484/featherweight/graphs/contributors)
  with write access. Changes reach `main` through pull requests.
- Approvers: Roy Clerx ([@dreamtheater484](https://github.com/dreamtheater484)), the project's owner, approves each
  release signing request in SignPath.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it.

Its only network access is the update check, which asks GitHub for the latest release and can be turned off (in
File > Options, or for every user with the "Turn off updates" policy), and the optional form script engine, which
is downloaded from the same GitHub release only after the user agrees. No telemetry, no account, no subscription.
See the [privacy statement](https://dreamtheater484.github.io/featherweight/privacy/).

## How signatures are used

Once releases are signed, Featherweight's updater installs a new version only when every downloaded file carries
a valid signature from the same publisher as the copy that is running, for this product and for the release's
version. An unsigned or differently signed file is refused (`src/Updater.cpp`).
