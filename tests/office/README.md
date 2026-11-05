# Office corpus (public)

Files for the never-damage tests (plan §10), made by `make_test_office.py`:

- `generated/`: documents, workbooks and decks made with python-docx, openpyxl and python-pptx.
- `edge/`: hand-made files that must round-trip:
  - Markup Compatibility (`mc:AlternateContent`, `mc:Ignorable`)
  - `extLst` extensions and repeated unknown attributes (`x14ac:dyDescent`)
  - unknown namespaces and custom XML with every XML corner: CRLF line ends, comments, processing instructions, CDATA, character references
  - a UTF-16 part, and part names with a space and a non-ASCII letter
  - unusual zip layouts: stored entries, data descriptors, zip64 headers, comments and extra fields
- `malicious/`: hand-made files that must be refused with a plain message:
  - zip bombs: by compression ratio, by declared size, by a lying header, and by overlapping entries
  - a nesting bomb, and a DTD with an entity-expansion bomb
  - missing content types, zip encryption, duplicate part names
  - broken XML, a file that isn't a zip, and a truncated file
- `reference/`: Office's own PDF of every file in `generated/` and `edge/`, plus `manifest.json`: the Office build, page count, the fonts Office drew with, and each source file's hash. These are the reference renders (plan §10) that `fwtool compare` measures Featherweight against. `scripts/reference-renders.ps1` makes them on the Office reference PC; run it again when a file changes or Office changes noticeably.
- `broken/`: hand-made files that open and save, but that `fwtool check` must reject, each for one consistency rule (plan §4.3, rule 2):
  - a relationship id that doesn't resolve, a duplicate paragraph id, a duplicate slide id
  - a bookmark that starts but doesn't end, a field that begins but doesn't end
  - a comment reference without its comment, a formula that names a missing sheet
  - a stale calculation chain (it names a cell without a formula)

`ctest` runs `fwtool` over all of them:

```bash
fwtool roundtrip tests/office/generated tests/office/edge
fwtool roundtrip --reserialize-all tests/office/generated tests/office/edge
fwtool check tests/office/generated tests/office/edge
fwtool check --expect-reject tests/office/broken
fwtool roundtrip --expect-reject tests/office/malicious
```

`fwtool check` applies the reference registry (`src/pkg/References.h`, rules in `ReferenceRules.cpp`): every reference resolves, ids are unique, pairs are complete, and the derived data it can see (the calculation chain, chart caches and embedded workbooks, VML notes) matches its sources. What Office tolerates, such as a missing style, is a warning. It also lists the extensions it doesn't know, near which structural edits will be blocked.

## Real-world corpus (private)

`real-world.json` lists real-world Word documents (blank templates and published documents from governments, universities, international organizations and publishers) for D1's fidelity measurements against Word (plan §10). The files aren't committed, because their licences vary. `scripts/fetch-corpus.ps1` downloads them into the folder that `FW_CORPUS` names, checks each hash, and screens every file before anything opens it: only allowed hosts over HTTPS, `.docx` only, no macros, ActiveX, OLE objects, attached templates, external links other than plain hyperlinks, or risky fields, then `fwtool check` and Microsoft Defender. `scripts/reference-renders.ps1 -Corpus` makes Word's PDFs:

```
<FW_CORPUS>\docx\<category>\<id>.docx
<FW_CORPUS>\reference\docx\<category>\<id>.docx.pdf
<FW_CORPUS>\reference\manifest.json
```

Each entry records the source, terms of use, hash, language, producer, Word's compatibility mode and tags for the structures it contains, so tests can pick files by tag. Rejected candidates stay in the manifest's `rejected` list with their reasons.

```bash
powershell -ExecutionPolicy Bypass -File scripts/fetch-corpus.ps1 -Out D:/FeatherweightCorpus
```

```bash
powershell -ExecutionPolicy Bypass -File scripts/reference-renders.ps1 -Corpus D:/FeatherweightCorpus
```

## Regenerating

Use a venv, never the global Python. From the repository root:

```bash
python -m venv .venv
```

```bash
.venv/Scripts/python -m pip install -r tests/office/requirements.txt
```

```bash
.venv/Scripts/python tests/office/make_test_office.py
```

The output is deterministic: document dates and zip timestamps are fixed, so regenerating changes nothing unless the script or a pinned library changes. `.gitattributes` keeps git from touching line ends in these files.
