# Packaging

How Featherweight is packaged and distributed (F1, f1-ship). Releases are built, signed and drafted in CI by
`.github/workflows/release.yml`; `scripts/release.ps1 -Publish` publishes them. Locally, `scripts/release.ps1` makes
the same packages unsigned, for testing.

| Folder or file | What |
|---|---|
| `package.ps1` | One architecture's packages from built program files: the portable zip, the installer (`installer/Featherweight.iss`), the MSI, the MSIX and the updater's assets |
| `msix/` | The MSIX manifest. Sideloaded packages are signed by the release's publisher; the Store's package is the same files with the identity Partner Center reserves (below) |
| `wix/` | The per-machine MSI (WiX 5): `msiexec /i Featherweight-x.y.z.msi /qn` installs for every user in Program Files, `msiexec /x` removes it |
| `admx/` | Group Policy templates: turn off updates, the update channel, the interface language (`HKLM\Software\Policies\Featherweight`, read by `src/Policy.cpp`) |
| `winget/`, `chocolatey/`, `scoop/` | The package managers' manifests, written by `manifests.ps1` from each release's hashes |
| `signpath/` | SignPath's artifact configurations (Roy's step 1) |
| `ci-tools.ps1`, `test-signing.ps1`, `msix-wack.ps1`, `install-test.ps1` | CI only: tools pinned by version and hash, the test certificate used until SignPath signs, the App Certification Kit, silent installs and uninstalls |

## Silent installs

- MSI, per machine: `msiexec /i Featherweight-x.y.z.msi /qn` (ARM64: `Featherweight-x.y.z-arm64.msi`).
- Installer, per user: `Featherweight-x.y.z-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.
- Installer, per machine (as an administrator): add `/ALLUSERS`.

A copy in Program Files doesn't update itself; deploy new versions the same way, and turn the update check off with
the "Turn off updates" policy.

## The Store's package

Partner Center reserves the package identity (Roy's step 2). Build the Store's MSIX from a release's signed program
files with it; it goes up unsigned, and the Store signs it:

```powershell
packaging\package.ps1 -Stage <folder with Featherweight.exe and pdfium.dll> -Arch x64 -Version 4.1.0 -Out store `
  -Kinds Msix -MsixName <Package/Identity/Name> -MsixPublisher "<Package/Identity/Publisher>" `
  -MsixPublisherDisplayName "<PublisherDisplayName>"
```

## The script engine

Packages carry PDFium without JavaScript and XFA (about 7.5 MB). A PDF whose form has scripts offers the script
engine; after the user agrees, the app downloads `pdfium-v8.dll` (or `pdfium-v8-arm64.dll`) from its own version's
release, checks it like an update and uses it from the next start. Installs that have it keep it when they update.
