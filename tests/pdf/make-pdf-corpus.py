"""The page-tools corpus (F1, f1-gate): PDFs made here, each built to test what page operations
(reorder, rotate, delete, insert from another PDF, extract, split), OCR and signing meet in real
files. Made, not downloaded, with the libraries on the reference PC (reportlab, PyMuPDF, pypdf,
cryptography) and a small writer of its own for what they can't make.

    python -I tests\\pdf\\make-pdf-corpus.py --out build\\pdf-corpus

Writes the PDFs and manifest.json: per file its pages, its password, what a page tool must do with
it ("edit"; "refuse:restricted" for a file whose permissions forbid changes; "refuse:xfa" for an XFA
form; "password" when it asks for one) and whether it is signed. tests\\pdf\\pdf-torture.ps1 adds
tests\\pdfs and a sample of the corpus's Office-made PDFs to these.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import random

try:
    import fitz  # PyMuPDF
    from PIL import Image, ImageDraw
    from pypdf import PdfReader, PdfWriter
    from pypdf.generic import DecodedStreamObject, NameObject
    from reportlab.lib.pagesizes import A3, A4, landscape, letter
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    from reportlab.pdfgen import canvas
except ImportError:  # --only signed needs cryptography alone (a CI runner)
    fitz = None
    letter = (612, 792)

FONTS = "C:\\Windows\\Fonts"
WORDS = ("page tools keep every page as it was while the order changes around it and nothing is lost "
         "a merged file carries its fonts images links and notes the rotation stays where it was set").split()


def sentence(rng, n=14):
    return " ".join(rng.choice(WORDS) for _ in range(n)).capitalize() + "."


def text_page(c, rng, label, size):
    """A page whose text says which file and page it is (the torture run tells pages apart by it)."""
    w, h = size
    c.setFont("Helvetica-Bold", 20)
    c.drawString(60, h - 80, label)
    c.setFont("Helvetica", 11)
    y = h - 120
    for _ in range(int((h - 200) / 16)):
        c.drawString(60, y, sentence(rng, 12))
        y -= 16


def reportlab_doc(path, pages, rng, name, size=letter, font=None, rotate=None):
    c = canvas.Canvas(path, pagesize=size)
    c.setTitle(name)
    for i in range(pages):
        if rotate:
            c.setPageRotation(rotate[i % len(rotate)])
        if font:
            c.setFont(font, 20)
            c.drawString(60, size[1] - 80, "%s, page %d" % (name, i + 1))
            c.setFont(font, 11)
            y = size[1] - 120
            for _ in range(30):
                c.drawString(60, y, sentence(rng, 11) + " Caf\u00e9 cr\u00e8me, na\u00efve, \u00c5ngstr\u00f6m \u2013 \u20ac 12,50.")
                y -= 16
        else:
            text_page(c, rng, "%s, page %d" % (name, i + 1), size)
        c.showPage()
    c.save()


class RawPdf:
    """A small PDF writer: objects by number, a classic cross-reference table."""

    def __init__(self):
        self.objects = {}

    def add(self, num, body):
        self.objects[num] = body if isinstance(body, bytes) else body.encode("latin-1")

    def stream(self, num, dict_text, data):
        self.add(num, ("<< %s /Length %d >>\nstream\n" % (dict_text, len(data))).encode("latin-1") + data + b"\nendstream")

    def bytes(self, root, info=None, header=b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"):
        out = bytearray(header)
        offsets = {}
        for num in sorted(self.objects):
            offsets[num] = len(out)
            out += b"%d 0 obj\n" % num + self.objects[num] + b"\nendobj\n"
        size = max(self.objects) + 1
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % size
        for num in range(1, size):
            out += (b"%010d 00000 n \n" % offsets[num]) if num in offsets else b"0000000000 65535 f \n"
        out += b"trailer\n<< /Size %d /Root %d 0 R" % (size, root)
        if info:
            out += b" /Info %d 0 R" % info
        out += b" >>\nstartxref\n%d\n%%%%EOF\n" % xref
        return bytes(out)


def incremental(base, objects, root, info=None):
    """Appends an incremental update to `base`: the given objects (new versions), a table and trailer."""
    out = bytearray(base)
    if not out.endswith(b"\n"):
        out += b"\n"
    prev = int(base[base.rfind(b"startxref") + 9:].split()[0])
    size = int(base[base.rfind(b"/Size") + 5:].split()[0].rstrip(b"/>"))
    offsets = {}
    for num in sorted(objects):
        offsets[num] = len(out)
        out += b"%d 0 obj\n" % num + objects[num] + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n"
    for num in sorted(offsets):
        out += b"%d 1\n%010d 00000 n \n" % (num, offsets[num])
    size = max(size, max(objects) + 1)
    out += b"trailer\n<< /Size %d /Root %d 0 R /Prev %d" % (size, root, prev)
    if info:
        out += b" /Info %d 0 R" % info
    out += b" >>\nstartxref\n%d\n%%%%EOF\n" % xref
    return bytes(out)


def content(lines, font="F1"):
    ops = ["BT /%s 18 Tf 60 760 Td" % font]
    for i, line in enumerate(lines):
        ops.append("(%s) Tj 0 -24 Td" % line.replace("(", "\\(").replace(")", "\\)"))
    ops.append("ET")
    return "\n".join(ops).encode("latin-1")


def inherited(path):
    """A two-level page tree whose pages inherit /MediaBox, /Rotate and /Resources from their parents."""
    p = RawPdf()
    p.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    p.add(2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 4 /MediaBox [0 0 595 842] /Rotate 90 "
             "/Resources << /Font << /F1 5 0 R >> >> >>")
    p.add(3, "<< /Type /Pages /Parent 2 0 R /Kids [6 0 R 7 0 R] /Count 2 /Rotate 0 >>")
    p.add(4, "<< /Type /Pages /Parent 2 0 R /Kids [8 0 R 9 0 R] /Count 2 /MediaBox [0 0 842 595] >>")
    p.add(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    for i, (num, cnum) in enumerate([(6, 10), (7, 11), (8, 12), (9, 13)]):
        parent = 3 if num < 8 else 4
        p.add(num, "<< /Type /Page /Parent %d 0 R /Contents %d 0 R >>" % (parent, cnum))
        p.stream(cnum, "", content(["inherited-attributes, page %d" % (i + 1),
                                    "MediaBox, Rotate and Resources come from the page tree."]))
    open(path, "wb").write(p.bytes(1))


def empty_page(path):
    p = RawPdf()
    p.add(1, "<< /Type /Catalog /Pages 2 0 R >>")
    p.add(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>")
    p.add(6, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    p.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents 7 0 R >>")
    p.stream(7, "", content(["empty-page, page 1"]))
    p.add(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")  # no contents, no resources
    p.add(5, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 6 0 R >> >> /Contents [8 0 R 9 0 R] >>")
    p.stream(8, "", b"BT /F1 18 Tf 60 760 Td (empty-page, page 3) Tj")
    p.stream(9, "", b" 0 -24 Td (contents in two streams) Tj ET")
    open(path, "wb").write(p.bytes(1))


def damaged_offsets(src, path):
    """Every cross-reference offset off by 3: readers rebuild the table; what a tool saves must be whole."""
    data = bytearray(open(src, "rb").read())
    at = data.rfind(b"\nxref") + 1  # the table, not the "xref" in startxref
    lines = data[at:].split(b"\n")
    for i, line in enumerate(lines):
        parts = line.split()
        if len(parts) == 3 and parts[2] == b"n" and len(parts[0]) == 10:
            lines[i] = b"%010d %s n " % (int(parts[0]) + 3, parts[1])
    open(path, "wb").write(bytes(data[:at]) + b"\n".join(lines))


def signed(path, changed_path, rng):
    """A PDF signed with a test certificate made here, in memory (adbe.pkcs7.detached, SHA-256), and the
    same file with an incremental change after signing (its signature then covers less than the file)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives.serialization import pkcs7
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Featherweight test signer (f1-gate)")])
    now = datetime.datetime(2026, 11, 25, tzinfo=datetime.timezone.utc)
    # A signing certificate's key usage: digital signature and non-repudiation (content commitment), which
    # validators such as pyHanko require of a document signer.
    usage = x509.KeyUsage(digital_signature=True, content_commitment=True, key_encipherment=False, data_encipherment=False,
                          key_agreement=False, key_cert_sign=False, crl_sign=False, encipher_only=False, decipher_only=False)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(4100).not_valid_before(now).not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(usage, critical=True).sign(key, hashes.SHA256()))
    hole = 8192  # hex digits for /Contents
    p = RawPdf()
    p.add(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] /SigFlags 3 >> >>")
    p.add(2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>")
    p.add(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    p.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 8 0 R /Annots [6 0 R] >>")
    p.add(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 9 0 R >>")
    p.stream(8, "", content(["signed, page 1", "Signed with a test certificate (f1-gate)."]))
    p.stream(9, "", content(["signed, page 2"]))
    p.add(6, "<< /FT /Sig /T (Signature1) /V 7 0 R /Type /Annot /Subtype /Widget /Rect [0 0 0 0] /F 132 /P 3 0 R >>")
    placeholder = "/ByteRange [0 0000000000 0000000000 0000000000]"
    p.add(7, "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached %s /M (D:20261125120000Z) "
             "/Reason (f1-gate test) /Contents <%s> >>" % (placeholder, "0" * hole))
    p.add(10, "<< /Title (signed) /Producer (f1-gate make-pdf-corpus.py) >>")
    data = bytearray(p.bytes(1, info=10))
    start = data.find(b"/Contents <", data.find(b"/Type /Sig")) + len(b"/Contents ")
    end = start + hole + 2
    br = b"/ByteRange [0 %010d %010d %010d]" % (start, end, len(data) - end)
    at = data.find(placeholder.encode())
    data[at:at + len(br)] = br
    signed_bytes = bytes(data[:start]) + bytes(data[end:])
    sig = (pkcs7.PKCS7SignatureBuilder().set_data(signed_bytes).add_signer(cert, key, hashes.SHA256())
           .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature]))
    hexsig = sig.hex().encode().ljust(hole, b"0")
    data[start + 1:start + 1 + hole] = hexsig
    final = bytes(data)
    open(path, "wb").write(final)
    changed = incremental(final, {10: b"<< /Title (signed, then changed) /Producer (f1-gate make-pdf-corpus.py) >>"}, 1, info=10)
    open(changed_path, "wb").write(changed)
    return cert.public_bytes(serialization.Encoding.PEM)


def tampered(src, path):
    """The signed file with one character of its signed content changed (same length): not intact."""
    data = open(src, "rb").read()
    assert data.count(b"(signed, page 2)") == 1
    open(path, "wb").write(data.replace(b"(signed, page 2)", b"(signed, page X)"))


def signed_samples(out, rng):
    """The signed samples and what an independent validator must say of each (tests\\pdf\\validate-signatures.py)."""
    p = lambda n: os.path.join(out, n)
    pem = signed(p("signed.pdf"), p("signed-then-changed.pdf"), rng)
    tampered(p("signed.pdf"), p("signed-tampered.pdf"))
    open(p("signer-cert.pem"), "wb").write(pem)
    expect = {"signed.pdf": "valid", "signed-then-changed.pdf": "changed-after-signing", "signed-tampered.pdf": "broken"}
    json.dump({"generator": "tests/pdf/make-pdf-corpus.py --only signed", "trustRoot": "signer-cert.pem", "expect": expect},
              open(p("signatures.json"), "w", encoding="utf-8"), indent=1)
    print("%d signed samples in %s" % (len(expect), out))


def xfa(src, path):
    w = PdfWriter(clone_from=src)
    xdp = DecodedStreamObject()
    xdp.set_data(b'<?xml version="1.0"?><xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/"><template '
                 b'xmlns="http://www.xfa.org/schema/xfa-template/3.3/"><subform name="form1"/></template></xdp:xdp>')
    w._root_object["/AcroForm"][NameObject("/XFA")] = w._add_object(xdp)
    w.write(path)


def acroform(path, rng):
    c = canvas.Canvas(path, pagesize=letter)
    for page in range(2):
        text_page(c, rng, "acroform, page %d" % (page + 1), letter)
        form = c.acroForm
        form.textfield(name="name%d" % page, tooltip="Name", x=60, y=500, width=300, height=24, value="Featherweight")
        form.checkbox(name="agree%d" % page, x=60, y=460, size=18, checked=page == 0)
        form.radio(name="choice%d" % page, value="a", selected=True, x=60, y=420, size=16)
        form.radio(name="choice%d" % page, value="b", selected=False, x=100, y=420, size=16)
        form.choice(name="list%d" % page, value="Two", options=["One", "Two", "Three"], x=60, y=370, width=160, height=24)
        c.showPage()
    c.save()


def images(path, rng):
    c = canvas.Canvas(path, pagesize=A4)
    tmp = path + ".tmp"
    os.makedirs(tmp, exist_ok=True)
    for page in range(2):
        text_page(c, rng, "images, page %d" % (page + 1), A4)
        im = Image.new("RGB", (640, 400), (200, 220, 240))
        d = ImageDraw.Draw(im)
        for k in range(30):
            x, y = rng.randrange(560), rng.randrange(320)
            d.ellipse([x, y, x + rng.randrange(20, 80), y + rng.randrange(20, 80)],
                      fill=(rng.randrange(256), rng.randrange(256), rng.randrange(256)))
        jpg = os.path.join(tmp, "p%d.jpg" % page)
        im.save(jpg, quality=85)
        c.drawImage(jpg, 60, 300, width=320, height=200)
        rgba = Image.new("RGBA", (200, 200), (0, 0, 0, 0))
        ImageDraw.Draw(rgba).ellipse([10, 10, 190, 190], fill=(220, 30, 30, 160))
        png = os.path.join(tmp, "p%d.png" % page)
        rgba.save(png)
        c.drawImage(png, 400, 300, width=120, height=120, mask="auto")
        c.showPage()
    c.save()
    for f in os.listdir(tmp):
        os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)


