"""Scores OCR text against the scan set's truth (tests\\pdf\\make-scan-set.py): the share of words right.

Run:  python -I tests\\pdf\\ocr-score.py --truth truth.json --ocr ocr.json --through windows|featherweight --out DIR

ocr.json: {"pages": {"scan-01-p1.png": "the OCR text of that page", ...}}, keyed by the truth's PNG names.
Both sides are normalized the same way (see NORMALIZATION), then each page's word sequences are aligned
with a word-level Levenshtein; a truth word is right when the alignment matches it exactly (rightInOrder;
among the alignments with the fewest edits, the one with the most matches counts). Windows' OCR reads a
table column by column, and splits a line with wide gaps into fragments it puts elsewhere, so words read in
another order count too (moved: see METHOD). percent counts both; percentInOrder the alignment alone.

Writes DIR\\summary.json and DIR\\summary.md. Exit 0 when at least 95 % of the words are right, else 1.
"""
import argparse
import json
import os
import sys
import unicodedata
from collections import Counter

THRESHOLD = 95.0
NORMALIZATION = ("Unicode NFKC; curly quotes to straight ones; en and em dashes (and other dashes) to '-'; "
                 "split on whitespace; leading and trailing '\"()[]{},.;:!? stripped from each word; "
                 "empty words dropped; case-sensitive.")
METHOD = ("Each page's words aligned with the truth's (word-level Levenshtein: substitution, insertion and "
          "deletion cost 1; ties go to the most matches); a truth word is right when the alignment matches it "
          "exactly (rightInOrder). Words the OCR read in another order (a table column by column, a line's "
          "fragments put elsewhere) count too: the words left over on both sides are paired by equal text, each "
          "OCR word at most once (moved; the same as aligning the leftovers again until nothing more matches). "
          "right = rightInOrder + moved: every truth word the page's OCR text has, spelled exactly.")
QUOTES = str.maketrans({
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'", "´": "'", "`": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"', "«": '"', "»": '"',
    "‐": "-", "‑": "-", "‒": "-", "–": "-", "—": "-", "―": "-", "−": "-",
})
STRIP = "'\"()[]{},.;:!?"


def normalize(text):
    text = unicodedata.normalize("NFKC", text or "").translate(QUOTES)
    out = []
    for tok in text.split():
        tok = tok.strip(STRIP)
        if tok:
            out.append(tok)
    return out


def aligned_matches(truth, ocr):
    """Matches in a minimum-edit (Levenshtein) alignment of two word lists, the most matches among the ties."""
    n, m = len(truth), len(ocr)
    if not n or not m:
        return 0
    # One integer per cell: edits * BIG - matches, so min() takes the fewest edits, then the most matches.
    big = 1 << 24
    ids = {}
    t = [ids.setdefault(w, len(ids)) for w in truth]
    o = [ids.setdefault(w, len(ids)) for w in ocr]
    prev = [j * big for j in range(m + 1)]
    for i in range(1, n + 1):
        ti = t[i - 1]
        cur = [i * big] + [0] * m
        left = cur[0]
        for j in range(1, m + 1):
            diag = prev[j - 1]
            best = diag - 1 if o[j - 1] == ti else diag + big
            up = prev[j] + big
            if up < best:
                best = up
            if left + big < best:
                best = left + big
            cur[j] = left = best
        prev = cur
    v = prev[m]
    edits = -((-v) // big)
    return edits * big - v


def right_words(truth, ocr):
    """(in order, moved) for one page.

    in order: truth words the Levenshtein alignment matches. moved: the words left over on both sides
    paired by equal text, each OCR word at most once. Aligning the leftovers again (longest common
    subsequence) until nothing more matches pairs exactly as many, since the alignment's matches are
    pairs of equal words too; so in order + moved is the size of the two pages' multiset intersection.
    """
    in_order = aligned_matches(truth, ocr)
    common = sum((Counter(truth) & Counter(ocr)).values())
    return in_order, common - in_order


def main():
    ap = argparse.ArgumentParser(description="Scores OCR text against the scan set's truth.")
    ap.add_argument("--truth", required=True)
    ap.add_argument("--ocr", required=True)
    ap.add_argument("--through", required=True, choices=["windows", "featherweight"])
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.truth, encoding="utf-8-sig") as fh:
        truth = json.load(fh)
    with open(args.ocr, encoding="utf-8-sig") as fh:
        ocr = json.load(fh).get("pages") or {}

    def pct(a, b):
        return round(100.0 * a / b, 2) if b else 0.0

    per_page, words, right, in_order, missing = [], 0, 0, 0, []
    for doc in truth["documents"]:
        for page in doc["pages"]:
            name = page["png"]
            want = normalize(" ".join(page["words"]))
            if name not in ocr:
                missing.append(name)
            got = normalize(ocr.get(name, ""))
            ordered, moved = right_words(want, got)
            per_page.append({"png": name, "pdf": doc["pdf"], "words": len(want), "right": ordered + moved,
                             "percent": pct(ordered + moved, len(want)) if want else 100.0,
                             "rightInOrder": ordered, "moved": moved, "ocrWords": len(got),
                             "layout": (page.get("look") or {}).get("layout")})
            words += len(want)
            right += ordered + moved
            in_order += ordered
    percent = pct(right, words)
    extra = sorted(set(ocr) - {p["png"] for p in per_page})
    summary = {
        "through": args.through, "pages": len(per_page), "words": words, "right": right, "percent": percent,
        "rightInOrder": in_order, "percentInOrder": pct(in_order, words),
        "threshold": THRESHOLD, "passed": percent >= THRESHOLD,
        "missingPages": missing, "extraPages": extra,
        "perPage": per_page, "normalization": NORMALIZATION, "method": METHOD,
        "truth": os.path.abspath(args.truth), "ocr": os.path.abspath(args.ocr),
    }
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(summary, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    worst = sorted(per_page, key=lambda p: p["percent"])[:5]
    md = ["# OCR accuracy (%s)" % args.through, "",
          "| measure | value |", "|---|---|",
          "| through | %s |" % args.through,
          "| pages | %d |" % len(per_page),
          "| words | %d |" % words,
          "| right | %d |" % right,
          "| percent | %.2f |" % percent,
          "| in reading order | %.2f |" % pct(in_order, words),
          "| threshold | %.0f |" % THRESHOLD,
          "| missing pages | %d |" % len(missing),
          "", "Worst pages:", "",
          "| page | layout | words | right | % | in order |", "|---|---|--:|--:|--:|--:|"]
    md += ["| %s | %s | %d | %d | %.2f | %d |" % (p["png"], p["layout"] or "", p["words"], p["right"], p["percent"],
                                                p["rightInOrder"]) for p in worst]
    md += ["", "Normalization: " + NORMALIZATION, "", "Method: " + METHOD, ""]
    with open(os.path.join(args.out, "summary.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(md))

    print("ocr-score (%s): %.2f %% of %d words right on %d pages (%.2f %% in reading order)%s" % (
        args.through, percent, words, len(per_page), pct(in_order, words),
        ", %d pages missing" % len(missing) if missing else ""))
    return 0 if percent >= THRESHOLD else 1


if __name__ == "__main__":
    sys.exit(main())
