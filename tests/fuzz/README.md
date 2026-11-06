# Fuzz targets

One target per parser (plan §10). So far:

- `fuzz_zip`: the zip reader and writer, and the package on top of them. Seeds: `tests/office`.
- `fuzz_xml`: `XmlPull`, the DOM and the canonical form. Seeds: `seeds/xml`, small parts taken from the corpus.
- `fuzz_opc`: the relationships and content types parsers, and the part-name helpers. Seeds: `seeds/opc`.
- `fuzz_check`: `fwtool check`'s path over a package: the reference registry, the consistency checker, the id renewer and the extension query. Seeds: `seeds/package`, corpus files written as text (see below), so the fuzzer can change the XML inside them.
- `fuzz_formula`: the formula scanner the registry uses (sheet names and cell references), and every other syntax a reference can have. Seeds: `seeds/formula`.
- `fuzz_docx`: the Word document model (`src/doc`): the reader, what pagination asks of the model (runs, inline items, resolved properties, list labels, fields, tables) and the writer. Seeds: `seeds/docx`, a small hand-written document with every structure the model reads, and `seeds/package`. Inputs are read like `fuzz_check`'s.
- `fuzz_drawing`: fwdraw (plan §4.5): DrawingML and VML into the shape model, preset and custom geometry, themes, picture decoding through WIC, metafiles and SVG, and drawing the result into a small software surface. Seeds: `seeds/drawing`, the corpus's drawings and a tiny picture in each format (see below).
- `fuzz_doc`: the Word 97-2003 reader (`src/doc/msdoc`): a compound file, or its streams one after another (a byte naming each, a 32-bit length, the bytes), read into a .docx that must open as a package, pass the reference check and read into the model. Seeds: `seeds/doc`, Word's `.doc` copies of small corpus documents.
- `fuzz_layout`: pagination (`src/doc/layout`): the document read into the model, laid out a page at a time with the PC's fonts, hit-tested and dumped. Seeds as `fuzz_docx`'s.
- `fuzz_picture`: inserting a picture into a Word document (`src/doc/edit/Pictures.cpp`, D3) with the app's picture limits, then changes a hash of the input picks (size, rotation, crop, wrap, place, alt text, reset, a second copy, undo), a save as the app saves, the file read back, and everything undone and redone. Seeds: `seeds/picture`, a tiny picture in each format (and an icon, which is refused).
- `fuzz_rtf`, `fuzz_html`: pasting from the clipboard (`src/doc/edit/Clip.cpp`): RTF or HTML read, written back in both formats and read again, then pasted into a small document through the editor. Seeds: `tests/clipboard`, what Word, LibreOffice and Chromium put on the clipboard.
- `fuzz_clip`: copying and pasting within Featherweight: a document's main story copied, written as RTF and as HTML and read back, then pasted over itself. Seeds as `fuzz_docx`'s.
- `fuzz_sfnt`: Export PDF's font subsetter (`src/draw/Sfnt.cpp`, D4): a font read as the export reads it, subset to glyphs a hash of the input picks, and embedded whole as a CFF font is. A subset must read back with the same glyph count, have right checksums, and subset again to the same bytes. Seeds: `seeds/font` (tiny TrueType, collection and CFF fonts), and locally up to 40 fonts of 256 KB or less from `C:\Windows\Fonts`, which `run-fuzzers.ps1` copies into the work folder (fonts' licences keep them out of the repository).

`make_seeds.py` makes `seeds/opc`, `seeds/package`, `seeds/formula`, `seeds/drawing` and `seeds/picture` from the corpus (standard library only): `python tests/fuzz/make_seeds.py`.

Besides crashes and AddressSanitizer reports, each target aborts when an invariant breaks:

- `XmlPull` and the DOM accept exactly the same documents.
- Writing the DOM back doesn't change what a document says (its canonical form).
- A kept fragment parses on its own.
- Saving a package without edits copies every entry byte for byte.
- Relationships and content types that Featherweight writes read back the same.
- Renaming references and renewing ids leaves a document that still parses.
- Every part the document model writes back equals what it read, after canonicalization; runs and inline items stay inside their paragraph's text.
- Pagination ends, within its page cap; every page has a size, and hit tests and carets land on the page and in the text.
- A drawing's extent, ink bounds, wrap outline and text boxes are finite, and a decoded picture stays within its limits.
- A picture is inserted or refused with a reason and nothing changed; one inserted is a picture the tools change; the saved file opens without package errors and has the pictures, each one the tools change; undoing everything gives the new document back (no picture, no media part), and redoing gives the pictures back.
- A pasted document is well-formed, passes the package checker when saved (if it did before), opens again with the same paragraphs, and one undo restores it exactly; a refused paste changes nothing.
- A document copied as RTF or HTML reads back with the same paragraphs, list labels, text and character formatting.

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
cmake --build build-fuzz --config RelWithDebInfo --parallel --target fuzz_zip fuzz_xml fuzz_opc fuzz_check fuzz_formula fuzz_docx fuzz_drawing fuzz_layout fuzz_picture fuzz_rtf fuzz_html fuzz_clip fuzz_sfnt
```

`FW_FUZZ` instruments `fwcore`, `fwpkg`, `fwdoc`, `fwdraw`, miniz and pugixml with AddressSanitizer and coverage, so keep it out of normal builds.

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
