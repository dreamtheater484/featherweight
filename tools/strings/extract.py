"""The catalog's template (F1, f1-lang): every string the interface translates, from the sources.

  python tools/strings/extract.py            writes strings/featherweight.pot
  python tools/strings/extract.py --check    fails (exit 1) if the template isn't current

A key is a literal inside T(...) (wide, UTF-16 or UTF-8; T(L"Open", L"verb") gives a context),
inside N_(...), or in a declaration of UiText, UiText8 or UiText16 (a table's strings). Entries are
sorted by their text, and name the files they come from without line numbers, so the template only
changes when strings do.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpplex  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
POT = os.path.join(ROOT, 'strings', 'featherweight.pot')
SKIP = {'src/core/Lang.h', 'src/core/Lang.cpp', 'src/ui/WidgetGallery.cpp', 'src/ui/WidgetGallery.h'}
CALL = re.compile(r'(?:\b|::)(T|N_)\(\s*$')
TABLE = re.compile(r'\bUiText(8|16)?\b')
TABLE_PREFIX = {None: ('L',), '8': ('', 'u8'), '16': ('u',)}  # the literals a UiText, UiText8, UiText16 takes
CONTEXT = re.compile(r'\s*,\s*(L|u8|u)?"((?:[^"\\]|\\.)*)"\s*\)')

HEADER = '''# Featherweight's interface strings: the template for every language's catalog (strings/<code>.po).
# Made by tools/strings/extract.py from the sources; don't edit by hand.
msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\\n"
"Content-Transfer-Encoding: 8bit\\n"
'''


def keys():
    """{(context, text): set of files}"""
    found = {}
    for d, _, fs in os.walk(os.path.join(ROOT, 'src')):
        for f in sorted(fs):
            if not f.endswith(('.cpp', '.h', '.inc')):
                continue
            p = os.path.join(d, f)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            if rel in SKIP:
                continue
            code = open(p, encoding='utf-8').read()
            if 'T(' not in code and 'N_(' not in code and 'UiText' not in code:
                continue
            for lit in cpplex.scan(code):
                if lit.test_only or not lit.text:
                    continue
                ctx = None
                if CALL.search(lit.statement):
                    m = CONTEXT.match(lit.after)
                    if m and re.search(r'\bT\(\s*$', lit.statement):
                        ctx = cpplex.decode(m.group(2))
                elif not (TABLE.search(lit.head) and lit.scope != 'function' or TABLE.search(lit.head) and lit.constexpr):
                    continue
                elif lit.prefix not in {p for m in TABLE.finditer(lit.head) for p in TABLE_PREFIX[m.group(1)]}:
                    continue  # a table's other column: {"dataConsolidate", N_(u"a data consolidation")}
                found.setdefault((ctx, lit.text), set()).add(rel)
    return found


def po_quote(s):
    out = s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')
    out = re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f]', lambda m: '\\%03o' % ord(m.group(0)), out)
    return '"' + out + '"'


def render(found):
    out = [HEADER]
    for (ctx, text) in sorted(found, key=lambda k: (k[1], k[0] or '')):
        out.append('')
        out.append('#: ' + ' '.join(sorted(found[(ctx, text)])))
        if ctx is not None:
            out.append('msgctxt ' + po_quote(ctx))
        out.append('msgid ' + po_quote(text))
        out.append('msgstr ""')
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--out', default=POT)
    a = ap.parse_args()
    text = render(keys())
    if a.check:
        old = open(a.out, encoding='utf-8').read() if os.path.exists(a.out) else ''
        if old != text:
            print(f'{os.path.relpath(a.out, ROOT)} is out of date: run python tools/strings/extract.py')
            return 1
        print('template current:', text.count('\nmsgid ') - 1, 'strings')
        return 0
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote', os.path.relpath(a.out, ROOT), text.count('\nmsgid ') - 1, 'strings')
    return 0


if __name__ == '__main__':
    sys.exit(main())
