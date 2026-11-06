// Word 97-2003 documents (D5, [MS-DOC]): the reader turns the binary streams into a package the
// docx reader reads, with Word's text, formatting, styles, lists, tables, fields, notes, bookmarks
// and sections. The streams are built here as Word lays them out, so the tests don't need Word.
#include "TestHarness.h"

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "doc/docx/Docx.h"
#include "doc/model/Utf.h"
#include "doc/msdoc/MsDoc.h"
#include "pkg/Check.h"

#ifndef REQUIRE
#define REQUIRE(cond)   do { if (!(cond)) { test::Fail(__FILE__, __LINE__, #cond); return; } } while (0)
#endif

using namespace fw;
using namespace fw::doc;
using Bytes = std::vector<uint8_t>;

namespace {

void Put16(Bytes& b, size_t at, uint32_t v) {
  if (b.size() < at + 2) b.resize(at + 2);
  b[at] = static_cast<uint8_t>(v), b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void Put32(Bytes& b, size_t at, uint32_t v) {
  Put16(b, at, v & 0xFFFF);
  Put16(b, at + 2, v >> 16);
}
void Add16(Bytes& b, uint32_t v) { Put16(b, b.size(), v); }
void Add32(Bytes& b, uint32_t v) { Put32(b, b.size(), v); }
void Add(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

// A sprm with its operand bytes.
Bytes Sprm(uint16_t op, Bytes operand) {
  Bytes b;
  Add16(b, op);
  Add(b, operand);
  return b;
}
Bytes S8(uint16_t op, uint8_t v) { return Sprm(op, {v}); }
Bytes S16(uint16_t op, uint32_t v) { return Sprm(op, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}); }
Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) Add(b, p);
  return b;
}

// A style (STD): StdfBase, the name, and its UPXs.
Bytes Std(uint16_t sti, int type, uint16_t base, std::u16string_view name, const Bytes& papx, const Bytes& chpx) {
  Bytes b;
  const int upx = type == 1 ? 2 : 1;
  Add16(b, sti);
  Add16(b, static_cast<uint32_t>(type) | (uint32_t{base} << 4));
  Add16(b, static_cast<uint32_t>(upx));
  Add16(b, 0);
  Add16(b, 0);
  Add16(b, static_cast<uint32_t>(name.size()));
  for (char16_t c : name) Add16(b, c);
  Add16(b, 0);
  auto upxOut = [&](const Bytes& u) {
    Add16(b, static_cast<uint32_t>(u.size()));
    Add(b, u);
    if (u.size() & 1) b.push_back(0);
  };
  if (type == 1) upxOut(Cat({{0, 0}, papx}));
  upxOut(chpx);
  return b;
}

// A Word 97 document's streams, as Word lays them out: the FIB, the text in one piece (UTF-16 or
// compressed), one page of character runs and one of paragraph runs, a stylesheet, a font table,
// one section and whatever other tables a test adds.
struct DocBuilder {
  std::u16string text;
  std::array<uint32_t, 8> ccp{};  // ccp[0] (the body) defaults to the whole text
  bool compressed = false;
  uint16_t nFib = 0xC1, flags = 0x0200;
  struct CharRun {
    uint32_t cpLim;
    Bytes grpprl;
  };
  std::vector<CharRun> chars;
  struct ParaRun {
    uint32_t cpLim;
    uint16_t istd;
    Bytes grpprl;
  };
  std::vector<ParaRun> paras;
  std::vector<Bytes> styles = {Std(0, 1, 0xFFF, u"Normal", {}, {})};
  std::vector<std::u16string> fonts = {u"Times New Roman", u"Arial"};
  Bytes sepx;
  std::map<size_t, Bytes> tables;  // FibRgFcLcb index -> its bytes in the table stream
  Bytes data;

