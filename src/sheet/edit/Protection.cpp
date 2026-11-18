#include "sheet/edit/Protection.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pugixml.hpp>

#include "pkg/OfficeCrypto.h"
#include "pkg/XmlWriter.h"
#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit {

namespace {

// Excel's words (Excel 2021, measured through automation). The second names where Featherweight
// has Unprotect Sheet.
constexpr char16_t kLockedCell[] =
    u"The cell or chart you're trying to change is on a protected sheet. To make a change, unprotect the sheet. You "
    u"might be requested to enter a password.";
constexpr char16_t kCommand[] =
    u"You cannot use this command on a protected sheet. To use this command, you must first unprotect the sheet "
    u"(right-click the sheet tab, Unprotect Sheet). You may be prompted for a password.";
constexpr char16_t kStructure[] =
    u"The workbook's structure is protected: sheets can't be added, deleted, moved, renamed or hidden. To change "
    u"them, unprotect the workbook (right-click a sheet tab, Protect Workbook). You might be requested to enter a "
    u"password.";

// The old 16-bit hash Excel 2010 and earlier kept (ECMA-376 Part 4 §14.7.1, "password").
uint16_t LegacyHash(std::wstring_view password) {
  uint32_t h = 0;
  for (size_t i = 0; i < password.size(); ++i) {
    const uint32_t v = static_cast<uint32_t>(password[i] & 0xFF) << ((i + 1) % 15);
    h ^= (v & 0x7FFF) | (v >> 15);
  }
  return static_cast<uint16_t>(h ^ password.size() ^ 0xCE4B);
}

bool ReadElement(pugi::xml_document& doc, std::string_view element, std::string_view local, pugi::xml_node& out) {
  if (element.empty() || !xlsx::ParseSlice(doc, element)) return false;
  out = doc.document_element();
  return out && xlsx::Local(out) == local;
}

void PutAttr(std::string& out, std::string_view name, std::string_view value) {
  out += ' ';
  out += name;
  out += "=\"";
  pkg::AppendEscapedAttribute(out, value);
  out += '"';
}

// A flag written when it isn't the schema's default.
void PutFlag(std::string& out, std::string_view name, bool value, bool schemaDefault) {
  if (value != schemaDefault) PutAttr(out, name, value ? "1" : "0");
}

bool LockedXf(const Styles& st, uint32_t xf) {
  const auto& xfs = st.CellXfs();
  return xf >= xfs.size() ? (xfs.empty() || xfs[0].protection.locked) : xfs[xf].protection.locked;
}

}  // namespace

// ---- Passwords

bool ProtectionPassword::Matches(std::wstring_view typed) const {
  if (!hash.empty())
    return pkg::crypto::ProtectionHash(algorithm.empty() ? std::string_view("SHA-512") : std::string_view(algorithm),
                                       salt, typed, spinCount) == hash;
  if (!legacy.empty()) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", LegacyHash(typed));
    return std::equal(legacy.begin(), legacy.end(), buf, buf + 4,
                      [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; });
  }
  return true;
}

ProtectionPassword ProtectionPassword::Make(std::wstring_view password) {
  ProtectionPassword p;
  if (password.empty()) return p;
  p.algorithm = "SHA-512";
  p.salt = pkg::crypto::NewProtectionSalt();
  p.spinCount = 100000;
  p.hash = pkg::crypto::ProtectionHash(p.algorithm, p.salt, password, p.spinCount);
  if (p.hash.empty() || p.salt.empty()) {  // Windows' cryptography failed: the old hash, as Excel 2007
    p = {};
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", LegacyHash(password));
    p.legacy = buf;
  }
  return p;
}

// ---- The elements

std::optional<SheetProtection> ReadSheetProtection(std::string_view element) {
  pugi::xml_document doc;
  pugi::xml_node n;
  if (!ReadElement(doc, element, "sheetProtection", n)) return std::nullopt;
  SheetProtection p;
  auto flag = [&](const char* name, bool def) { return xlsx::BoolAttr(n, name, def); };
  p.sheet = flag("sheet", false);
  p.objects = flag("objects", false);
  p.scenarios = flag("scenarios", false);
  p.formatCells = flag("formatCells", true);
  p.formatColumns = flag("formatColumns", true);
  p.formatRows = flag("formatRows", true);
  p.insertColumns = flag("insertColumns", true);
  p.insertRows = flag("insertRows", true);
  p.insertHyperlinks = flag("insertHyperlinks", true);
  p.deleteColumns = flag("deleteColumns", true);
  p.deleteRows = flag("deleteRows", true);
  p.selectLockedCells = flag("selectLockedCells", false);
  p.sort = flag("sort", true);
  p.autoFilter = flag("autoFilter", true);
  p.pivotTables = flag("pivotTables", true);
  p.selectUnlockedCells = flag("selectUnlockedCells", false);
  p.password.algorithm = xlsx::Attr(n, "algorithmName");
  p.password.hash = xlsx::Attr(n, "hashValue");
  p.password.salt = xlsx::Attr(n, "saltValue");
  p.password.spinCount = xlsx::UIntAttr(n, "spinCount", 0);
  p.password.legacy = xlsx::Attr(n, "password");
  return p;
}

