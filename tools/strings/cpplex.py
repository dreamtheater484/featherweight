"""A small C++ lexer for the string tools (F1, f1-lang): finds string literals with their place,
their scope (namespace, class or function body) and the statement around them.

It's not a parser. It knows comments, character literals, raw strings, preprocessor lines and
braces, which is all the marking pass and the catalog check need.
"""
import re

_PREFIX = re.compile(r'(u8|u|U|L)?(R)?"')
_IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


class Literal:
    """One string literal, or several adjacent ones that C++ joins ("a" "b")."""

    def __init__(self, start, end, prefix, raw_parts, line):
        self.start = start        # offset of the first character (the prefix)
        self.end = end            # offset after the closing quote of the last part
        self.prefix = prefix      # '', 'L', 'u8', 'u' or 'U'
        self.raw_parts = raw_parts  # the source text between the quotes, per part (raw strings decoded)
        self.line = line          # 1-based line of the start
        self.scope = 'namespace'  # 'namespace', 'class' or 'function' (innermost that decides)
        self.depth = 0            # braces open around it
        self.statement = ''       # the source from the statement's start to the literal
        self.after = ''           # the source after it, to the end of the statement (capped)
        self.constexpr = False    # its statement declares something constexpr or constinit
        self.static = False       # its statement declares something static
        self.test_only = False    # inside #if FW_TEST_SCREENS (test builds only)

    @property
    def text(self):
        return ''.join(decode(p) if not r else p for p, r in self.raw_parts)


