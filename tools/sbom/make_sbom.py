"""Writes the CycloneDX SBOM of a Featherweight release from the pinned dependencies, and checks that
THIRD_PARTY_NOTICES.md covers exactly the components it lists (F1, docs/f1-plan.md).

    python tools/sbom/make_sbom.py --out Featherweight-4.1.0.cdx.json [--pdfium DIR ...]
    python tools/sbom/make_sbom.py --check-notices          (exit 1 if the notices and the SBOM differ)

Sources, all pinned in the repository: cmake/FetchPdfium.cmake (PDFium's build and archive hashes),
the PDFium package's licenses/ folder (the libraries inside pdfium.dll), cmake/FetchDeps.cmake (miniz,
pugixml and the hyphenation files with their hashes) and tools/notices (data). Without --pdfium, every
package of the pinned build under third_party/pdfium is read (the V8 and the standard one if present).

The output is reproducible: the timestamp is SOURCE_DATE_EPOCH or the HEAD commit's time, and the
serial number is derived from the version and the commit.
"""
import argparse
import datetime
import glob
import json
import os
import re
import subprocess
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPO_URL = "https://github.com/dreamtheater484/featherweight"

# The libraries inside pdfium.dll, by the name of their file in the package's licenses/ folder, with
# the SPDX expression of the licence text that file carries, and phrases that text must contain. A
# PDFium update that changes a licence fails the check here, so the expression is reviewed.
PDFIUM_LIBRARIES = {
    "abseil": ("Apache-2.0", ["Apache License", "Version 2.0"]),
    "agg23": (None, ["Anti-Grain Geometry - Version 2.3", "Permission to copy, use, modify, sell and distribute"]),
    "bigint": (None, ["placing it in the public domain"]),
    "dragonbox": ("Apache-2.0 WITH LLVM-exception OR BSL-1.0", ["Apache License", "Boost Software License"]),
    "fast_float": ("MIT", ["MIT License", "Permission is hereby granted, free of charge"]),
    "fdlibm": ("SunPro", ["Developed at SunSoft", "is freely granted, provided that this notice"]),
    "fp16": ("MIT", ["The MIT License", "Permission is hereby granted, free of charge"]),
    "freetype": ("FTL", ["The FreeType Project LICENSE"]),
    "harfbuzz": ("MIT-Modern-Variant", ['"Old MIT" license', "without written agreement and without"]),
    "highway": ("Apache-2.0", ["Apache License", "Version 2.0"]),
    "icu": ("Unicode-3.0", ["UNICODE LICENSE V3"]),
    "inspector_protocol": ("BSD-3-Clause", ["Redistribution and use in source and binary forms", "Neither the name"]),
    "lcms": ("MIT", ["Little Color Management System", "Permission is hereby granted, free of charge"]),
    "libjpeg_turbo": ("IJG AND BSD-3-Clause AND Zlib", ["IJG", "Modified (3-clause) BSD License", "zlib License"]),
    "libopenjpeg": ("BSD-2-Clause", ["2-clauses", "BSD License"]),
    "libpng": ("libpng-2.0", ["PNG Reference Library License version 2"]),
    "libtiff": ("libtiff", ["Sam Leffler", "Permission to use, copy, modify, distribute, and sell"]),
    "llvm-libc": ("Apache-2.0 WITH LLVM-exception", ["Apache License v2.0 with LLVM Exceptions"]),
    "pdfium": ("BSD-3-Clause AND Apache-2.0", ["The PDFium Authors", "Neither the name", "Apache License"]),
    "simdutf": ("MIT", ["Permission is hereby granted, free of charge"]),
    "strongtalk": ("BSD-3-Clause", ["Sun Microsystems", "Neither the name"]),
    "v8": ("BSD-3-Clause", ["the V8 project authors", "Neither the name"]),
    "zlib": ("Zlib", ["Jean-loup Gailly and Mark Adler", "This software is provided 'as-is'"]),
}
# Licence files named after a library plus a suffix (one library, two licence texts).
LICENSE_FILE_ALIASES = {"dragonbox-Apache2-LLVM": "dragonbox", "dragonbox-Boost": "dragonbox"}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def cmake_var(text, name):
    m = re.search(r'set\(\s*' + re.escape(name) + r'\s+"([^"]*)"', text)
    if not m:
        sys.exit(f"{name} not found")
    return m.group(1)