  std::map<std::string, Bytes> Build() const {
    const uint32_t step = compressed ? 1 : 2, textFc = 1024;
    Bytes word(textFc, 0);
    Put16(word, 0, 0xA5EC);
    Put16(word, 2, nFib);
    Put16(word, 10, flags);
    Put16(word, 32, 14);
    Put16(word, 62, 22);
    std::array<uint32_t, 8> counts = ccp;
    if (counts[0] == 0) counts[0] = static_cast<uint32_t>(text.size());
    for (size_t k = 0; k < 8; ++k) Put32(word, 64 + 4 * (3 + k), counts[k]);
    Put16(word, 152, 93);
    for (char16_t c : text) {
      if (compressed) word.push_back(static_cast<uint8_t>(c));
      else Add16(word, c);
    }
    const uint32_t fcLim = textFc + static_cast<uint32_t>(text.size()) * step;
    uint32_t sepxFc = 0xFFFFFFFF;
    if (!sepx.empty()) {
      sepxFc = static_cast<uint32_t>(word.size());
      Add16(word, static_cast<uint32_t>(sepx.size()));
      Add(word, sepx);
    }
    word.resize((word.size() + 511) / 512 * 512);
    // The character runs' page.
    const uint32_t chpxPn = static_cast<uint32_t>(word.size() / 512);
    {
      Bytes page(512, 0);
      size_t free = 511;
      Put32(page, 0, textFc);
      for (size_t i = 0; i < chars.size(); ++i) {
        Put32(page, 4 * (i + 1), textFc + chars[i].cpLim * step);
        if (chars[i].grpprl.empty()) continue;
        free = (free - 1 - chars[i].grpprl.size()) & ~size_t{1};
        page[free] = static_cast<uint8_t>(chars[i].grpprl.size());
        std::copy(chars[i].grpprl.begin(), chars[i].grpprl.end(), page.begin() + static_cast<std::ptrdiff_t>(free + 1));
        page[4 * (chars.size() + 1) + i] = static_cast<uint8_t>(free / 2);
      }
      page[511] = static_cast<uint8_t>(chars.size());
      Add(word, page);
    }
    const uint32_t papxPn = static_cast<uint32_t>(word.size() / 512);
    {
      Bytes page(512, 0);
      size_t free = 511;
      Put32(page, 0, textFc);
      for (size_t i = 0; i < paras.size(); ++i) {
        Put32(page, 4 * (i + 1), textFc + paras[i].cpLim * step);
        Bytes d = Cat({{static_cast<uint8_t>(paras[i].istd), static_cast<uint8_t>(paras[i].istd >> 8)}, paras[i].grpprl});
        Bytes papx = d.size() & 1 ? Cat({{static_cast<uint8_t>((d.size() + 1) / 2)}, d})
                                  : Cat({{0, static_cast<uint8_t>(d.size() / 2)}, d});
        free = (free - papx.size()) & ~size_t{1};
        std::copy(papx.begin(), papx.end(), page.begin() + static_cast<std::ptrdiff_t>(free));
        page[4 * (paras.size() + 1) + 13 * i] = static_cast<uint8_t>(free / 2);
      }
      page[511] = static_cast<uint8_t>(paras.size());
      Add(word, page);
    }

    Bytes table;
    std::map<size_t, std::pair<uint32_t, uint32_t>> at;
    auto put = [&](size_t index, const Bytes& b) {
      at[index] = {static_cast<uint32_t>(table.size()), static_cast<uint32_t>(b.size())};
      Add(table, b);
    };
    uint32_t total = 0;
    for (uint32_t c : counts) total += c;
    {  // the piece table: one piece
      Bytes plc;
      Add32(plc, 0);
      Add32(plc, std::max<uint32_t>(total, static_cast<uint32_t>(text.size())));
      Add16(plc, 0);
      Add32(plc, compressed ? (textFc * 2) | 0x40000000 : textFc);
      Add16(plc, 0);
      Bytes clx = {2};
      Add32(clx, static_cast<uint32_t>(plc.size()));
      Add(clx, plc);
      put(33, clx);
    }
    auto bte = [&](uint32_t pn) {
      Bytes b;
      Add32(b, textFc);
      Add32(b, fcLim);
      Add32(b, pn);
      return b;
    };
    put(12, bte(chpxPn));
    put(13, bte(papxPn));
    {
      Bytes st;
      Add16(st, 18);
      Add16(st, static_cast<uint32_t>(styles.size()));
      Add16(st, 10);
      Add16(st, 1);
      st.resize(st.size() + 6, 0);
      Add16(st, 0);  // ascii: Times New Roman
      Add16(st, 0);
      Add16(st, 0);
      for (const Bytes& s : styles) {
        Add16(st, static_cast<uint32_t>(s.size()));
        Add(st, s);
      }
      put(1, st);
    }
    {
      Bytes f;
      Add16(f, static_cast<uint32_t>(fonts.size()));
      Add16(f, 0);
      for (const std::u16string& name : fonts) {
        Bytes ffn(39, 0);
        ffn[0] = 0x26;  // variable pitch, TrueType, roman
        for (char16_t c : name) Add16(ffn, c);
        Add16(ffn, 0);
        f.push_back(static_cast<uint8_t>(ffn.size()));
        Add(f, ffn);
      }
      put(15, f);
    }
    {
      Bytes sed;
      Add32(sed, 0);
      Add32(sed, counts[0]);
      Add16(sed, 0);
      Add32(sed, sepxFc);
      Add16(sed, 0);
      Add32(sed, 0);
      put(6, sed);
    }
    for (const auto& [index, bytes] : tables) put(index, bytes);
    for (const auto& [index, range] : at) {
      Put32(word, 154 + 8 * index, range.first);
      Put32(word, 154 + 8 * index + 4, range.second);
    }
    std::map<std::string, Bytes> out = {{"WordDocument", word}, {(flags & 0x0200) ? "1Table" : "0Table", table}};
    if (!data.empty()) out["Data"] = data;
    return out;
  }
};