std::string SheetProtectionXml(const SheetProtection& p, std::string_view prefix) {
  std::string out = "<";
  out += prefix;
  out += "sheetProtection";
  if (!p.password.legacy.empty()) PutAttr(out, "password", p.password.legacy);
  if (!p.password.hash.empty()) {
    PutAttr(out, "algorithmName", p.password.algorithm);
    PutAttr(out, "hashValue", p.password.hash);
    PutAttr(out, "saltValue", p.password.salt);
    PutAttr(out, "spinCount", std::to_string(p.password.spinCount));
  }
  PutFlag(out, "sheet", p.sheet, false);
  PutFlag(out, "objects", p.objects, false);
  PutFlag(out, "scenarios", p.scenarios, false);
  PutFlag(out, "formatCells", p.formatCells, true);
  PutFlag(out, "formatColumns", p.formatColumns, true);
  PutFlag(out, "formatRows", p.formatRows, true);
  PutFlag(out, "insertColumns", p.insertColumns, true);
  PutFlag(out, "insertRows", p.insertRows, true);
  PutFlag(out, "insertHyperlinks", p.insertHyperlinks, true);
  PutFlag(out, "deleteColumns", p.deleteColumns, true);
  PutFlag(out, "deleteRows", p.deleteRows, true);
  PutFlag(out, "selectLockedCells", p.selectLockedCells, false);
  PutFlag(out, "sort", p.sort, true);
  PutFlag(out, "autoFilter", p.autoFilter, true);
  PutFlag(out, "pivotTables", p.pivotTables, true);
  PutFlag(out, "selectUnlockedCells", p.selectUnlockedCells, false);
  return out + "/>";
}

std::optional<WorkbookProtection> ReadWorkbookProtection(std::string_view element) {
  pugi::xml_document doc;
  pugi::xml_node n;
  if (!ReadElement(doc, element, "workbookProtection", n)) return std::nullopt;
  WorkbookProtection p;
  p.lockStructure = xlsx::BoolAttr(n, "lockStructure", false);
  p.lockWindows = xlsx::BoolAttr(n, "lockWindows", false);
  p.lockRevision = xlsx::BoolAttr(n, "lockRevision", false);
  for (pugi::xml_attribute a : n.attributes()) {
    std::string_view name = a.name();
    const size_t colon = name.find(':');
    const std::string_view local = colon == std::string_view::npos ? name : name.substr(colon + 1);
    if (local == "workbookAlgorithmName") p.password.algorithm = a.value();
    else if (local == "workbookHashValue") p.password.hash = a.value();
    else if (local == "workbookSaltValue") p.password.salt = a.value();
    else if (local == "workbookSpinCount") p.password.spinCount = a.as_uint();
    else if (local == "workbookPassword") p.password.legacy = a.value();
    else if (local != "lockStructure" && local != "lockWindows" && local != "lockRevision") PutAttr(p.other, name, a.value());
  }
  return p;
}

std::string WorkbookProtectionXml(const WorkbookProtection& p, std::string_view prefix) {
  std::string out = "<";
  out += prefix;
  out += "workbookProtection";
  if (!p.password.legacy.empty()) PutAttr(out, "workbookPassword", p.password.legacy);
  if (!p.password.hash.empty()) {
    PutAttr(out, "workbookAlgorithmName", p.password.algorithm);
    PutAttr(out, "workbookHashValue", p.password.hash);
    PutAttr(out, "workbookSaltValue", p.password.salt);
    PutAttr(out, "workbookSpinCount", std::to_string(p.password.spinCount));
  }
  out += p.other;
  PutFlag(out, "lockStructure", p.lockStructure, false);
  PutFlag(out, "lockWindows", p.lockWindows, false);
  PutFlag(out, "lockRevision", p.lockRevision, false);
  return out + "/>";
}

std::optional<SheetProtection> SheetProtectionOf(const Sheet& s) {
  const std::string& after = s.Text().afterData;
  if (after.find("sheetProtection") == std::string::npos) return std::nullopt;
  for (const xlsx::XmlPiece& p : xlsx::TopLevel(after))
    if (p.local == "sheetProtection") return ReadSheetProtection(std::string_view(after).substr(p.begin, p.end - p.begin));
  return std::nullopt;
}

bool SheetProtected(const Sheet& s) {
  const auto p = SheetProtectionOf(s);
  return p && p->sheet;
}

std::optional<WorkbookProtection> WorkbookProtectionOf(const Workbook& b) {
  return ReadWorkbookProtection(b.ProtectionXml());
}