def fail(problems):
    for p in problems:
        print("  " + p, file=sys.stderr)
    sys.exit(1)


def version_of_app():
    m = re.search(r"project\(Featherweight VERSION ([0-9.]+)", read(os.path.join(ROOT, "CMakeLists.txt")))
    return m.group(1) if m else "0.0.0"


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def pdfium_components(dirs):
    """PDFium itself, with the libraries in its licenses/ folder as its parts."""
    text = read(os.path.join(ROOT, "cmake", "FetchPdfium.cmake"))
    build = cmake_var(text, "FW_PDFIUM_BUILD")
    variants = {}  # "v8" / "standard" -> (asset, sha256)
    for m in re.finditer(r'set\(_fw_pdfium_variant "(\w+)"\)\s*set\(_fw_pdfium_asset "([^"]+)"\)\s*'
                         r'set\(_fw_pdfium_sha256 "([0-9a-f]{64})"\)', text):
        variants[m.group(1)] = (m.group(2), m.group(3))
    if not dirs:
        dirs = sorted(glob.glob(os.path.join(ROOT, "third_party", "pdfium", f"{build}-*")))
    if not dirs:
        sys.exit("No PDFium package found; configure the CMake build first, or pass --pdfium.")
    components, problems = [], []
    for d in dirs:
        variant = os.path.basename(os.path.normpath(d)).split("-", 1)[-1]
        if variant not in variants:
            sys.exit(f"{d}: variant {variant!r} isn't pinned in cmake/FetchPdfium.cmake")
        asset, sha256 = variants[variant]
        url = f"https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F{build}/{asset}"
        parts = {}
        for path in sorted(glob.glob(os.path.join(d, "licenses", "*"))):
            stem = os.path.splitext(os.path.basename(path))[0]
            name = LICENSE_FILE_ALIASES.get(stem, stem)
            if name not in PDFIUM_LIBRARIES:
                problems.append(f"PDFium {variant}: no licence entry for {stem} (add it to PDFIUM_LIBRARIES)")
                continue
            expression, phrases = PDFIUM_LIBRARIES[name]
            body = read(path)
            parts.setdefault(name, [expression, ""])[1] += body
        for name, (expression, body) in parts.items():
            missing = [p for p in PDFIUM_LIBRARIES[name][1] if p not in body]
            if missing:
                problems.append(f"PDFium {variant}: {name}'s licence no longer says {missing}; review its SPDX expression")
        components.append({
            "type": "library",
            "bom-ref": f"pdfium-{variant}",
            "supplier": {"name": "bblanchon/pdfium-binaries", "url": ["https://github.com/bblanchon/pdfium-binaries"]},
            "name": "pdfium",
            "version": f"chromium/{build}",
            "description": f"PDFium, prebuilt ({variant}); pdfium.dll"
                           + (", or the script component" if variant == "v8" else ""),
            "licenses": [{"expression": "BSD-3-Clause AND Apache-2.0"}],
            "hashes": [{"alg": "SHA-256", "content": sha256}],
            "purl": f"pkg:github/bblanchon/pdfium-binaries@chromium%2F{build}#{asset}",
            "externalReferences": [{"type": "distribution", "url": url},
                                   {"type": "vcs", "url": "https://pdfium.googlesource.com/pdfium/"}],
            "components": [
                {"type": "library", "bom-ref": f"pdfium-{variant}/pdfium-binaries", "name": "pdfium-binaries",
                 "description": "The build scripts' licence, shipped with the package",
                 "licenses": [{"license": {"id": "MIT"}}]}
            ] + [
                {"type": "library", "bom-ref": f"pdfium-{variant}/{name}", "name": name,
                 "description": "Built into pdfium.dll",
                 "licenses": ([{"expression": expr}] if expr else
                              [{"license": {"name": f"{name} licence, quoted in THIRD_PARTY_NOTICES.md"}}])}
                for name, (expr, _) in sorted(parts.items()) if name != "pdfium"
            ],
        })
    return components, problems