msdoc::StreamFn From(const std::map<std::string, Bytes>& streams) {
  return [streams](std::string_view name) -> std::optional<Bytes> {
    auto it = streams.find(std::string(name));
    if (it == streams.end()) return std::nullopt;
    return it->second;
  };
}

// The document the streams make, read and checked as a package (null when either fails).
std::unique_ptr<Document> Open(const std::map<std::string, Bytes>& streams, pkg::Status* status = nullptr) {
  pkg::Status st;
  Bytes docx = msdoc::ToDocx(From(streams), st);
  if (status) *status = st;
  if (docx.empty()) {
    if (!status) std::printf("  ToDocx: %s\n", st.message.c_str());
    return nullptr;
  }
  auto pkg = test::OpenBytes(docx, st);
  if (!pkg) {
    std::printf("  package: %s\n", st.message.c_str());
    return nullptr;
  }
  const pkg::CheckReport report = pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  if (!report.Ok()) return nullptr;
  auto doc = docx::Read(std::move(pkg), st);
  if (!doc) std::printf("  docx: %s\n", st.message.c_str());
  return doc;
}

std::vector<uint32_t> Body(const Document& doc) { return doc.StoryParagraphs(doc.MainStory()); }
std::string TextOf(const Document& doc, uint32_t para) { return ToUtf8(doc.Para(para).text); }
const RunProps& RunAt(const Document& doc, uint32_t para, size_t offset) {
  for (const TextRun& r : doc.Runs(para))
    if (r.start <= offset && offset < r.end && r.run) return doc.ResolvedRun(para, r.run);
  static const RunProps none;
  return none;
}

}  // namespace

TEST(MsDoc_TextAndFormatting) {
  // Two paragraphs: "world" bold, 14 pt and in Arial; the first centered, the second indented.
  // The same in a compressed (8-bit) piece, whose 0x93 is a curly quote as in Windows-1252.
  for (bool compressed : {false, true}) {
    DocBuilder b;
    b.compressed = compressed;
    b.text = u"Hello world \x93x\x94\rSecond\r";
    if (!compressed) b.text[12] = u'\x201C', b.text[14] = u'\x201D';
    b.chars = {{6, {}}, {11, Cat({S8(0x0835, 1), S16(0x4A43, 28), S16(0x4A4F, 1)})}, {23, {}}};
    b.paras = {{16, 0, S8(0x2403, 1)}, {23, 0, S16(0x845E, 720)}};
    auto doc = Open(b.Build());
    REQUIRE(doc);
    const std::vector<uint32_t> p = Body(*doc);
    REQUIRE(p.size() == 2u);
    CHECK_EQ(TextOf(*doc, p[0]), std::string("Hello world \xE2\x80\x9Cx\xE2\x80\x9D"));
    CHECK_EQ(TextOf(*doc, p[1]), std::string("Second"));
    const RunProps& world = RunAt(*doc, p[0], 7);
    CHECK(world.bold && world.size.Value() == 28 && world.fonts.ascii == "Arial");
    CHECK(!RunAt(*doc, p[0], 2).bold && RunAt(*doc, p[0], 2).fonts.ascii == "Times New Roman");
    CHECK(doc->ResolvedPara(p[0]).jc == Justification::Center);
    CHECK(doc->ResolvedPara(p[1]).indStart.Value() == 720);
  }
}