bool StructureProtected(const Workbook& b) {
  const auto p = WorkbookProtectionOf(b);
  return p && p->lockStructure;
}

// ---- Cells

bool CellLocked(const Sheet& s, const Styles& st, uint32_t row, uint32_t col) {
  return LockedXf(st, s.StyleAt(row, col));
}

bool FormulaHidden(const Sheet& s, const Styles& st, uint32_t row, uint32_t col) {
  const auto& xfs = st.CellXfs();
  const uint32_t xf = s.StyleAt(row, col);
  return xf < xfs.size() && xfs[xf].protection.hidden;
}

bool AllUnlocked(const Sheet& s, const Styles& st, const Range& r) {
  // Outside the sheet: the edit refuses it anyway.
  if (r.firstRow > r.lastRow || r.firstCol > r.lastCol || r.lastRow >= kMaxRows || r.lastCol >= kMaxCols) return true;
  if (r.firstRow == r.lastRow && r.firstCol == r.lastCol) return !CellLocked(s, st, r.firstRow, r.firstCol);
  // An empty cell shows its row's format (when the row has one of its own), else its column's.
  std::unordered_set<uint32_t> formatted;  // rows of `r` with a format of their own
  bool ok = true;
  s.ForEachRow(r.firstRow, r.lastRow, [&](uint32_t row, const RowInfo& info) {
    if (info.customFormat) formatted.insert(row);
    return true;
  });
  const uint32_t width = r.lastCol - r.firstCol + 1;
  std::vector<uint32_t> stored(width, 0), storedInFormatted(width, 0);
  std::unordered_map<uint32_t, uint32_t> perRow;  // stored cells of formatted rows
  s.ForEachCell(r, [&](const CellInfo& c) {
    if (LockedXf(st, c.style)) return ok = false;
    ++stored[c.col - r.firstCol];
    if (formatted.count(c.row)) {
      ++storedInFormatted[c.col - r.firstCol];
      ++perRow[c.row];
    }
    return true;
  });
  if (!ok) return false;
  for (uint32_t row : formatted) {
    auto it = perRow.find(row);
    if ((it == perRow.end() || it->second < width) && LockedXf(st, s.Row(row).style)) return false;
  }
  const uint64_t height = uint64_t{r.lastRow} - r.firstRow + 1;
  for (uint32_t c = r.firstCol; c <= r.lastCol; ++c) {
    const uint32_t i = c - r.firstCol;
    // Rows with neither a cell here nor a format of their own show the column's format.
    const uint64_t covered = uint64_t{stored[i]} + formatted.size() - storedInFormatted[i];
    if (covered < height && LockedXf(st, s.Column(c).style)) return false;
  }
  return true;
}

// ---- Refusals

const char16_t* LockedCellMessage() { return kLockedCell; }

std::u16string ProtectionRefusal(const Workbook& b, int sheet, ProtectAct act, const Range& r) {
  if (act == ProtectAct::Structure) return StructureProtected(b) ? kStructure : u"";
  if (sheet < 0 || static_cast<size_t>(sheet) >= b.SheetCount()) return {};
  const Sheet& s = b.GetSheet(static_cast<size_t>(sheet));
  const std::optional<SheetProtection> p = SheetProtectionOf(s);
  if (!p || !p->sheet) return {};
  const Styles& st = b.GetStyles();
  auto unlocked = [&] { return AllUnlocked(s, st, r); };
  switch (act) {
    case ProtectAct::Content: return unlocked() ? u"" : kLockedCell;
    case ProtectAct::FormatCells: return p->formatCells ? kLockedCell : u"";
    case ProtectAct::FormatColumns: return p->formatColumns ? kLockedCell : u"";
    case ProtectAct::FormatRows: {
      if (!p->formatRows) return {};
      // Use AutoFilter: the filter hides and shows its own rows.
      if (!p->autoFilter && s.Filter() && r.firstRow >= s.Filter()->ref.firstRow && r.lastRow <= s.Filter()->ref.lastRow)
        return {};
      return kLockedCell;
    }
    case ProtectAct::InsertColumns: return p->insertColumns ? kLockedCell : u"";
    case ProtectAct::InsertRows: return p->insertRows ? kLockedCell : u"";
    case ProtectAct::DeleteColumns: return p->deleteColumns || !unlocked() ? kLockedCell : u"";
    case ProtectAct::DeleteRows: return p->deleteRows || !unlocked() ? kLockedCell : u"";
    case ProtectAct::InsertLink: return p->insertHyperlinks || !unlocked() ? kLockedCell : u"";
    case ProtectAct::Objects: return p->objects ? kLockedCell : u"";
    case ProtectAct::Sort: return p->sort || !unlocked() ? kLockedCell : u"";
    case ProtectAct::Filter: return p->autoFilter ? kCommand : u"";
    case ProtectAct::Never: return kCommand;
    case ProtectAct::Structure: break;
  }
  return {};
}

}  // namespace fw::sheet::edit