def outline_links(path, rng):
    c = canvas.Canvas(path, pagesize=letter)
    for page in range(5):
        text_page(c, rng, "outline-links, page %d" % (page + 1), letter)
        key = "p%d" % page
        c.bookmarkPage(key)
        c.addOutlineEntry("Page %d" % (page + 1), key, level=0)
        if page % 2:
            c.addOutlineEntry("Detail of page %d" % (page + 1), key, level=1)
        c.linkAbsolute("to page 1", "p0", Rect=(60, 60, 200, 80))
        c.linkURL("https://example.com/featherweight", (300, 60, 500, 80))
        c.drawString(60, 64, "Go to page 1")
        c.drawString(300, 64, "example.com")
        c.showPage()
    c.save()


def annotations(path, rng):
    doc = fitz.open()
    for page in range(3):
        pg = doc.new_page(width=595, height=842)
        pg.insert_text((60, 80), "annotations, page %d" % (page + 1), fontsize=20)
        pg.insert_textbox(fitz.Rect(60, 110, 535, 400), " ".join(sentence(rng) for _ in range(10)), fontsize=11)
        pg.add_text_annot((500, 60), "A note on page %d" % (page + 1))
        pg.add_highlight_annot(fitz.Rect(60, 110, 400, 125))
        pg.add_ink_annot([[(100, 500), (150, 540), (200, 500), (250, 560)]])
        pg.add_freetext_annot(fitz.Rect(300, 600, 520, 640), "Free text %d" % (page + 1), fontsize=12)
        pg.add_rect_annot(fitz.Rect(60, 650, 200, 720))
    doc.save(path)


