"""Keeps each language's catalog in step with the template (F1, f1-lang).

  python tools/strings/update.py              every strings/<code>.po
  python tools/strings/update.py de nl        those languages
  python tools/strings/update.py --new de     start strings/de.po (empty translations)

Run after tools/strings/extract.py. For each catalog, in the template's order:
- a string the template still has keeps its translation (and its fuzzy flag);
- a new string is added untranslated;
- a string whose English changed gets the old translation marked fuzzy (a suggestion, not shown:
  the app shows English until a translator confirms it), with the old English as "#| msgid";
- translations of strings the template no longer has stay at the end as obsolete ("#~"), for reuse.
`python tools/strings/check.py --json` lists what's untranslated or fuzzy per language.
"""
import argparse
import difflib
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check import unquote  # noqa: E402
from extract import po_quote  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
POT = os.path.join(ROOT, 'strings', 'featherweight.pot')
NAMES = {'nl': 'Dutch', 'de': 'German', 'fr': 'French', 'es': 'Spanish'}


def read(path):
    """[{ctx, id, str, refs, fuzzy, prev, obsolete}] with the header (empty msgid) left out."""
    entries, cur, field = [], None, None

    def flush():
        nonlocal cur
        if cur and 'id' in cur:
            if cur['id'] or cur.get('ctx'):
                entries.append(cur)
        cur = None

    with open(path, encoding='utf-8') as f:
        for raw in f:
            line = raw.rstrip('\n').rstrip('\r')
            obsolete = line.startswith('#~ ')
            if obsolete:
                line = line[3:]
            if not line.strip():
                flush()
                continue
            if line.startswith('#'):
                if cur and 'str' in cur:
                    flush()
                if cur is None:
                    cur = {'refs': '', 'fuzzy': False, 'prev': None, 'obsolete': False}
                if line.startswith('#:'):
                    cur['refs'] = line[2:].strip()
                elif line.startswith('#,'):
                    cur['fuzzy'] = 'fuzzy' in line
                elif line.startswith('#| msgid '):
                    cur['prev'] = unquote(line[len('#| msgid '):])
                continue
            m = re.match(r'(msgctxt|msgid|msgstr)\s+(".*")$', line)
            if m:
                key = {'msgctxt': 'ctx', 'msgid': 'id', 'msgstr': 'str'}[m.group(1)]
                if key in ('ctx', 'id') and cur and 'str' in cur:
                    flush()
                if cur is None:
                    cur = {'refs': '', 'fuzzy': False, 'prev': None, 'obsolete': False}
                cur[key] = unquote(m.group(2))
                cur['obsolete'] = cur['obsolete'] or obsolete
                field = key
            elif line.startswith('"') and cur is not None and field:
                cur[field] += unquote(line)
    flush()
    for e in entries:
        e.setdefault('ctx', None)
        e.setdefault('str', '')
    return entries


def header(code):
    return ('# Featherweight\'s interface in %s. Made from strings/featherweight.pot by tools/strings/update.py;\n'
            '# translations are edited here (or in a PO editor), strings only in the sources.\n'
            'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Content-Transfer-Encoding: 8bit\\n"\n'
            '"Language: %s\\n"\n' % (NAMES.get(code, code), code))


def write(path, code, template, entries_out, obsolete):
    out = [header(code)]
    for e in entries_out:
        out.append('')
        if e['refs']:
            out.append('#: ' + e['refs'])
        if e['fuzzy']:
            out.append('#, fuzzy')
            if e.get('prev') is not None:
                out.append('#| msgid ' + po_quote(e['prev']))
        if e['ctx'] is not None:
            out.append('msgctxt ' + po_quote(e['ctx']))
        out.append('msgid ' + po_quote(e['id']))
        out.append('msgstr ' + po_quote(e['str']))
    for e in obsolete:
        out.append('')
        if e['ctx'] is not None:
            out.append('#~ msgctxt ' + po_quote(e['ctx']))
        out.append('#~ msgid ' + po_quote(e['id']))
        out.append('#~ msgstr ' + po_quote(e['str']))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


def update(path, code, template, translations=None):
    """Merges `path` (if it exists) and `translations` ({(ctx, id): str}, new ones win) with the template."""
    old = read(path) if os.path.exists(path) else []
    have = {(e['ctx'], e['id']): e for e in old if e['str']}
    if translations:
        for k, s in translations.items():
            if s:
                have[k] = {'ctx': k[0], 'id': k[1], 'str': s, 'fuzzy': False, 'prev': None}
    keys = {(t['ctx'], t['id']) for t in template}
    stale = [e for k, e in have.items() if k not in keys]
    stale_ids = [e['id'] for e in stale]
    out, used, used_ids = [], set(), set()
    counts = {'kept': 0, 'fuzzy': 0, 'new': 0}
    for t in template:
        k = (t['ctx'], t['id'])
        e = {'ctx': t['ctx'], 'id': t['id'], 'refs': t['refs'], 'str': '', 'fuzzy': False, 'prev': None}
        if k in have:
            e['str'], e['fuzzy'], e['prev'] = have[k]['str'], have[k].get('fuzzy', False), have[k].get('prev')
            counts['fuzzy' if e['fuzzy'] else 'kept'] += 1
        else:
            # A changed string: the closest old English not suggested yet (each old translation once).
            near = difflib.get_close_matches(t['id'], [i for i in stale_ids if i not in used_ids], n=1, cutoff=0.8)
            if near:
                s = next(x for x in stale if x['id'] == near[0])
                e['str'], e['fuzzy'], e['prev'] = s['str'], True, s['id']
                used.add((s['ctx'], s['id']))
                used_ids.add(s['id'])
                counts['fuzzy'] += 1
            else:
                counts['new'] += 1
        out.append(e)
    obsolete = [e for e in stale if (e['ctx'], e['id']) not in used]
    write(path, code, template, out, obsolete)
    return counts, len(obsolete)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('codes', nargs='*')
    ap.add_argument('--new', action='store_true', help='create the catalogs named if they do not exist')
    a = ap.parse_args()
    template = read(POT)
    codes = a.codes or sorted(os.path.splitext(os.path.basename(p))[0]
                              for p in glob.glob(os.path.join(ROOT, 'strings', '*.po')))
    for code in codes:
        path = os.path.join(ROOT, 'strings', code + '.po')
        if not os.path.exists(path) and not a.new:
            print(f'{code}: no strings/{code}.po (use --new to start one)')
            continue
        counts, obsolete = update(path, code, template)
        print(f'{code}: {counts["kept"]} translated, {counts["fuzzy"]} fuzzy (English changed), {counts["new"]} untranslated, '
              f'{obsolete} obsolete')
    return 0


if __name__ == '__main__':
    sys.exit(main())
