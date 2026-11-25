"""Signed PDFs through an independent validator, pyHanko (F1, f1-gate's pdf.signatures). Runs on a CI
runner (.github/workflows/pdf-signatures.yml), where pyHanko is installed pinned by version and hash;
nothing is installed on the reference PC.

    python -I tests/pdf/validate-signatures.py --dir build/pdf-signed [--dir ...] --json out.json

Each --dir has signatures.json: {"trustRoot": "signer-cert.pem", "expect": {"file.pdf": EXPECT}}, where
EXPECT is
  "valid"                  every signature intact, cryptographically valid, trusted (the test root),
                           and covering the whole file;
  "changed-after-signing"  intact and valid for its revision, but the file has changes after it
                           (coverage short of the whole file): the change must be caught;
  "broken"                 the signed bytes were changed: not intact.
Exit 1 when any file isn't as expected.
"""
import argparse
import json
import os
import sys
import traceback

from pyhanko.pdf_utils.reader import PdfFileReader
from pyhanko.sign.validation import validate_pdf_signature
from pyhanko_certvalidator import ValidationContext

try:
    from pyhanko.keys import load_cert_from_pemder
except ImportError:  # older pyHanko
    from pyhanko.sign.general import load_cert_from_pemder


def name_of(v):
    return getattr(v, "name", None) or (str(v) if v is not None else None)


def check(path, vc):
    with open(path, "rb") as f:
        reader = PdfFileReader(f, strict=False)
        out = []
        for sig in reader.embedded_signatures:
            st = validate_pdf_signature(sig, vc)
            out.append({
                "field": sig.field_name,
                "intact": bool(st.intact),
                "valid": bool(st.valid),
                "trusted": bool(st.trusted),
                "coverage": name_of(st.coverage),
                "modification": name_of(getattr(st, "modification_level", None)),
                "summary": st.summary(),
            })
        return out


def judge(expect, sigs):
    if not sigs:
        return False, "no signature found"
    for s in sigs:
        whole = s["coverage"] == "ENTIRE_FILE"
        if expect == "valid" and not (s["intact"] and s["valid"] and s["trusted"] and whole):
            return False, "expected valid over the whole file: " + s["summary"]
        if expect == "changed-after-signing" and not (s["intact"] and s["valid"] and not whole):
            return False, "expected a valid signature with changes after it (caught): " + s["summary"]
        if expect == "broken" and s["intact"]:
            return False, "expected the changed signed bytes to be caught (not intact): " + s["summary"]
    return True, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", action="append", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()
    rows, bad = [], 0
    for d in args.dir:
        spec = json.load(open(os.path.join(d, "signatures.json"), encoding="utf-8"))
        root = load_cert_from_pemder(os.path.join(d, spec["trustRoot"]))
        vc = ValidationContext(trust_roots=[root], allow_fetching=False)
        for name, expect in sorted(spec["expect"].items()):
            path = os.path.join(d, name)
            try:
                sigs = check(path, vc)
                ok, why = judge(expect, sigs)
            except Exception as e:  # a file pyHanko can't read fails
                sigs, ok, why = [], False, "pyHanko: %s: %s" % (type(e).__name__, e)
                # Where in pyHanko, for whoever fixes the file: the last frames.
                trace = [ "%s:%d %s" % (os.path.basename(fr.filename), fr.lineno, fr.name)
                          for fr in traceback.extract_tb(e.__traceback__)[-6:] ]
                why += " (at " + " < ".join(reversed(trace)) + ")"
            bad += 0 if ok else 1
            rows.append({"file": path, "expect": expect, "ok": ok, "why": why, "signatures": sigs})
            print("[%s] %s (%s)%s" % ("OK" if ok else "FAIL", path, expect, "" if ok else ": " + why))
    import pyhanko
    result = {"validator": "pyHanko %s" % getattr(pyhanko, "__version__", "?"), "pass": bad == 0,
              "files": len(rows), "failed": bad, "rows": rows}
    if args.json:
        json.dump(result, open(args.json, "w", encoding="utf-8"), indent=1)
    print("%d files, %d not as expected (%s)" % (len(rows), bad, result["validator"]))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
