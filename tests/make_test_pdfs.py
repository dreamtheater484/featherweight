"""Generates the test PDF corpus in tests/pdfs/.

Requires: pip install reportlab pypdf
Run:      python tests/make_test_pdfs.py
"""
import os

from pypdf import PdfReader, PdfWriter
from pypdf.generic import NameObject, NumberObject
from reportlab.lib.colors import HexColor
from reportlab.lib.pagesizes import A4, landscape, letter
from reportlab.pdfgen import canvas

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pdfs")
os.makedirs(OUT, exist_ok=True)

LOREM = (
    "Featherweight is a light, fast PDF reader. It opens documents instantly, fills in forms, "
    "prints, and stays out of your way. The quick brown fox jumps over the lazy dog. "
    "Café crème, naïve façade, ångström and Straße test accented text. "
)


def wrap(c, text, x, y, width, font="Helvetica", size=11, leading=15):
    c.setFont(font, size)
    words = text.split()
    line = ""
    for w in words:
        trial = (line + " " + w).strip()
        if c.stringWidth(trial, font, size) > width:
            c.drawString(x, y, line)
            y -= leading
            line = w
        else:
            line = trial
    if line:
        c.drawString(x, y, line)
        y -= leading
    return y


def text_document():
    path = os.path.join(OUT, "text.pdf")
    c = canvas.Canvas(path, pagesize=A4)
    c.setTitle("Featherweight test document")
    w, h = A4
    for page in range(1, 13):
        c.bookmarkPage(f"p{page}")
        c.setFillColor(HexColor("#3E48D6"))
        c.setFont("Helvetica-Bold", 22)
        c.drawString(56, h - 80, f"Chapter {page}")
        c.setFillColor(HexColor("#111111"))
        y = h - 120
        for para in range(6):
            y = wrap(c, LOREM * 2, 56, y, w - 112) - 10
        if page == 1:
            c.setFont("Helvetica", 12)
            c.setFillColor(HexColor("#0645AD"))
            c.drawString(56, 120, "Go to chapter 5 (internal link)")
            c.linkRect("", "p5", (56, 115, 260, 132), relative=0)
            c.drawString(56, 96, "Visit https://github.com/dreamtheater484/featherweight")
            c.linkURL("https://github.com/dreamtheater484/featherweight", (56, 91, 400, 108), relative=0)
        c.setFillColor(HexColor("#888888"))
        c.setFont("Helvetica", 9)
        c.drawCentredString(w / 2, 30, f"Page {page} of 12")
        c.showPage()
    c.save()
    return path


def form_document():
    path = os.path.join(OUT, "form.pdf")
    c = canvas.Canvas(path, pagesize=letter)
    c.setTitle("Featherweight form test")
    w, h = letter
    form = c.acroForm
    c.setFont("Helvetica-Bold", 20)
    c.drawString(56, h - 72, "Application form")
    c.setFont("Helvetica", 11)
    y = h - 120
    labels = [("Full name", "name"), ("Email address", "email"), ("City", "city")]
    for label, name in labels:
        c.drawString(56, y + 6, label)
        form.textfield(name=name, tooltip=label, x=180, y=y, width=300, height=22, borderWidth=1,
                       borderColor=HexColor("#999999"), fillColor=HexColor("#FFFFFF"), forceBorder=True)
        y -= 40
    c.drawString(56, y + 6, "Country")
    form.choice(name="country", tooltip="Country", value="Netherlands",
                options=["Netherlands", "Belgium", "Germany", "France", "United Kingdom"], x=180, y=y, width=200,
                height=22, borderWidth=1, borderColor=HexColor("#999999"), forceBorder=True)
    y -= 40
    c.drawString(56, y + 6, "Subscribe to updates")
    form.checkbox(name="subscribe", tooltip="Subscribe", x=180, y=y, size=18, buttonStyle="check",
                  borderColor=HexColor("#999999"), fillColor=HexColor("#FFFFFF"), forceBorder=True)
    y -= 40
    c.drawString(56, y + 6, "Contact by")
    for i, opt in enumerate(["Email", "Phone", "Post"]):
        form.radio(name="contact", tooltip="Contact", value=opt, selected=(i == 0), x=180 + i * 90, y=y, size=16,
                   buttonStyle="circle", borderColor=HexColor("#999999"), fillColor=HexColor("#FFFFFF"),
                   forceBorder=True)
        c.drawString(200 + i * 90, y + 3, opt)
    y -= 40
    c.drawString(56, y + 6, "Interests")
    form.listbox(name="interests", tooltip="Interests", value="Reading",
                 options=["Reading", "Cycling", "Music", "Cooking"], x=180, y=y - 50, width=200, height=70,
                 borderWidth=1, borderColor=HexColor("#999999"), forceBorder=True)
    y -= 100
    c.drawString(56, y + 46, "Comments")
    form.textfield(name="comments", tooltip="Comments", x=180, y=y - 20, width=300, height=80, borderWidth=1,
                   borderColor=HexColor("#999999"), fieldFlags="multiline", forceBorder=True)
    c.showPage()
    # Second page with one more field, to test Tab across pages.
    c.setFont("Helvetica-Bold", 16)
    c.drawString(56, h - 72, "Page 2")
    c.setFont("Helvetica", 11)
    c.drawString(56, h - 114, "Signature name")
    form.textfield(name="signname", tooltip="Signature name", x=180, y=h - 120, width=300, height=22, borderWidth=1,
                   borderColor=HexColor("#999999"), forceBorder=True)
    c.showPage()
    c.save()
    return path


