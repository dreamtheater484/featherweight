"""The languages' catalog check (F1, f1-lang; docs/f1-plan.md, gate row "Languages").

  python tools/strings/check.py [strings/de.po ...]     every strings/<code>.po when none is named
  python tools/strings/check.py --build build --json out.json   the 4.1 gate's lang.catalog row

For each translated string of each catalog: the same placeholders as the English ({0}, %d, %.1f), the
same shortcut after a tab, the same number of line breaks, and an access key (&) where the English has
one, before a letter or digit, and nowhere else. Also: no entry the template doesn't have, and every
template string present. Exit 1 with a line per problem.
"""
import argparse
import collections
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
POT = os.path.join(ROOT, 'strings', 'featherweight.pot')

PLACEHOLDER = re.compile(r'\{\d\}|%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsSp]|%%')
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\'}


def unquote(s):
    s = s.strip()
    assert s.startswith('"') and s.endswith('"'), s
    out, i, body = [], 0, s[1:-1]
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            n = body[i + 1]
            if n in ESCAPES:
                out.append(ESCAPES[n])
                i += 2
                continue
            m = re.match(r'[0-7]{1,3}', body[i + 1:])
            if m:
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
        out.append(c)
        i += 1
    return ''.join(out)


def read_po(path):
    """[(context or None, msgid, msgstr, line)]"""
    entries, cur, field = [], None, None
    with open(path, encoding='utf-8') as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            if not line.strip() or line.startswith('#'):
                if cur and 'msgstr' in cur:
                    entries.append(cur)
                    cur = None
                continue
            m = re.match(r'(msgctxt|msgid|msgstr)\s+(".*")$', line)
            if m:
                if m.group(1) in ('msgctxt', 'msgid') and cur and 'msgstr' in cur:
                    entries.append(cur)
                    cur = None
                if cur is None:
                    cur = {'line': n}
                field = m.group(1)
                cur[field] = unquote(m.group(2))
            elif line.startswith('"') and cur is not None and field:
                cur[field] += unquote(line)
    if cur and 'msgstr' in cur:
        entries.append(cur)
    return [(e.get('msgctxt'), e['msgid'], e['msgstr'], e['line']) for e in entries if e['msgid']]


def access_keys(s):
    """Positions of the & that mark an access key: before a letter or digit. && is a literal ampersand, and so
    is one before a space or at the end ("Find & Select" on the ribbon, drawn as written)."""
    out, i = [], 0
    while i < len(s):
        if s[i] == '&':
            if i + 1 < len(s) and s[i + 1] == '&':
                i += 2
                continue
            if i + 1 < len(s) and not s[i + 1].isspace():
                out.append(i)
        i += 1
    return out


def problems_of(en, tr):
    out = []
    # {0} always; printf's %d only in a string whose English has one (elsewhere "10 %" is just text).
    printf = any(p.startswith('%') for p in PLACEHOLDER.findall(en))
    kinds = (lambda t: [p for p in PLACEHOLDER.findall(t) if printf or p.startswith('{')])
    if collections.Counter(kinds(en)) != collections.Counter(kinds(tr)):
        out.append('placeholders differ')
    en_tab, tr_tab = en.find('\t'), tr.find('\t')
    if (en_tab < 0) != (tr_tab < 0) or en_tab >= 0 and en[en_tab:] != tr[tr_tab:]:
        out.append('the shortcut after the tab differs')
    if en.count('\n') != tr.count('\n'):
        out.append('a different number of line breaks')
    ek, tk = access_keys(en), access_keys(tr)
    if len(ek) != len(tk):
        out.append('an access key (&) the English %s' % ('has' if ek else "doesn't have"))
    elif tk and not (tk[0] + 1 < len(tr) and tr[tk[0] + 1].isalnum()):
        out.append('the access key (&) is not before a letter or digit')
    if en.strip() and not tr.strip():
        out.append('only spaces')
    if en.startswith(' ') != tr.startswith(' ') or en.endswith(' ') != tr.endswith(' '):
        out.append('spaces at the start or end differ')
    return out


