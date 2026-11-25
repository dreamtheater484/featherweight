"""The marking pass (F1, f1-lang; docs/f1-plan.md "Order", round 1): wraps the interface's strings in
T() so the catalog (src/core/Lang.h) can translate them.

  python tools/strings/mark.py --report out.tsv [paths...]   what it would mark, and why
  python tools/strings/mark.py --apply [paths...]            marks them in place

Without paths it walks src/. A string is marked when it reads as words for people ("Increase font
size", "Bold", "Page %d of %d") and nothing around it says it's data: a comparison, an XML name, a
setting's key, a font, a file pattern, a script op, a log line. Engine folders (file formats,
layout, calculation) mark only sentences that reach the user. Inside a function the literal becomes
T(L"..."); in a table that's built before main (namespace scope, constexpr) it becomes N_(L"...")
and the code that shows it calls T() on it (done by hand; the catalog check lists those tables).

Lines can opt out with a comment: `// lang: no` (this line) or `// lang: skip-file` (anywhere in the
file). Re-running it is safe: marked literals are skipped.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpplex  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

# Folders whose strings are data first: only sentences shown to the user are marked there.
ENGINE = [
    'src/pkg', 'src/doc/model', 'src/doc/docx', 'src/doc/odf', 'src/doc/msdoc', 'src/doc/layout',
    'src/doc/proof', 'src/doc/edit', 'src/text', 'src/draw', 'src/chart', 'src/sheet/xlsx',
    'src/sheet/xls', 'src/sheet/ods', 'src/sheet/calc', 'src/sheet/numfmt', 'src/sheet/condfmt',
    'src/sheet/pivot', 'src/sheet/edit', 'src/sheet/data', 'src/sheet/objects', 'src/slide/odp',
    'src/slide/ppt', 'src/slide/render', 'src/slide/edit', 'src/slide/objects', 'src/slide/show',
    'src/core', 'src/sheet/csv', 'src/sheet/model', 'src/slide/Reader.cpp', 'src/slide/Shapes.cpp',
    'src/slide/Tables.cpp', 'src/slide/TextStyles.cpp', 'src/slide/Internal.h', 'src/slide/Presentation.h',
    'src/slide/Text.h',
]
# Inside the engine folders, the parts that speak to the user: undo steps' names, messages, the names
# of effects, rules and kinds of file, function help. Their words are marked as in the views.
ENGINE_UI = [
    'src/doc/edit', 'src/slide/edit', 'src/sheet/edit', 'src/slide/show', 'src/sheet/data/Rules.h',
    'src/sheet/data/Rules.cpp', 'src/core/FileKinds.cpp', 'src/core/Strings.cpp',
    'src/sheet/calc/FunctionHelp.cpp', 'src/slide/render/Slide.cpp',
]
# Never marked: test-only screens, the string tools' own subject, the package checker's findings
# (fwtool's, for developers), schema errors, and files whose words are data (AutoCorrect's entries,
# month names for number formats, fonts, a chart's default series names, DPAPI's description).
EXCLUDE_FILES = {
    'src/ui/WidgetGallery.cpp', 'src/ui/WidgetGallery.h', 'src/core/Lang.cpp', 'src/core/Lang.h',
    'src/pkg/Check.cpp', 'src/pkg/Derived.cpp', 'src/pkg/ReferenceRules.cpp', 'src/doc/model/Schema.cpp',
    'src/doc/proof/AutoCorrect.cpp', 'src/pkg/Recovery.cpp', 'src/sheet/numfmt/NumLocale.cpp',
    'src/chart/Draw.cpp', 'src/text/Fonts.cpp', 'src/doc/layout/Layout.cpp', 'src/core/LayoutDump.cpp',
    # Settings' keys and the registry's names (the language key is the catalog's own).
    'src/Settings.cpp', 'src/FileAssoc.cpp',
    # Content written into files or drawn as a file's content.
    'src/draw/Pdf.cpp', 'src/draw/D2DPainter.cpp', 'src/chart/Read.cpp', 'src/slide/view/NewDeckSource.cpp',
}

# Literals that read as words but are names the system or a file knows.
EXCLUDE_TEXT = re.compile(
    r'^(Microsoft Print to PDF|HTML Format|Rich Text Format|HTML|Accept: .*|Featherweight recovery|Featherweight|Aa)$'
    r'|^[dDMyYHhmst]+([ ,./:-]+[dDMyYHhmst]+)+$'  # date pictures: "d MMMM yyyy"
    r'|.*&[A-Z](?![a-z]).*'  # Excel's header and footer codes: "&CPage &P of &N"
    r'|.* Placeholder ?$'  # placeholder shapes' names in a slide's file
)
# Places in a file whose words are names a file keeps: (file, pattern, where it's looked for: the
# literal's line, or the head of its statement).
EXCLUDE_AT = [
    ('src/sheet/view/WorkbookFormat.cpp', r'\badd\(\s*L"', 'line'),  # cell styles: their names go in the file
    ('src/sheet/view/WorkbookFormat.cpp', r'case 0: return L"General";', 'line'),  # a number format's code
    ('src/slide/view/SlideDesign.cpp', r'\b(schemes|themes)\s*=', 'head'),  # theme and color scheme names
]
# Assignments whose value is data.
DATA_TARGETS = re.compile(r'(\.|->|\b)(shortcut|keys|recoveryDir|headers|g_uiFont|cmap|fontFamily|face|typeface|'
                          r'className|progId|verb|section|iniKey|regKey|op|ops)\s*=\s*$')

FONTS = {
    'segoe ui', 'segoe ui variable', 'segoe ui semibold', 'segoe ui light', 'segoe ui symbol',
    'segoe ui emoji', 'segoe mdl2 assets', 'segoe fluent icons', 'calibri', 'calibri light', 'cambria',
    'cambria math', 'arial', 'arial black', 'arial narrow', 'times new roman', 'courier new', 'consolas',
    'aptos', 'aptos display', 'aptos narrow', 'aptos mono', 'georgia', 'verdana', 'tahoma', 'symbol',
    'wingdings', 'webdings', 'candara', 'corbel', 'constantia', 'garamond', 'gill sans mt',
    'century gothic', 'franklin gothic', 'franklin gothic book', 'trebuchet ms', 'lucida console',
    'lucida sans unicode', 'book antiqua', 'palatino linotype', 'impact', 'comic sans ms',
    'sitka', 'sitka text', 'ebrima', 'gadugi', 'nirmala ui', 'leelawadee ui', 'malgun gothic',
    'microsoft yahei', 'yu gothic', 'ms gothic', 'simsun', 'meiryo', 'mangal', 'tw cen mt',
    'segoe ui variable text', 'segoe ui variable display', 'segoe ui variable small', 'proxima nova rg',
    'cascadia code', 'cascadia mono', 'segoe print', 'segoe script', 'ink free', 'bahnschrift',
}

# A call whose string arguments are names in a file, a setting, the registry or the system.
DATA_CALLS = re.compile(
    r'(\b(Attr|AttrW|Attribute|attribute|append_attribute|child|Child|Children|append_child|prepend_child|'
    r'select_node|select_nodes|Find(Child|Attr)?|GetAttr|SetAttr|HasAttr|Remove(Attr|Child)?|Ns|Local|Rel|'
    r'Part|PartName|ContentType|Relationship|Ini\w*|Get(Int|Bool|String|Double|Value)|Set(Int|Bool|String|Double|Value)|'
    r'Reg\w+|LoadLibrary\w*|GetModuleHandle\w*|GetProcAddress|CreateFont\w*|CreateTextFormat|FontFace|'
    r'ShellExecute\w*|CreateProcess\w*|RegisterClass\w*|CreateWindowEx\w*|FindWindow\w*|CreateEvent\w*|'
    r'CreateMutex\w*|OpenEvent\w*|Log\w*|Trace\w*|OutputDebugString\w*|Debug\w*|Assert\w*|'
    r'Expect\w*|wcscmp|_wcsicmp|wcsncmp|_wcsnicmp|strcmp|_stricmp|strncmp|lstrcmpi\w*|CompareString\w*|'
    r'EqualsNoCase|EqualNoCase|StartsWith\w*|EndsWith\w*|starts_with|ends_with|find|rfind|find_first_of|'
    r'find_last_of|Contains\w*|AtNoCase|IsP|IsA|Is|Has|Named|Tag|Key|Env\w*|GetEnvironmentVariable\w*|'
    r'SHGetKnownFolderPath|PathCombine\w*|PathAppend\w*|CreateFile\w*|DeleteFile\w*|MoveFile\w*|'
    r'CopyFile\w*|FindFirstFile\w*|Script\w*|Op|Arg|Flag|Option|ApplyStyle|ApplyNumberCode|StyleById|'
    r'StyleByName|FindStyle|GetClipboard\w*|SetClipboard\w*|RegisterClipboardFormat\w*|PrintPages|PrintTo|'
    r'FontExists|fwprintf|wprintf|printf|fprintf|puts|fputws|AbortBench|SetEnvironmentVariable\w*|FPDF\w*)\s*\()', re.S)


def rel(path):
    return os.path.relpath(path, ROOT).replace('\\', '/')


def _under(r, dirs):
    return any(r == d or r.startswith(d + '/') for d in dirs)


def is_engine(r):
    return _under(r, ENGINE) and not _under(r, ENGINE_UI)


WORD = re.compile(r"[A-Za-z][a-z]+")


def looks_like_words(t):
    """Text a person reads: words, a sentence, a label."""
    s = t.strip()
    if not s or not re.search(r'[A-Za-z]', s):
        return False
    if len(s) == 1:
        return False
    # Escapes, glyphs and other code points outside text.
    if any(0xE000 <= ord(ch) <= 0xF8FF for ch in s):
        return False
    if re.search(r'[<>{}\[\]\\=]', s) and not re.search(r'\{\d\}', s):
        return False
    if '://' in s or s.startswith(('.', '*', '#', '/', '-', '@', '$', '_')):
        return False
    if s.lower() in FONTS:
        return False
    tokens = s.split()
    if len(tokens) == 1:
        tok = tokens[0].rstrip(':…').rstrip('.')
        if tok.endswith('...'):
            tok = tok[:-3]
        # A single word: capitalised and lowercase after ("Bold", "Home"), or with a following ….
        if ':' in tok or '_' in tok or '/' in tok or '.' in tok or '+' in tok:
            return False
        return bool(re.fullmatch(r"[A-Z][a-z']+(-[a-z]+)?", tok))
    # Several tokens: at least one real word, and not code ("a = b", "x y z" of identifiers).
    words = [w for w in tokens if WORD.fullmatch(w.strip('.,:;!?()"\'…%'))]
    if not words:
        return False
    if all(re.fullmatch(r'[a-z][a-z0-9]*', w) for w in tokens) and len(tokens) <= 2:
        # "wingdings 2", "heading 1": lower-case names (style ids, fonts)
        return False
    return True


SHORTCUT = re.compile(r'^((Ctrl|Shift|Alt|Win)\+)+.+$|^F\d{1,2}$|^(Ctrl|Shift|Alt)$')


def classify(lit, r, code):
    """('T'|'N'|'', reason)."""
    t = lit.text
    st = lit.statement
    before = st[-120:]
    if lit.prefix not in ('L', '', 'u', 'u8'):
        return '', 'prefix'
    if lit.test_only:
        return '', 'test only'
    if SHORTCUT.match(t.strip()):
        return '', 'shortcut'
    if not looks_like_words(t):
        return '', 'not words'
    if EXCLUDE_TEXT.match(t.strip()):
        return '', 'system name'
    for f, rx, where in EXCLUDE_AT:
        if f == r:
            ls = code.rfind('\n', 0, lit.start) + 1
            le = code.find('\n', lit.start)
            if re.search(rx, code[ls:le] if where == 'line' else lit.head):
                return '', 'file data'
    if DATA_TARGETS.search(before):
        return '', 'data target'
    # Already marked.
    if re.search(r'\b(T|N_)\(\s*$', before):
        return '', 'marked'
    # Joined with a macro's literal ("Version " FW_VERSION_WSTRING): left for a hand edit.
    if re.search(r'\b[A-Z_][A-Z0-9_]{2,}\s*$', before) or re.match(r'\s*[A-Z_][A-Z0-9_]{2,}\s*(L|u8|u|U)?"', lit.after):
        return '', 'joined with a macro'
    # Opt-outs.
    line_end = code.find('\n', lit.end)
    line_start = code.rfind('\n', 0, lit.start) + 1
    if '// lang: no' in code[line_start:line_end if line_end >= 0 else len(code)]:
        return '', 'opt-out'
    # Comparisons: data, or a label compared with another (left for review).
    if re.search(r'(==|!=)\s*$', before) or re.match(r'\s*(==|!=)', lit.after):
        return '', 'comparison'
    # The argument of a data call (the innermost open call before it).
    depth = 0
    k = len(st) - 1
    call = None
    while k >= 0:
        ch = st[k]
        if ch == ')':
            depth += 1
        elif ch == '(':
            if depth == 0:
                m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*$', st[:k])
                call = m.group(1) if m else ''
                break
            depth -= 1
        elif ch in '{};' and depth == 0:
            break
        k -= 1
    if call is not None and call and DATA_CALLS.fullmatch(call + '('):
        return '', 'data call ' + call
    if call in ('static_assert', 'TEXT', '_T', 'sizeof', 'L', 'operator'):
        return '', 'data call ' + call
    if re.search(r'\b(op|key|name_?|id|tag|ext|path|file|dir|font|family|xml|uri|url|cls|clsid)\s*=\s*$', before, re.I) and is_engine(r):
        return '', 'data assignment'
    if (lit.prefix == '' or lit.prefix == 'u8') and _under(r, ENGINE):
        # Narrow strings in the engines are file data almost everywhere (style, shape and placeholder
        # names): sentences only, and function help's descriptions (they end with a full stop).
        help_text = r == 'src/sheet/calc/FunctionHelp.cpp' and t.endswith('.')
        if not help_text and (len(t.split()) < 3 or not re.search(r'[a-z]{3,} [a-z]{2,}', t)):
            return '', 'narrow, not a sentence'
    if is_engine(r):
        # Engine folders: sentences for the user only (an error, a warning, a prompt on a slide).
        if len(t.split()) < 3 and not re.search(r'\b(error|message|reason|warning|note|why|prompt|label|title|name)\w*\s*[=(,]\s*$', before, re.I):
            return '', 'engine, short'
    if lit.constexpr or lit.scope in ('namespace', 'class'):
        return 'N', 'table'
    return 'T', 'words'


HIDES_T = re.compile(r'\b(using\s+T\s*=|typename\s+T\b|class\s+T\b|struct\s+T\b|enum\s+(class\s+)?T\b|typedef\s[^;]*\bT\s*;)')
INCLUDE = '#include "core/Lang.h"'


def add_include(code):
    """`code` with core/Lang.h included, in sorted place in its last block of quoted includes."""
    if INCLUDE in code:
        return code
    lines = code.split('\n')
    blocks = []  # (first, last) line indexes of contiguous quoted includes
    for i, l in enumerate(lines):
        if l.startswith('#include "'):
            if blocks and blocks[-1][1] == i - 1:
                blocks[-1][1] = i
            else:
                blocks.append([i, i])
    first_inc = next((i for i, l in enumerate(lines) if l.startswith('#include')), -1)
    own_only = len(blocks) == 1 and blocks[0] == [first_inc, first_inc]  # just the file's own header
    if blocks and not own_only:
        first, last = blocks[-1]
        at = last + 1
        for i in range(first, last + 1):
            if lines[i] > INCLUDE:
                at = i
                break
        lines.insert(at, INCLUDE)
        return '\n'.join(lines)
    incs = [i for i, l in enumerate(lines) if l.startswith('#include')]
    if incs:
        lines[incs[-1] + 1:incs[-1] + 1] = ['', INCLUDE]
    else:
        once = next((i for i, l in enumerate(lines) if l.startswith('#pragma once')), -1)
        lines[once + 1:once + 1] = ['', INCLUDE]
    return '\n'.join(lines)


def files(paths):
    for p in paths:
        p = os.path.join(ROOT, p) if not os.path.isabs(p) else p
        if os.path.isfile(p):
            yield p
            continue
        for d, _, fs in os.walk(p):
            for f in fs:
                if f.endswith(('.cpp', '.h', '.inc')):
                    yield os.path.join(d, f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report')
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--all', action='store_true', help='report unmarked literals too')
    ap.add_argument('--conflicts', help='texts marked in one place and used as data in another')
    ap.add_argument('--check', action='store_true',
                    help='the catalog check (ctest strings_marked): fail if any string would be marked')
    ap.add_argument('paths', nargs='*')
    a = ap.parse_args()
    paths = a.paths or ['src']
    # Pass 1: each literal classified.
    scanned = []
    for path in sorted(files(paths)):
        r = rel(path)
        if r in EXCLUDE_FILES:
            continue
        code = open(path, encoding='utf-8').read()
        if '// lang: skip-file' in code:
            continue
        scanned.append((path, r, code, [(lit, *classify(lit, r, code)) for lit in cpplex.scan(code)]))
    # Pass 2: a label compared with the same label (the same text and kind of string, marked
    # elsewhere, outside the engines) is marked too, so both sides read the same in every language.
    # Across files only for undo steps' names (u"Typing"): narrow and wide words elsewhere are as
    # often a file's names ("Print", a PDF's named action).
    steps = {lit.text for _, _, _, ls in scanned for lit, kind, _ in ls if kind and lit.prefix == 'u'}
    rows = []
    changed = 0
    marked_at, data_at = {}, {}
    for path, r, code, ls in scanned:
        edits = []
        here = {(lit.text, lit.prefix) for lit, kind, _ in ls if kind}
        for lit, kind, why in ls:
            same = (lit.text, lit.prefix) in here or (lit.prefix == 'u' and lit.text in steps)
            if (not kind and why == 'comparison' and not is_engine(r) and same
                    and not lit.constexpr and lit.scope == 'function'):
                kind, why = 'T', 'compared with a label'
            if kind:
                marked_at.setdefault(lit.text, []).append(f'{r}:{lit.line}')
            elif why in ('comparison', 'data target', 'system name') or why.startswith('data '):
                data_at.setdefault(lit.text, []).append(f'{r}:{lit.line} ({why})')
            if kind or a.all:
                ctx = re.sub(r'\s+', ' ', lit.statement[-70:]).strip()
                aft = re.sub(r'\s+', ' ', lit.after[:40]).strip()
                rows.append((r, lit.line, kind or '-', why, lit.scope, 'constexpr' if lit.constexpr else '',
                             repr(lit.text)[1:-1], ctx, aft))
            if kind:
                edits.append((lit, kind))
        if a.apply and edits:
            out = []
            pos = 0
            # A file that names something else T (a template parameter, an alias) calls ::T.
            t_name = '::T' if HIDES_T.search(code) else 'T'
            for lit, kind in edits:
                out.append(code[pos:lit.start])
                macro = t_name if kind == 'T' else 'N_'
                out.append(macro + '(' + code[lit.start:lit.end] + ')')
                pos = lit.end
            out.append(code[pos:])
            new = add_include(''.join(out))
            if new != code:
                with open(path, 'w', encoding='utf-8', newline='') as f:
                    f.write(new)
                changed += 1
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            f.write('file\tline\tmark\twhy\tscope\tconstexpr\ttext\tbefore\tafter\n')
            for row in rows:
                f.write('\t'.join(str(x).replace('\t', ' ') for x in row) + '\n')
    if a.conflicts:
        with open(a.conflicts, 'w', encoding='utf-8') as f:
            for t in sorted(set(marked_at) & set(data_at)):
                f.write(f'{t!r}\n  marked: {", ".join(marked_at[t][:6])}\n  data:   {", ".join(data_at[t][:6])}\n')
    marked = sum(1 for row in rows if row[2] != '-')
    if a.check:
        for row in rows:
            if row[2] != '-':
                print(f'{row[0]}:{row[1]}: "{row[6]}" isn\'t marked ({"N_() or a UiText table" if row[2] == "N" else "T()"})')
        if marked:
            print(f'\n{marked} strings a person reads don\'t go through the catalog (docs/f1-plan.md, Rules: every new '
                  'UI string goes through T()).\nWrap them (python tools/strings/mark.py --apply <files> does it), or, '
                  'for data that only looks like words, end the line with // lang: no.')
            return 1
        print('every string is marked')
        return 0
    print(f'{marked} strings to mark in {len({row[0] for row in rows if row[2] != "-"})} files'
          + (f'; {changed} files changed' if a.apply else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