TEST(MsDoc_StylesListsSectionsAndHeaders) {
  // Heading 1 (bold, 16 pt, outline level 1) on the first paragraph; the second numbered "1." from a
  // simple list; a landscape section with a title page and a header "Head".
  DocBuilder b;
  b.text = u"Title\rItem\rHead\r\r";
  b.ccp[0] = 11;
  b.ccp[2] = 6;
  b.styles.push_back(Std(1, 1, 0, u"heading 1,h1", Cat({S8(0x2640, 0), S8(0x2406, 1)}), Cat({S8(0x0835, 1), S16(0x4A43, 32)})));
  b.chars = {{17, {}}};
  b.paras = {{6, 1, {}}, {11, 0, Cat({S16(0x460B, 1), S8(0x260A, 0)})}, {16, 0, {}}, {17, 0, {}}};
  b.sepx = Cat({S16(0xB01F, 15840), S16(0xB020, 12240), S16(0xB021, 1440), S8(0x300A, 1)});
  {  // one simple list, decimal "%1.", indented, and one LFO for it
    Bytes lst;
    Add16(lst, 1);
    Add32(lst, 0x1234ABCD);
    Add32(lst, 0);
    for (int k = 0; k < 9; ++k) Add16(lst, 0x0FFF);
    lst.push_back(1);  // fSimpleList
    lst.push_back(0);
    const Bytes papx = Cat({S16(0x845E, 720), S16(0x8460, 0x10000 - 360)});
    Bytes lvl;
    Add32(lvl, 1);
    lvl.push_back(0);  // decimal
    lvl.push_back(0);
    lvl.resize(lvl.size() + 10, 0);
    lvl.resize(lvl.size() + 8, 0);
    lvl.push_back(0);  // no chpx
    lvl.push_back(static_cast<uint8_t>(papx.size()));
    lvl.push_back(0);
    lvl.push_back(0);
    Add(lvl, papx);
    Add16(lvl, 2);
    Add16(lvl, 0);
    Add16(lvl, '.');
    b.tables[73] = Cat({lst, lvl});
    Bytes lfo;
    Add32(lfo, 1);
    Add32(lfo, 0x1234ABCD);
    Add32(lfo, 0);
    Add32(lfo, 0);
    Add32(lfo, 0);
    Add32(lfo, 0xFFFFFFFF);
    b.tables[74] = lfo;
  }
  {  // headers: six separators, then the section's six stories, of which only the default header
    Bytes hdd;
    for (uint32_t cp : {0, 0, 0, 0, 0, 0, 0, 0, 5, 5, 5, 5, 5, 6}) Add32(hdd, cp);
    b.tables[11] = hdd;
  }
  auto doc = Open(b.Build());
  REQUIRE(doc);
  const std::vector<uint32_t> p = Body(*doc);
  REQUIRE(p.size() == 2u);
  const ParaProps& heading = doc->ResolvedPara(p[0]);
  CHECK(heading.style == "Heading1" && heading.outlineLevel == 0 && heading.keepNext);
  CHECK(RunAt(*doc, p[0], 0).bold && RunAt(*doc, p[0], 0).size.Value() == 32);
  CHECK(!RunAt(*doc, p[1], 0).bold);
  const std::optional<ListLabel> label = doc->Label(p[1]);
  CHECK(label && label->text == u"1." && doc->ResolvedPara(p[1]).indStart.Value() == 720);
  REQUIRE(doc->Sections().size() == 1u);
  const Section& sec = doc->Sections()[0];
  CHECK(sec.props.pageWidth.Value() == 15840 && sec.props.pageHeight.Value() == 12240 && sec.props.landscape);
  CHECK(sec.props.marginStart.Value() == 1440 && sec.props.titlePage);
  const uint32_t header = sec.headers[0];
  REQUIRE(header != kNone);
  const std::vector<uint32_t> hp = doc->StoryParagraphs(header);
  CHECK(!hp.empty() && TextOf(*doc, hp[0]) == "Head");
  CHECK(sec.headers[1] == kNone && sec.footers[0] == kNone);
}

