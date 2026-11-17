#pragma once

// Helpers shared by the workbook edit and save tests.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "sheet/model/Workbook.h"
#include "sheet/xlsx/Xlsx.h"

namespace sheet_test {

inline constexpr const char* kMain = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

// A workbook package's parts with these sheets (the worksheet's content inside <worksheet>), two
// defined names, two cell formats and no shared strings.
inline std::vector<std::pair<std::string, std::string>> WorkbookParts(const std::vector<std::string>& sheets) {
  std::string types =
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/xl/workbook.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
      "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
  std::string rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                     "<Relationship Id=\"rS\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>";
  std::string list;
  std::vector<std::pair<std::string, std::string>> parts;
  for (size_t i = 0; i < sheets.size(); ++i) {
    std::string n = std::to_string(i + 1);
    types += "<Override PartName=\"/xl/worksheets/sheet" + n +
             ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    rels += "<Relationship Id=\"rId" + n +
            "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" +
            n + ".xml\"/>";
    list += "<sheet name=\"Sheet" + n + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
    parts.push_back({"xl/worksheets/sheet" + n + ".xml", "<worksheet xmlns=\"" + std::string(kMain) + "\">" + sheets[i] + "</worksheet>"});
  }
  parts.insert(parts.begin(),
               {{"[Content_Types].xml", types + "</Types>"},
                {"_rels/.rels",
                 "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                 "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
                 "Target=\"xl/workbook.xml\"/></Relationships>"},
                {"xl/workbook.xml", "<workbook xmlns=\"" + std::string(kMain) +
                                        "\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>" +
                                        list + "</sheets><definedNames><definedName name=\"Total\">Sheet1!$A$1</definedName>"
                                        "<definedName name=\"Local\" localSheetId=\"1\">Sheet2!$B$2</definedName></definedNames></workbook>"},
                {"xl/_rels/workbook.xml.rels", rels + "</Relationships>"},
                {"xl/styles.xml", "<styleSheet xmlns=\"" + std::string(kMain) +
                                      "\"><fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
                                      "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
                                      "<borders count=\"1\"><border/></borders><cellStyleXfs count=\"1\"><xf/></cellStyleXfs>"
                                      "<cellXfs count=\"2\"><xf/><xf numFmtId=\"14\" applyNumberFormat=\"1\"/></cellXfs></styleSheet>"}});
  return parts;
}

// A workbook read, every sheet loaded, from a zip of these parts.
inline std::unique_ptr<fw::sheet::Workbook> OpenZip(std::vector<uint8_t> zip) {
  fw::pkg::Status st;
  auto pkg = test::OpenBytes(std::move(zip), st);
  CHECK(pkg != nullptr);
  if (!pkg) return nullptr;
  auto book = fw::sheet::xlsx::Read(std::move(pkg), st, {.loadAll = true});
  CHECK(book != nullptr);
  return book;
}

inline std::unique_ptr<fw::sheet::Workbook> OpenParts(const std::vector<std::string>& sheets) {
  return OpenZip(test::MakeZip(WorkbookParts(sheets)));
}

// A file of the repository's tests folder ("sheet/objects.xlsx"), whole.
inline std::vector<uint8_t> TestFile(const char* name) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / name;
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string Text(const fw::sheet::Sheet& s, uint32_t row, uint32_t col) {
  using namespace fw::sheet;
  Value v = s.GetValue(row, col);
  if (v.type == ValueType::String) return std::string(v.text);
  if (v.type == ValueType::Number) return std::to_string(static_cast<long long>(v.number));
  return v.type == ValueType::Empty ? "" : "?";
}

inline std::string FormulaText(const fw::sheet::Sheet& s, uint32_t row, uint32_t col) {
  fw::sheet::FormulaInfo f;
  return s.GetFormula(row, col, f) ? std::string(f.text) : std::string("-");
}

}  // namespace sheet_test