def decode(s):
    """The value of a literal's body (C++ escapes resolved), as a Python str."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            break
        c = s[i]
        simple = {'n': '\n', 't': '\t', 'r': '\r', '0': None, '\\': '\\', '"': '"', "'": "'", '?': '?',
                  'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}
        if c == 'x':
            j = i + 1
            while j < n and s[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(chr(int(s[i + 1:j], 16)))
            i = j
        elif c == 'u' or c == 'U':
            k = 4 if c == 'u' else 8
            out.append(chr(int(s[i + 1:i + 1 + k], 16)))
            i += 1 + k
        elif c in '01234567':
            j = i
            while j < n and j < i + 3 and s[j] in '01234567':
                j += 1
            out.append(chr(int(s[i:j], 8)))
            i = j
        elif c in simple:
            out.append(simple[c] if simple[c] is not None else '\0')
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _function_brace(code, pos):
    """Whether the '{' at `pos` opens a function body or a block inside one (judged by what precedes it)."""
    # What precedes it, without comments ("// ... (S4, W13)" isn't a parameter list).
    start = max(0, pos - 400)
    code = re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', code[start:pos], flags=re.S))
    pos = len(code)
    j = pos - 1
    while j >= 0 and code[j] in ' \t\r\n':
        j -= 1
    if j < 0:
        return None
    if code[j] == ')':
        return 'function'
    if code[j] == ']':  # a lambda without parameters: [] { ... }
        return 'function'
    m = re.search(r'([A-Za-z_][A-Za-z0-9_:<>]*)\s*$', code[max(0, j - 80):j + 1])
    if m:
        word = m.group(1)
        if word in ('const', 'override', 'noexcept', 'final', 'mutable', 'else', 'do', 'try'):
            return 'function'
    return None


def scan(code):
    """All literals in `code`, in order, with their scope and statement."""
    lits = []
    stack = []  # per open brace: 'namespace', 'class', 'function' or 'init'
    i = 0
    n = len(code)
    line = 1
    stmt_start = 0
    pending = None  # a Literal being joined with adjacent parts
    line_start = True
    pp = []  # open #if blocks: 'test' (FW_TEST_SCREENS), 'not' (its #else) or 'other'
    while i < n:
        c = code[i]
        if c == '\n':
            line += 1
            line_start = True
            i += 1
            continue
        if c in ' \t\r':
            i += 1
            continue
        # Preprocessor lines: skipped (with their continuations); #if FW_TEST_SCREENS regions noted.
        if line_start and c == '#':
            d = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', code[i:code.find('\n', i) if '\n' in code[i:] else n])
            if d:
                kw, cond = d.group(1), d.group(2)
                if kw in ('if', 'ifdef', 'ifndef'):
                    if 'FW_TEST_SCREENS' in cond:
                        pp.append('not' if kw == 'ifndef' or '!' in cond else 'test')
                    else:
                        pp.append('other')
                elif kw in ('else', 'elif') and pp:
                    pp[-1] = {'test': 'not', 'not': 'test'}.get(pp[-1], 'other')
                elif kw == 'endif' and pp:
                    pp.pop()
            while i < n and code[i] != '\n':
                if code[i] == '\\' and i + 1 < n and code[i + 1] == '\n':
                    line += 1
                    i += 2
                    continue
                i += 1
            stmt_start = i
            continue
        line_start = False
        if code.startswith('//', i):
            while i < n and code[i] != '\n':
                i += 1
            continue
        if code.startswith('/*', i):
            j = code.find('*/', i + 2)
            j = n if j < 0 else j + 2
            line += code.count('\n', i, j)
            i = j
            continue
        m = _PREFIX.match(code, i)
        if m and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == '_')):
            prefix = m.group(1) or ''
            raw = bool(m.group(2))
            q = m.end()  # after the opening quote
            if raw:
                paren = code.index('(', q)
                delim = code[q:paren]
                close = code.index(')' + delim + '"', paren)
                body = code[paren + 1:close]
                end = close + len(delim) + 2
                part = (body, True)
            else:
                j = q
                while code[j] != '"':
                    if code[j] == '\\':
                        j += 1
                    j += 1
                body = code[q:j]
                end = j + 1
                part = (body, False)
            if pending is not None and code[pending.end:i].strip() == '':
                pending.raw_parts.append(part)
                pending.end = end
            else:
                pending = Literal(i, end, prefix, [part], line)
                pending.depth = len(stack)
                scope = 'namespace'
                for s in reversed(stack):
                    if s != 'init':
                        scope = s
                        break
                pending.scope = scope
                pending.test_only = 'test' in pp
                pending.statement = code[max(stmt_start, i - 400):i]
                pending.head = code[stmt_start:min(i, stmt_start + 300)]
                lits.append(pending)
            line += code.count('\n', i, end)
            i = end
            continue
        if c == "'":
            # A character literal (or a digit separator: 1'000).
            if i > 0 and (code[i - 1].isalnum() or code[i - 1] == '_'):
                m = re.search(r'[A-Za-z0-9_]+$', code[max(0, i - 40):i])
                if not (m and m.group(0) in ('u8', 'u', 'U', 'L')):
                    i += 1
                    continue
            j = i + 1
            while j < n and code[j] != "'":
                if code[j] == '\\':
                    j += 1
                j += 1
            i = j + 1
            continue
        if pending is not None and not c.isspace():
            pending = None
        if c == '{':
            kind = _function_brace(code, i)
            head = code[max(stmt_start, i - 300):i]
            if kind is None and (not stack or stack[-1] != 'init'):
                # A constructor's body after its initializer list (": a_(x), b_{y} {"), or a function
                # with a trailing return type ("auto f() -> int {").
                if re.search(r'\)\s*(const\s*)?(noexcept\s*)?:[^:]', head) and not re.search(r'[=?]', head):
                    kind = 'function'
                elif re.search(r'\)\s*(const\s*)?->', head):
                    kind = 'function'
            if kind is None:
                if re.search(r'\bnamespace\b[^;{}]*$', head) or re.search(r'extern\s+"C"\s*$', head):
                    kind = 'namespace'
                elif re.search(r'\b(struct|class|union|enum)\b[^;{}()=]*$', head):
                    kind = 'class'
                else:
                    kind = 'init'
            stack.append(kind)
            if kind != 'init':
                stmt_start = i + 1
            i += 1
            continue
        if c == '}':
            kind = stack.pop() if stack else 'namespace'
            if kind != 'init':
                stmt_start = i + 1
            i += 1
            continue
        if c == ';':
            if not stack or stack[-1] != 'init':
                stmt_start = i + 1
            i += 1
            continue
        i += 1
    # The statements' flags and the code after each literal.
    for lit in lits:
        st = lit.head
        lit.constexpr = bool(re.search(r'\b(constexpr|constinit|consteval)\b', st))
        lit.static = bool(re.search(r'\bstatic\b', st))
        tail = code[lit.end:lit.end + 200]
        k = tail.find(';')
        lit.after = tail if k < 0 else tail[:k]
    return lits
