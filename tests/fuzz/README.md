# Fuzz targets

One target per parser (plan §10). So far:

- `fuzz_zip`: the zip reader and writer, and the package on top of them. Seeds: `tests/office`.
- `fuzz_xml`: `XmlPull`, the DOM and the canonical form. Seeds: `seeds/xml`, small parts taken from the corpus.
- `fuzz_opc`: the relationships and content types parsers, and the part-name helpers. Seeds: `seeds/opc`.
- `fuzz_check`: `fwtool check`'s path over a package: the reference registry, the consistency checker, the id renewer and the extension query. Seeds: `seeds/package`, corpus files written as text (see below), so the fuzzer can change the XML inside them.
- `fuzz_formula`: the formula scanner the registry uses (sheet names and cell references), and every other syntax a reference can have. Seeds: `seeds/formula`.
- `fuzz_drawing`: fwdraw (plan §4.5): DrawingML and VML into the shape model, preset and custom geometry, themes, picture decoding through WIC, metafiles and SVG, and drawing the result into a small software surface. Seeds: `seeds/drawing`, the corpus's drawings and a tiny picture in each format (see below).

`make_seeds.py` makes `seeds/opc`, `seeds/package`, `seeds/formula` and `seeds/drawing` from the corpus (standard library only): `python tests/fuzz/make_seeds.py`.

Besides crashes and AddressSanitizer reports, each target aborts when an invariant breaks:

- `XmlPull` and the DOM accept exactly the same documents.
- Writing the DOM back doesn't change what a document says (its canonical form).
- A kept fragment parses on its own.
- Saving a package without edits copies every entry byte for byte.
- Relationships and content types that Featherweight writes read back the same.
- Renaming references and renewing ids leaves a document that still parses.
- A drawing's extent, ink bounds, wrap outline and text boxes are finite, and a decoded picture stays within its limits.

### Drawings

`fuzz_drawing` reads a picture when the input doesn't start with `<`. Otherwise the input is a drawing's XML (`w:drawing`, `wp:inline`, `w:pict`, a VML shape, `p:spTree` or a shape), and lines `@<name>` start sections after it: `@theme` is the theme, any other name a relationship id, whose content is a picture or a part.

```
<wp:inline xmlns:wp="..." ...>...<a:blip r:embed="rId1"/>...</wp:inline>
@rId1
<svg xmlns="http://www.w3.org/2000/svg" ...>...</svg>
```

### Packages as text

`fuzz_check` reads a zip as it is (an input that starts with `PK`), or parts as text: each part starts on a line `@<part name>`, and its content runs to the next such line.

```
@[Content_Types].xml
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">...</Types>
@word/document.xml
<w:document ...>...</w:document>
```

## Building

Fuzzers get their own build folder. Use RelWithDebInfo: Release adds `/GL`, which the instrumented libraries don't need.

```bash
cmake -S . -B build-fuzz -DFW_FUZZ=ON
```

```bash
cmake --build build-fuzz --config RelWithDebInfo --parallel --target fuzz_zip fuzz_xml fuzz_opc fuzz_check fuzz_formula fuzz_drawing
```

`FW_FUZZ` instruments `fwcore`, `fwpkg`, `fwdraw`, miniz and pugixml with AddressSanitizer and coverage, so keep it out of normal builds.

### Toolchains

- **VS 2022 (17.0 and later):** `/fsanitize=fuzzer`, as Microsoft documents it. This is what CI uses (GitHub's `windows-latest`). AddressSanitizer's runtime is a DLL there (`clang_rt.asan_dynamic-x86_64.dll`, in the MSVC `bin\Hostx64\x64` folder), so that folder must be on the `PATH` to run the targets; the CI job adds it.
- **VS 2019 16.11 (the local toolchain today):** `cl` ignores `/fsanitize=fuzzer` ("unknown option"), but everything it would do is there: the `/fsanitize-coverage=...` flags, `clang_rt.fuzzer_MT-x86_64.lib` (libFuzzer and its `main`) and `libsancov.lib`. `CMakeLists.txt` passes the flags and links the two libraries by hand, and the result is a working libFuzzer build with AddressSanitizer, linked statically. Its first minute found a real bug (`regressions/xml/nul-in-attribute.xml`).
- **Anything else:** `-DFW_FUZZ_ENGINE=replay` builds plain executables that run each file given once, so seeds and regressions can be replayed anywhere.

## Running

`run-fuzzers.ps1` runs every target (or those given with `-Targets`) for a fixed time, with its seeds and regressions, and fails if any finds something. CI runs it on every push with `-Seconds 60`; locally it is the same:

```bash
powershell -File tests/fuzz/run-fuzzers.ps1 -BuildDir build-fuzz -Seconds 60 -WorkDir <work folder> -ArtifactDir <artifact folder>
```

libFuzzer adds the inputs it finds to `<work folder>\<target>` (keep it outside the repository; CI caches it between runs). Crashing inputs and each target's log go to `<artifact folder>\<target>`; CI uploads that folder when the job fails. For long runs, add `-BelowNormal` so the PC stays responsive and `-KeepAwake` so it doesn't sleep while the script runs, and give each parallel run its own folders:

```bash
powershell -File tests/fuzz/run-fuzzers.ps1 -Targets fuzz_xml -Seconds 28800 -BelowNormal -KeepAwake -WorkDir <work folder> -ArtifactDir <artifact folder>
```

A single target can also be run by hand, for example:

```bash
build-fuzz/tests/fuzz/RelWithDebInfo/fuzz_xml.exe -max_total_time=600 -max_len=65536 <work folder> tests/fuzz/seeds/xml tests/fuzz/regressions/xml
```

`ctest --test-dir build-fuzz -C RelWithDebInfo -R fuzz_` replays the seeds and the regressions once, without fuzzing.

## Regressions

Every crash becomes a file in `regressions/<target>` (plan §10), and a unit test where it fits. With `FW_FUZZ` on, `ctest` replays them, and so does every fuzz run.

## Not done yet

- Long runs nightly. CI runs each target for a minute on every push; the D0 gate (8 hours per fuzzer without findings) is run locally.