TEST(MsDoc_TablesFieldsNotesBookmarksAndPictures) {
  // A row of two cells; then "See" (bookmarked), a PAGE field showing 1, a footnote and an inline
  // picture (1 x 0.5 inch, a PNG in the Data stream).
  DocBuilder b;
  b.text = u"A\x07" u"B\x07\x07See \x13 PAGE \x14" u"1\x15\x02\x01\r\x02 Note.\r\r";
  b.ccp[0] = 22;
  b.ccp[1] = 9;
  const Bytes spec = S8(0x0855, 1);
  b.chars = {{19, {}}, {20, spec}, {21, Cat({spec, Sprm(0x6A03, {0, 0, 0, 0})})}, {22, {}}, {23, spec}, {31, {}}};
  Bytes row = Cat({S8(0x2416, 1), S8(0x2417, 1), S16(0x9602, 108)});
  {
    Bytes def;
    Add16(def, 48);
    def.push_back(2);
    for (int x : {-108, 2000, 4000}) Add16(def, static_cast<uint32_t>(x) & 0xFFFF);
    def.resize(def.size() + 40, 0);
    Add(row, Sprm(0xD608, def));
    Add(row, S16(0x3404, 1));  // a header row: after the table definition, so its size must be right
  }
  b.paras = {{2, 0, S8(0x2416, 1)}, {4, 0, S8(0x2416, 1)}, {5, 0, row}, {22, 0, {}}, {30, 0, {}}, {31, 0, {}}};
  {  // the footnote: its reference at 19, its text from 0 to 8 in the footnote story
    Bytes ref;
    Add32(ref, 19);
    Add32(ref, 22);
    Add16(ref, 1);
    b.tables[2] = ref;
    Bytes txt;
    for (uint32_t cp : {0, 8, 9}) Add32(txt, cp);
    b.tables[3] = txt;
  }
  {  // the bookmark "mark" around "See"
    Bytes names;
    Add16(names, 0xFFFF);
    Add16(names, 1);
    Add16(names, 0);
    Add16(names, 4);
    for (char16_t c : std::u16string(u"mark")) Add16(names, c);
    b.tables[21] = names;
    Bytes bkf;
    Add32(bkf, 5);
    Add32(bkf, 22);
    Add16(bkf, 0);
    Add16(bkf, 0);
    b.tables[22] = bkf;
    Bytes bkl;
    Add32(bkl, 8);
    Add32(bkl, 22);
    b.tables[23] = bkl;
  }
  {  // the picture: a PICF, the shape, and an FBSE holding the PNG
    const Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3, 4};
    Bytes blip;
    Add16(blip, 0x6E0 << 4);
    Add16(blip, 0xF01E);
    Add32(blip, static_cast<uint32_t>(17 + png.size()));
    blip.resize(blip.size() + 17, 0);
    Add(blip, png);
    Bytes fbse;
    Add16(fbse, 0x0002);
    Add16(fbse, 0xF007);
    Add32(fbse, static_cast<uint32_t>(36 + blip.size()));
    fbse.resize(fbse.size() + 36, 0);
    Add(fbse, blip);
    Bytes picf(0x44, 0);
    Put16(picf, 4, 0x44);
    Put16(picf, 6, 0x64);
    Put16(picf, 28, 1440);
    Put16(picf, 30, 720);
    Put16(picf, 32, 1000);
    Put16(picf, 34, 1000);
    Bytes sp;
    Add16(sp, 0x000F);
    Add16(sp, 0xF004);
    Add32(sp, 0);
    b.data = Cat({picf, sp, fbse});
    Put32(b.data, 0, static_cast<uint32_t>(b.data.size()));
  }
  auto doc = Open(b.Build());
  REQUIRE(doc);
  const std::vector<uint32_t> p = Body(*doc);
  REQUIRE(p.size() == 3u);  // two cells' paragraphs, then the body's
  CHECK_EQ(TextOf(*doc, p[0]), std::string("A"));
  CHECK_EQ(TextOf(*doc, p[1]), std::string("B"));
  const Node tbl = doc->ParaNode(p[0]).Parent().Parent().Parent();
  CHECK(tbl.Is(Name("tbl")) && doc->TableRows(tbl).size() == 1u && doc->RowCells(doc->TableRows(tbl)[0]).size() == 2u);
  CHECK(doc->TableGrid(tbl).size() == 2u && doc->TableGrid(tbl)[0].Value() == 2108);
  CHECK(doc->TableRows(tbl)[0].Path({Name("trPr"), Name("tblHeader")}));
  const std::vector<Field> fields = doc->Fields(doc->MainStory());
  CHECK(fields.size() == 1u && fields[0].Type() == u"PAGE");
  // The footnote: referred to, with its text.
  bool referred = false;
  for (const InlineItem& it : doc->Items(p[2])) referred = referred || it.kind == ItemKind::FootnoteRef;
  CHECK(referred);
  const uint32_t note = doc->NoteStory(StoryKind::Footnote, "1");
  REQUIRE(note != kNone);
  CHECK(TextOf(*doc, doc->StoryParagraphs(note)[0]).ends_with(" Note."));
  CHECK(doc->MainStory() != kNone && doc->Pairs().size() >= 1u);
  bool bookmark = false;
  for (const PairInfo& pr : doc->Pairs()) bookmark = bookmark || pr.open.AttrOr(Name("name")) == "mark";
  CHECK(bookmark);
  REQUIRE(doc->Drawings().size() == 1u);
  CHECK(doc->Drawings()[0].cx.Value() == 914400 && doc->Drawings()[0].cy.Value() == 457200);
  std::vector<uint8_t> image;
  CHECK(doc->Package().ReadPart("/word/media/image1.png", image).Ok() && image.size() == 12u);
}

