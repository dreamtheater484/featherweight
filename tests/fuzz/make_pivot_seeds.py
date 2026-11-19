# Seeds for fuzz_pivot: each pivot table of the Excel-made fixtures (tests/office/sheet/pivots) as
# its cache definition, records and table definition, separated by NUL bytes.
#
#   python tests/fuzz/make_pivot_seeds.py
import os, posixpath, re, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "office", "sheet", "pivots")
OUT = os.path.join(HERE, "seeds", "pivot")


def rels(z, part):
    folder, name = posixpath.split(part)
    try:
        xml = z.read(posixpath.join(folder, "_rels", name + ".rels")).decode("utf-8")
    except KeyError:
        return {}
    out = {}
    for m in re.finditer(r'<Relationship [^>]*?Type="[^"]*/(\w+)"[^>]*?Target="([^"]+)"', xml):
        out[m.group(1)] = posixpath.normpath(posixpath.join(folder, m.group(2)))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(SRC)):
        if not f.endswith(".xlsx") or "stale" in f:
            continue
        z = zipfile.ZipFile(os.path.join(SRC, f))
        for part in sorted(p for p in z.namelist() if re.fullmatch(r"xl/pivotTables/pivotTable\d+\.xml", p)):
            cache = rels(z, part).get("pivotCacheDefinition")
            if not cache:
                continue
            records = rels(z, cache).get("pivotCacheRecords")
            data = z.read(cache) + b"\0" + (z.read(records) if records else b"") + b"\0" + z.read(part)
            name = "%s-%s.bin" % (f[:-5], posixpath.basename(part)[:-4])
            open(os.path.join(OUT, name), "wb").write(data)
            n += 1
    print("written", n, "seeds to", OUT)


if __name__ == "__main__":
    main()
