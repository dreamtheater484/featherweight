# The interface's languages

Featherweight's interface comes in English, Dutch (`nl`), German (`de`), French (`fr`) and Spanish
(`es`). Every string a person reads goes through `T()` (src/core/Lang.h); the English text is the key.

- `featherweight.pot`: the template, every string, made from the sources by `python tools/strings/extract.py`.
- `<code>.po`: a language's catalog (gettext). An empty `msgstr` shows the English.
- `glossary.csv`: the terms each language uses for the same thing everywhere, as Microsoft Office does.

The build compiles the catalogs into the exe (`tools/strings/fwstrings.cpp`): one compressed resource
per language, read only when that language is chosen (File > Options > Language, or Windows' language).

## Translating

- Keep placeholders as they are and use each once: `{0}`, `{1}` (a language may reorder them), `%d`,
  `%.1f`. Keep `\t` and what follows it (a shortcut: `\tCtrl+S`) and line breaks `\n`.
- `&` marks an access key, the letter Alt plus a key chooses: keep one `&` per string, before a
  letter of the translation, and don't give two items of the same menu the same letter.
- A `msgctxt` says which meaning is meant ("verb", "ribbon tab").
- Shortcut names (Ctrl, Shift, Alt) are written in English in the catalog; the app shows each
  language's names itself.
- Text a person reads in a narrow place (a ribbon button, a tab) should stay short: the clipping check
  (`tests/screens/lang.cmake`) fails a label that doesn't fit at 1366 × 768.

Each language's voice:

| | Address | Quotation marks | Notes |
|---|---|---|---|
| nl | je (informal, as Windows 11) | “…” | Compounds written together: *paginanummer*. |
| de | Sie | „…“ | Nouns capitalised; infinitive for commands (*Datei öffnen*). |
| fr | vous | « … » | A non-breaking space before : ; ? ! and inside « ». Infinitive for commands. |
| es | tú (as Windows 11) | «…» | Infinitive for commands (*Abrir archivo*); ¿…? and ¡…! |
