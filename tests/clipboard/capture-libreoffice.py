# Captures what LibreOffice Writer puts on the Windows clipboard, for the clipboard tests
# (test_edit.cpp): opens Word's own copy of the sample document (word-all.rtf) in a private,
# invisible LibreOffice, copies three selections (all of it, part of a paragraph, the table) and
# saves each one's "Rich Text Format", "HTML Format" and text as libreoffice-<name>.*. The
# clipboard's text is put back afterwards. Run with LibreOffice's own Python:
#   "C:\Program Files\LibreOffice\program\python.exe" tests\clipboard\capture-libreoffice.py tests\clipboard
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time

import uno
from com.sun.star.beans import PropertyValue

OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(__file__))
SOFFICE = os.path.join(os.path.dirname(sys.executable), "soffice.exe")

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
user32.GetClipboardData.restype = ctypes.c_void_p
user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
user32.RegisterClipboardFormatW.argtypes = [ctypes.c_wchar_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalSize.restype = ctypes.c_size_t
kernel32.GlobalSize.argtypes = [ctypes.c_void_p]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
CF_UNICODETEXT = 13


def open_clipboard():
    for _ in range(40):
        if user32.OpenClipboard(None):
            return
        time.sleep(0.05)
    raise RuntimeError("the clipboard is busy")


def get(fmt):
    open_clipboard()
    try:
        h = user32.GetClipboardData(fmt)
        if not h:
            return None
        p = kernel32.GlobalLock(h)
        data = ctypes.string_at(p, kernel32.GlobalSize(h))
        kernel32.GlobalUnlock(h)
        return data
    finally:
        user32.CloseClipboard()


def put_text(text):
    data = (text + "\0").encode("utf-16-le")
    open_clipboard()
    try:
        user32.EmptyClipboard()
        h = kernel32.GlobalAlloc(2, len(data))  # GMEM_MOVEABLE
        ctypes.memmove(kernel32.GlobalLock(h), data, len(data))
        kernel32.GlobalUnlock(h)
        user32.SetClipboardData(CF_UNICODETEXT, h)
    finally:
        user32.CloseClipboard()


def save(name):
    time.sleep(0.3)
    for fmt, ext in (("Rich Text Format", "rtf"), ("HTML Format", "html"), (None, "txt")):
        data = get(user32.RegisterClipboardFormatW(fmt) if fmt else CF_UNICODETEXT)
        if data is None:
            print(f"  libreoffice-{name}.{ext}: not on the clipboard")
            continue
        if ext == "txt":
            data = data.decode("utf-16-le").split("\0")[0].encode("utf-8")
        else:
            data = data.rstrip(b"\0")
            # The user's name in temporary picture paths, replaced (same length: the offsets stay right).
            user = os.environ.get("USERNAME", "").encode()
            if user:
                for sep in (b"/", b"\\"):
                    data = data.replace(b"Users" + sep + user + sep, b"Users" + sep + b"x" * len(user) + sep)
        with open(os.path.join(OUT, f"libreoffice-{name}.{ext}"), "wb") as f:
            f.write(data)
        print(f"  libreoffice-{name}.{ext}: {len(data)} bytes")


def prop(name, value):
    p = PropertyValue()
    p.Name, p.Value = name, value
    return p


saved = get(CF_UNICODETEXT)
profile = tempfile.mkdtemp(prefix="fw-lo-")
pipe = "fwclip" + str(os.getpid())
office = subprocess.Popen([SOFFICE, "-env:UserInstallation=" + uno.systemPathToFileUrl(profile), "--invisible",
                           "--norestore", "--nologo", "--nodefault", f"--accept=pipe,name={pipe};urp;"])
try:
    local = uno.getComponentContext()
    resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
    for _ in range(120):
        try:
            ctx = resolver.resolve(f"uno:pipe,name={pipe};urp;StarOffice.ComponentContext")
            break
        except Exception:
            time.sleep(0.5)
    else:
        raise RuntimeError("LibreOffice didn't start")
    smgr = ctx.ServiceManager
    desktop = smgr.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
    clipboard = smgr.createInstanceWithContext("com.sun.star.datatransfer.clipboard.SystemClipboard", ctx)
    url = uno.systemPathToFileUrl(os.path.join(OUT, "word-all.rtf"))
    doc = desktop.loadComponentFromURL(url, "_blank", 0, (prop("Hidden", True),))
    controller = doc.getCurrentController()

    def copy(selection, name):
        controller.select(selection)
        clipboard.setContents(controller.getTransferable(), None)
        save(name)

    text = doc.getText()
    whole = text.createTextCursor()
    whole.gotoStart(False)
    whole.gotoEnd(True)
    copy(whole, "all")
    # Part of the second paragraph: from its third word to the end of its fifth.
    paras = text.createEnumeration()
    paras.nextElement()
    second = paras.nextElement()
    part = text.createTextCursorByRange(second.getStart())
    part.gotoNextWord(False)
    part.gotoNextWord(False)
    part.gotoNextWord(True)
    part.gotoNextWord(True)
    part.gotoNextWord(True)
    copy(part, "partial")
    tables = doc.getTextTables()
    if tables.getCount():
        table = tables.getByIndex(0)
        cells = table.createCursorByCellName("A1")
        cells.gotoEnd(True)
        copy(table.getCellRangeByName(cells.getRangeName()), "table")
    doc.close(True)
    try:
        desktop.terminate()
    except Exception:
        pass  # the bridge goes as LibreOffice ends
finally:
    try:
        office.wait(30)
    except subprocess.TimeoutExpired:
        print("LibreOffice is still running: close it from the Task Manager (soffice.bin)")
    if saved is not None:
        put_text(saved.decode("utf-16-le").split("\0")[0])
    shutil.rmtree(profile, ignore_errors=True)