def deps_components():
    """miniz, pugixml and the hyphenation files, from cmake/FetchDeps.cmake."""
    text = read(os.path.join(ROOT, "cmake", "FetchDeps.cmake"))
    out = []
    for name, url, licence in (
            ("miniz", "https://github.com/richgel999/miniz/releases/download/{v}/miniz-{v}.zip", "MIT"),
            ("pugixml", "https://github.com/zeux/pugixml/releases/download/v{v}/pugixml-{v}.tar.gz", "MIT")):
        version = cmake_var(text, f"FW_{name.upper()}_VERSION")
        sha256 = cmake_var(text, f"_fw_{name}_sha256")
        out.append({
            "type": "library", "bom-ref": name, "name": name, "version": version,
            "description": "Compiled into Featherweight.exe",
            "licenses": [{"license": {"id": licence}}],
            "hashes": [{"alg": "SHA-256", "content": sha256}],
            "externalReferences": [{"type": "distribution", "url": url.format(v=version)}],
        })
    hyph_version = cmake_var(text, "FW_HYPH_UTF8_VERSION")
    hyph_commit = cmake_var(text, "_fw_hyph_utf8_commit")
    notices = hyphenation_licences()
    for m in re.finditer(r"^\s*(hyph-[\w-]+?)\.(pat|hyp)\.txt=([0-9a-f]{64})", text, re.M):
        lang, kind, sha256 = m.groups()
        out.append(data_component(
            f"{lang}.{kind}.txt", hyph_version, sha256, notices.get(lang),
            f"https://raw.githubusercontent.com/hyphenation/tex-hyphen/{hyph_commit}/hyph-utf8/tex/generic/"
            f"hyph-utf8/patterns/txt/{lang}.{kind}.txt", "hyph-utf8"))
    # The TeX originals, shipped byte for byte where their licences ask for that (f1-word: Swedish, Finnish).
    tex_url = (f"https://raw.githubusercontent.com/hyphenation/tex-hyphen/{hyph_commit}/hyph-utf8/tex/generic/"
               "hyph-utf8/patterns/tex")
    for m in re.finditer(r'/(hyph-[\w-]+)\.tex"\s*\n\s*([0-9a-f]{64})', text):
        lang, sha256 = m.groups()
        out.append(data_component(f"{lang}.tex", hyph_version, sha256, notices.get(lang), f"{tex_url}/{lang}.tex",
                                  "hyph-utf8"))
    nl_version = cmake_var(text, "FW_OPENTAAL_HYPH_VERSION")
    nl_commit = cmake_var(text, "_fw_opentaal_commit")
    m = re.search(r"/nl_NL/hyph_nl_NL\.dic\"\s*\n\s*([0-9a-f]{64})", text)
    if m:
        out.append(data_component(
            "hyph_nl_NL.dic", nl_version, m.group(1), notices.get("hyph_nl_NL"),
            f"https://raw.githubusercontent.com/LibreOffice/dictionaries/{nl_commit}/nl_NL/hyph_nl_NL.dic", "OpenTaal"))
    return out


def data_component(file, version, sha256, licences, url, group):
    # Named by the language's id (hyph-en-us), as THIRD_PARTY_NOTICES.md names it; the file is the bom-ref.
    return {
        "type": "data", "bom-ref": file, "group": group, "name": file.split(".")[0], "version": version,
        "description": f"Hyphenation patterns ({file}), compressed into Featherweight.exe",
        "properties": [{"name": "fw:file", "value": file}],
        "licenses": [{"license": {"id": n} if n == "MIT" else {"name": n}}
                     for n in (licences or ["Quoted in THIRD_PARTY_NOTICES.md"])],
        "hashes": [{"alg": "SHA-256", "content": sha256}],
        "externalReferences": [{"type": "distribution", "url": url}],
    }


def hyphenation_id(heading):
    """The first file id in a heading's brackets: 'Dutch (OpenTaal, hyph_nl_NL.dic ...)' -> hyph_nl_NL."""
    start = heading.find("(")
    return re.search(r"(hyph[-_][\w-]+)", heading[start:]) if start >= 0 else None


def hyphenation_licences():
    """Language id -> licence names, from the 'name:' lines of each section in tools/notices."""
    path = os.path.join(ROOT, "tools", "notices", "hyphenation-patterns.md")
    result = {}
    if not os.path.exists(path):
        return result
    for section in re.split(r"^### ", read(path), flags=re.M)[1:]:
        m = hyphenation_id(section.splitlines()[0])
        if m:
            names = [n.strip() for n in re.findall(r"^\s*name:\s*(.+)$", section, re.M)]
            result[m.group(1)] = names or None
    return result


