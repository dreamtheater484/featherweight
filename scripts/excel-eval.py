"""Evaluates formulas in Excel, to check the formula engine's behavior against the real thing.

Always a new, separate Excel instance (DispatchEx), with alerts, events, link updates and macros
off; it ends only the Excel it started, and always ends it (Roy may be using Excel).

Input (JSON, a file or stdin): a list of items, in order:
  {"cell": "A1", "value": 5}            sets a cell's value (number, text, bool, or null to clear)
  {"cell": "B1", "formula": "=A1*2"}    sets a cell's formula
  {"eval": "=A1&\\"\\""}                 evaluates a formula in a scratch cell (column ZZ)
  {"eval": "=A1:A3*2", "array": true}   as an array formula (FormulaArray)
  {"date1904": true}                    switches the workbook's date system (first item only)
Output (JSON lines): for each "eval" (and each formula cell, after all are set): the formula,
Excel's value and its type (number, text, bool, error, empty), and Range.Text.

  python scripts/excel-eval.py probes.json
"""
import json
import sys

import pythoncom
import win32api
import win32con
import win32process
import win32com.client

ERRORS = {-2146826281: "#DIV/0!", -2146826246: "#N/A", -2146826259: "#NAME?", -2146826288: "#NULL!",
          -2146826252: "#NUM!", -2146826265: "#REF!", -2146826273: "#VALUE!", -2146826245: "#GETTING_DATA",
          -2146826236: "#SPILL!", -2146826238: "#CALC!", -2146826239: "#FIELD!", -2146826240: "#BLOCKED!",
          -2146826237: "#UNKNOWN!", -2146826241: "#CONNECT!", -2146826235: "#BUSY!"}


def typed(v):
    if v is None:
        return "empty", None
    if isinstance(v, bool):
        return "bool", v
    if isinstance(v, int) and v in ERRORS:
        return "error", ERRORS[v]
    if isinstance(v, (int, float)):
        return "number", float(v)
    if isinstance(v, str):
        return "text", v
    return "other", repr(v)


def main():
    items = json.load(open(sys.argv[1], encoding="utf-8") if len(sys.argv) > 1 else sys.stdin)
    pythoncom.CoInitialize()
    xl = win32com.client.DispatchEx("Excel.Application")
    pid = None
    try:
        _, pid = win32process.GetWindowThreadProcessId(xl.Hwnd)
        xl.Visible = False
        xl.DisplayAlerts = False
        xl.EnableEvents = False
        xl.AskToUpdateLinks = False
        xl.AutomationSecurity = 3  # msoAutomationSecurityForceDisable
        wb = xl.Workbooks.Add()
        if items and isinstance(items[0], dict) and items[0].get("date1904"):
            wb.Date1904 = True
        ws = wb.Worksheets(1)
        row = 1
        formula_cells = []
        for it in items:
            if "cell" in it:
                c = ws.Range(it["cell"])
                if "formula" in it:
                    c.Formula2 = it["formula"]
                    formula_cells.append(it["cell"])
                else:
                    v = it.get("value")
                    if v is None:
                        c.ClearContents()
                    elif isinstance(v, str):
                        c.NumberFormat = "@"
                        c.Value = v
                        c.NumberFormat = "General"
                    else:
                        c.Value = v
            elif "eval" in it:
                c = ws.Range("ZZ%d" % row)
                row += 1
                try:
                    if it.get("array"):
                        c.FormulaArray = it["eval"]
                    else:
                        c.Formula2 = it["eval"]
                    t, v = typed(c.Value)
                    print(json.dumps({"formula": it["eval"], "type": t, "value": v, "text": c.Text}), flush=True)
                except Exception as e:  # noqa: BLE001
                    print(json.dumps({"formula": it["eval"], "type": "rejected", "value": str(e)[:200]}), flush=True)
        for addr in formula_cells:
            c = ws.Range(addr)
            t, v = typed(c.Value)
            print(json.dumps({"cell": addr, "formula": c.Formula2, "type": t, "value": v, "text": c.Text}),
                  flush=True)
        wb.Close(SaveChanges=False)
    finally:
        try:
            xl.Quit()
        except Exception:  # noqa: BLE001
            pass
        del xl
        pythoncom.CoUninitialize()
        if pid:
            try:  # still running? end it: it is ours
                h = win32api.OpenProcess(win32con.PROCESS_TERMINATE | win32con.SYNCHRONIZE, False, pid)
                import win32event
                if win32event.WaitForSingleObject(h, 5000) != win32event.WAIT_OBJECT_0:
                    win32api.TerminateProcess(h, 1)
                win32api.CloseHandle(h)
            except Exception:  # noqa: BLE001
                pass


if __name__ == "__main__":
    main()
