# Turns the deck scripts/slide-table-styles.ps1 saves into src/slide/TableStylesData.inc: PowerPoint's
# 74 built-in table styles as 15 templates (a family's accents differ only in the accent's name),
# byte-pair encoded to keep the reader small.
#
#   python scripts/slide-table-styles-to-cpp.py styles.pptx src/slide/TableStylesData.inc
import re, sys, zipfile

COLORS = ["", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "dk1", "tx1", "lt1", "bg1"]
xml = zipfile.ZipFile(sys.argv[1]).read("ppt/tableStyles.xml").decode("utf-8")
styles = re.findall(r'<a:tblStyle styleId="([^"]+)" styleName="([^"]+)">(.*?)</a:tblStyle>', xml, re.S)
assert len(styles) == 74, len(styles)

# Templates: "@" for a family's accent, "#" for Dark Style 2's second one.
templates, entries = [], []
family_template = {}
for sid, name, body in styles:
    accents = re.findall(r"Accent (\d)", name)
    if accents:
        t = body.replace(f'"accent{accents[0]}"', '"@"')
        if len(accents) == 2:
            t = t.replace(f'"accent{accents[1]}"', '"#"')
        fam = name.split(" - ")[0]
        if fam not in family_template:
            family_template[fam] = len(templates)
            templates.append(t)
        assert templates[family_template[fam]] == t, name
for sid, name, body in styles:
    accents = re.findall(r"Accent (\d)", name)
    fam = name.split(" - ")[0]
    if accents:
        c2 = COLORS.index(f"accent{accents[1]}") if len(accents) == 2 else 0
        entries.append((sid, name, family_template[fam], COLORS.index(f"accent{accents[0]}"), c2))
        continue
    found = None
    if fam in family_template:
        t = templates[family_template[fam]]
        for c in COLORS[7:]:
            for c2 in COLORS[7:]:
                if found is None and t.replace('"@"', f'"{c}"').replace('"#"', f'"{c2}"') == body:
                    found = (family_template[fam], COLORS.index(c), COLORS.index(c2) if '"#"' in t else 0)
    if found is None:
        found = (len(templates), 0, 0)
        templates.append(body)
    entries.append((sid, name) + found)

# Byte-pair encoding: codes 128-255 each stand for a pair of codes.
seqs = [list(t.encode("ascii")) for t in templates]
assert all(b < 128 and b != 0 for s in seqs for b in s)
pairs = []
for code in range(128, 256):
    counts = {}
    for s in seqs:
        for i in range(len(s) - 1):
            k = (s[i], s[i + 1])
            counts[k] = counts.get(k, 0) + 1
    if not counts:
        break
    best = max(counts, key=counts.get)
    if counts[best] < 3:
        break
    pairs.append(best)
    for n, s in enumerate(seqs):
        out, i = [], 0
        while i < len(s):
            if i + 1 < len(s) and (s[i], s[i + 1]) == best:
                out.append(code)
                i += 2
            else:
                out.append(s[i])
                i += 1
        seqs[n] = out


def expand(c):
    return expand(pairs[c - 128][0]) + expand(pairs[c - 128][1]) if c >= 128 else [c]


for s, t in zip(seqs, templates):
    assert bytes(b for c in s for b in expand(c)).decode("ascii") == t

text = [b for s in seqs for b in s + [0]]
lines = ["// PowerPoint's built-in table styles, read from PowerPoint 2021 by scripts/slide-table-styles.ps1",
         "// and written by scripts/slide-table-styles-to-cpp.py. Do not edit.",
         "// kStylePairs[i] is what code 128 + i stands for; kStyleText holds the templates (each ending in",
         "// 0) in those codes, with \"@\" and \"#\" for a style's colors (kStyleColors).",
         f"constexpr unsigned char kStylePairs[][2] = {{",]
for i in range(0, len(pairs), 8):
    lines.append("    " + " ".join(f"{{{a}, {b}}}," for a, b in pairs[i:i + 8]))
lines.append("};")
lines.append("constexpr unsigned char kStyleText[] = {")
for i in range(0, len(text), 24):
    lines.append("    " + " ".join(f"{b}," for b in text[i:i + 24]))
lines.append("};")
lines.append("constexpr const char* kStyleColors[] = {" + ", ".join(f'"{c}"' for c in COLORS) + "};")
lines.append("// {id's bytes in the order written, template, color, second color}")
lines.append("constexpr BuiltInStyle kBuiltInStyles[] = {")
for sid, name, t, c, c2 in entries:
    hexs = re.sub(r"[^0-9A-Fa-f]", "", sid)
    b = ", ".join(f"0x{hexs[i:i + 2]}" for i in range(0, 32, 2))
    lines.append(f"    {{{{{b}}}, {t}, {c}, {c2}}},  // {name}")
lines.append("};")
grid = [i for i, e in enumerate(entries) if e[1] == "No Style, Table Grid"][0]
lines.append(f"constexpr int kTableGridStyle = {grid};  // what PowerPoint draws for an id it doesn't know")
open(sys.argv[2], "w", newline="\n").write("\n".join(lines) + "\n")
print(f"{len(templates)} templates, {sum(len(t) for t in templates)} bytes as text, {len(text)} encoded, {len(pairs)} pairs")