TEST(MsDoc_RefusesWhatItCannotRead) {
  using Code = pkg::Status::Code;
  auto status = [](const std::map<std::string, Bytes>& streams) {
    pkg::Status st;
    const Bytes docx = msdoc::ToDocx(From(streams), st);
    return docx.empty() ? st.code : Code::Ok;
  };
  DocBuilder b;
  b.text = u"Text\r";
  b.chars = {{5, {}}};
  b.paras = {{5, 0, {}}};
  const auto good = b.Build();
  CHECK(status(good) == Code::Ok);
  CHECK(status({}) == Code::Damaged);                            // not a Word document
  CHECK(status({{"WordDocument", Bytes(600, 0)}}) == Code::Damaged);  // no FIB
  {
    auto streams = good;
    streams.erase("1Table");
    CHECK(status(streams) == Code::Damaged);
  }
  DocBuilder old = b;
  old.nFib = 0x65;  // Word 6
  CHECK(status(old.Build()) == Code::Unsupported);
  DocBuilder locked = b;
  locked.flags |= 0x0100;
  CHECK(status(locked.Build()) == Code::Unsupported);
  CHECK(msdoc::IsEncrypted(locked.Build().at("WordDocument")) && !msdoc::IsEncrypted(good.at("WordDocument")));
  {  // decrypted streams keep the flag; the reader is told
    pkg::Status st;
    CHECK(!msdoc::ToDocx(From(locked.Build()), st, true).empty());
  }
  // Cut short anywhere, or with any byte of the FIB changed, the streams never crash the reader:
  // it either fails or makes a package the docx reader takes.
  for (const char* name : {"WordDocument", "1Table"}) {
    const Bytes& whole = good.at(name);
    for (size_t n = 0; n < whole.size(); n += 37) {
      auto streams = good;
      streams[name].resize(n);
      pkg::Status st;
      const Bytes docx = msdoc::ToDocx(From(streams), st);
      if (!docx.empty()) CHECK(test::OpenBytes(docx, st) != nullptr);
    }
  }
  for (size_t at = 0; at < 1024; at += 3) {
    auto streams = good;
    streams["WordDocument"][at] ^= 0xA5;
    pkg::Status st;
    const Bytes docx = msdoc::ToDocx(From(streams), st);
    if (!docx.empty()) {
      auto pkg = test::OpenBytes(docx, st);
      CHECK(pkg && docx::Read(std::move(pkg), st) != nullptr);
    }
  }
}
