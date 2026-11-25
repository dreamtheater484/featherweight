# Reproducible builds

Anyone can check that a Featherweight release was built from its source: build the same commit with the same
compiler, and the program files come out byte for byte the same as the release's, once its signatures are removed.

The release workflow (`.github/workflows/release.yml`) checks this for every build: it builds each commit twice,
on two fresh GitHub runners, and fails if the program files differ ("reproducible (compare)").

## What is reproducible

- `Featherweight.exe`, for x64 and ARM64.
- `pdfium-standard.dll` and `pdfium-v8.dll`: PDFium's prebuilt DLLs from
  [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), pinned by build number and SHA-256 in
  `cmake/FetchPdfium.cmake`. The release copies them unchanged, apart from the signature.

The installer, MSI, MSIX and zip wrap these files with their own timestamps, so compare the program files inside
them (the zip and the installer's install folder have the same files).

## How the build is made deterministic

- `/Brepro` for the compiler and the linker (`FW_REPRODUCIBLE`, on by default in `CMakeLists.txt`): the timestamps
  in the object files and the PE header are replaced by hashes of the content.
- `/PDBALTPATH:%_PDB%`: the exe names its PDB without the build folder.
- No `__DATE__`, `__TIME__` or `__FILE__` in the program's sources.
- Every dependency is pinned by version and SHA-256 (`cmake/FetchDeps.cmake`, `cmake/FetchPdfium.cmake`).

Two local builds of the same commit in different build folders give the same `Featherweight.exe`, and release.yml
checks two runners on every build. The source folder can still show up in the binary through the compiler's
paths, so check out into the same folder as the release build to be safe: `D:\a\featherweight\featherweight` on
GitHub's runners.

## Checking a release

You need Windows, Git, CMake and the Visual Studio version the release was built with: release.yml runs on
GitHub's `windows-2022` image (x64) and `windows-11-arm` (ARM64); the run's log names the exact MSVC version in its
"Build" step.

```powershell
git clone https://github.com/dreamtheater484/featherweight D:\a\featherweight\featherweight
cd D:\a\featherweight\featherweight
git checkout v4.1.0
cmake -S . -B build-release -A x64 -DFW_BUILD_TESTS=ON -DFW_TEST_SCREENS=OFF -DFW_WITH_V8=OFF -DFW_PDFIUM_COMPONENT=ON
cmake --build build-release --config Release --target Featherweight
```

Then remove the signature from the released file and compare (`signtool` comes with the Windows SDK):

```powershell
Copy-Item Featherweight.exe released.exe            # from the release, or from the installed folder
signtool remove /s released.exe                     # strips the Authenticode signature
(Get-FileHash released.exe).Hash -eq (Get-FileHash build-release\Release\Featherweight.exe).Hash
```

`True` means the release is exactly this source. For ARM64 use `-A ARM64` and `Featherweight-arm64.exe`.

A release before SignPath's signing starts (4.1, until Roy's step 1 in `docs/f1-plan.md`) is unsigned, so there's
nothing to remove: compare the hashes directly, or with `SHA256SUMS.txt`.
