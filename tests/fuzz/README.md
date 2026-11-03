# Fuzz targets

One target per parser (plan §10). So far:

- `fuzz_zip`: the zip reader and writer, and the package on top of them. Seeds: `tests/office`.
- `fuzz_xml`: `XmlPull`, the DOM and the canonical form. Seeds: `seeds/xml`, small parts taken from the corpus.

Besides crashes and AddressSanitizer reports, each target aborts when an invariant breaks:

- `XmlPull` and the DOM accept exactly the same documents.
- Writing the DOM back doesn't change what a document says (its canonical form).
- A kept fragment parses on its own.
- Saving a package without edits copies every entry byte for byte.

## Building

Fuzzers get their own build folder. Use RelWithDebInfo: Release adds `/GL`, which the instrumented libraries don't need.

```bash
cmake -S . -B build-fuzz -DFW_FUZZ=ON
cmake --build build-fuzz --config RelWithDebInfo --target fuzz_zip fuzz_xml
```

`FW_FUZZ` instruments `fwcore`, `fwpkg`, miniz and pugixml with AddressSanitizer and coverage, so keep it out of normal builds.

### Toolchains

- **VS 2022 (17.0 and later):** `/fsanitize=fuzzer`, as Microsoft documents it. Not tried yet: VS 2022 isn't installed on the development PC (decision 14).
- **VS 2019 16.11 (the local toolchain today):** `cl` ignores `/fsanitize=fuzzer` ("unknown option"), but everything it would do is there: the `/fsanitize-coverage=...` flags, `clang_rt.fuzzer_MT-x86_64.lib` (libFuzzer and its `main`) and `libsancov.lib`. `CMakeLists.txt` passes the flags and links the two libraries by hand, and the result is a working libFuzzer build with AddressSanitizer. Its first minute found a real bug (`regressions/xml/nul-in-attribute.xml`).
- **Anything else:** `-DFW_FUZZ_ENGINE=replay` builds plain executables that run each file given once, so seeds and regressions can be replayed anywhere.

## Running

```bash
build-fuzz/tests/fuzz/RelWithDebInfo/fuzz_xml.exe -max_total_time=600 -max_len=65536 <work folder> tests/fuzz/seeds/xml tests/fuzz/regressions/xml
```

```bash
build-fuzz/tests/fuzz/RelWithDebInfo/fuzz_zip.exe -max_total_time=600 -max_len=200000 <work folder> tests/office/generated tests/office/edge tests/office/malicious
```

libFuzzer writes new inputs into the first folder, so keep it outside the repository. Add `-artifact_prefix=<folder>/` to choose where crash files go.

`ctest --test-dir build-fuzz -C RelWithDebInfo` replays the corpus, the seeds and the regressions once, without fuzzing.

## Regressions

Every crash becomes a file in `regressions/zip` or `regressions/xml` (plan §10), and a unit test where it fits. With `FW_FUZZ` on, `ctest` replays them.

## Not done yet

- The D0 gate is 8 hours per fuzzer without findings. So far each target has run for 10 minutes at a time.
- Fuzzing in CI (short runs on every push, long runs nightly). GitHub's `windows-latest` has VS 2022, so `/fsanitize=fuzzer` will work there directly.
