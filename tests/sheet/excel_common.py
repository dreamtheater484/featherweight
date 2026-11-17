# Shared by tests/sheet's Excel scripts.
import os, re, zipfile

AUTHOR = "Featherweight tests"


def scrub_author(path):
    """Excel writes Windows' user name as the file's creator and as note authors: replace it with
    the test files' own name (as tests/office's are), keeping every other byte of the parts."""
    tmp = path + ".tmp"
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(tmp, "w") as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            if info.filename == "docProps/core.xml":
                text = data.decode("utf-8")
                text = re.sub(r"(<dc:creator>)[^<]*(</dc:creator>)", r"\g<1>" + AUTHOR + r"\g<2>", text)
                text = re.sub(r"(<cp:lastModifiedBy>)[^<]*(</cp:lastModifiedBy>)", r"\g<1>" + AUTHOR + r"\g<2>", text)
                data = text.encode("utf-8")
            elif re.fullmatch(r"xl/comments\d*\.xml", info.filename):
                text = data.decode("utf-8")
                text = re.sub(r"<author>[^<]*</author>", "<author>" + AUTHOR + "</author>", text)
                data = text.encode("utf-8")
            dst.writestr(info, data)
    os.replace(tmp, path)
