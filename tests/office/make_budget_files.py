"""Generates S1's opening-budget files (plan section 11, docs/s1-plan.md "Budgets"), which are too
big for the repository:

  million-cells.xlsx   50,000 rows by 20 columns: numbers, text, dates, booleans, a few formats,
                       and a formula column (cached values included, as Excel writes them)
  100mb.csv            about 100 MB of UTF-8 CSV: 1.1 million rows of ids, names, amounts,
                       dates and codes with leading zeros, some fields quoted
  fifty-mb.xlsx        S4's very large workbook (docs/s4-plan.md, "Scale"): the million-cell
                       workbook's rows, 384,000 of them (7.68 million cells), about 50 MB

Output goes to the folder given (default: build/budgets, which git ignores):
  .venv\\Scripts\\python tests/office/make_budget_files.py [folder]

The output is deterministic (fixed seed, fixed dates), so timings compare across runs.
"""
import os
import random
import sys
from datetime import datetime

import xlsxwriter
from xlsxwriter.utility import xl_col_to_name

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORDS = ["apple", "pear", "plum", "cherry", "grape", "melon", "lemon", "kiwi", "mango", "peach", "Café", "Straße", "naïve"]


def million_cells(path, rows=50_000, title="One million cells"):
    cols = 20
    rnd = random.Random(1)
    wb = xlsxwriter.Workbook(path, {"constant_memory": True, "strings_to_numbers": False})
    wb.set_properties({"title": title, "author": "Featherweight tests", "created": datetime(2026, 1, 1)})
    ws = wb.add_worksheet("Data")
    bold = wb.add_format({"bold": True})
    money = wb.add_format({"num_format": "#,##0.00"})
    date = wb.add_format({"num_format": "yyyy-mm-dd"})
    pct = wb.add_format({"num_format": "0.0%"})
    ws.freeze_panes(1, 1)
    ws.write_row(0, 0, ["Id", "Name", "Amount", "Date", "Share", "Flag"] + [f"Value {c}" for c in range(7, cols)] + ["Total"], bold)
    for r in range(1, rows):
        a = rnd.random() * 10000
        ws.write_number(r, 0, r)
        ws.write_string(r, 1, f"{rnd.choice(WORDS)} {r % 997}")
        ws.write_number(r, 2, round(a, 2), money)
        ws.write_number(r, 3, 45000 + (r % 3650), date)
        ws.write_number(r, 4, rnd.random(), pct)
        ws.write_boolean(r, 5, r % 3 == 0)
        vals = [rnd.randint(-1000, 1000) for _ in range(6, cols - 1)]
        for c, v in enumerate(vals, start=6):
            ws.write_number(r, c, v)
        # The formula's cached value is written too, as Excel would have it.
        ws.write_formula(r, cols - 1, f"=SUM(G{r + 1}:{xl_col_to_name(cols - 2)}{r + 1})+C{r + 1}", money,
                         sum(vals) + round(a, 2))
    ws.set_column(1, 1, 18)
    ws.set_column(2, 3, 12)
    wb.close()


def big_csv(path, target=100 * 1024 * 1024):
    rnd = random.Random(2)
    size = 0
    n = 0
    with open(path, "w", encoding="utf-8", newline="") as f:
        header = "id,name,amount,date,postcode,code,note\r\n"
        f.write(header)
        size += len(header.encode("utf-8"))
        while size < target:
            n += 1
            name = f"{rnd.choice(WORDS)} {n % 1009}"
            note = '"said ""hello"", then left"' if n % 50 == 0 else ('"multi, part"' if n % 7 == 0 else "plain")
            line = (f"{n},{name},{rnd.random() * 100000:.2f},2025-{1 + n % 12:02d}-{1 + n % 28:02d},"
                    f"{n % 100000:05d},{rnd.randint(0, 10**17):018d},{note}\r\n")
            f.write(line)
            size += len(line.encode("utf-8"))
    return n


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "budgets")
    os.makedirs(out, exist_ok=True)
    x = os.path.join(out, "million-cells.xlsx")
    million_cells(x)
    print(f"  {x}  ({os.path.getsize(x):,} bytes, 1,000,000 cells)")
    c = os.path.join(out, "100mb.csv")
    rows = big_csv(c)
    print(f"  {c}  ({os.path.getsize(c):,} bytes, {rows:,} rows)")
    f = os.path.join(out, "fifty-mb.xlsx")
    million_cells(f, rows=384_000, title="About 50 MB")
    print(f"  {f}  ({os.path.getsize(f):,} bytes, 7,680,000 cells)")


if __name__ == "__main__":
    main()