def data_notice_components():
    """The data notices other than hyphenation (tools/notices), as data components."""
    out = []
    for path in sorted(glob.glob(os.path.join(ROOT, "tools", "notices", "*.md"))):
        if os.path.basename(path) == "hyphenation-patterns.md":
            continue
        title = next((l[3:].strip() for l in read(path).splitlines() if l.startswith("## ")), None)
        if title:
            out.append({"type": "data", "bom-ref": os.path.basename(path)[:-3], "name": title,
                        "description": "Compiled into Featherweight.exe",
                        "licenses": [{"license": {"name": "Notice quoted in THIRD_PARTY_NOTICES.md"}}]})
    return out


def make_bom(pdfium_dirs):
    pdfium, problems = pdfium_components(pdfium_dirs)
    components = pdfium + deps_components() + data_notice_components()
    version = version_of_app()
    commit = git("rev-parse", "HEAD") or "unknown"
    epoch = os.environ.get("SOURCE_DATE_EPOCH") or git("log", "-1", "--format=%ct") or "0"
    stamp = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, f"{REPO_URL}@{version}/{commit}")),
        "version": 1,
        "metadata": {
            "timestamp": stamp,
            "tools": {"components": [{"type": "application", "name": "tools/sbom/make_sbom.py"}]},
            "component": {
                "type": "application", "bom-ref": "featherweight", "name": "Featherweight", "version": version,
                "description": "PDF reader and editor for documents, workbooks and presentations, for Windows",
                "licenses": [{"license": {"id": "MIT"}}],
                "purl": f"pkg:github/dreamtheater484/featherweight@v{version}",
                "externalReferences": [{"type": "vcs", "url": REPO_URL},
                                       {"type": "website", "url": "https://dreamtheater484.github.io/featherweight/"}],
                "properties": [{"name": "fw:commit", "value": commit}],
            },
        },
        "components": components,
        "dependencies": [{"ref": "featherweight", "dependsOn": [c["bom-ref"] for c in components]}],
    }
    return bom, problems


def names_in_bom(bom):
    """What the notices must name: each top-level library, each part inside PDFium, each data file's
    language, and each data notice's title."""
    names = set()
    for c in bom["components"]:
        if c["type"] == "data" and c.get("group"):
            names.add(re.split(r"\.", c["name"])[0])  # hyph-en-us.pat.txt -> hyph-en-us
        else:
            names.add(c["name"])
        for part in c.get("components", []):
            names.add(part["name"])
    return names


def names_in_notices(path):
    text = read(path)
    names = set()
    in_hyphenation = False
    for line in text.splitlines():
        if line.startswith("## "):
            title = line[3:].strip()
            in_hyphenation = title == "Hyphenation patterns"
            if not in_hyphenation:
                names.add(LICENSE_FILE_ALIASES.get(title, title))
        elif line.startswith("### ") and in_hyphenation:
            m = hyphenation_id(line)
            if m:
                names.add(m.group(1))
    return names


def check_notices(bom, path):
    in_bom, in_notices = names_in_bom(bom), names_in_notices(path)
    problems = [f"in the SBOM, not in {os.path.basename(path)}: {n}" for n in sorted(in_bom - in_notices)]
    problems += [f"in {os.path.basename(path)}, not in the SBOM: {n}" for n in sorted(in_notices - in_bom)]
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="where to write the SBOM (CycloneDX JSON)")
    ap.add_argument("--pdfium", action="append", default=[], help="a PDFium package folder (repeatable)")
    ap.add_argument("--check-notices", nargs="?", const=os.path.join(ROOT, "THIRD_PARTY_NOTICES.md"),
                    help="check that the notices name exactly the SBOM's components")
    args = ap.parse_args()
    bom, problems = make_bom(args.pdfium)
    if args.check_notices:
        problems += check_notices(bom, args.check_notices)
    if problems:
        print("SBOM check failed:", file=sys.stderr)
        fail(problems)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(bom, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print("wrote", args.out, f"({len(bom['components'])} components)")
    if args.check_notices:
        print(f"notices and SBOM match ({len(names_in_bom(bom))} names)")


if __name__ == "__main__":
    main()
