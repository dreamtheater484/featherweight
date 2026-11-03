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

`ctest` runs `fwtool` over all of them:

```bash
fwtool roundtrip tests/office/generated tests/office/edge
fwtool roundtrip --reserialize-all tests/office/generated tests/office/edge
fwtool check tests/office/generated tests/office/edge
fwtool roundtrip --expect-reject tests/office/malicious
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
