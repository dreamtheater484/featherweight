"""Writes a pip requirements file pinned by version and SHA-256 from the files in a folder that
`pip download` filled (for pdf-signatures.yml's pyHanko lock, made once on a runner, then committed as
tests/pdf/pyhanko-requirements.txt).

    python -I tests/pdf/lock-wheels.py WHEELS_DIR OUT_FILE
"""
import hashlib
import os
import re
import sys

wheels, out = sys.argv[1], sys.argv[2]
lines = []
for f in sorted(os.listdir(wheels)):
    m = re.match(r"^(.+?)-(\d[^-]*?)(-.*\.whl|\.tar\.gz|\.zip)$", f)
    if not m:
        raise SystemExit("can't read a name and version from " + f)
    h = hashlib.sha256(open(os.path.join(wheels, f), "rb").read()).hexdigest()
    lines.append("%s==%s --hash=sha256:%s" % (m.group(1).replace("_", "-").lower(), m.group(2), h))
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("\n".join(lines))