def mixed_sizes(path, rng):
    doc = fitz.open()
    for i, (w, h) in enumerate([(595, 842), (612, 792), (1191, 842), (200, 500), (842, 595)]):
        pg = doc.new_page(width=w, height=h)
        pg.insert_text((20, 40), "mixed-sizes, page %d" % (i + 1), fontsize=14)
        pg.insert_textbox(fitz.Rect(20, 60, w - 20, h - 20), " ".join(sentence(rng) for _ in range(6)), fontsize=9)
        if i in (1, 3):
            pg.set_cropbox(fitz.Rect(10, 10, w - 10, h - 30))
    doc.save(path)


def objstm(src, path):
    doc = fitz.open(src)
    doc.save(path, garbage=3, deflate=True, use_objstms=1)


def incremental_fitz(src, path):
    doc = fitz.open(src)
    doc.save(path)
    doc.close()
    for k in range(2):
        doc = fitz.open(path)
        doc[k].add_text_annot((300, 300), "Added in incremental update %d" % (k + 1))
        doc.set_metadata({"title": "incremental, update %d" % (k + 1)})
        doc.save(path, incremental=True, encryption=fitz.PDF_ENCRYPT_KEEP)
        doc.close()


def encrypted(src, path, user, owner, permissions, method=None):
    method = fitz.PDF_ENCRYPT_AES_256 if method is None else method
    doc = fitz.open(src)
    doc.save(path, encryption=method, user_pw=user, owner_pw=owner, permissions=permissions)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="build\\pdf-corpus")
    ap.add_argument("--seed", type=int, default=41)
    ap.add_argument("--only", choices=["signed"], help="only the signed samples (for pdf-signatures.yml: no fonts or PDF libraries)")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = args.out
    os.makedirs(out, exist_ok=True)
    if args.only == "signed":
        signed_samples(out, rng)
        return
    pdfmetrics.registerFont(TTFont("Arial", os.path.join(FONTS, "arial.ttf")))
    m = []

    def add(name, expect="edit", password="", signed_=False, note=""):
        path = os.path.join(out, name)
        pages = 0
        try:
            d = fitz.open(path)
            if d.needs_pass:
                d.authenticate(password)
            pages = d.page_count
        except Exception as e:  # damaged on purpose: fitz may still read it
            note = (note + "; " if note else "") + "fitz: %s" % e
        m.append({"file": name, "pages": pages, "expect": expect, "password": password, "signed": signed_, "note": note})

    p = lambda n: os.path.join(out, n)
    reportlab_doc(p("plain-text.pdf"), 3, rng, "plain-text"); add("plain-text.pdf", note="standard 14 font, not embedded")
    reportlab_doc(p("embedded-truetype.pdf"), 4, rng, "embedded-truetype", size=A4, font="Arial"); add("embedded-truetype.pdf", note="Arial subset embedded, non-ASCII text")
    reportlab_doc(p("rotated.pdf"), 4, rng, "rotated", rotate=[0, 90, 180, 270]); add("rotated.pdf", note="/Rotate 0, 90, 180, 270")
    mixed_sizes(p("mixed-sizes-cropbox.pdf"), rng); add("mixed-sizes-cropbox.pdf", note="five sizes, two with a CropBox")
    inherited(p("inherited-attributes.pdf")); add("inherited-attributes.pdf", note="MediaBox, Rotate, Resources inherited from a two-level page tree")
    outline_links(p("outline-links.pdf"), rng); add("outline-links.pdf", note="outline, internal links, a URI link")
    annotations(p("annotations.pdf"), rng); add("annotations.pdf", note="note, highlight, ink, free text, square")
    acroform(p("acroform.pdf"), rng); add("acroform.pdf", note="text, check box, radio, choice fields")
    xfa(p("acroform.pdf"), p("xfa-form.pdf")); add("xfa-form.pdf", expect="refuse:xfa", note="AcroForm with /XFA")
    reportlab_doc(p("source-6.pdf"), 6, rng, "objstm-xrefstream")
    objstm(p("source-6.pdf"), p("objstm-xrefstream.pdf")); os.remove(p("source-6.pdf")); add("objstm-xrefstream.pdf", note="object streams, a cross-reference stream")
    reportlab_doc(p("source-3.pdf"), 3, rng, "incremental")
    incremental_fitz(p("source-3.pdf"), p("incremental-updates.pdf")); add("incremental-updates.pdf", note="two incremental updates")
    reportlab_doc(p("source-4.pdf"), 4, rng, "encrypted")
    allp = fitz.PDF_PERM_PRINT | fitz.PDF_PERM_MODIFY | fitz.PDF_PERM_COPY | fitz.PDF_PERM_ANNOTATE | fitz.PDF_PERM_FORM | fitz.PDF_PERM_ACCESSIBILITY | fitz.PDF_PERM_ASSEMBLE | fitz.PDF_PERM_PRINT_HQ
    encrypted(p("source-4.pdf"), p("encrypted-open.pdf"), "", "owner-f1", allp); add("encrypted-open.pdf", note="AES-256, no user password, every permission")
    encrypted(p("source-4.pdf"), p("encrypted-restricted.pdf"), "", "owner-f1", fitz.PDF_PERM_PRINT | fitz.PDF_PERM_COPY)
    add("encrypted-restricted.pdf", expect="refuse:restricted", note="AES-256, no user password, no changes or assembly allowed")
    encrypted(p("source-4.pdf"), p("encrypted-password.pdf"), "featherweight", "owner-f1", allp, fitz.PDF_ENCRYPT_RC4_128)
    add("encrypted-password.pdf", expect="password", password="featherweight", note="RC4-128, user password featherweight")
    for f in ("source-3.pdf", "source-4.pdf"):
        os.remove(p(f))
    images(p("images.pdf"), rng); add("images.pdf", note="JPEG, PNG with a soft mask")
    try:
        reportlab_doc(p("source-l.pdf"), 5, rng, "linearized")
        fitz.open(p("source-l.pdf")).save(p("linearized.pdf"), linear=True)
        add("linearized.pdf", note="linearized (fast web view)")
    except Exception as e:
        print("linearized: not made (%s)" % e)
    if os.path.exists(p("source-l.pdf")):
        os.remove(p("source-l.pdf"))
    reportlab_doc(p("many-pages-150.pdf"), 150, rng, "many-pages"); add("many-pages-150.pdf")
    signed_samples(out, rng)
    add("signed.pdf", signed_=True, note="adbe.pkcs7.detached, a test certificate; editing warns that it breaks the signature")
    add("signed-then-changed.pdf", signed_=True, note="signed, then an incremental change: the signature covers less than the file")
    damaged_offsets(p("plain-text.pdf"), p("damaged-offsets.pdf")); add("damaged-offsets.pdf", note="every cross-reference offset off by 3")
    empty_page(p("empty-page.pdf")); add("empty-page.pdf", note="a page with no contents, one with two content streams")
    json.dump({"generator": "tests/pdf/make-pdf-corpus.py", "seed": args.seed, "files": m},
              open(os.path.join(out, "manifest.json"), "w", encoding="utf-8"), indent=1)
    print("%d PDFs in %s" % (len(m), out))


if __name__ == "__main__":
    main()
