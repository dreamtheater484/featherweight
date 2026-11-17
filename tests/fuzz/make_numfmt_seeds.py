"""Writes tests/fuzz/seeds/numfmt from tests/sheet/numfmt_cases.txt (standard library only):
one seed per format code: the code, a newline, a value as 8 bytes, and typed text lines."""
import os
import struct
import sys

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
cases = os.path.join(root, "tests", "sheet", "numfmt_cases.txt")
out = os.path.join(root, "tests", "fuzz", "seeds", "numfmt")
os.makedirs(out, exist_ok=True)
typed = ["1,234.50", "15/03/2023 13:45", "2023-03-15", "50%", "$1,234", "(12.5)", "007", "1-2", "1.5E+10",
         "13:45:30.25", "March 15, 2023", "TRUE", "1.234,50", "-0", "1e999"]
seen = {}
with open(cases, encoding="utf-8") as f:
    for line in f:
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        code, value = line.split("\t")[:2]
        if code.startswith("builtin:") or code in seen:
            continue
        number = 1234.5 if value.startswith('"') else float(value)
        seen[code] = number
for i, (code, number) in enumerate(sorted(seen.items())):
    data = code.encode("utf-8") + b"\n" + struct.pack("<d", number) + b"\n" + typed[i % len(typed)].encode("utf-8")
    with open(os.path.join(out, "%03d.txt" % i), "wb") as f:
        f.write(data)
print(len(seen), "seeds in", out)