def mixed_document():
    path = os.path.join(OUT, "mixed-sizes.pdf")
    c = canvas.Canvas(path)
    for i, size in enumerate([A4, landscape(A4), letter, (300, 300), A4]):
        c.setPageSize(size)
        w, h = size
        c.setFillColor(HexColor(["#E8F0FE", "#FCE8E6", "#E6F4EA", "#FEF7E0", "#F3E8FD"][i]))
        c.rect(0, 0, w, h, fill=1, stroke=0)
        c.setFillColor(HexColor("#202124"))
        c.setFont("Helvetica-Bold", 28)
        c.drawCentredString(w / 2, h / 2, f"Page {i + 1}")
        c.setFont("Helvetica", 12)
        c.drawCentredString(w / 2, h / 2 - 24, f"{int(w)} x {int(h)} pt")
        c.showPage()
    c.save()
    # Give page 5 an intrinsic /Rotate of 90.
    reader = PdfReader(path)
    writer = PdfWriter()
    for i, p in enumerate(reader.pages):
        if i == 4:
            p[NameObject("/Rotate")] = NumberObject(90)
        writer.add_page(p)
    with open(path, "wb") as f:
        writer.write(f)
    return path


def big_document():
    path = os.path.join(OUT, "big-600-pages.pdf")
    c = canvas.Canvas(path, pagesize=letter)
    w, h = letter
    for page in range(1, 601):
        c.setFont("Helvetica-Bold", 16)
        c.drawString(56, h - 72, f"Section {page}")
        y = wrap(c, LOREM * 3, 56, h - 100, w - 112)
        if page % 50 == 0:
            c.drawString(56, y - 20, "needle in a haystack")
        c.setFont("Helvetica", 9)
        c.drawCentredString(w / 2, 30, str(page))
        c.showPage()
    c.save()
    return path


def encrypted_documents(source):
    reader = PdfReader(source)
    w = PdfWriter()
    for p in reader.pages:
        w.add_page(p)
    w.encrypt(user_password="secret", owner_password="owner", algorithm="AES-256")
    with open(os.path.join(OUT, "password-secret.pdf"), "wb") as f:
        w.write(f)

    w2 = PdfWriter()
    for p in reader.pages:
        w2.add_page(p)
    # Empty user password: opens without asking, but printing is not allowed.
    w2.encrypt(user_password="", owner_password="owner", permissions_flag=0b1111_1111_1111_1111_1111_0000_1100_0000,
               algorithm="AES-128")
    with open(os.path.join(OUT, "no-print.pdf"), "wb") as f:
        w2.write(f)


def damaged_document(source):
    data = open(source, "rb").read()
    with open(os.path.join(OUT, "damaged.pdf"), "wb") as f:
        f.write(data[: len(data) // 3])
    with open(os.path.join(OUT, "not-a-pdf.pdf"), "wb") as f:
        f.write(b"This is just text pretending to be a PDF.\n")


if __name__ == "__main__":
    t = text_document()
    form_document()
    mixed_document()
    big_document()
    encrypted_documents(t)
    damaged_document(t)
    for name in sorted(os.listdir(OUT)):
        print(f"{os.path.getsize(os.path.join(OUT, name)):>10,}  {name}")