def check(path, template):
    """(problems, counts): problem lines, and {translated, missing, placeholderErrors, accessKeyErrors, otherErrors,
    untranslated: [English], fuzzy: [English]} (fuzzy: the English changed since it was translated)."""
    import update  # noqa: E402 (it imports this module)
    try:
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
    except ValueError:  # another drive
        rel = path
    seen, out = set(), []
    counts = {'translated': 0, 'placeholderErrors': 0, 'accessKeyErrors': 0, 'otherErrors': 0,
              'untranslated': [], 'fuzzy': []}
    for e in update.read(path):
        if e['obsolete']:
            continue
        ctx, en, tr = e['ctx'], e['id'], e['str']
        key = (ctx, en)
        if key not in template:
            out.append(f'{rel}: not in the template (run tools/strings/update.py): {en!r}')
            counts['otherErrors'] += 1
            continue
        seen.add(key)
        if not tr:
            counts['untranslated'].append(en)
            continue
        if e['fuzzy']:
            counts['fuzzy'].append(en)
            continue
        counts['translated'] += 1
        for p in problems_of(en, tr):
            out.append(f'{rel}: {p}: {en!r} -> {tr!r}')
            kind = 'accessKeyErrors' if 'access key' in p else 'placeholderErrors' if p in (
                'placeholders differ', 'the shortcut after the tab differs', 'a different number of line breaks') \
                else 'otherErrors'
            counts[kind] += 1
    missing = [en for (ctx, en) in sorted(template, key=lambda k: (k[1], k[0] or '')) if (ctx, en) not in seen]
    counts['missing'] = len(missing)
    counts['untranslated'] += missing
    return out, counts


def unmarked():
    """The strings a person reads that don't go through T() (mark.py --check)."""
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'strings', 'mark.py'), '--check'],
                       capture_output=True, text=True, encoding='utf-8')
    return sum(1 for line in r.stdout.splitlines() if "isn't marked" in line)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='*', help='catalogs (default: every strings/<code>.po)')
    ap.add_argument('--build', help='a build folder: each language\'s compressed size from generated/lang/lang_<code>.bin')
    ap.add_argument('--json', help='write the gate\'s summary here (FeatherweightNotes/f1/gate-contract.md, lang.catalog)')
    ap.add_argument('--languages', default='nl,de,fr,es', help='with --json: the languages 4.1 needs')
    ap.add_argument('--max-bytes', type=int, default=40 * 1024, help='with --build: a language\'s budget in the exe')
    a = ap.parse_args()
    template = {(c, e) for c, e, _, _ in read_po(POT)}
    paths = a.paths or sorted(glob.glob(os.path.join(ROOT, 'strings', '*.po')))
    failed = False
    languages = {}
    for path in paths:
        out, counts = check(path, template)
        for line in out:
            print(line)
        code = os.path.splitext(os.path.basename(path))[0]
        size = None
        if a.build:
            bin_path = os.path.join(a.build, 'generated', 'lang', f'lang_{code}.bin')
            size = os.path.getsize(bin_path) if os.path.exists(bin_path) else None
            if size is None or size > a.max_bytes:
                out.append(f'{code}: compiled catalog {"missing" if size is None else f"{size} bytes, over {a.max_bytes}"}')
                print(out[-1])
        print(f'{code}: {counts["translated"]} of {len(template)} strings translated, {len(counts["fuzzy"])} fuzzy '
              f'(English changed), {len(counts["untranslated"])} untranslated ({counts["missing"]} missing from the file), '
              f'{len(out)} problems' + (f', {size} bytes compressed' if size is not None else ''))
        languages[code] = {'strings': len(template), 'translated': counts['translated'],
                           'placeholderErrors': counts['placeholderErrors'], 'accessKeyErrors': counts['accessKeyErrors'],
                           'otherErrors': counts['otherErrors'], 'compressedBytes': size,
                           'untranslated': counts['untranslated'], 'fuzzy': counts['fuzzy']}
        failed |= bool(out)
    if not paths:
        print('no catalogs yet (strings/<code>.po)')
    if a.json:
        # The gate's view: every language 4.1 needs is there, translated whole, and nothing is unmarked.
        n = unmarked()
        for code in a.languages.split(','):
            lang = languages.get(code)
            if not lang or lang['translated'] < lang['strings']:
                print(f'{code}: {"no catalog" if not lang else "not every string translated"}')
                failed = True
        if n:
            print(f'{n} strings a person reads are not marked (python tools/strings/mark.py --check)')
            failed = True
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'languages': languages, 'unmarked': n, 'ok': not failed}, f, ensure_ascii=False, indent=2)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
