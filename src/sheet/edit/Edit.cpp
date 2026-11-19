#include "sheet/edit/Edit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <new>
#include <unordered_map>
#include <unordered_set>

#include "pkg/Formula.h"
#include "pkg/OfficeCrypto.h"
#include "pkg/XmlWriter.h"
#include "sheet/calc/Formula.h"
#include "sheet/calc/Typing.h"
#include "sheet/edit/Charts.h"
#include "sheet/edit/Drawing.h"
#include "sheet/edit/Journal.h"
#include "sheet/edit/Notes.h"
#include "sheet/edit/PageSetupXml.h"
#include "sheet/edit/Refs.h"
#include "sheet/xlsx/Internal.h"
#include "sheet/numfmt/NumFormat.h"
#include "core/Strings.h"

namespace fw::sheet::edit {

using detail::CellRec;
using detail::CellSide;
using detail::FormulaRec;
using detail::RowBlock;
using detail::RowRec;

// The model's insides, for the edit layer only (a friend of Sheet and Workbook).
class Access {
 public:
  static std::vector<std::shared_ptr<RowBlock>>& Blocks(Sheet& s) { return s.blocks_; }
  static std::vector<uint32_t>& FirstRows(Sheet& s) { return s.blockFirstRow_; }
  static size_t& CellCount(Sheet& s) { return s.cellCount_; }
  static size_t& FormulaCells(Sheet& s) { return s.formulaCells_; }
  static std::vector<FormulaRec>& Formulas(Sheet& s) { return s.formulas_; }
  static TextPool& Texts(Sheet& s) { return s.texts_; }
  static pkg::RoundTripStore& Store(Sheet& s) { return s.store_; }
  static std::vector<ColumnInfo>& Cols(Sheet& s) { return s.cols_; }
  static std::vector<Range>& Merges(Sheet& s) { return s.merges_; }
  static uint32_t& MergeMaxHeight(Sheet& s) { return s.mergeMaxHeight_; }
  static std::vector<SheetView>& Views(Sheet& s) { return s.views_; }
  static SheetProps& Props(Sheet& s) { return s.props_; }
  static SheetFormat& Format(Sheet& s) { return s.format_; }
  static std::vector<Hyperlink>& Links(Sheet& s) { return s.links_; }
  static std::vector<Table>& Tables(Sheet& s) { return s.tables_; }
  static std::optional<SheetFilter>& Filter(Sheet& s) { return s.filter_; }
  static std::vector<PageBreak>& Breaks(Sheet& s, bool rows) { return rows ? s.rowBreaks_ : s.colBreaks_; }
  static PageSetup& Page(Sheet& s) { return s.page_; }
  static HeaderFooter& HF(Sheet& s) { return s.headerFooter_; }
  static std::vector<detail::Kept>& Kept(Sheet& s) { return s.kept_; }
  static std::vector<std::pair<uint32_t, CellAddr>>& SharedMasters(Sheet& s) { return s.sharedMasters_; }
  static std::string& Name(Sheet& s) { return s.name_; }
  static SheetState& State(Sheet& s) { return s.state_; }
  static uint32_t SheetId(const Sheet& s) { return s.sheetId_; }
  static std::string& Part(Sheet& s) { return s.part_; }
  static std::string& RelId(Sheet& s) { return s.relId_; }
  static detail::PartText& Text(Sheet& s) { return s.text_; }
  static std::string& StringsPart(Workbook& b) { return b.stringsPart_; }
  static FileFormat& Format(Workbook& b) { return b.format_; }
  static std::vector<std::unique_ptr<Sheet>>& Sheets(Workbook& b) { return b.sheets_; }
  static std::vector<DefinedName>& Names(Workbook& b) { return b.names_; }
  static uint32_t& Active(Workbook& b) { return b.activeSheet_; }
  static uint32_t& FirstTab(Workbook& b) { return b.firstTab_; }
  static std::string& ProtectionXml(Workbook& b) { return b.protectionXml_; }
};

namespace {

using A = Access;
constexpr uint32_t kBlockRows = SheetBuilder::kBlockRows;

// What a step saved of a sheet (SheetSave::saved).
enum SaveBit : uint32_t {
  kSaveCells = 1u << 0,  // blocks (touched ones, or all with the layout) and the counts
  kSaveCols = 1u << 1,
  kSaveMerges = 1u << 2,
  kSaveViews = 1u << 3,
  kSaveProps = 1u << 4,
  kSaveFormat = 1u << 5,
  kSaveLinks = 1u << 6,
  kSaveName = 1u << 7,
  kSaveState = 1u << 8,
  // Structural edits (the reference registry): what they move besides cells.
  kSaveText = 1u << 9,      // the part's text around sheetData
  kSaveTables = 1u << 10,
  kSaveFilter = 1u << 11,
  kSaveBreaks = 1u << 12,   // row and column page breaks
  kSaveKept = 1u << 13,     // XML kept inside sheetData, and shared formulas' masters
  kSaveFormulas = 1u << 14, // every formula record (else `formulas` holds the ones changed in place)
  kSavePage = 1u << 24,     // page setup and headers and footers (s3-print)
};
constexpr int kPieces = 7;  // SheetPiece bits

// A sheet as it was before a step (or, once undone, as it was after it): only what the step
// changed. Undo and redo swap it with the sheet's.
struct SheetSave {
  Sheet* sheet = nullptr;
  uint32_t saved = 0;  // SaveBit
  uint32_t dirty = 0;  // SheetPiece bits the step changed
  bool fullBlocks = false;
  std::vector<std::pair<uint32_t, std::shared_ptr<RowBlock>>> touched;  // !fullBlocks: block index -> version
  std::vector<std::shared_ptr<RowBlock>> blocks;                        // fullBlocks: every block
  std::vector<uint32_t> firstRows;
  size_t cellCount = 0, formulaCells = 0;
  std::vector<std::pair<uint32_t, FormulaRec>> formulas;  // formulas changed in place
  std::vector<ColumnInfo> cols;
  std::vector<Range> merges;
  uint32_t mergeMaxHeight = 0;
  std::vector<SheetView> views;
  SheetProps props;
  SheetFormat format;
  std::vector<Hyperlink> links;
  std::string name;
  SheetState state = SheetState::Visible;
  detail::PartText text;
  std::vector<Table> tables;
  std::optional<SheetFilter> filter;
  std::vector<PageBreak> rowBreaks, colBreaks;
  PageSetup page;
  HeaderFooter headerFooter;
  std::vector<detail::Kept> kept;
  std::vector<std::pair<uint32_t, CellAddr>> masters;
  std::vector<FormulaRec> allFormulas;

  void Swap() {
    Sheet& s = *sheet;
    if (saved & kSaveCells) {
      if (fullBlocks) {
        std::swap(A::Blocks(s), blocks);
        std::swap(A::FirstRows(s), firstRows);
      } else {
        for (auto& [i, p] : touched) std::swap(A::Blocks(s)[i], p);
      }
      std::swap(A::CellCount(s), cellCount);
      std::swap(A::FormulaCells(s), formulaCells);
    }
    for (auto& [id, rec] : formulas) std::swap(A::Formulas(s)[id], rec);
    if (saved & kSaveFormulas) std::swap(A::Formulas(s), allFormulas);
    if (saved & kSaveText) std::swap(A::Text(s), text);
    if (saved & kSaveTables) std::swap(A::Tables(s), tables);
    if (saved & kSaveFilter) std::swap(A::Filter(s), filter);
    if (saved & kSaveBreaks) {
      std::swap(A::Breaks(s, true), rowBreaks);
      std::swap(A::Breaks(s, false), colBreaks);
    }
    if (saved & kSaveKept) {
      std::swap(A::Kept(s), kept);
      std::swap(A::SharedMasters(s), masters);
    }
    if (saved & kSavePage) {
      std::swap(A::Page(s), page);
      std::swap(A::HF(s), headerFooter);
    }
    if (saved & kSaveCols) std::swap(A::Cols(s), cols);
    if (saved & kSaveMerges) {
      std::swap(A::Merges(s), merges);
      std::swap(A::MergeMaxHeight(s), mergeMaxHeight);
    }
    if (saved & kSaveViews) std::swap(A::Views(s), views);
    if (saved & kSaveProps) std::swap(A::Props(s), props);
    if (saved & kSaveFormat) std::swap(A::Format(s), format);
    if (saved & kSaveLinks) std::swap(A::Links(s), links);
    if (saved & kSaveName) std::swap(A::Name(s), name);
    if (saved & kSaveState) std::swap(A::State(s), state);
  }

  // Takes in what a later step (an inner group) saved: the older version stays.
  void Absorb(SheetSave&& later) {
    if (later.saved & kSaveCells) {
      if (!(saved & kSaveCells)) {
        fullBlocks = later.fullBlocks;
        touched = std::move(later.touched);
        blocks = std::move(later.blocks);
        firstRows = std::move(later.firstRows);
        cellCount = later.cellCount;
        formulaCells = later.formulaCells;
      } else if (!fullBlocks && later.fullBlocks) {
        // The later step saved the whole layout as this one left it: put back the blocks this one
        // touched as they were before it.
        blocks = std::move(later.blocks);
        firstRows = std::move(later.firstRows);
        for (auto& [i, p] : touched) blocks[i] = std::move(p);
        touched.clear();
        fullBlocks = true;
      } else if (!fullBlocks) {
        for (auto& t : later.touched)
          if (std::none_of(touched.begin(), touched.end(), [&](const auto& x) { return x.first == t.first; }))
            touched.push_back(std::move(t));
      }
    }
    if (later.saved & kSaveFormulas) {
      if (!(saved & kSaveFormulas)) {
        // The later step kept every record as this one left them: put back the ones it changed.
        allFormulas = std::move(later.allFormulas);
        for (auto& [id, rec] : formulas)
          if (id < allFormulas.size()) allFormulas[id] = std::move(rec);
        formulas.clear();
      }
    } else if (!(saved & kSaveFormulas)) {
      for (auto& f : later.formulas)
        if (std::none_of(formulas.begin(), formulas.end(), [&](const auto& x) { return x.first == f.first; }))
          formulas.push_back(std::move(f));
    }
    auto take = [&](uint32_t bit, auto& mine, auto& theirs) {
      if ((later.saved & bit) && !(saved & bit)) mine = std::move(theirs);
    };
    take(kSaveCols, cols, later.cols);
    if ((later.saved & kSaveMerges) && !(saved & kSaveMerges)) {
      merges = std::move(later.merges);
      mergeMaxHeight = later.mergeMaxHeight;
    }
    take(kSaveViews, views, later.views);
    take(kSaveProps, props, later.props);
    take(kSaveFormat, format, later.format);
    take(kSaveLinks, links, later.links);
    take(kSaveName, name, later.name);
    take(kSaveState, state, later.state);
    take(kSaveText, text, later.text);
    take(kSaveTables, tables, later.tables);
    take(kSaveFilter, filter, later.filter);
    if ((later.saved & kSaveBreaks) && !(saved & kSaveBreaks)) {
      rowBreaks = std::move(later.rowBreaks);
      colBreaks = std::move(later.colBreaks);
    }
    if ((later.saved & kSaveKept) && !(saved & kSaveKept)) {
      kept = std::move(later.kept);
      masters = std::move(later.masters);
    }
    if ((later.saved & kSavePage) && !(saved & kSavePage)) {
      page = std::move(later.page);
      headerFooter = std::move(later.headerFooter);
    }
    saved |= later.saved;
    dirty |= later.dirty;
  }
};

// The workbook's own state as it was: the list of sheets, the names, the active tab.
struct BookSave {
  bool sheets = false, names = false, active = false, protection = false;
  std::vector<Sheet*> order;
  std::vector<std::unique_ptr<Sheet>> held;  // sheets of `order` the workbook doesn't hold now
  std::vector<DefinedName> nameList;
  uint32_t activeSheet = 0, firstTab = 0;
  std::string protectionXml;

  void Swap(Workbook& b) {
    if (sheets) {
      auto& list = A::Sheets(b);
      std::vector<Sheet*> now;
      for (auto& p : list) now.push_back(p.get());
      std::vector<std::unique_ptr<Sheet>> pool = std::move(list);
      for (auto& h : held) pool.push_back(std::move(h));
      held.clear();
      list.clear();
      for (Sheet* s : order)
        for (auto& p : pool)
          if (p.get() == s) {
            list.push_back(std::move(p));
            break;
          }
      for (auto& p : pool)
        if (p) held.push_back(std::move(p));
      order = std::move(now);
    }
    if (names) std::swap(A::Names(b), nameList);
    if (active) {
      std::swap(A::Active(b), activeSheet);
      std::swap(A::FirstTab(b), firstTab);
    }
    if (protection) std::swap(A::ProtectionXml(b), protectionXml);
  }

  void Absorb(BookSave&& later) {
    if (later.sheets && !sheets) {
      sheets = true;
      order = std::move(later.order);
    }
    for (auto& h : later.held) held.push_back(std::move(h));  // sheets added and deleted inside
    if (later.names && !names) {
      names = true;
      nameList = std::move(later.nameList);
    }
    if (later.active && !active) {
      active = true;
      activeSheet = later.activeSheet;
      firstTab = later.firstTab;
    }
    if (later.protection && !protection) {
      protection = true;
      protectionXml = std::move(later.protectionXml);
    }
  }
};

struct InChange {
  Sheet* sheet = nullptr;  // null: the workbook
  Change change;
};

struct Step {
  std::u16string name;
  std::vector<SheetSave> sheets;
  BookSave book;
  bool bookDirty = false;
  std::vector<InChange> changes;
  // Package parts the step rewrote, added or removed (drawings, charts, notes, tables...), as they
  // were; the content types and the relationship sets (by source; none: it had none) likewise.
  struct PartWas {
    std::string name;
    bool existed = false;
    std::vector<uint8_t> bytes;
  };
  std::vector<PartWas> parts;
  std::optional<pkg::ContentTypes> types;
  std::vector<std::pair<std::string, std::optional<pkg::RelationshipSet>>> rels;
  uint64_t serial = 0;

  bool Empty() const {
    return sheets.empty() && !book.sheets && !book.names && !book.active && changes.empty() && parts.empty() &&
           !types && rels.empty();
  }

  // Undo or redo: everything back the other way.
  void Swap(Workbook& b) {
    for (auto it = sheets.rbegin(); it != sheets.rend(); ++it) it->Swap();
    book.Swap(b);
    pkg::Package* package = b.GetPackage();
    if (!package) return;
    // Relationships first: one put back may add its content type, which the types then settle.
    for (auto& [source, set] : rels) {
      std::optional<pkg::RelationshipSet> now;
      if (const pkg::RelationshipSet* r = package->RelationshipsOf(source)) now = *r;
      if (set) {
        package->SetRelationships(*set);
      } else if (now) {
        pkg::RelationshipSet none = *now;
        none.items.clear();
        package->SetRelationships(none);
        package->DeletePart(none.relsPart);
      }
      set = std::move(now);
    }
    if (types) {
      pkg::ContentTypes now = package->Types();
      package->SetContentTypes(std::move(*types));
      types = std::move(now);
    }
    for (PartWas& p : parts) {
      const bool exists = package->FindPart(p.name) != nullptr;
      std::vector<uint8_t> now;
      if (exists) package->ReadPart(p.name, now);
      if (p.existed) package->SetPart(p.name, std::move(p.bytes));
      else if (exists) package->DeletePart(p.name);
      p.existed = exists;
      p.bytes = std::move(now);
    }
  }

  void Absorb(Step&& later) {
    for (auto& s : later.sheets) {
      auto it = std::find_if(sheets.begin(), sheets.end(), [&](const SheetSave& x) { return x.sheet == s.sheet; });
      if (it == sheets.end()) sheets.push_back(std::move(s));
      else it->Absorb(std::move(s));
    }
    book.Absorb(std::move(later.book));
    bookDirty = bookDirty || later.bookDirty;
    for (auto& c : later.changes) changes.push_back(std::move(c));
    for (auto& p : later.parts)
      if (std::none_of(parts.begin(), parts.end(), [&](const PartWas& x) { return x.name == p.name; }))
        parts.push_back(std::move(p));
    if (!types) types = std::move(later.types);
    for (auto& r : later.rels)
      if (std::none_of(rels.begin(), rels.end(), [&](const auto& x) { return x.first == r.first; }))
        rels.push_back(std::move(r));
  }
};

// Sheet and defined names, for compiling and printing formulas (shared formulas split into their cells).
class BookNames : public calc::Names {
 public:
  explicit BookNames(const Workbook& b) : b_(b) {}
  int FindSheet(std::string_view name) const override { return b_.FindSheet(name); }
  std::string SheetName(int sheet) const override {
    return sheet >= 0 && static_cast<size_t>(sheet) < b_.SheetCount() ? b_.GetSheet(static_cast<size_t>(sheet)).Name()
                                                                       : std::string();
  }
  uint32_t NameSlot(std::string_view name) override {
    for (size_t i = 0; i < slots_.size(); ++i)
      if (NameEqualsNoCase(slots_[i], name)) return static_cast<uint32_t>(i);
    slots_.emplace_back(name);
    return static_cast<uint32_t>(slots_.size() - 1);
  }
  std::string SlotName(uint32_t slot) const override { return slot < slots_.size() ? slots_[slot] : std::string(); }

 private:
  const Workbook& b_;
  std::vector<std::string> slots_;
};

// The first sheet a formula names that the workbook doesn't have (another workbook's, [1]Sheet1!A1,
// isn't asked for); empty when there's none.
std::string MissingSheet(const Workbook& b, std::string_view formula) {
  class Recording : public BookNames {
   public:
    using BookNames::BookNames;
    int FindSheet(std::string_view name) const override {
      int i = BookNames::FindSheet(name);
      if (i < 0 && missing.empty()) missing = std::string(name);
      return i;
    }
    mutable std::string missing;
  } names(b);
  calc::CompileOptions at;
  (void)calc::Compile(formula, at, names);
  return names.missing;
}

// (Not std::to_chars for doubles: it brings over 100 KB of tables into the exe.)
std::string Number(double v) {
  char buf[48];
  return std::string(buf, xlsx::FormatNumber(v, 0, buf));
}

// Characters XML 1.0 can't hold (and Excel's files never do).
bool XmlSafe(std::string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
    if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBF &&
        (static_cast<unsigned char>(s[i + 2]) == 0xBE || static_cast<unsigned char>(s[i + 2]) == 0xBF))
      return false;  // U+FFFE, U+FFFF
  }
  return true;
}

size_t Utf16Length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? (c >= 0xF0 ? 2 : 1) : 0;
  return n;
}

bool WholeRows(const Range& r) { return r.firstCol == 0 && r.lastCol == kMaxCols - 1; }
bool WholeCols(const Range& r) { return r.firstRow == 0 && r.lastRow == kMaxRows - 1; }
bool Valid(const Range& r) {
  return r.firstRow <= r.lastRow && r.firstCol <= r.lastCol && r.lastRow < kMaxRows && r.lastCol < kMaxCols;
}
bool Overlaps(const Range& a, const Range& b) {
  return a.firstRow <= b.lastRow && b.firstRow <= a.lastRow && a.firstCol <= b.lastCol && b.firstCol <= a.lastCol;
}
bool Inside(const Range& inner, const Range& outer) {
  return inner.firstRow >= outer.firstRow && inner.lastRow <= outer.lastRow && inner.firstCol >= outer.firstCol &&
         inner.lastCol <= outer.lastCol;
}

}  // namespace

// ---------- The editor's state ----------

struct Editor::State {
  Workbook& b;
  std::vector<Step> undo, redo;
  std::vector<Step> open;  // the groups being built, outermost first
  uint64_t serial = 0, savedSerial = 0;
  std::unordered_map<const Sheet*, std::array<int, kPieces>> dirty;
  std::unordered_set<const Sheet*> viewDirty;
  int bookDirty = 0;
  bool bookViewDirty = false;
  size_t xfsAtSave = 0, fontsAtSave = 0, fillsAtSave = 0, bordersAtSave = 0, numFmtsAtSave = 0, stringsAtSave = 0;
  size_t cellStylesAtSave = 0, cellStylesSaving = 0;  // cell style xfs and cell styles, together
  // A save in progress (BeginSave): what it writes, so that view changes and formats added
  // meanwhile still count as unsaved after it.
  bool saving = false;
  std::unordered_set<const Sheet*> viewsWhileSaving;
  bool bookViewWhileSaving = false;
  size_t xfsSaving = 0, fontsSaving = 0, fillsSaving = 0, bordersSaving = 0, numFmtsSaving = 0, stringsSaving = 0;
  // Written from the model by an earlier save (Editor::MustWrite).
  std::unordered_set<const Sheet*> mustWrite;
  bool mustWriteStrings = false;
  // Plain shared strings by hash, so text typed twice is stored once (Excel's own rule).
  std::unordered_multimap<size_t, uint32_t> stringIndex;
  uint32_t stringsIndexed = 0;

  explicit State(Workbook& book) : b(book) { Saved(); }

  void Saved() {
    const Styles& st = b.GetStyles();
    xfsAtSave = st.CellXfs().size();
    fontsAtSave = st.Fonts().size();
    fillsAtSave = st.Fills().size();
    bordersAtSave = st.Borders().size();
    numFmtsAtSave = st.CustomFormats().size();
    stringsAtSave = b.Strings().Size();
    cellStylesAtSave = st.CellStyleXfs().size() + st.CellStyles().size() + st.Dxfs().size();
  }

  void ViewChanged(const Sheet* s) {
    viewDirty.insert(s);
    if (saving) viewsWhileSaving.insert(s);
  }
  void BookViewChanged() {
    bookViewDirty = true;
    if (saving) bookViewWhileSaving = true;
  }

  Step& Cur() { return open.back(); }

  SheetSave& Save(Sheet& s) {
    Step& st = Cur();
    for (auto& x : st.sheets)
      if (x.sheet == &s) return x;
    st.sheets.emplace_back();
    st.sheets.back().sheet = &s;
    return st.sheets.back();
  }

  int IndexOf(const Sheet* s) const {
    for (size_t i = 0; i < b.SheetCount(); ++i)
      if (&b.GetSheet(i) == s) return static_cast<int>(i);
    return -1;
  }

  void Change(Sheet* s, uint32_t what, const Range& cells, const Shift& shift = {}) {
    InChange c;
    c.sheet = s;
    c.change.what = what;
    c.change.cells = cells;
    c.change.shift = shift;
    Cur().changes.push_back(c);
  }

  // A step done (+1) or undone (-1). Dirty counts measure the distance from the file: undoing a
  // step the file already holds moves away from it, and redoing it moves back.
  void Count(const Step& st, int by, bool undoRedo = false) {
    if (undoRedo && st.serial <= savedSerial) by = -by;
    for (const SheetSave& s : st.sheets)
      for (int p = 0; p < kPieces; ++p)
        if (s.dirty & (1u << p)) dirty[s.sheet][p] += by;
    if (st.bookDirty) bookDirty += by;
  }

  std::vector<edit::Change> Resolve(const Step& st, bool reverse) const {
    std::vector<edit::Change> out;
    for (const InChange& c : st.changes) {
      edit::Change x = c.change;
      x.sheet = c.sheet ? IndexOf(c.sheet) : -1;
      if (reverse && (x.what & kShift)) x.shift.count = -x.shift.count;
      out.push_back(x);
    }
    if (reverse) std::reverse(out.begin(), out.end());
    return out;
  }

  // ---- Cells

  void SaveCells(SheetSave& sv) {
    sv.dirty |= kPieceCells;
    if (sv.saved & kSaveCells) return;
    sv.saved |= kSaveCells;
    sv.cellCount = A::CellCount(*sv.sheet);
    sv.formulaCells = A::FormulaCells(*sv.sheet);
  }

  // Before the list of blocks changes: the step keeps all of them.
  void SaveLayout(SheetSave& sv) {
    SaveCells(sv);
    if (sv.fullBlocks) return;
    sv.blocks = A::Blocks(*sv.sheet);
    sv.firstRows = A::FirstRows(*sv.sheet);
    for (auto& [i, p] : sv.touched) sv.blocks[i] = std::move(p);
    sv.touched.clear();
    sv.fullBlocks = true;
  }

  RowBlock& Writable(SheetSave& sv, size_t bi) {
    SaveCells(sv);
    auto& blocks = A::Blocks(*sv.sheet);
    if (!sv.fullBlocks && std::none_of(sv.touched.begin(), sv.touched.end(), [&](const auto& t) { return t.first == bi; }))
      sv.touched.push_back({static_cast<uint32_t>(bi), blocks[bi]});
    if (blocks[bi].use_count() > 1) blocks[bi] = std::make_shared<RowBlock>(*blocks[bi]);
    return *blocks[bi];
  }

  void SaveFormula(SheetSave& sv, uint32_t id) {
    if (sv.saved & kSaveFormulas) return;
    if (std::none_of(sv.formulas.begin(), sv.formulas.end(), [&](const auto& f) { return f.first == id; }))
      sv.formulas.push_back({id, A::Formulas(*sv.sheet)[id]});
  }

  template <class T>
  void SaveList(SheetSave& sv, uint32_t bit, uint32_t piece, T& mine, const T& theirs) {
    sv.dirty |= piece;
    if (sv.saved & bit) return;
    sv.saved |= bit;
    mine = theirs;
  }
  void SaveCols(SheetSave& sv) { SaveList(sv, kSaveCols, kPieceCols, sv.cols, A::Cols(*sv.sheet)); }
  void SaveMerges(SheetSave& sv) {
    if (!(sv.saved & kSaveMerges)) sv.mergeMaxHeight = A::MergeMaxHeight(*sv.sheet);
    SaveList(sv, kSaveMerges, kPieceMerges, sv.merges, A::Merges(*sv.sheet));
  }
  void SaveViews(SheetSave& sv) { SaveList(sv, kSaveViews, kPieceViews, sv.views, A::Views(*sv.sheet)); }
  void SaveProps(SheetSave& sv) { SaveList(sv, kSaveProps, kPieceProps, sv.props, A::Props(*sv.sheet)); }
  void SaveFormat(SheetSave& sv) { SaveList(sv, kSaveFormat, kPieceProps, sv.format, A::Format(*sv.sheet)); }
  void SaveLinks(SheetSave& sv) { SaveList(sv, kSaveLinks, kPieceLinks, sv.links, A::Links(*sv.sheet)); }
  // The XML around the cells. It also holds what the writer makes from the model (sheetPr, views,
  // cols, merges, links) as last written: the text an undo or redo puts back may be from before a
  // save, so those are written from the model again.
  void SaveText(SheetSave& sv) {
    SaveList(sv, kSaveText, kPieceOther | kPieceProps | kPieceViews | kPieceCols | kPieceMerges | kPieceLinks, sv.text,
             A::Text(*sv.sheet));
  }
  void SaveTables(SheetSave& sv) { SaveList(sv, kSaveTables, kPieceOther, sv.tables, A::Tables(*sv.sheet)); }
  void SaveFilter(SheetSave& sv) { SaveList(sv, kSaveFilter, kPieceOther, sv.filter, A::Filter(*sv.sheet)); }
  void SaveBreaks(SheetSave& sv) {
    if (!(sv.saved & kSaveBreaks)) sv.colBreaks = A::Breaks(*sv.sheet, false);
    SaveList(sv, kSaveBreaks, kPieceOther, sv.rowBreaks, A::Breaks(*sv.sheet, true));
  }
  void SavePage(SheetSave& sv) {
    if (!(sv.saved & kSavePage)) sv.headerFooter = A::HF(*sv.sheet);
    SaveList(sv, kSavePage, kPieceOther, sv.page, A::Page(*sv.sheet));
  }
  void SaveKept(SheetSave& sv) {
    if (!(sv.saved & kSaveKept)) sv.masters = A::SharedMasters(*sv.sheet);
    SaveList(sv, kSaveKept, kPieceCells, sv.kept, A::Kept(*sv.sheet));
  }
  // Every formula record, before many change (the ones already saved one by one are put back).
  void SaveAllFormulas(SheetSave& sv) {
    SaveCells(sv);
    if (sv.saved & kSaveFormulas) return;
    sv.saved |= kSaveFormulas;
    sv.allFormulas = A::Formulas(*sv.sheet);
    for (auto& [id, rec] : sv.formulas)
      if (id < sv.allFormulas.size()) sv.allFormulas[id] = std::move(rec);
    sv.formulas.clear();
  }
  // A package part rewritten: the step keeps what it was.
  void SetPart(const std::string& name, std::string bytes) {
    pkg::Package* package = b.GetPackage();
    if (!package) return;
    KeepPart(name);
    package->SetPart(name, std::vector<uint8_t>(bytes.begin(), bytes.end()));
  }
  // The step keeps a part (whether it was there, and its bytes), the content types, a source's
  // relationships, as they were before its first change.
  void KeepPart(const std::string& name) {
    Step& st = Cur();
    if (std::any_of(st.parts.begin(), st.parts.end(), [&](const Step::PartWas& x) { return x.name == name; })) return;
    Step::PartWas was;
    was.name = name;
    was.existed = b.GetPackage()->FindPart(name) != nullptr;
    if (was.existed) b.GetPackage()->ReadPart(name, was.bytes);
    st.parts.push_back(std::move(was));
  }
  void KeepTypes() {
    Step& st = Cur();
    if (!st.types) st.types = b.GetPackage()->Types();
  }
  void KeepRels(const std::string& source) {
    Step& st = Cur();
    if (std::any_of(st.rels.begin(), st.rels.end(), [&](const auto& x) { return x.first == source; })) return;
    std::optional<pkg::RelationshipSet> was;
    if (const pkg::RelationshipSet* r = b.GetPackage()->RelationshipsOf(source)) was = *r;
    st.rels.push_back({source, std::move(was)});
  }

  void SaveBook(bool sheets, bool names) {
    Step& st = Cur();
    st.bookDirty = true;
    if (sheets && !st.book.sheets) {
      st.book.sheets = true;
      for (auto& p : A::Sheets(b)) st.book.order.push_back(p.get());
    }
    if (names && !st.book.names) {
      st.book.names = true;
      st.book.nameList = A::Names(b);
    }
    if (!st.book.active) {
      st.book.active = true;
      st.book.activeSheet = A::Active(b);
      st.book.firstTab = A::FirstTab(b);
    }
  }

  // The block holding `row` (kNone: before the first block, or none).
  static size_t BlockOf(Sheet& s, uint32_t row) {
    auto& fr = A::FirstRows(s);
    auto it = std::upper_bound(fr.begin(), fr.end(), row);
    return it == fr.begin() ? kNone : static_cast<size_t>(it - fr.begin()) - 1;
  }

  // The row's record in a writable block, made if the sheet has none: {block, row index}.
  std::pair<size_t, size_t> EnsureRow(SheetSave& sv, uint32_t row) {
    Sheet& s = *sv.sheet;
    size_t bi = BlockOf(s, row);
    if (bi == kNone) {
      SaveLayout(sv);
      auto& blocks = A::Blocks(s);
      auto& fr = A::FirstRows(s);
      if (blocks.empty() || blocks[0]->rows.size() >= kBlockRows) {
        blocks.insert(blocks.begin(), std::make_shared<RowBlock>());
        fr.insert(fr.begin(), row);
      } else {
        fr[0] = row;
      }
      bi = 0;
    }
    RowBlock& blk = Writable(sv, bi);
    auto r = std::lower_bound(blk.rows.begin(), blk.rows.end(), row, [](const RowRec& x, uint32_t v) { return x.row < v; });
    if (r == blk.rows.end() || r->row != row) {
      RowRec rec;
      rec.row = row;
      rec.first = r == blk.rows.end() ? static_cast<uint32_t>(blk.cells.size()) : r->first;
      r = blk.rows.insert(r, rec);
      if (blk.rows.size() > 2 * kBlockRows) {
        MaybeSplit(sv, bi);
        bi = BlockOf(s, row);
        RowBlock& now = Writable(sv, bi);
        auto again =
            std::lower_bound(now.rows.begin(), now.rows.end(), row, [](const RowRec& x, uint32_t v) { return x.row < v; });
        return {bi, static_cast<size_t>(again - now.rows.begin())};
      }
    }
    return {bi, static_cast<size_t>(r - blk.rows.begin())};
  }

  // A stored cell: {block, row index, cell index}; block kNone when there's none.
  struct Loc {
    size_t block = kNone, row = 0;
    uint32_t cell = 0;
  };
  static Loc Find(Sheet& s, uint32_t row, uint32_t col) {
    Loc l;
    size_t bi = BlockOf(s, row);
    if (bi == kNone) return l;
    const RowBlock& blk = *A::Blocks(s)[bi];
    auto r = std::lower_bound(blk.rows.begin(), blk.rows.end(), row, [](const RowRec& x, uint32_t v) { return x.row < v; });
    if (r == blk.rows.end() || r->row != row) return l;
    auto first = blk.cells.begin() + r->first, last = first + r->count;
    auto c = std::lower_bound(first, last, col, [](const CellRec& x, uint32_t v) { return x.col < v; });
    if (c == last || c->col != col) return l;
    l.block = bi;
    l.row = static_cast<size_t>(r - blk.rows.begin());
    l.cell = static_cast<uint32_t>(c - blk.cells.begin());
    return l;
  }

  static uint32_t FormulaIdOf(const RowBlock& blk, uint32_t index) {
    auto it = std::lower_bound(blk.formulas.begin(), blk.formulas.end(), index,
                               [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) { return x.first < v; });
    return it != blk.formulas.end() && it->first == index ? it->second : kNone;
  }
  static void SetFormulaId(RowBlock& blk, uint32_t index, uint32_t id) {
    auto it = std::lower_bound(blk.formulas.begin(), blk.formulas.end(), index,
                               [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) { return x.first < v; });
    if (it != blk.formulas.end() && it->first == index) it->second = id;
    else blk.formulas.insert(it, {index, id});
  }
  static void DropFormulaId(RowBlock& blk, uint32_t index) {
    auto it = std::lower_bound(blk.formulas.begin(), blk.formulas.end(), index,
                               [](const std::pair<uint32_t, uint32_t>& x, uint32_t v) { return x.first < v; });
    if (it != blk.formulas.end() && it->first == index) blk.formulas.erase(it);
  }
  static CellSide* SideOf(RowBlock& blk, uint32_t index) {
    auto it = std::lower_bound(blk.sides.begin(), blk.sides.end(), index,
                               [](const CellSide& x, uint32_t v) { return x.cell < v; });
    return it != blk.sides.end() && it->cell == index ? &*it : nullptr;
  }

  // A new cell in row `ri` of the writable block, in column order; returns its index.
  uint32_t InsertCell(Sheet& s, RowBlock& blk, size_t ri, uint32_t col, uint32_t style) {
    RowRec& r = blk.rows[ri];
    auto first = blk.cells.begin() + r.first, last = first + r.count;
    auto c = std::lower_bound(first, last, col, [](const CellRec& x, uint32_t v) { return x.col < v; });
    uint32_t index = static_cast<uint32_t>(c - blk.cells.begin());
    CellRec rec;
    rec.col = static_cast<uint16_t>(col);
    rec.style = style;
    if (style) rec.flags |= detail::kStylePresent;
    blk.cells.insert(blk.cells.begin() + index, rec);
    ++r.count;
    for (size_t k = ri + 1; k < blk.rows.size(); ++k) ++blk.rows[k].first;
    for (auto& f : blk.formulas)
      if (f.first >= index) ++f.first;
    for (auto& sd : blk.sides)
      if (sd.cell >= index) ++sd.cell;
    ++A::CellCount(s);
    return index;
  }

  void RemoveCell(Sheet& s, RowBlock& blk, size_t ri, uint32_t index) {
    blk.cells.erase(blk.cells.begin() + index);
    --blk.rows[ri].count;
    for (size_t k = ri + 1; k < blk.rows.size(); ++k) --blk.rows[k].first;
    DropFormulaId(blk, index);
    for (auto& f : blk.formulas)
      if (f.first > index) --f.first;
    blk.sides.erase(std::remove_if(blk.sides.begin(), blk.sides.end(), [&](const CellSide& x) { return x.cell == index; }),
                    blk.sides.end());
    for (auto& sd : blk.sides)
      if (sd.cell > index) --sd.cell;
    --A::CellCount(s);
    // A row left with nothing of its own goes too (one the file wrote with attributes stays).
    const RowRec& r = blk.rows[ri];
    if (r.count == 0 && r.attrs == 0 && r.flags == 0 && r.height == 0 && r.outlineLevel == 0)
      blk.rows.erase(blk.rows.begin() + static_cast<ptrdiff_t>(ri));
  }

  // A cell's side attributes without the ones named (cm when its formula changes, vm when its value does).
  void DropSideAttrs(Sheet& s, CellSide& sd, std::initializer_list<std::string_view> names) {
    if (!sd.attrs) return;
    auto& store = A::Store(s);
    std::vector<pkg::XmlAttr> keep;
    for (const auto& a : store.Attributes(sd.attrs))
      if (std::find(names.begin(), names.end(), std::string_view(a.qname)) == names.end()) keep.push_back(a);
    if (keep.size() != store.Attributes(sd.attrs).size()) sd.attrs = store.Intern(std::move(keep));
  }

  uint32_t SharedString(std::string_view text) {
    StringTable& t = b.Strings();
    for (; stringsIndexed < t.Size(); ++stringsIndexed)
      if (!(t.Flags(stringsIndexed) & StringTable::kRaw) && t.Runs(stringsIndexed).empty())
        stringIndex.emplace(std::hash<std::string_view>()(t.Text(stringsIndexed)), stringsIndexed);
    size_t h = std::hash<std::string_view>()(text);
    for (auto [it, end] = stringIndex.equal_range(h); it != end; ++it)
      if (t.Text(it->second) == text) return it->second;
    bool preserve = !text.empty() && (text.front() == ' ' || text.back() == ' ' ||
                                      text.find_first_of("\t\n\r") != std::string_view::npos);
    uint32_t id = t.AddRead(text, preserve ? StringTable::kPreserve : 0, {}, {});
    stringIndex.emplace(h, id);
    stringsIndexed = t.Size();
    return id;
  }

  // The ranges of the sheet's array formulas (Ctrl+Shift+Enter) over several cells, which can only
  // change as a whole.
  static std::vector<Range> Arrays(Sheet& s) {
    std::vector<Range> out;
    for (const FormulaRec& f : A::Formulas(s))
      if (f.kind == FormulaKind::Array && !f.dynamic && f.hasRef && f.text != kNone &&
          !(f.ref.firstRow == f.ref.lastRow && f.ref.firstCol == f.ref.lastCol)) {
        // Only one still in use: its anchor holds it.
        FormulaInfo info;
        if (s.GetFormula(f.ref.firstRow, f.ref.firstCol, info) && info.kind == FormulaKind::Array && info.ref == f.ref &&
            std::find(out.begin(), out.end(), f.ref) == out.end())
          out.push_back(f.ref);
      }
    return out;
  }
  static const Range* ArrayAt(const std::vector<Range>& arrays, uint32_t row, uint32_t col) {
    for (const Range& r : arrays)
      if (r.Contains(row, col)) return &r;
    return nullptr;
  }

  // Splits a block that grew past twice the usual size, so copying it on the next edit stays cheap.
  void MaybeSplit(SheetSave& sv, size_t bi) {
    Sheet& s = *sv.sheet;
    const RowBlock& peek = *A::Blocks(s)[bi];
    if (peek.rows.size() < 2 || (peek.rows.size() <= 2 * kBlockRows && peek.cells.size() <= 2 * SheetBuilder::kBlockCells))
      return;
    SaveLayout(sv);
    RowBlock& blk = Writable(sv, bi);
    size_t half = blk.rows.size() / 2;
    uint32_t cut = blk.rows[half].first;
    auto second = std::make_shared<RowBlock>();
    second->rows.assign(blk.rows.begin() + static_cast<ptrdiff_t>(half), blk.rows.end());
    for (RowRec& r : second->rows) r.first -= cut;
    second->cells.assign(blk.cells.begin() + cut, blk.cells.end());
    for (const auto& f : blk.formulas)
      if (f.first >= cut) second->formulas.push_back({f.first - cut, f.second});
    for (const CellSide& sd : blk.sides)
      if (sd.cell >= cut) {
        second->sides.push_back(sd);
        second->sides.back().cell -= cut;
      }
    blk.rows.resize(half);
    blk.cells.resize(cut);
    blk.formulas.erase(std::remove_if(blk.formulas.begin(), blk.formulas.end(), [&](const auto& f) { return f.first >= cut; }),
                       blk.formulas.end());
    blk.sides.erase(std::remove_if(blk.sides.begin(), blk.sides.end(), [&](const CellSide& x) { return x.cell >= cut; }),
                    blk.sides.end());
    uint32_t firstRow = second->rows.front().row;
    A::Blocks(s).insert(A::Blocks(s).begin() + static_cast<ptrdiff_t>(bi) + 1, std::move(second));
    A::FirstRows(s).insert(A::FirstRows(s).begin() + static_cast<ptrdiff_t>(bi) + 1, firstRow);
  }

  // A shared formula's master is about to change: every cell sharing it gets its own copy of the
  // formula, written for that cell. False when a formula can't be rewritten.
  bool SplitShared(SheetSave& sv, uint32_t row, uint32_t col, const FormulaRec& master, std::u16string& why) {
    Sheet& s = *sv.sheet;
    int index = IndexOf(&s);
    std::string text(A::Texts(s).Get(master.text));
    BookNames names(b);
    calc::CompileOptions at;
    at.sheet = index;
    at.row = static_cast<int32_t>(row);
    at.col = static_cast<int32_t>(col);
    std::vector<CellAddr> followers;
    s.ForEachCell(master.ref, [&](const CellInfo& c) {
      if (!c.hasFormula || (c.row == row && c.col == col)) return true;
      Loc l = Find(s, c.row, c.col);
      uint32_t id = l.block == kNone ? kNone : FormulaIdOf(*A::Blocks(s)[l.block], l.cell);
      if (id == kNone) return true;
      const FormulaRec& f = A::Formulas(s)[id];
      if (f.kind == FormulaKind::Shared && f.text == kNone && f.sharedIndex == master.sharedIndex)
        followers.push_back({c.row, c.col});
      return true;
    });
    if (followers.empty()) return true;
    auto compiled = calc::Compile(text, at, names);
    if (!compiled) {
      why = u"Other cells share this cell's formula, and Featherweight can't rewrite it for them.";
      return false;
    }
    for (const CellAddr& f : followers) {
      calc::CompileOptions here = at;
      here.row = static_cast<int32_t>(f.row);
      here.col = static_cast<int32_t>(f.col);
      FormulaRec rec;
      rec.text = A::Texts(s).Add(calc::Print(*compiled, here, names));
      Loc l = Find(s, f.row, f.col);
      RowBlock& blk = Writable(sv, l.block);
      uint32_t old = FormulaIdOf(blk, l.cell);
      rec.alwaysCalc = old != kNone && A::Formulas(s)[old].alwaysCalc;
      if (rec.alwaysCalc) rec.attrs = A::Store(s).Intern({{"ca", "", "1"}});
      A::Formulas(s).push_back(rec);
      SetFormulaId(blk, l.cell, static_cast<uint32_t>(A::Formulas(s).size() - 1));
    }
    return true;
  }

  // Sets one cell: its content (`content`, or cleared when `clear`; else kept) and its format
  // (`style`; nullopt keeps it). Shared and dynamic-array formulas it held are taken care of.
  bool Put(SheetSave& sv, uint32_t row, uint32_t col, const CellInput* content, bool clear,
           std::optional<uint32_t> style, std::u16string& why) {
    Sheet& s = *sv.sheet;
    bool contentChanges = content || clear;
    Loc l = Find(s, row, col);
    if (contentChanges && l.block != kNone) {
      uint32_t id = FormulaIdOf(*A::Blocks(s)[l.block], l.cell);
      if (id != kNone) {
        FormulaRec f = A::Formulas(s)[id];
        if (f.kind == FormulaKind::Shared && f.text != kNone && !SplitShared(sv, row, col, f, why)) return false;
        if (f.dynamic && f.hasRef) {
          // The values it spilled go with it.
          std::vector<CellAddr> spilled;
          s.ForEachCell(f.ref, [&](const CellInfo& c) {
            if (!(c.row == row && c.col == col) && !c.hasFormula) spilled.push_back({c.row, c.col});
            return true;
          });
          for (const CellAddr& a : spilled)
            if (!Put(sv, a.row, a.col, nullptr, true, std::nullopt, why)) return false;
        }
        l = Find(s, row, col);
      }
    }
    bool storesSomething = (content && (content->type != ValueType::Empty || !content->formula.empty())) ||
                           (style && *style != 0);
    if (l.block == kNone && !storesSomething) {
      if (!style) return true;  // nothing there, nothing to store
    }
    uint32_t rowStyle = 0;
    if (l.block == kNone) {
      rowStyle = s.StyleAt(row, col);
      if (!storesSomething && rowStyle == 0) return true;
    }
    auto [bi, ri] = EnsureRow(sv, row);
    RowBlock& blk = *A::Blocks(s)[bi];
    uint32_t index;
    if (l.block == kNone) {
      index = InsertCell(s, blk, ri, col, rowStyle);
    } else {
      Loc again = Find(s, row, col);
      index = again.cell;
    }
    CellRec& c = blk.cells[index];
    if (contentChanges) {
      bool hadFormula = (c.flags & detail::kHasFormula) != 0;
      bool formula = content && !content->formula.empty();
      if (hadFormula && !formula) {
        DropFormulaId(blk, index);
        c.flags = static_cast<uint8_t>(c.flags & ~detail::kHasFormula);
        --A::FormulaCells(s);
      }
      if (formula) {
        FormulaRec rec;
        rec.text = A::Texts(s).Add(content->formula);
        if (content->array || content->dynamic) {
          rec.kind = FormulaKind::Array;
          rec.dynamic = content->dynamic;
          rec.hasRef = true;
          rec.ref = content->arrayRange;
          rec.attrs = A::Store(s).Intern({{"t", "", "array"}, {"ref", "", RangeName(content->arrayRange)}});
        }
        A::Formulas(s).push_back(rec);
        SetFormulaId(blk, index, static_cast<uint32_t>(A::Formulas(s).size() - 1));
        if (!hadFormula) ++A::FormulaCells(s);
        c.flags |= detail::kHasFormula;
      }
      detail::XmlType oldType = c.type;
      c.flags = static_cast<uint8_t>(c.flags & ~(detail::kHasValue | detail::kPreserve | detail::kComputed));
      ValueType vt = content ? content->type : ValueType::Empty;
      switch (vt) {
        case ValueType::Empty:
          c.type = detail::XmlType::N;
          c.v.number = 0;
          break;
        case ValueType::Number:
          c.type = detail::XmlType::N;
          c.numStyle = 0;
          c.v.number = content->number;
          c.flags |= detail::kHasValue;
          break;
        case ValueType::String:
          if (formula) {
            c.type = detail::XmlType::Str;
            c.v.index = A::Texts(s).Add(content->text);
          } else {
            c.type = detail::XmlType::S;
            c.v.index = SharedString(content->text);
          }
          c.flags |= detail::kHasValue;
          break;
        case ValueType::Bool:
          c.type = detail::XmlType::B;
          c.v.index = content->boolean ? 1 : 0;
          c.flags |= detail::kHasValue;
          break;
        case ValueType::Error:
          c.type = detail::XmlType::E;
          c.v.index = static_cast<uint32_t>(content->error);
          c.flags |= detail::kHasValue;
          break;
      }
      if (c.type != oldType || vt == ValueType::Empty)
        c.flags = static_cast<uint8_t>(c.flags & ~detail::kTypePresent);
      if (CellSide* sd = SideOf(blk, index)) {
        sd->valueText = kNone;
        sd->inner = kNone;
        DropSideAttrs(s, *sd, {"cm", "vm"});
      }
    }
    if (style) {
      c.style = *style;
      if (*style) c.flags |= detail::kStylePresent;
      else c.flags = static_cast<uint8_t>(c.flags & ~detail::kStylePresent);
    }
    // An empty cell with no format of its own goes.
    bool empty = !(c.flags & (detail::kHasValue | detail::kHasFormula));
    if (empty && c.style == 0) {
      const CellSide* sd = SideOf(blk, index);
      bool plain = !sd || (sd->attrs == 0 && !sd->noRef);
      uint32_t under = 0;
      if (plain) {
        // The format it would show without its own: the row's, else the column's.
        const RowRec& r = blk.rows[ri];
        under = (r.flags & detail::kRowCustomFormat) ? r.style : s.Column(col).style;
      }
      if (plain && under == 0) RemoveCell(s, blk, ri, index);
      else if (under != 0) c.flags |= detail::kStylePresent;
    }
    return true;
  }

  // The stored cells inside `r`, row by row.
  static std::vector<CellAddr> StoredIn(const Sheet& s, const Range& r) {
    std::vector<CellAddr> out;
    s.ForEachCell(r, [&](const CellInfo& c) {
      out.push_back({c.row, c.col});
      return true;
    });
    return out;
  }

  // ---- Rows: their typed fields written back into the attributes the writer writes.
  void SyncRowAttrs(Sheet& s, RowRec& r) {
    auto& store = A::Store(s);
    struct Known {
      std::string_view name;
      std::string value;  // empty: absent
    };
    std::vector<Known> known = {
        {"s", (r.flags & detail::kRowCustomFormat) ? std::to_string(r.style) : std::string()},
        {"customFormat", (r.flags & detail::kRowCustomFormat) ? "1" : ""},
        {"ht", r.height > 0 ? Number(r.height) : std::string()},
        {"hidden", (r.flags & detail::kRowHidden) ? "1" : ""},
        {"customHeight", (r.flags & detail::kRowCustomHeight) ? "1" : ""},
        {"outlineLevel", r.outlineLevel ? std::to_string(r.outlineLevel) : std::string()},
        {"collapsed", (r.flags & detail::kRowCollapsed) ? "1" : ""},
        {"thickTop", (r.flags & detail::kRowThickTop) ? "1" : ""},
        {"thickBot", (r.flags & detail::kRowThickBottom) ? "1" : ""},
    };
    std::vector<pkg::XmlAttr> out;
    std::vector<bool> done(known.size(), false);
    for (const auto& a : store.Attributes(r.attrs)) {
      auto k = std::find_if(known.begin(), known.end(), [&](const Known& x) { return a.nsUri.empty() && x.name == a.qname; });
      if (k == known.end()) {
        out.push_back(a);
        continue;
      }
      done[static_cast<size_t>(k - known.begin())] = true;
      if (!k->value.empty()) out.push_back({a.qname, "", k->value});
    }
    for (size_t i = 0; i < known.size(); ++i)
      if (!done[i] && !known[i].value.empty()) out.push_back({std::string(known[i].name), "", known[i].value});
    r.attrs = store.Intern(std::move(out));
  }

  // Applies `fn` to rows first..last, made where the sheet has none.
  void EditRows(SheetSave& sv, uint32_t first, uint32_t last, const std::function<void(RowRec&)>& fn) {
    for (uint32_t row = first; row <= last; ++row) {
      auto [bi, ri] = EnsureRow(sv, row);
      RowRec& r = A::Blocks(*sv.sheet)[bi]->rows[ri];
      fn(r);
      SyncRowAttrs(*sv.sheet, r);
    }
  }
  // Applies `fn` to the rows the sheet stores between first and last.
  void EditStoredRows(SheetSave& sv, uint32_t first, uint32_t last, const std::function<void(RowRec&)>& fn) {
    Sheet& s = *sv.sheet;
    for (size_t bi = 0; bi < A::Blocks(s).size(); ++bi) {
      const RowBlock& peek = *A::Blocks(s)[bi];
      bool any = std::any_of(peek.rows.begin(), peek.rows.end(), [&](const RowRec& r) { return r.row >= first && r.row <= last; });
      if (!any) continue;
      RowBlock& blk = Writable(sv, bi);
      for (RowRec& r : blk.rows)
        if (r.row >= first && r.row <= last) {
          fn(r);
          SyncRowAttrs(s, r);
        }
    }
  }

  // ---- Columns: cols as ranges, split and joined around the change.
  void EditCols(SheetSave& sv, uint32_t first, uint32_t last, const std::function<void(ColumnInfo&)>& fn) {
    SaveCols(sv);
    std::vector<ColumnInfo>& cols = A::Cols(*sv.sheet);
    std::vector<ColumnInfo> out;
    uint32_t next = first;  // the first column of [first, last] not yet given
    auto emit = [&](ColumnInfo c) {
      if (!out.empty() && out.back().last + 1 == c.first) {
        ColumnInfo a = out.back(), x = c;
        a.first = x.first = 0;
        a.last = x.last = 0;
        if (a.width == x.width && a.style == x.style && a.outlineLevel == x.outlineLevel && a.hidden == x.hidden &&
            a.customWidth == x.customWidth && a.bestFit == x.bestFit && a.collapsed == x.collapsed &&
            a.phonetic == x.phonetic) {
          out.back().last = c.last;
          return;
        }
      }
      out.push_back(c);
    };
    auto fill = [&](uint32_t upTo) {  // columns next..upTo-1 inside [first, last] that cols doesn't give
      uint32_t end = std::min(upTo, last + 1);
      if (next < end) {
        ColumnInfo c;
        c.first = next;
        c.last = end - 1;
        fn(c);
        emit(c);
        next = end;
      }
    };
    for (const ColumnInfo& c : cols) {
      if (c.last < first || c.first > last) {
        if (c.first > last) fill(c.first);
        emit(c);
        continue;
      }
      fill(c.first);
      if (c.first < first) {
        ColumnInfo before = c;
        before.last = first - 1;
        emit(before);
      }
      ColumnInfo mid = c;
      mid.first = std::max(c.first, first);
      mid.last = std::min(c.last, last);
      fn(mid);
      emit(mid);
      next = mid.last + 1;
      if (c.last > last) {
        ColumnInfo after = c;
        after.first = last + 1;
        emit(after);
      }
    }
    fill(last + 1);
    // Columns with nothing to say aren't written.
    out.erase(std::remove_if(out.begin(), out.end(), [](const ColumnInfo& c) {
                return c.width == 0 && c.style == 0 && c.outlineLevel == 0 && !c.hidden && !c.customWidth &&
                       !c.bestFit && !c.collapsed && !c.phonetic;
              }),
              out.end());
    cols = std::move(out);
  }

  void RecountMerges(Sheet& s) {
    auto& m = A::Merges(s);
    std::sort(m.begin(), m.end(), [](const Range& a, const Range& b) {
      return a.firstRow != b.firstRow ? a.firstRow < b.firstRow : a.firstCol < b.firstCol;
    });
    uint32_t h = 0;
    for (const Range& r : m) h = std::max(h, r.lastRow - r.firstRow + 1);
    A::MergeMaxHeight(s) = h;
  }

  void RemapNameSheets(const std::vector<int>& newIndexOfOld) {
    for (DefinedName& n : A::Names(b))
      if (n.sheet >= 0 && static_cast<size_t>(n.sheet) < newIndexOfOld.size()) n.sheet = newIndexOfOld[n.sheet];
  }
};

// ---------- Editor ----------

Editor::Editor(Workbook& book) : book_(book), s_(std::make_unique<State>(book)) {}
Editor::~Editor() = default;

// Records an edit in the journal (Journal.h): when it succeeds (Done) and is the outermost Editor
// call, or with `always` whatever called it (formats added). Void calls set `commit`: recorded
// however they return.
struct Editor::Rec {
  template <class Write>
  Rec(Editor& e, JournalOp op, Write&& write, bool always = false)
      : ed(e), on(e.journal_ && (always || e.calls_ == 0)), rec(op) {
    ++e.calls_;
    if (on) write(rec);
  }
  ~Rec() {
    --ed.calls_;
    if (commit) Add();
  }
  Rec(const Rec&) = delete;
  Rec& operator=(const Rec&) = delete;
  Result Done(Result r) {
    if (r) Add();
    return r;
  }
  void Add() {
    if (!on) return;
    try {
      ed.journal_->Add(rec);
    } catch (const std::bad_alloc&) {  // no room for it: recovery gets the edits before it
    }
  }
  Editor& ed;
  const bool on;
  bool commit = false;
  JournalRecord rec;
};

// Excel's refusals on a protected sheet or workbook, for an edit's outermost call: each public
// edit makes one before it changes anything, and checks once it knows its sheet and cells.
struct Editor::Guard {
  explicit Guard(Editor& e) : ed(e) { ++ed.guarded_; }
  ~Guard() { --ed.guarded_; }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  Result Check(int sheet, ProtectAct act, const Range& r = {}) const {
    if (ed.guarded_ > 1) return {};
    if (sheet >= 0 && static_cast<size_t>(sheet) < ed.book_.SheetCount())
      (void)ed.book_.LoadSheet(static_cast<size_t>(sheet));  // its part says whether it's protected
    std::u16string why = ProtectionRefusal(ed.book_, sheet, act, r);
    return why.empty() ? Result{} : Result::Fail(std::move(why));
  }
  // Content into these cells (typing, pasting): each one unlocked.
  Result CheckCells(int sheet, const std::vector<std::pair<CellAddr, CellInput>>& cells) const {
    if (ed.guarded_ > 1 || cells.empty() || sheet < 0 || static_cast<size_t>(sheet) >= ed.book_.SheetCount()) return {};
    const Sheet& s = ed.book_.GetSheet(static_cast<size_t>(sheet));
    if (!SheetProtected(s)) return {};
    for (const auto& [at, in] : cells)
      if (CellLocked(s, ed.book_.GetStyles(), at.row, at.col)) return Check(sheet, ProtectAct::Content, Range{at.row, at.col, at.row, at.col});
    return {};
  }
  Editor& ed;
};

void Editor::DropHistory() {
  s_->undo.clear();
  s_->redo.clear();
}

bool Editor::CanEdit(std::u16string* why) const {
  (void)why;
  return true;
}

namespace {

// Runs an edit as one step (or part of the open group): BeginGroup, then EndGroup when it
// succeeded, CancelGroup when it didn't.
template <class Fn>
Result Run(Editor& ed, std::u16string name, Fn&& fn) {
  if (ed.Saving()) return Result::Fail(u"The workbook is being saved. Try again in a moment.");
  ed.BeginGroup(std::move(name));
  Result r = fn();
  if (r) ed.EndGroup();
  else ed.CancelGroup();
  return r;
}

Result NoSheet() { return Result::Fail(u"There's no such sheet."); }
Result BadRange() { return Result::Fail(u"The range is outside the sheet."); }

}  // namespace

namespace {

struct FCell {
  uint32_t row = kNone, col = 0, id = 0;
};

// A sheet's formula cells and their records, in order.
std::vector<FCell> FormulaCellsOf(Sheet& s) {
  std::vector<FCell> out;
  for (const auto& bp : A::Blocks(s)) {
    const RowBlock& b = *bp;
    size_t ri = 0;
    for (const auto& [cell, id] : b.formulas) {
      while (ri < b.rows.size() && b.rows[ri].first + b.rows[ri].count <= cell) ++ri;
      if (ri == b.rows.size()) break;
      out.push_back({b.rows[ri].row, b.cells[cell].col, id});
    }
  }
  return out;
}


// Attributes without the ones in `drop`, `ref` set to `ref` when given.
pkg::AttrSetId EditAttrs(pkg::RoundTripStore& store, pkg::AttrSetId id, std::initializer_list<std::string_view> drop,
                         const std::string& ref = {}) {
  if (id == 0) return id;
  bool any = false;
  for (const auto& a : store.Attributes(id))
    any = any || (a.nsUri.empty() && (std::find(drop.begin(), drop.end(), std::string_view(a.qname)) != drop.end() ||
                                      (!ref.empty() && a.qname == "ref" && a.value != ref)));
  if (!any) return id;
  std::vector<pkg::XmlAttr> out;
  bool changed = false;
  for (const auto& a : store.Attributes(id)) {
    bool plain = a.nsUri.empty();
    if (plain && std::find(drop.begin(), drop.end(), std::string_view(a.qname)) != drop.end()) {
      changed = true;
      continue;
    }
    out.push_back(a);
    if (plain && !ref.empty() && a.qname == "ref" && a.value != ref) {
      out.back().value = ref;
      changed = true;
    }
  }
  return changed ? store.Intern(std::move(out)) : id;
}
}  // namespace

// Checks a cell's new content before anything changes.
static Result CheckInput(const CellInput& in) {
  if (in.type == ValueType::Number && !std::isfinite(in.number)) return Result::Fail(u"That number is too large.");
  if (in.type == ValueType::String) {
    if (Utf16Length(in.text) > 32767) return Result::Fail(u"A cell can hold at most 32,767 characters.");
    if (!XmlSafe(in.text)) return Result::Fail(u"The text holds characters a workbook can't store.");
  }
  if (!in.formula.empty()) {
    if (Utf16Length(in.formula) > 8192) return Result::Fail(u"A formula can be at most 8,192 characters long.");
    if (!XmlSafe(in.formula)) return Result::Fail(u"The formula holds characters a workbook can't store.");
  }
  if ((in.array || in.dynamic) && !Valid(in.arrayRange)) return BadRange();
  return {};
}

Result Editor::SetCell(int sheet, uint32_t row, uint32_t col, const CellInput& in) {
  return SetCells(sheet, {{CellAddr{row, col}, in}});
}

Result Editor::SetCells(int sheet, const std::vector<std::pair<CellAddr, CellInput>>& cells) {
  Guard guard(*this);
  if (Result no = guard.CheckCells(sheet, cells); !no) return no;
  Rec rec(*this, JournalOp::SetCells, [&](JournalRecord& w) { w.I(sheet).U(cells.size()); for (const auto& [a, in] : cells) w.A(a).Input(in); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    const std::vector<Range> arrays = State::Arrays(s);
    for (const auto& [at, in] : cells) {
      if (at.row >= kMaxRows || at.col >= kMaxCols) return BadRange();
      if (Result r = CheckInput(in); !r) return r;
      // Excel would look for a workbook by that name; a file can't name a sheet it doesn't have.
      if (!in.formula.empty())
        if (!MissingSheet(book_, in.formula).empty())
          return Result::Fail(u"The formula names a sheet this workbook doesn't have.");
      if ((in.array || in.dynamic) && (in.arrayRange.firstRow != at.row || in.arrayRange.firstCol != at.col))
        return BadRange();
      if (const Range* arr = State::ArrayAt(arrays, at.row, at.col); arr && !(in.array && in.arrayRange == *arr))
        return Result::Fail(u"You can't change part of an array.");
      // Excel turns a formula typed into a table's header into its value as text; its value isn't
      // known here.
      if (!in.formula.empty())
        for (const Table& t : s.Tables())
          if (t.headerRows && at.row == t.ref.firstRow && at.col >= t.ref.firstCol && at.col <= t.ref.lastCol)
            return Result::Fail(u"A table's header can't hold a formula. Type its name instead.");
    }
    return Run(*this, u"Typing", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      std::u16string why;
      std::vector<Range> spills;  // blocked spill ranges: their cells changed too
      // Typing into a dynamic array's spill range blocks it, as in Excel: it shows #SPILL! in its own
      // cell alone, and the values it spilled go (a file whose spill overlaps a cell is repaired).
      for (const FCell& c : FormulaCellsOf(s)) {
        auto& recs = A::Formulas(s);
        if (c.id >= recs.size()) continue;
        const FormulaRec f = recs[c.id];
        if (!f.dynamic || !f.hasRef || f.text == kNone || f.ref.firstRow != c.row || f.ref.firstCol != c.col ||
            (f.ref.lastRow == c.row && f.ref.lastCol == c.col))
          continue;
        bool blocked = false;
        for (const auto& [at, in] : cells)
          blocked = blocked || (!(at.row == c.row && at.col == c.col) && at.row >= f.ref.firstRow && at.row <= f.ref.lastRow &&
                                at.col >= f.ref.firstCol && at.col <= f.ref.lastCol &&
                                (in.type != ValueType::Empty || !in.formula.empty()));
        if (!blocked) continue;
        spills.push_back(f.ref);
        st.SaveFormula(sv, c.id);
        recs[c.id].ref = Range{c.row, c.col, c.row, c.col};
        recs[c.id].attrs = EditAttrs(A::Store(s), recs[c.id].attrs, {}, RangeName(recs[c.id].ref));
        for (const CellAddr& a : State::StoredIn(s, f.ref)) {
          if (a.row == c.row && a.col == c.col) continue;
          State::Loc l = State::Find(s, a.row, a.col);
          if (l.block != kNone && State::FormulaIdOf(*A::Blocks(s)[l.block], l.cell) != kNone) continue;  // its own formula
          if (!st.Put(sv, a.row, a.col, nullptr, true, std::nullopt, why)) return Result::Fail(why);
        }
        State::Loc l = State::Find(s, c.row, c.col);
        if (l.block != kNone) {
          RowBlock& blk = st.Writable(sv, l.block);
          CellRec& cell = blk.cells[l.cell];
          if (cell.type != detail::XmlType::E) cell.flags = static_cast<uint8_t>(cell.flags & ~detail::kTypePresent);
          cell.type = detail::XmlType::E;
          cell.v.index = static_cast<uint32_t>(ErrorCode::Spill);
          cell.flags |= detail::kHasValue;
          for (CellSide& sd : blk.sides)
            if (sd.cell == l.cell) sd.valueText = kNone;
        }
      }
      Range touched{kMaxRows, kMaxCols, 0, 0};
      bool formulas = false;
      for (const auto& [at, in] : cells) {
        if (!st.Put(sv, at.row, at.col, &in, false, in.style, why)) return Result::Fail(why);
        touched.firstRow = std::min(touched.firstRow, at.row);
        touched.firstCol = std::min(touched.firstCol, at.col);
        touched.lastRow = std::max(touched.lastRow, at.row);
        touched.lastCol = std::max(touched.lastCol, at.col);
        formulas = formulas || !in.formula.empty();
        if (in.array) {
          // The other cells of a new array formula hold nothing of their own.
          for (const CellAddr& a : State::StoredIn(s, in.arrayRange))
            if (!(a == at) && !st.Put(sv, a.row, a.col, nullptr, true, std::nullopt, why)) return Result::Fail(why);
        }
      }
      for (const Range& r : spills)
        touched = Range{std::min(touched.firstRow, r.firstRow), std::min(touched.firstCol, r.firstCol),
                        std::max(touched.lastRow, r.lastRow), std::max(touched.lastCol, r.lastCol)};
      if (!s.Tables().empty()) {
        std::vector<CellAddr> at;
        for (const auto& [a, in] : cells) at.push_back(a);
        FixHeaders(s, at);
      }
      if (!cells.empty())
        st.Change(&s, kValues | kFormulas | (std::any_of(cells.begin(), cells.end(), [](const auto& c) { return c.second.style.has_value(); }) ? kStyles : 0u),
                  touched);
      (void)formulas;
      return Result{};
    });
  }());
}

Result Editor::ClearContents(int sheet, const Range& r) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Content, r); !no) return no;
  Rec rec(*this, JournalOp::ClearContents, [&](JournalRecord& w) { w.I(sheet).R(r); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    std::vector<CellAddr> cells = State::StoredIn(s, r);
    for (const Range& arr : State::Arrays(s))
      if (Overlaps(arr, r) && !Inside(arr, r)) return Result::Fail(u"You can't change part of an array.");
    return Run(*this, u"Clear", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      std::u16string why;
      for (const CellAddr& a : cells)
        if (!st.Put(sv, a.row, a.col, nullptr, true, std::nullopt, why)) return Result::Fail(why);
      if (!s.Tables().empty()) FixHeaders(s, cells);
      st.Change(&s, kValues | kFormulas, r);
      return Result{};
    });
  }());
}

Result Editor::ClearFormats(int sheet, const Range& r) {
  // On a protected sheet, as clearing contents: unlocked cells only, whatever the permissions (Excel).
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Content, r); !no) return no;
  return MapStyles(sheet, r, [](uint32_t) { return 0u; });
}

Result Editor::ClearAll(int sheet, const Range& r) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Content, r); !no) return no;
  return Run(*this, u"Clear All", [&]() -> Result {
    if (Result x = ClearContents(sheet, r); !x) return x;
    return ClearFormats(sheet, r);
  });
}

Result Editor::SetStyle(int sheet, const Range& r, uint32_t style) {
  return MapStyles(sheet, r, [style](uint32_t) { return style; });
}

Result Editor::MapStyles(int sheet, const Range& r, const std::function<uint32_t(uint32_t)>& map) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::FormatCells, r); !no) return no;
  Rec rec(*this, JournalOp::MapStyles, [&](JournalRecord& w) { w.I(sheet).R(r); });
  std::map<uint32_t, uint32_t> seen;
  const Result result = [&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, u"Format Cells", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      std::u16string why;
      // On a protected sheet a format's Locked and Hidden stay (Excel refuses that whatever the
      // permissions); clearing formats changes them (allowed on unlocked cells: ClearFormats).
      const bool keepProtection = guarded_ == 1 && SheetProtected(s);
      bool protectionChanged = false;
      auto mapped = [&](uint32_t old) {
        uint32_t x = map(old);
        if (rec.on) seen.emplace(old, x);
        const auto& xfs = book_.GetStyles().CellXfs();
        x = x < xfs.size() ? x : 0u;
        if (keepProtection && old < xfs.size() && x < xfs.size() && !(xfs[x].protection == xfs[old].protection))
          protectionChanged = true;
        return x;
      };
      if (WholeRows(r) && !WholeCols(r)) {
        st.EditRows(sv, r.firstRow, r.lastRow, [&](RowRec& row) {
          uint32_t old = (row.flags & detail::kRowCustomFormat) ? row.style : 0;
          row.style = mapped(old);
          if (row.style) row.flags |= detail::kRowCustomFormat;
          else row.flags = static_cast<uint8_t>(row.flags & ~detail::kRowCustomFormat);
        });
        for (const CellAddr& a : State::StoredIn(s, r)) {
          CellInfo c;
          s.GetCell(a.row, a.col, c);
          if (!st.Put(sv, a.row, a.col, nullptr, false, mapped(c.style), why)) return Result::Fail(why);
        }
      } else if (WholeCols(r)) {
        st.EditCols(sv, r.firstCol, r.lastCol, [&](ColumnInfo& c) { c.style = mapped(c.style); });
        // Rows with a format of their own keep showing it in these columns: their cells there get the new one.
        std::vector<std::pair<uint32_t, uint32_t>> formattedRows;  // row, its style
        s.ForEachRow(0, kMaxRows - 1, [&](uint32_t row, const RowInfo& info) {
          if (info.customFormat) formattedRows.push_back({row, info.style});
          return true;
        });
        if (WholeRows(r)) {
          st.EditStoredRows(sv, 0, kMaxRows - 1, [&](RowRec& row) {
            if (row.flags & detail::kRowCustomFormat) {
              row.style = mapped(row.style);
              if (!row.style) row.flags = static_cast<uint8_t>(row.flags & ~detail::kRowCustomFormat);
            }
          });
        } else {
          for (auto [row, style] : formattedRows)
            for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
              CellInfo c;
              if (s.GetCell(row, col, c)) continue;  // stored cells below
              if (!st.Put(sv, row, col, nullptr, false, mapped(style), why)) return Result::Fail(why);
            }
        }
        for (const CellAddr& a : State::StoredIn(s, r)) {
          CellInfo c;
          s.GetCell(a.row, a.col, c);
          if (!st.Put(sv, a.row, a.col, nullptr, false, mapped(c.style), why)) return Result::Fail(why);
        }
      } else {
        for (uint32_t row = r.firstRow; row <= r.lastRow; ++row)
          for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
            CellInfo c;
            bool stored = s.GetCell(row, col, c);
            uint32_t now = stored ? c.style : s.StyleAt(row, col), next = mapped(now);
            if (next == now) continue;  // nothing to change, and no cell to make
            if (!st.Put(sv, row, col, nullptr, false, next, why)) return Result::Fail(why);
          }
      }
      if (protectionChanged) return Result::Fail(LockedCellMessage());
      st.Change(&s, kStyles, r);
      return Result{};
    });
  }();
  // The mapping as it was used (each format once): `map` itself can't be recorded.
  rec.rec.U(seen.size());
  for (const auto& [from, to] : seen) rec.rec.U(from).U(to);
  return rec.Done(result);
}

uint32_t Editor::AddCellStyle(const CellStyle& style, const Xf& styleXf) {
  Rec rec(*this, JournalOp::AddCellStyle, [&](JournalRecord& w) {
    w.S(style.name).I(style.builtinId).I(style.outlineLevel).B(style.hidden).B(style.customBuiltin);
    w.S(xlsx::StyleItemXml(styleXf));
  }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    for (const CellStyle& s : sb.CellStyles())
      if (style.builtinId >= 0 ? s.builtinId == style.builtinId && s.outlineLevel == style.outlineLevel
                               : NameEqualsNoCase(s.name, style.name))
        return s.xf;
    sb.CellStyleXfs().push_back(styleXf);
    CellStyle added = style;
    added.xf = static_cast<uint32_t>(sb.CellStyleXfs().size() - 1);
    sb.CellStyles().push_back(std::move(added));
    return static_cast<uint32_t>(sb.CellStyleXfs().size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddXf(const Xf& xf) {
  Rec rec(*this, JournalOp::AddXf, [&](JournalRecord& w) { w.S(xlsx::StyleItemXml(xf)); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& xfs = sb.CellXfs();
    auto same = [&](const Xf& a) {
      return a.numFmt == xf.numFmt && a.font == xf.font && a.fill == xf.fill && a.border == xf.border &&
             a.parent == xf.parent && a.alignment == xf.alignment && a.protection == xf.protection &&
             a.applyNumFmt == xf.applyNumFmt && a.applyFont == xf.applyFont && a.applyFill == xf.applyFill &&
             a.applyBorder == xf.applyBorder && a.applyAlignment == xf.applyAlignment &&
             a.applyProtection == xf.applyProtection && a.quotePrefix == xf.quotePrefix && a.pivotButton == xf.pivotButton;
    };
    for (size_t i = 0; i < xfs.size(); ++i)
      if (same(xfs[i])) return static_cast<uint32_t>(i);
    xfs.push_back(xf);
    return static_cast<uint32_t>(xfs.size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddFont(const Font& font) {
  Rec rec(*this, JournalOp::AddFont, [&](JournalRecord& w) { w.S(xlsx::StyleItemXml(font)); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& v = sb.Fonts();
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == font) return static_cast<uint32_t>(i);
    v.push_back(font);
    return static_cast<uint32_t>(v.size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddFill(const Fill& fill) {
  Rec rec(*this, JournalOp::AddFill, [&](JournalRecord& w) { w.S(xlsx::StyleItemXml(fill)); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& v = sb.Fills();
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == fill) return static_cast<uint32_t>(i);
    v.push_back(fill);
    return static_cast<uint32_t>(v.size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddBorder(const Border& border) {
  Rec rec(*this, JournalOp::AddBorder, [&](JournalRecord& w) { w.S(xlsx::StyleItemXml(border)); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& v = sb.Borders();
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == border) return static_cast<uint32_t>(i);
    v.push_back(border);
    return static_cast<uint32_t>(v.size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddNumFmt(std::string_view code) {
  Rec rec(*this, JournalOp::AddNumFmt, [&](JournalRecord& w) { w.S(code); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& fmts = sb.NumFmts();
    for (const auto& [id, c] : fmts)
      if (c == code) return id;
    uint32_t id = 164;
    for (const auto& [i, c] : fmts) id = std::max(id, i + 1);
    fmts.emplace(id, std::string(code));
    return id;
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

uint32_t Editor::AddDxf(const Dxf& dxf) {
  const std::string xml = xlsx::StyleItemXml(dxf);
  Rec rec(*this, JournalOp::AddDxf, [&](JournalRecord& w) { w.S(xml); }, true);
  const uint32_t at = [&]() -> uint32_t {
    StylesBuilder sb(book_.GetStyles());
    auto& v = sb.Dxfs();
    for (size_t i = 0; i < v.size(); ++i)
      if (xlsx::StyleItemXml(v[i]) == xml) return static_cast<uint32_t>(i);
    v.push_back(dxf);
    return static_cast<uint32_t>(v.size() - 1);
  }();
  rec.rec.U(at);
  rec.commit = true;
  return at;
}

// ---- Rows and columns

Result Editor::SetRowHeight(int sheet, uint32_t first, uint32_t last, double points) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::FormatRows, Range{first, 0, std::max(first, last), kMaxCols - 1}); !no) return no;
  Rec rec(*this, JournalOp::SetRowHeight, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).D(points); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxRows) return BadRange();
    if (!(points >= 0 && points <= 409.5)) return Result::Fail(u"Row height must be between 0 and 409.5 points.");
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, u"Row Height", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      auto apply = [&](RowRec& r) {
        r.height = static_cast<float>(points);
        if (points > 0) r.flags |= detail::kRowCustomHeight;
        else r.flags = static_cast<uint8_t>(r.flags & ~detail::kRowCustomHeight);
      };
      if (first == 0 && last == kMaxRows - 1) {
        st.SaveFormat(sv);
        SheetFormat& f = A::Format(s);
        f.defaultRowHeight = points > 0 ? points : 15;
        f.customHeight = points > 0;
        st.EditStoredRows(sv, first, last, apply);
      } else {
        st.EditRows(sv, first, last, apply);
      }
      st.Change(&s, kRowProps, Range{first, 0, last, kMaxCols - 1});
      return Result{};
    });
  }());
}

Result Editor::HideRows(int sheet, uint32_t first, uint32_t last, bool hidden) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::FormatRows, Range{first, 0, std::max(first, last), kMaxCols - 1}); !no) return no;
  Rec rec(*this, JournalOp::HideRows, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).B(hidden); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxRows) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, hidden ? u"Hide Rows" : u"Unhide Rows", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      auto apply = [&](RowRec& r) {
        if (hidden) r.flags |= detail::kRowHidden;
        else r.flags = static_cast<uint8_t>(r.flags & ~detail::kRowHidden);
      };
      if (hidden) st.EditRows(sv, first, last, apply);
      else st.EditStoredRows(sv, first, last, apply);
      st.Change(&s, kRowProps, Range{first, 0, last, kMaxCols - 1});
      return Result{};
    });
  }());
}

Result Editor::SetRowOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::SetRowOutline, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).U(level).B(collapsed); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxRows) return BadRange();
    if (level > 7) return Result::Fail(u"Outlines have at most 7 levels.");
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, level ? u"Group" : u"Ungroup", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      auto apply = [&](RowRec& r) {
        r.outlineLevel = level;
        if (collapsed) r.flags |= detail::kRowCollapsed;
        else r.flags = static_cast<uint8_t>(r.flags & ~detail::kRowCollapsed);
      };
      if (level) st.EditRows(sv, first, last, apply);
      else st.EditStoredRows(sv, first, last, apply);
      st.Change(&s, kRowProps, Range{first, 0, last, kMaxCols - 1});
      return Result{};
    });
  }());
}

Result Editor::SetColumnWidth(int sheet, uint32_t first, uint32_t last, double chars) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::FormatColumns, Range{0, first, kMaxRows - 1, std::max(first, last)}); !no) return no;
  Rec rec(*this, JournalOp::SetColumnWidth, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).D(chars); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxCols) return BadRange();
    if (!(chars >= 0 && chars <= 255)) return Result::Fail(u"Column width must be between 0 and 255 characters.");
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, u"Column Width", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.EditCols(sv, first, last, [&](ColumnInfo& c) {
        c.width = chars;
        c.customWidth = chars > 0;
        c.bestFit = false;
      });
      st.Change(&s, kColProps, Range{0, first, kMaxRows - 1, last});
      return Result{};
    });
  }());
}

Result Editor::HideColumns(int sheet, uint32_t first, uint32_t last, bool hidden) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::FormatColumns, Range{0, first, kMaxRows - 1, std::max(first, last)}); !no) return no;
  Rec rec(*this, JournalOp::HideColumns, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).B(hidden); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxCols) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, hidden ? u"Hide Columns" : u"Unhide Columns", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.EditCols(sv, first, last, [&](ColumnInfo& c) { c.hidden = hidden; });
      st.Change(&s, kColProps, Range{0, first, kMaxRows - 1, last});
      return Result{};
    });
  }());
}

Result Editor::SetColumnOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::SetColumnOutline, [&](JournalRecord& w) { w.I(sheet).U(first).U(last).U(level).B(collapsed); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (first > last || last >= kMaxCols) return BadRange();
    if (level > 7) return Result::Fail(u"Outlines have at most 7 levels.");
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, level ? u"Group" : u"Ungroup", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.EditCols(sv, first, last, [&](ColumnInfo& c) {
        c.outlineLevel = level;
        c.collapsed = collapsed;
      });
      st.Change(&s, kColProps, Range{0, first, kMaxRows - 1, last});
      return Result{};
    });
  }());
}

// Structural edits arrive with the reference registry: until then they're refused, so nothing
// half-done ever reaches a file.
// ---- Structural edits (S2 step 4, the reference registry). sheet/edit/Refs.h rewrites what the
// file says about positions; this moves the model: cells and rows, columns, formulas everywhere,
// names, merges, links, tables, the autofilter, page breaks. Excel's behavior, measured: see Refs.h.

namespace {

// Whether `text` may name the sheet `name`: it holds the name's longest run of letters and digits
// (without case; XML escapes and quoting don't touch those). A cheap test before a costly one.
bool Mentions(std::string_view text, std::string_view name) {
  size_t best = 0, len = 0;
  for (size_t i = 0; i < name.size();) {
    size_t j = i;
    while (j < name.size() && (std::isalnum(static_cast<unsigned char>(name[j])) || static_cast<unsigned char>(name[j]) >= 0x80)) ++j;
    if (j - i > len) best = i, len = j - i;
    i = j == i ? i + 1 : j;
  }
  if (len == 0) return true;
  std::string_view key = name.substr(best, len);
  auto up = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
  for (size_t i = 0; i + key.size() <= text.size(); ++i) {
    size_t k = 0;
    while (k < key.size() && up(text[i + k]) == up(key[k])) ++k;
    if (k == key.size()) return true;
  }
  return false;
}

// What the edit does to an area: 0 nothing, or moves it whole; 1 grows or shrinks it (lines inserted
// inside, some deleted); 2 deletes all of it; -1 moves part of it (cells shifted in a band that holds
// some of its columns or rows).
int Effect(const refs::Mover& m, const Range& r) {
  const bool rows = m.Rows();
  uint32_t first = rows ? r.firstRow : r.firstCol, last = rows ? r.lastRow : r.lastCol;
  int across = m.Across(rows ? r.firstCol : r.firstRow, rows ? r.lastCol : r.lastRow);
  if (last < m.sh.at || across == 0) return 0;
  if (across < 0) return -1;
  if (m.sh.count > 0) return first < m.sh.at ? 1 : 0;
  uint32_t end = m.sh.at + m.Deleted();
  if (first >= end) return 0;
  return first >= m.sh.at && last < end ? 2 : 1;
}

// The formulas of one sheet after the edit.
struct FormulaPlan {
  std::unordered_map<uint32_t, std::string> texts;   // record -> its new text
  std::vector<std::pair<uint32_t, Range>> refs;      // record -> its new ref (arrays, shared masters)
  std::vector<std::pair<CellAddr, std::string>> own;  // a cell sharing a formula that gets its own
  std::vector<uint32_t> unshare;                     // shared masters that become plain formulas
  bool Empty() const { return texts.empty() && refs.empty() && own.empty() && unshare.empty(); }
};

// A shared formula stays shared where every cell's formula still follows from the master's; a cell
// whose doesn't gets its own, and when the master's has #REF! none is shared any more (Excel).
FormulaPlan PlanFormulas(Sheet& x, int xi, bool edited, const std::function<bool(std::string_view)>& touches,
                         const refs::Mover& m, const refs::FormulaFn& shift, const refs::CopyFn& copy) {
  FormulaPlan plan;
  const auto& recs = A::Formulas(x);
  const TextPool& texts = A::Texts(x);
  struct Group {
    FCell master;
    std::vector<FCell> followers;
  };
  std::unordered_map<uint32_t, Group> groups;
  for (const FCell& c : FormulaCellsOf(x)) {
    if (c.id >= recs.size()) continue;
    const FormulaRec& f = recs[c.id];
    if (f.kind == FormulaKind::Shared && f.sharedIndex != kNone) {
      Group& g = groups[f.sharedIndex];
      if (f.text != kNone) g.master = c;
      else g.followers.push_back(c);
    }
    if (f.text == kNone) continue;
    std::string_view text = texts.Get(f.text);
    if (edited || touches(text)) {
      std::string now = shift(text, xi, c.row, c.col);
      if (now != text) plan.texts[c.id] = std::move(now);
    }
    if (edited && f.hasRef && (f.kind == FormulaKind::Array || f.kind == FormulaKind::Shared || f.dynamic)) {
      Range r = f.ref;
      if (m.Area(r) && !(r == f.ref)) plan.refs.push_back({c.id, r});
    }
  }
  for (auto& [si, g] : groups) {
    if (g.master.row == kNone || g.followers.empty()) continue;
    std::string text(texts.Get(recs[g.master.id].text));
    if (!edited && !touches(text)) continue;
    auto it = plan.texts.find(g.master.id);
    const std::string& now = it == plan.texts.end() ? text : it->second;
    CellAddr master{g.master.row, g.master.col}, masterNow = master;
    bool gone = edited && !m.Cell(masterNow.row, masterNow.col);
    // The group's cells after the edit: one moved out of them (cells shifted in a band across
    // the group) can't share any more; the master moved out, none can.
    const FormulaRec& mr = recs[g.master.id];
    Range ref = mr.ref;
    if (edited && mr.hasRef) (void)m.Area(ref);
    auto in = [&](CellAddr a) {
      return !edited || !mr.hasRef ||
             (a.row >= ref.firstRow && a.row <= ref.lastRow && a.col >= ref.firstCol && a.col <= ref.lastCol);
    };
    bool all = gone || now.find("#REF!") != std::string::npos || !in(masterNow);
    for (const FCell& f : g.followers) {
      CellAddr at{f.row, f.col}, to = at;
      if (edited && !m.Cell(to.row, to.col)) continue;
      std::string mine = shift(copy(text, xi, master, at), xi, f.row, f.col);
      if (all || !in(to) || copy(now, xi, masterNow, to) != mine) plan.own.push_back({at, std::move(mine)});
    }
    if (all && !gone) plan.unshare.push_back(g.master.id);
  }
  return plan;
}

FormulaRec OwnRecord(Sheet& x, const std::string& text, const FormulaRec& was) {
  FormulaRec rec;
  rec.text = A::Texts(x).Add(text);
  rec.alwaysCalc = was.alwaysCalc;
  if (rec.alwaysCalc) rec.attrs = A::Store(x).Intern({{"ca", "", "1"}});
  return rec;
}

void Unshare(Sheet& x, FormulaRec& rec) {
  rec.kind = FormulaKind::Normal;
  rec.sharedIndex = kNone;
  rec.hasRef = false;
  rec.attrs = EditAttrs(A::Store(x), rec.attrs, {"t", "ref", "si"});
}

// The location of a pivot table part's table (its <location ref>), read as text.
bool PivotLocation(std::string_view xml, Range& out) {
  for (size_t at = xml.find("location"); at != std::string_view::npos; at = xml.find("location", at + 1)) {
    if (at == 0 || (xml[at - 1] != '<' && xml[at - 1] != ':')) continue;
    size_t end = xml.find('>', at), ref = xml.find(" ref=\"", at);
    if (ref == std::string_view::npos || ref > end) continue;
    size_t q = xml.find('"', ref + 6);
    return q != std::string_view::npos && ParseRange(xml.substr(ref + 6, q - ref - 6), out);
  }
  return false;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

Result Editor::InsertRows(int sheet, uint32_t at, uint32_t count) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::InsertRows); !no) return no;
  return ShiftCells(sheet, Shift{Shift::Axis::Rows, at, static_cast<int32_t>(count), Range::All()}, u"Insert Rows");
}
Result Editor::DeleteRows(int sheet, uint32_t first, uint32_t count) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::DeleteRows, Range{first, 0, count ? first + std::min(count, kMaxRows) - 1 : first, kMaxCols - 1}); !no) return no;
  return ShiftCells(sheet, Shift{Shift::Axis::Rows, first, -static_cast<int32_t>(count), Range::All()}, u"Delete Rows");
}
Result Editor::InsertColumns(int sheet, uint32_t at, uint32_t count) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::InsertColumns); !no) return no;
  return ShiftCells(sheet, Shift{Shift::Axis::Columns, at, static_cast<int32_t>(count), Range::All()},
                    u"Insert Columns");
}
Result Editor::DeleteColumns(int sheet, uint32_t first, uint32_t count) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::DeleteColumns, Range{0, first, kMaxRows - 1, count ? first + std::min(count, kMaxCols) - 1 : first}); !no) return no;
  return ShiftCells(sheet, Shift{Shift::Axis::Columns, first, -static_cast<int32_t>(count), Range::All()},
                    u"Delete Columns");
}
Result Editor::InsertCells(int sheet, const Range& r, Direction shift) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  if (!Valid(r)) return BadRange();
  if (shift == Direction::DownOrUp)
    return WholeRows(r) ? InsertRows(sheet, r.firstRow, r.lastRow - r.firstRow + 1)
                        : ShiftCells(sheet,
                                     Shift{Shift::Axis::Rows, r.firstRow, static_cast<int32_t>(r.lastRow - r.firstRow + 1),
                                           Range{0, r.firstCol, kMaxRows - 1, r.lastCol}},
                                     u"Insert Cells");
  return WholeCols(r) ? InsertColumns(sheet, r.firstCol, r.lastCol - r.firstCol + 1)
                      : ShiftCells(sheet,
                                   Shift{Shift::Axis::Columns, r.firstCol, static_cast<int32_t>(r.lastCol - r.firstCol + 1),
                                         Range{r.firstRow, 0, r.lastRow, kMaxCols - 1}},
                                   u"Insert Cells");
}
Result Editor::DeleteCells(int sheet, const Range& r, Direction shift) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  if (!Valid(r)) return BadRange();
  if (shift == Direction::DownOrUp)
    return WholeRows(r) ? DeleteRows(sheet, r.firstRow, r.lastRow - r.firstRow + 1)
                        : ShiftCells(sheet,
                                     Shift{Shift::Axis::Rows, r.firstRow, -static_cast<int32_t>(r.lastRow - r.firstRow + 1),
                                           Range{0, r.firstCol, kMaxRows - 1, r.lastCol}},
                                     u"Delete Cells");
  return WholeCols(r) ? DeleteColumns(sheet, r.firstCol, r.lastCol - r.firstCol + 1)
                      : ShiftCells(sheet,
                                   Shift{Shift::Axis::Columns, r.firstCol, -static_cast<int32_t>(r.lastCol - r.firstCol + 1),
                                         Range{r.firstRow, 0, r.lastRow, kMaxCols - 1}},
                                   u"Delete Cells");
}

Result Editor::ShiftCells(int sheet, const Shift& sh, std::u16string stepName) {
  Rec rec(*this, JournalOp::ShiftCells, [&](JournalRecord& w) { w.I(sheet).U(sh.axis == Shift::Axis::Columns ? 1 : 0).U(sh.at).I(sh.count).R(sh.band).S16(stepName); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    const refs::Mover m(sh);
    if (sh.count == 0) return {};
    if (sh.at >= m.max || (sh.count < 0 && static_cast<uint64_t>(sh.at) + m.Deleted() > m.max)) return BadRange();
    if (!book_.LoadAll().Ok()) return Result::Fail(u"A sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet) return Result::Fail(u"This sheet has no cells.");
    const bool rows = m.Rows();
    const bool whole = m.Across(0, rows ? kMaxCols - 1 : kMaxRows - 1) == 1;
    const std::string name = s.Name();
    pkg::Package* package = book_.GetPackage();

    // ---- What Excel refuses, and what Featherweight can't move yet.
    auto cantYet = [](std::u16string_view what) {
      return Result::Fail(u"Featherweight can't insert or delete cells on this sheet yet: it has " + std::u16string(what) +
                          u".");
    };
    if (std::u16string what = refs::CheckSheetXml(A::Text(s)); !what.empty()) return cantYet(what);
    std::vector<std::pair<std::string, std::string>> sheetParts;  // relationship type, part
    if (package && !s.PartName().empty())
      if (const pkg::RelationshipSet* rels = package->RelationshipsOf(s.PartName()))
        for (const pkg::Relationship& r : rels->items) {
          static constexpr std::string_view kKnown[] = {
              "/drawing", "/vmlDrawing", "/comments", "/threadedComment", "/table", "/pivotTable", "/hyperlink",
              "/printerSettings", "/image", "/oleObject", "/package", "/control", "/ctrlProp", "/slicer", "/timeline",
              "/customProperty", "/webextension"};
          if (std::none_of(std::begin(kKnown), std::end(kKnown), [&](std::string_view k) { return EndsWith(r.type, k); }))
            return cantYet(u"parts Featherweight doesn't know");
          if (!r.external && !r.targetPart.empty()) sheetParts.push_back({r.type, r.targetPart});
        }
    for (const FormulaRec& f : A::Formulas(s))
      if (f.kind == FormulaKind::DataTable) return cantYet(u"a what-if data table");
    for (const Range& a : State::Arrays(s))
      if (int e = Effect(m, a); e == 1 || e == -1) return Result::Fail(u"You can't change part of an array.");
    for (const Range& g : s.Merges())
      if (Effect(m, g) == -1)
        return Result::Fail(u"Part of a merged cell would move. Unmerge it first, or insert or delete whole rows or columns.");
    for (const Table& t : s.Tables()) {
      int e = Effect(m, t.ref);
      if (e == -1) return Result::Fail(u"This won't work because it would move cells in a table on your worksheet.");
      if (e == 2) return Result::Fail(u"Featherweight can't delete a whole table this way yet.");
      if (e == 1 && rows && sh.count < 0 && t.headerRows && t.ref.firstRow >= sh.at && t.ref.firstRow < sh.at + m.Deleted())
        return Result::Fail(u"A table's header row can't be deleted on its own.");
    }
    if (package)
      for (const auto& [type, part] : sheetParts) {
        std::vector<uint8_t> bytes;
        Range where;
        if (EndsWith(type, "/pivotTable") && package->ReadPart(part, bytes).Ok() &&
            PivotLocation(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where) &&
            Effect(m, where) != 0)
          return Result::Fail(u"We can't make this change for the selected cells because it will affect a PivotTable.");
      }
    if (sh.count > 0) {
      // Cells with something in them would be pushed off the sheet.
      Range end = rows ? Range{m.max - static_cast<uint32_t>(sh.count), sh.band.firstCol, m.max - 1, sh.band.lastCol}
                       : Range{sh.band.firstRow, m.max - static_cast<uint32_t>(sh.count), sh.band.lastRow, m.max - 1};
      bool full = false;
      s.ForEachCell(end, [&](const CellInfo& c) {
        full = full || c.hasFormula || c.value.type != ValueType::Empty;
        return !full;
      });
      if (full) return Result::Fail(u"There's no room: cells with something in them would be pushed off the sheet.");
    }

    // Table columns deleted: structured references to them become #REF! wherever they are.
    std::vector<std::pair<std::string, std::vector<std::string>>> goneColumns;
    if (!rows && sh.count < 0)
      for (const Table& t : s.Tables()) {
        std::vector<std::string> cols;
        for (uint32_t c = std::max(t.ref.firstCol, sh.at); c <= t.ref.lastCol && c < sh.at + m.Deleted(); ++c)
          if (c - t.ref.firstCol < t.columns.size()) cols.push_back(t.columns[c - t.ref.firstCol].name);
        if (!cols.empty() && Effect(m, t.ref) == 1) goneColumns.push_back({t.displayName, std::move(cols)});
      }
    // Whether a formula or part elsewhere may need rewriting.
    auto touches = [&](std::string_view text) {
      if (Mentions(text, name)) return true;
      for (const auto& g : goneColumns)
        if (Mentions(text, g.first)) return true;
      return false;
    };

    return Run(*this, std::move(stepName), [&]() -> Result {
      State& st = *s_;
      BookNames names(book_);
      refs::Context ctx{m, sheet, name, {}, {}};
      ctx.formula = [&](std::string_view text, int at, uint32_t row, uint32_t col) -> std::string {
        if (at != sheet && !touches(text)) return std::string(text);
        calc::CompileOptions where;
        where.sheet = at;
        where.row = static_cast<int32_t>(row);
        where.col = static_cast<int32_t>(col);
        std::string out = calc::ShiftReferences(text, where, sheet, sh, names);
        for (const auto& [table, cols] : goneColumns) out = refs::DropTableColumns(out, table, cols);
        return out;  // as written where nothing moved (ShiftReferences keeps the text)
      };
      ctx.copy = [&](std::string_view text, int at, CellAddr from, CellAddr to) -> std::string {
        if (from == to) return std::string(text);
        calc::CompileOptions a, b;
        a.sheet = b.sheet = at;
        a.row = static_cast<int32_t>(from.row);
        a.col = static_cast<int32_t>(from.col);
        b.row = static_cast<int32_t>(to.row);
        b.col = static_cast<int32_t>(to.col);
        std::string out = calc::CopyFormula("=" + std::string(text), a, b, names);
        return !out.empty() && out[0] == '=' ? out.substr(1) : out;
      };

      // ---- Formulas on every sheet (planned before any cell moves: they're read where they are).
      std::vector<FormulaPlan> plans(book_.SheetCount());
      for (size_t i = 0; i < book_.SheetCount(); ++i)
        plans[i] = PlanFormulas(book_.GetSheet(i), static_cast<int>(i), static_cast<int>(i) == sheet, touches, m,
                                ctx.formula, ctx.copy);
      for (size_t i = 0; i < book_.SheetCount(); ++i) {
        if (static_cast<int>(i) == sheet || plans[i].Empty()) continue;
        Sheet& x = book_.GetSheet(i);
        SheetSave& sv = st.Save(x);
        st.SaveCells(sv);
        auto& recs = A::Formulas(x);
        for (auto& [id, text] : plans[i].texts) {
          st.SaveFormula(sv, id);
          recs[id].text = A::Texts(x).Add(text);
        }
        for (uint32_t id : plans[i].unshare) {
          st.SaveFormula(sv, id);
          Unshare(x, recs[id]);
        }
        for (auto& [at, text] : plans[i].own) {
          State::Loc l = State::Find(x, at.row, at.col);
          if (l.block == kNone) continue;
          RowBlock& blk = st.Writable(sv, l.block);
          uint32_t old = State::FormulaIdOf(blk, l.cell);
          FormulaRec rec = OwnRecord(x, text, old != kNone ? recs[old] : FormulaRec{});
          recs.push_back(rec);
          State::SetFormulaId(blk, l.cell, static_cast<uint32_t>(recs.size() - 1));
        }
        st.Change(&x, kFormulas, Range::All());
      }

      // ---- The edited sheet: its formula records, then its cells and rows.
      SheetSave& sv = st.Save(s);
      st.SaveAllFormulas(sv);
      st.SaveLayout(sv);
      st.SaveKept(sv);
      auto& recs = A::Formulas(s);
      const FormulaPlan& plan = plans[static_cast<size_t>(sheet)];
      for (auto& [id, text] : plan.texts) recs[id].text = A::Texts(s).Add(text);
      for (uint32_t id : plan.unshare) Unshare(s, recs[id]);
      for (auto& [id, r] : plan.refs) {
        recs[id].ref = r;
        recs[id].attrs = EditAttrs(A::Store(s), recs[id].attrs, {}, RangeName(r));
      }
      std::unordered_map<uint64_t, uint32_t> own;  // old cell -> its own formula record
      auto key = [](uint32_t row, uint32_t col) { return (static_cast<uint64_t>(row) << 16) | col; };
      for (auto& [at, text] : plan.own) {
        State::Loc l = State::Find(s, at.row, at.col);
        uint32_t old = l.block == kNone ? kNone : State::FormulaIdOf(*A::Blocks(s)[l.block], l.cell);
        recs.push_back(OwnRecord(s, text, old != kNone ? recs[old] : FormulaRec{}));
        own[key(at.row, at.col)] = static_cast<uint32_t>(recs.size() - 1);
      }

      // Cells and rows, block by block. Blocks the edit doesn't reach stay as they are (shared with the
      // undo step: writes copy them). Where lines only renumber, or cells only move along their rows,
      // a block is rewritten in one pass; cells shifted up or down in a band of columns cross rows, so
      // from the first block they reach on they're gathered and sorted.
      auto& store = A::Store(s);
      const auto& oldBlocks = A::Blocks(s);
      const auto& oldFirst = A::FirstRows(s);
      const bool adds = sh.count > 0 && sh.at > 0;
      const uint32_t prev = adds ? sh.at - 1 : 0;
      const uint32_t addCount = adds ? std::min<uint32_t>(static_cast<uint32_t>(sh.count), m.max - sh.at) : 0;
      const uint32_t firstMoved = adds && rows ? prev : sh.at;  // rows before it keep their cells (rows axis)
      std::vector<uint32_t> ownRows;
      for (const auto& [at, text] : plan.own) ownRows.push_back(at.row);
      std::sort(ownRows.begin(), ownRows.end());
      auto formulaOf = [&](uint32_t row, uint32_t col, uint32_t id) {
        if (!own.empty())
          if (auto o = own.find(key(row, col)); o != own.end()) return o->second;
        return id;
      };
      std::vector<std::shared_ptr<RowBlock>> blocks;
      std::vector<uint32_t> firstRows;
      bool open = false;  // whether the last block takes more rows
      auto beginRow = [&](RowRec rec) -> RowBlock& {
        if (!open || blocks.back()->rows.size() >= kBlockRows || blocks.back()->cells.size() >= SheetBuilder::kBlockCells) {
          blocks.push_back(std::make_shared<RowBlock>());
          firstRows.push_back(rec.row);
          open = true;
        }
        RowBlock& b = *blocks.back();
        rec.first = static_cast<uint32_t>(b.cells.size());
        rec.count = 0;
        b.rows.push_back(rec);
        return b;
      };
      auto addCell = [](RowBlock& b, const CellRec& cell, uint32_t formula, const CellSide* side) {
        uint32_t index = static_cast<uint32_t>(b.cells.size());
        b.cells.push_back(cell);
        if (formula != kNone && (cell.flags & detail::kHasFormula)) b.formulas.push_back({index, formula});
        if (side) {
          b.sides.push_back(*side);
          b.sides.back().cell = index;
        }
        ++b.rows.back().count;
      };
      auto styled = [](const CellRec& c) { return (c.flags & detail::kStylePresent) && c.style != 0; };
      auto formatOnly = [](uint32_t col, uint32_t style) {
        CellRec c;
        c.col = static_cast<uint16_t>(col);
        c.style = style;
        c.flags = detail::kStylePresent;
        return c;
      };
      // One block whose cells stay in their rows: whole rows renumbered, or cells moved along rows.
      auto rewrite = [&](const RowBlock& b) {
        size_t fi = 0, si = 0;
        for (const RowRec& r0 : b.rows) {
          RowRec rr = r0;
          if (rows && whole) {
            rr.row = m.Line(r0.row);
            if (rr.row == kNone) continue;  // deleted, with its cells
          } else if (!rows && m.Across(r0.row, r0.row) != 0) {
            rr.attrs = EditAttrs(store, rr.attrs, {"spans"});  // its cells move across columns
          }
          RowBlock* out = &beginRow(rr);
          for (uint32_t k = r0.first; k < r0.first + r0.count; ++k) {
            const CellRec& c0 = b.cells[k];
            while (fi < b.formulas.size() && b.formulas[fi].first < k) ++fi;
            while (si < b.sides.size() && b.sides[si].cell < k) ++si;
            uint32_t row = r0.row, col = c0.col;
            if (!m.Cell(row, col)) continue;
            CellRec c = c0;
            c.col = static_cast<uint16_t>(col);
            const uint32_t id = fi < b.formulas.size() && b.formulas[fi].first == k ? b.formulas[fi].second : kNone;
            CellSide side;
            const bool hasSide = si < b.sides.size() && b.sides[si].cell == k;
            if (hasSide) {
              side = b.sides[si];
              if (row != r0.row || col != c0.col) side.attrs = EditAttrs(store, side.attrs, {"r"});
            }
            addCell(*out, c, formulaOf(r0.row, c0.col, id), hasSide ? &side : nullptr);
            // Columns inserted after this one take its format (Excel's "Format Same As Left").
            if (!rows && addCount && c0.col == prev && styled(c0) && m.Across(r0.row, r0.row) == 1)
              for (uint32_t j = 0; j < addCount; ++j) addCell(*out, formatOnly(sh.at + j, c0.style), kNone, nullptr);
          }
          // Rows inserted after this one take its height and format ("Format Same As Above").
          if (rows && whole && addCount && r0.row == prev)
            for (uint32_t j = 0; j < addCount; ++j) {
              RowRec c = r0;
              c.row = sh.at + j;
              c.flags = static_cast<uint8_t>(c.flags & ~(detail::kRowHidden | detail::kRowCollapsed));
              st.SyncRowAttrs(s, c);
              out = &beginRow(c);
              for (uint32_t k = r0.first; k < r0.first + r0.count; ++k)
                if (styled(b.cells[k])) addCell(*out, formatOnly(b.cells[k].col, b.cells[k].style), kNone, nullptr);
            }
        }
      };
      // Blocks from `from` on, when cells move up or down in a band of columns: the band's cells, where
      // they go (still in order: they all move alike), merged into the rows with the cells beside the
      // band, which stay.
      auto gather = [&](size_t from) {
        struct Item {
          uint32_t row = 0;
          CellRec cell;
          uint32_t formula = kNone;
          bool hasSide = false;
          CellSide side;
        };
        auto inBand = [&](uint32_t col) { return m.Across(col, col) != 0; };
        std::vector<Item> band;
        std::vector<CellRec> above;  // the styled band cells of the row before inserted cells
        for (size_t bi = from; bi < oldBlocks.size(); ++bi) {
          const RowBlock& b = *oldBlocks[bi];
          size_t fi = 0, si = 0;
          for (const RowRec& r0 : b.rows) {
            for (uint32_t k = r0.first; k < r0.first + r0.count; ++k) {
              const CellRec& c0 = b.cells[k];
              if (!inBand(c0.col)) continue;
              while (fi < b.formulas.size() && b.formulas[fi].first < k) ++fi;
              while (si < b.sides.size() && b.sides[si].cell < k) ++si;
              uint32_t row = r0.row, col = c0.col;
              if (addCount && row == prev && styled(c0)) above.push_back(c0);
              if (!m.Cell(row, col)) continue;
              Item it;
              it.row = row;
              it.cell = c0;
              it.formula = formulaOf(r0.row, c0.col, fi < b.formulas.size() && b.formulas[fi].first == k ? b.formulas[fi].second : kNone);
              if (si < b.sides.size() && b.sides[si].cell == k) {
                it.hasSide = true;
                it.side = b.sides[si];
                if (row != r0.row) it.side.attrs = EditAttrs(store, it.side.attrs, {"r"});
              }
              band.push_back(it);
            }
            if (r0.row == prev && !above.empty()) {  // cells inserted below take the format of the cell above
              for (uint32_t j = 0; j < addCount; ++j)
                for (const CellRec& c : above) {
                  Item add;
                  add.row = sh.at + j;
                  add.cell = formatOnly(c.col, c.style);
                  band.push_back(add);
                }
            }
          }
        }
        size_t bx = 0;
        auto bandRow = [&](uint32_t row, RowBlock& out, uint32_t beforeCol) {
          for (; bx < band.size() && band[bx].row == row && band[bx].cell.col < beforeCol; ++bx)
            addCell(out, band[bx].cell, band[bx].formula, band[bx].hasSide ? &band[bx].side : nullptr);
        };
        auto bandOnly = [&](uint32_t before) {  // rows only band cells land in
          while (bx < band.size() && band[bx].row < before) {
            RowRec rec;
            rec.row = band[bx].row;
            bandRow(rec.row, beginRow(rec), kMaxCols);
          }
        };
        for (size_t bi = from; bi < oldBlocks.size(); ++bi) {
          const RowBlock& b = *oldBlocks[bi];
          size_t fi = 0, si = 0;
          for (const RowRec& r0 : b.rows) {
            bandOnly(r0.row);
            RowRec rr = r0;
            rr.attrs = EditAttrs(store, rr.attrs, {"spans"});
            RowBlock& out = beginRow(rr);
            for (uint32_t k = r0.first; k < r0.first + r0.count; ++k) {
              const CellRec& c0 = b.cells[k];
              if (inBand(c0.col)) continue;
              while (fi < b.formulas.size() && b.formulas[fi].first < k) ++fi;
              while (si < b.sides.size() && b.sides[si].cell < k) ++si;
              bandRow(r0.row, out, c0.col);
              const uint32_t id = fi < b.formulas.size() && b.formulas[fi].first == k ? b.formulas[fi].second : kNone;
              addCell(out, c0, formulaOf(r0.row, c0.col, id),
                      si < b.sides.size() && b.sides[si].cell == k ? &b.sides[si] : nullptr);
            }
            bandRow(r0.row, out, kMaxCols);
          }
        }
        bandOnly(kNone);
      };
      for (size_t bi = 0; bi < oldBlocks.size(); ++bi) {
        const RowBlock& b = *oldBlocks[bi];
        if (b.rows.empty()) continue;
        const uint32_t first = b.rows.front().row, last = b.rows.back().row;
        if (rows && !whole && last >= firstMoved) {
          gather(bi);
          break;
        }
        auto o = std::lower_bound(ownRows.begin(), ownRows.end(), first);
        const bool owns = o != ownRows.end() && *o <= last;
        const bool reached = rows ? last >= firstMoved : m.Across(first, last) != 0;
        if (!owns && !reached) {
          blocks.push_back(oldBlocks[bi]);
          firstRows.push_back(oldFirst[bi]);
          open = false;
        } else {
          rewrite(b);
        }
      }
      size_t cellCount = 0, formulaCells = 0;
      for (const auto& b : blocks) {
        cellCount += b->cells.size();
        formulaCells += b->formulas.size();
      }
      A::Blocks(s) = std::move(blocks);
      A::FirstRows(s) = std::move(firstRows);
      A::CellCount(s) = cellCount;
      A::FormulaCells(s) = formulaCells;
      // Shared formulas' masters, where they are now.
      auto& masters = A::SharedMasters(s);
      masters.clear();
      for (const FCell& c : FormulaCellsOf(s))
        if (c.id < recs.size() && recs[c.id].kind == FormulaKind::Shared && recs[c.id].text != kNone &&
            recs[c.id].sharedIndex != kNone)
          masters.push_back({recs[c.id].sharedIndex, CellAddr{c.row, c.col}});
      std::sort(masters.begin(), masters.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      // XML kept inside sheetData.
      std::vector<detail::Kept> kept;
      for (detail::Kept k : A::Kept(s)) {
        if (k.row != kNone) {
          if (k.col != detail::Kept::kBeforeRow && k.col != detail::Kept::kRowEnd) {
            uint32_t r = k.row, c = k.col;
            if (!m.Cell(r, c)) continue;
            k.row = r;
            k.col = c;
          } else if (rows && whole) {
            if ((k.row = m.Line(k.row)) == kNone) continue;
          }
        }
        kept.push_back(std::move(k));
      }
      std::stable_sort(kept.begin(), kept.end(), [](const detail::Kept& a, const detail::Kept& b) { return a.row < b.row; });
      A::Kept(s) = std::move(kept);

      // ---- Columns: widths and formats move; inserted ones take the one before's.
      if (!rows && whole && !s.Columns().empty()) {
        st.SaveCols(sv);
        std::vector<ColumnInfo> out;
        std::optional<ColumnInfo> before;
        const uint32_t at = sh.at, d = m.Deleted();
        for (const ColumnInfo& c : A::Cols(s)) {
          if (sh.count > 0 && at > 0 && c.first <= at - 1 && at - 1 <= c.last) before = c;
          if (c.last < at) {
            out.push_back(c);
            continue;
          }
          ColumnInfo a = c, b = c;
          if (sh.count > 0) {
            if (c.first < at) {
              a.last = at - 1;
              out.push_back(a);
            }
            uint64_t first = std::max(c.first, at) + static_cast<uint64_t>(sh.count);
            if (first < kMaxCols) {
              b.first = static_cast<uint32_t>(first);
              b.last = static_cast<uint32_t>(std::min<uint64_t>(c.last + static_cast<uint64_t>(sh.count), kMaxCols - 1));
              out.push_back(b);
            }
          } else {
            if (c.first < at) {
              a.last = at - 1;
              out.push_back(a);
            }
            if (c.last >= at + d) {
              b.first = std::max(c.first, at + d) - d;
              b.last = c.last == kMaxCols - 1 ? c.last : c.last - d;
              out.push_back(b);
            }
          }
        }
        if (before) {
          ColumnInfo c = *before;
          c.first = at;
          c.last = std::min<uint32_t>(at + static_cast<uint32_t>(sh.count) - 1, kMaxCols - 1);
          c.hidden = c.collapsed = false;
          out.push_back(c);
        }
        std::sort(out.begin(), out.end(), [](const ColumnInfo& a, const ColumnInfo& b) { return a.first < b.first; });
        // Neighbours that say the same are one entry (as Excel writes them).
        std::vector<ColumnInfo> joined;
        for (const ColumnInfo& c : out) {
          if (!joined.empty() && joined.back().last + 1 == c.first) {
            ColumnInfo a = joined.back(), b = c;
            a.first = b.first = a.last = b.last = 0;
            if (a.width == b.width && a.style == b.style && a.outlineLevel == b.outlineLevel && a.hidden == b.hidden &&
                a.customWidth == b.customWidth && a.bestFit == b.bestFit && a.collapsed == b.collapsed &&
                a.phonetic == b.phonetic) {
              joined.back().last = c.last;
              continue;
            }
          }
          joined.push_back(c);
        }
        A::Cols(s) = std::move(joined);
      }

      // ---- Merges, links, the autofilter, page breaks.
      {
        std::vector<Range> out;
        bool changed = false;
        for (Range r : s.Merges()) {
          Range x = r;
          if (!m.Area(x) || (x.firstRow == x.lastRow && x.firstCol == x.lastCol)) {
            changed = true;
            continue;
          }
          changed = changed || !(x == r);
          out.push_back(x);
        }
        if (changed) {
          st.SaveMerges(sv);
          A::Merges(s) = std::move(out);
          st.RecountMerges(s);
        }
      }
      {
        std::vector<Hyperlink> out;
        bool changed = false;
        for (Hyperlink h : s.Hyperlinks()) {
          Range x = h.ref;
          if (!m.Area(x)) {
            changed = true;
            continue;
          }
          changed = changed || !(x == h.ref);
          h.ref = x;
          out.push_back(std::move(h));
        }
        if (changed) {
          st.SaveLinks(sv);
          A::Links(s) = std::move(out);
        }
      }
      if (const auto& f = s.Filter()) {
        Range x = f->ref;
        bool keep = m.Area(x);
        // Its columns are counted from the range's first column, as in the written autoFilter
        // (MoveSheetXml): they follow columns inserted and deleted; a deleted one goes.
        std::vector<FilterColumn> cols = f->columns;
        bool colsChanged = false;
        if (keep && !m.Rows() && m.Across(f->ref.firstRow, f->ref.lastRow) == 1) {
          std::vector<FilterColumn> out;
          for (FilterColumn& c : cols) {
            const uint32_t col = m.Line(f->ref.firstCol + c.colId);
            if (col == kNone || col < x.firstCol || col > x.lastCol) {
              colsChanged = true;
              continue;
            }
            if (col - x.firstCol != c.colId) {
              c.colId = col - x.firstCol;
              if (const size_t a = c.xml.find("colId=\""); a != std::string::npos)  // Other: as the file has it
                if (const size_t b = c.xml.find('"', a + 7); b != std::string::npos)
                  c.xml.replace(a + 7, b - a - 7, std::to_string(c.colId));
              colsChanged = true;
            }
            out.push_back(std::move(c));
          }
          cols = std::move(out);
        }
        if (!keep || !(x == f->ref) || colsChanged) {
          st.SaveFilter(sv);
          if (keep) {
            SheetFilter& now = *A::Filter(s);
            now.ref = x;
            now.columns = std::move(cols);
            now.filtered = !now.columns.empty();
          } else {
            A::Filter(s).reset();
          }
        }
      }
      if (whole) {
        std::vector<PageBreak> out;
        bool changed = false;
        for (PageBreak b : A::Breaks(s, rows)) {
          uint32_t n = m.Line(b.id);
          changed = changed || n != b.id;
          if (n == kNone) continue;
          b.id = n;
          out.push_back(b);
        }
        if (changed) {
          st.SaveBreaks(sv);
          A::Breaks(s, rows) = std::move(out);
        }
      }

      // ---- Tables: their areas, and columns inserted or deleted inside them (named as Excel names
      // new ones: Column1, ...), with their header cells.
      std::vector<Table> tablesBefore = s.Tables();
      std::vector<std::pair<CellAddr, std::string>> headers;
      if (!tablesBefore.empty()) {
        std::vector<Table> after = tablesBefore;
        bool changed = false;
        for (Table& t : after) {
          Range x = t.ref;
          if (!m.Area(x)) continue;
          if (!rows && Effect(m, t.ref) == 1) {
            if (sh.count > 0) {
              uint32_t nextId = 1, nextName = 1;
              for (const TableColumn& c : t.columns) nextId = std::max(nextId, c.id + 1);
              auto pos = t.columns.begin() + (sh.at - t.ref.firstCol);
              for (int32_t k = 0; k < sh.count && t.columns.size() < kMaxCols; ++k) {
                TableColumn c;
                c.id = nextId++;
                for (;; ++nextName) {
                  c.name = "Column" + std::to_string(nextName);
                  if (t.FindColumn(c.name) < 0) break;
                }
                pos = t.columns.insert(pos, c) + 1;
                if (t.headerRows) headers.push_back({{t.ref.firstRow, sh.at + static_cast<uint32_t>(k)}, c.name});
              }
            } else {
              uint32_t from = std::max(sh.at, t.ref.firstCol) - t.ref.firstCol;
              uint32_t to = std::min(sh.at + m.Deleted(), t.ref.lastCol + 1) - t.ref.firstCol;
              t.columns.erase(t.columns.begin() + from, t.columns.begin() + to);
            }
          }
          for (TableColumn& c : t.columns) {
            CellAddr at{t.ref.firstRow + t.headerRows, t.ref.firstCol};
            if (!c.calculatedFormula.empty()) c.calculatedFormula = ctx.formula(c.calculatedFormula, sheet, at.row, at.col);
            if (!c.totalsFormula.empty()) c.totalsFormula = ctx.formula(c.totalsFormula, sheet, t.ref.lastRow, at.col);
          }
          t.ref = x;
          changed = true;
        }
        if (changed) {
          st.SaveTables(sv);
          A::Tables(s) = after;
        }
      }
      for (const auto& [at, text] : headers) {
        CellInput in = CellInput::Text(text);
        std::u16string why;
        st.Put(sv, at.row, at.col, &in, false, std::nullopt, why);
      }
      // Rows inserted into a table get its calculated columns' formulas (Excel fills them).
      for (size_t k = 0; rows && sh.count > 0 && k < tablesBefore.size() && k < s.Tables().size(); ++k) {
        const Table& t = s.Tables()[k];
        if (Effect(m, tablesBefore[k].ref) != 1) continue;
        for (size_t c = 0; c < t.columns.size(); ++c) {
          if (t.columns[c].calculatedFormula.empty()) continue;
          CellInput in = CellInput::Formula(t.columns[c].calculatedFormula);
          for (uint32_t r = sh.at; r < sh.at + static_cast<uint32_t>(sh.count) && r <= t.ref.lastRow; ++r) {
            std::u16string why;
            st.Put(sv, r, t.ref.firstCol + static_cast<uint32_t>(c), &in, false, std::nullopt, why);
          }
        }
      }

      // ---- Names (print areas and titles among them).
      {
        std::vector<DefinedName> list = A::Names(book_);
        bool changed = false;
        for (DefinedName& n : list) {
          if (n.sheet != sheet && !touches(n.formula)) continue;
          std::string now = ctx.formula(n.formula, n.sheet >= 0 ? n.sheet : 0, 0, 0);
          changed = changed || now != n.formula;
          n.formula = std::move(now);
        }
        if (changed) {
          st.SaveBook(false, true);
          A::Names(book_) = std::move(list);
          st.Change(nullptr, kNames, Range{});
        }
      }

      // ---- The sheets' XML around their cells: rules, sparklines, filters, breaks...
      for (size_t i = 0; i < book_.SheetCount(); ++i) {
        Sheet& x = book_.GetSheet(i);
        const detail::PartText& t = A::Text(x);
        if (static_cast<int>(i) != sheet && !touches(t.afterData)) continue;
        detail::PartText now = t;
        refs::MoveSheetXml(now, static_cast<int>(i), ctx);
        if (now.afterData == t.afterData && now.beforeData == t.beforeData) continue;
        st.SaveText(st.Save(x));
        A::Text(x) = std::move(now);
      }

      // ---- Parts: the sheets' drawings, notes, controls, tables and pivot tables; charts and pivot
      // caches anywhere.
      if (package) {
        auto edit = [&](const std::string& part, const std::function<bool(std::string&)>& fn) {
          std::vector<uint8_t> bytes;
          if (!package->ReadPart(part, bytes).Ok()) return;
          std::string xml(bytes.begin(), bytes.end());
          if (fn(xml)) st.SetPart(part, std::move(xml));
        };
        refs::CacheFn cache = [&](std::string_view f, bool visibleOnly, std::vector<refs::CachePoint>& out) {
          size_t bang = f.rfind('!');
          if (bang == std::string_view::npos) return false;
          std::string sn(f.substr(0, bang));
          if (sn.size() >= 2 && sn.front() == '\'' && sn.back() == '\'') {
            std::string u;
            for (size_t k = 1; k + 1 < sn.size(); ++k) {
              u += sn[k];
              if (sn[k] == '\'' && k + 2 < sn.size() && sn[k + 1] == '\'') ++k;
            }
            sn = std::move(u);
          }
          Range r;
          int xi = book_.FindSheet(sn);
          if (xi < 0 || !ParseRange(f.substr(bang + 1), r) || (r.firstRow != r.lastRow && r.firstCol != r.lastCol) ||
              static_cast<uint64_t>(r.lastRow - r.firstRow) + (r.lastCol - r.firstCol) > 100000)
            return false;
          const Sheet& x = book_.GetSheet(static_cast<size_t>(xi));
          for (uint32_t row = r.firstRow; row <= r.lastRow; ++row)
            for (uint32_t col = r.firstCol; col <= r.lastCol; ++col) {
              if (visibleOnly && (x.Row(row).hidden || x.Column(col).hidden)) continue;
              Value v = x.GetValue(row, col);
              refs::CachePoint p;
              if (v.type == ValueType::Number) p = {refs::CachePoint::Number, Number(v.number)};
              else if (v.type == ValueType::String) p = {refs::CachePoint::Text, std::string(v.text)};
              else if (v.type == ValueType::Bool) p = {refs::CachePoint::Text, v.boolean ? "TRUE" : "FALSE"};
              else if (v.type == ValueType::Error) p = {refs::CachePoint::Text, "#N/A"};
              out.push_back(std::move(p));
            }
          return true;
        };
        for (size_t i = 0; i < book_.SheetCount(); ++i) {
          Sheet& x = book_.GetSheet(i);
          const int xi = static_cast<int>(i);
          const bool mine = xi == sheet;
          const pkg::RelationshipSet* rels = x.PartName().empty() ? nullptr : package->RelationshipsOf(x.PartName());
          if (!rels) continue;
          for (const pkg::Relationship& r : rels->items) {
            if (r.external || r.targetPart.empty()) continue;
            auto ours = [&](const std::string& xml) { return mine || touches(xml); };
            if (EndsWith(r.type, "/drawing"))
              edit(r.targetPart, [&](std::string& xml) { return ours(xml) && refs::MoveDrawing(xml, xi, ctx); });
            else if (EndsWith(r.type, "/vmlDrawing"))
              edit(r.targetPart, [&](std::string& xml) { return ours(xml) && refs::MoveVml(xml, xi, ctx); });
            else if (EndsWith(r.type, "/ctrlProp"))
              edit(r.targetPart, [&](std::string& xml) { return ours(xml) && refs::MoveControl(xml, xi, ctx); });
            else if (mine && (EndsWith(r.type, "/comments") || EndsWith(r.type, "/threadedComment")))
              edit(r.targetPart, [&](std::string& xml) { return refs::MoveComments(xml, ctx); });
            else if (mine && EndsWith(r.type, "/pivotTable"))
              edit(r.targetPart, [&](std::string& xml) { return refs::MovePivotTable(xml, ctx); });
          }
        }
        for (size_t k = 0; k < tablesBefore.size() && k < s.Tables().size(); ++k) {
          const Table& before = tablesBefore[k];
          const Table& after = s.Tables()[k];
          edit(after.part, [&](std::string& xml) { return refs::MoveTable(xml, before, after, sheet, ctx); });
        }
        for (const pkg::Part& part : package->Parts()) {
          if (part.deleted) continue;
          std::string_view ct = package->ContentTypeOf(part.name);
          bool chart = ct == "application/vnd.openxmlformats-officedocument.drawingml.chart+xml" ||
                       ct == "application/vnd.ms-office.chartex+xml";
          bool pivot = ct == "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml";
          if (chart || pivot)
            edit(part.name, [&](std::string& xml) {
              return touches(xml) && (chart ? refs::MoveChart(xml, 0, ctx, cache) : refs::MovePivotCache(xml, ctx));
            });
        }
      }

      Range moved = rows ? Range{sh.at, sh.band.firstCol, kMaxRows - 1, sh.band.lastCol}
                         : Range{sh.band.firstRow, sh.at, sh.band.lastRow, kMaxCols - 1};
      st.Change(&s, kShift | kValues | kFormulas | kStyles | kMerges | (rows ? kRowProps : kColProps), moved, sh);
      return Result{};
    });
  }());
}

// ---- Tables' header cells

void Editor::FixHeaders(Sheet& s, const std::vector<CellAddr>& cells) {
  const std::vector<Table> before = s.Tables();
  std::vector<Table> tables = before;
  State& st = *s_;
  struct Renamed {
    std::string table, from, to;
  };
  std::vector<Renamed> renamed;
  std::u16string why;
  bool totals = false;
  for (Table& t : tables) {
    // The totals row: a formula typed there is the column's custom total, text its label, as
    // Excel writes them (a label beside a formula is repaired).
    if (t.totalsRows)
      for (const CellAddr& a : cells) {
        if (a.row != t.ref.lastRow || a.col < t.ref.firstCol || a.col > t.ref.lastCol) continue;
        const size_t k = a.col - t.ref.firstCol;
        if (k >= t.columns.size()) continue;
        TableColumn now = t.columns[k];
        FormulaInfo f;
        if (s.GetFormula(a.row, a.col, f) && f.kind == FormulaKind::Normal) {
          now.totalsFunction = "custom";
          now.totalsFormula = std::string(f.text);
          now.totalsLabel.clear();
        } else {
          const Value v = s.GetValue(a.row, a.col);
          now.totalsFunction.clear();
          now.totalsFormula.clear();
          now.totalsLabel = v.type == ValueType::String ? std::string(v.text) : std::string();
        }
        TableColumn& was = t.columns[k];
        if (now.totalsFunction != was.totalsFunction || now.totalsFormula != was.totalsFormula ||
            now.totalsLabel != was.totalsLabel) {
          was = std::move(now);
          totals = true;
        }
      }
    if (!t.headerRows) continue;
    for (const CellAddr& a : cells) {
      if (a.row != t.ref.firstRow || a.col < t.ref.firstCol || a.col > t.ref.lastCol) continue;
      const size_t k = a.col - t.ref.firstCol;
      if (k >= t.columns.size()) continue;
      // The header as Excel makes it: always text, the value as the cell shows it.
      Value v = s.GetValue(a.row, a.col);
      std::string text;
      if (v.type == ValueType::String) {
        text = std::string(v.text);
      } else if (v.type == ValueType::Bool) {
        text = v.boolean ? "TRUE" : "FALSE";
      } else if (v.type == ValueType::Error) {
        text = std::string(ErrorText(v.error));
      } else if (v.type == ValueType::Number) {
        const Styles& styles = book_.GetStyles();
        const uint32_t fmt = styles.CellXf(s.StyleAt(a.row, a.col)).numFmt;
        const std::string_view code = styles.FormatCode(fmt);
        const NumLocale& loc = locale_ ? *locale_ : EnUsLocale();
        NumberFormat nf = code.empty() ? NumberFormat::Builtin(static_cast<int>(fmt), loc) : NumberFormat(Utf8ToWide(code));
        FormatContext context;
        context.locale = &loc;
        context.date1904 = book_.Date1904();
        Formatted shown;
        nf.Format(v.number, context, shown);
        text = WideToUtf8(shown.text);
      }
      // Unique in the table, ignoring case: an empty one is the next "ColumnN", a taken one gets 2, 3...
      auto taken = [&](std::string_view name) {
        for (size_t j = 0; j < t.columns.size(); ++j)
          if (j != k && NameEqualsNoCase(t.columns[j].name, name)) return true;
        return false;
      };
      const std::string typed = text;
      for (int n = text.empty() ? 1 : 2; text.empty() || taken(text); ++n) text = (typed.empty() ? "Column" : typed) + std::to_string(n);
      if (v.type != ValueType::String || v.text != text) {
        CellInput in = CellInput::Text(text);
        st.Put(st.Save(s), a.row, a.col, &in, false, std::nullopt, why);
      }
      if (t.columns[k].name != text) {
        renamed.push_back({t.displayName, t.columns[k].name, text});
        t.columns[k].name = text;
      }
    }
  }
  if (renamed.empty() && !totals) return;
  st.SaveTables(st.Save(s));
  A::Tables(s) = tables;
  int index = 0;
  for (size_t i = 0; i < book_.SheetCount(); ++i)
    if (&book_.GetSheet(i) == &s) index = static_cast<int>(i);
  if (pkg::Package* package = book_.GetPackage()) {
    refs::Context ctx{refs::Mover(Shift{}), index, s.Name(), {}, {}};
    ctx.formula = [](std::string_view text, int, uint32_t, uint32_t) { return std::string(text); };
    ctx.copy = [](std::string_view text, int, CellAddr, CellAddr) { return std::string(text); };
    for (size_t i = 0; i < tables.size() && i < before.size(); ++i) {
      std::vector<uint8_t> bytes;
      if (tables[i].part.empty() || !package->ReadPart(tables[i].part, bytes).Ok()) continue;
      std::string xml(bytes.begin(), bytes.end());
      if (refs::MoveTable(xml, before[i], tables[i], index, ctx)) st.SetPart(tables[i].part, std::move(xml));
    }
  }
  // Structured references to the renamed columns, everywhere.
  if (!renamed.empty())
    RewriteSheetRefs(
      -1,
      [&](std::string_view text, int) {
        std::string out(text);
        for (const Renamed& r : renamed) out = refs::RenameTableColumn(out, r.table, r.from, r.to);
        return out;
      },
      [&](std::string_view text) {
        for (const Renamed& r : renamed)
          if (Mentions(text, r.table)) return true;
        return false;
      });
}

// ---- Sorting

namespace {

// How a sort copies a formula moved by (dr, dc): references without a sheet name follow the cell, as
// a copy does; references naming a sheet, even the formula's own, stay where they were (Excel,
// measured). Printed for the sheet kPlain, which only references without a sheet name report.
class SortCopy : public calc::RefMap {
 public:
  static constexpr int kPlain = -2;
  SortCopy(int32_t dr, int32_t dc) : dr_(dr), dc_(dc) {}
  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool, bool) const override {
    if (sheet == kPlain) return true;
    if (!(abs & kRow1)) r1 -= dr_;
    if (!(abs & kCol1)) c1 -= dc_;
    if (!(abs & kRow2)) r2 -= dr_;
    if (!(abs & kCol2)) c2 -= dc_;
    return true;
  }

 private:
  int32_t dr_, dc_;
};

std::string SortedFormula(std::string_view text, int sheet, CellAddr from, CellAddr to, calc::Names& names) {
  calc::CompileOptions at;
  at.sheet = sheet;
  at.row = static_cast<int32_t>(from.row);
  at.col = static_cast<int32_t>(from.col);
  auto f = calc::Compile(text, at, names);
  if (!f) return std::string(text);
  calc::CompileOptions print = at;
  print.sheet = SortCopy::kPlain;
  print.row = static_cast<int32_t>(to.row);
  print.col = static_cast<int32_t>(to.col);
  SortCopy map(static_cast<int32_t>(to.row) - at.row, static_cast<int32_t>(to.col) - at.col);
  calc::PrintOptions how;
  how.offSheetRef = true;
  how.map = &map;
  return calc::Print(*f, print, names, how);
}

}  // namespace

Result Editor::SortRows(int sheet, const Range& r, const std::vector<uint32_t>& order, const std::vector<SortKey>& keys) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Sort, r); !no) return no;
  return SortLines(sheet, r, order, keys, true);
}
Result Editor::SortColumns(int sheet, const Range& r, const std::vector<uint32_t>& order,
                           const std::vector<SortKey>& keys) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Sort, r); !no) return no;
  return SortLines(sheet, r, order, keys, false);
}

Result Editor::SortLines(int sheet, const Range& r, const std::vector<uint32_t>& order, const std::vector<SortKey>& keys,
                         bool rows) {
  Rec rec(*this, JournalOp::SortLines, [&](JournalRecord& w) { w.I(sheet).R(r).U(order.size()); for (uint32_t o : order) w.U(o); w.U(keys.size()); for (const SortKey& k : keys) w.U(k.line).B(k.descending); w.B(rows); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r)) return BadRange();
    const uint32_t first = rows ? r.firstRow : r.firstCol;
    const uint32_t n = (rows ? r.lastRow : r.lastCol) - first + 1;
    std::vector<uint32_t> dest(n, kNone);
    bool same = true;
    if (order.size() != n) return Result::Fail(u"The sort order doesn't fit the range.");
    for (uint32_t i = 0; i < n; ++i) {
      if (order[i] >= n || dest[order[i]] != kNone) return Result::Fail(u"The sort order doesn't fit the range.");
      dest[order[i]] = i;
      same = same && order[i] == i;
    }
    for (const SortKey& k : keys)
      if (k.line < (rows ? r.firstCol : r.firstRow) || k.line > (rows ? r.lastCol : r.lastRow))
        return Result::Fail(u"A sort key is outside the range.");
    if (same && keys.empty()) return {};
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet) return Result::Fail(u"This sheet has no cells.");
    pkg::Package* package = book_.GetPackage();

    // ---- What Excel refuses.
    for (const Range& g : s.Merges())
      if (Overlaps(g, r)) return Result::Fail(u"To do this, all the merged cells need to be the same size.");
    for (const Range& a : State::Arrays(s))
      if (Overlaps(a, r)) return Result::Fail(u"You can't change part of an array.");
    const std::vector<FCell> fcells = FormulaCellsOf(s);
    auto inside = [&](uint32_t row, uint32_t col) {
      return row >= r.firstRow && row <= r.lastRow && col >= r.firstCol && col <= r.lastCol;
    };
    for (const FCell& c : fcells) {
      if (!inside(c.row, c.col) || c.id >= A::Formulas(s).size()) continue;
      const FormulaRec& f = A::Formulas(s)[c.id];
      if (f.kind == FormulaKind::DataTable) return Result::Fail(u"Featherweight can't sort a what-if data table yet.");
      if (f.dynamic && f.hasRef && !(f.ref.firstRow == f.ref.lastRow && f.ref.firstCol == f.ref.lastCol))
        return Result::Fail(u"You can't change part of an array.");
    }
    std::vector<std::pair<std::string, std::string>> sheetParts;  // relationship type, part
    if (package && !s.PartName().empty())
      if (const pkg::RelationshipSet* rels = package->RelationshipsOf(s.PartName()))
        for (const pkg::Relationship& rel : rels->items)
          if (!rel.external && !rel.targetPart.empty()) sheetParts.push_back({rel.type, rel.targetPart});
    if (package)
      for (const auto& [type, part] : sheetParts) {
        std::vector<uint8_t> bytes;
        Range where;
        if (EndsWith(type, "/pivotTable") && package->ReadPart(part, bytes).Ok() &&
            PivotLocation(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where) &&
            Overlaps(where, r))
          return Result::Fail(u"We can't make this change for the selected cells because it will affect a PivotTable.");
      }

    return Run(*this, u"Sort", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveAllFormulas(sv);
      st.SaveLayout(sv);
      st.SaveKept(sv);
      Shift sh{rows ? Shift::Axis::Rows : Shift::Axis::Columns, first, 0,
               rows ? Range{0, r.firstCol, kMaxRows - 1, r.lastCol} : Range{r.firstRow, 0, r.lastRow, kMaxCols - 1}};
      refs::Mover m(sh);
      m.dest = dest;
      auto moved = [&](uint32_t row, uint32_t col) {
        CellAddr a{row, col};
        if (inside(row, col)) m.Cell(a.row, a.col);
        return a;
      };
      auto key = [](uint32_t row, uint32_t col) { return (static_cast<uint64_t>(row) << 16) | col; };

      // ---- Formulas: moved ones copied to their new cells, each a record of its own; shared formulas
      // reaching into the range shared no more (Excel writes every cell's own).
      BookNames names(book_);
      auto& recs = A::Formulas(s);
      TextPool& texts = A::Texts(s);
      auto& store = A::Store(s);
      std::unordered_map<uint32_t, std::pair<CellAddr, std::string>> masters;  // shared index -> master
      std::unordered_set<uint32_t> unshared;
      for (const FCell& c : fcells)
        if (c.id < recs.size() && recs[c.id].kind == FormulaKind::Shared && recs[c.id].sharedIndex != kNone &&
            inside(c.row, c.col))
          unshared.insert(recs[c.id].sharedIndex);
      for (const FCell& c : fcells)
        if (c.id < recs.size() && recs[c.id].kind == FormulaKind::Shared && recs[c.id].text != kNone &&
            unshared.count(recs[c.id].sharedIndex))
          masters[recs[c.id].sharedIndex] = {CellAddr{c.row, c.col}, std::string(texts.Get(recs[c.id].text))};
      std::unordered_map<uint64_t, uint32_t> own;  // cell (where it was) -> its new record
      for (const FCell& c : fcells) {
        if (c.id >= recs.size()) continue;
        const CellAddr at{c.row, c.col}, to = moved(c.row, c.col);
        const bool moves = !(at == to);
        const FormulaRec was = recs[c.id];
        std::string text;
        if (was.kind == FormulaKind::Shared && was.sharedIndex != kNone && unshared.count(was.sharedIndex)) {
          auto mi = masters.find(was.sharedIndex);
          if (mi == masters.end()) continue;  // a follower without its master: left as read
          text = was.text != kNone ? mi->second.second
                                   : [&] {
                                       calc::CompileOptions a, b;
                                       a.sheet = b.sheet = sheet;
                                       a.row = static_cast<int32_t>(mi->second.first.row);
                                       a.col = static_cast<int32_t>(mi->second.first.col);
                                       b.row = static_cast<int32_t>(c.row);
                                       b.col = static_cast<int32_t>(c.col);
                                       std::string out = calc::CopyFormula("=" + mi->second.second, a, b, names);
                                       return !out.empty() && out[0] == '=' ? out.substr(1) : out;
                                     }();
        } else if (!moves || was.text == kNone) {
          continue;
        } else {
          text = std::string(texts.Get(was.text));
        }
        if (moves) text = SortedFormula(text, sheet, at, to, names);
        FormulaRec rec = was;
        if (rec.kind == FormulaKind::Shared) {
          rec.kind = FormulaKind::Normal;
          rec.sharedIndex = kNone;
          rec.hasRef = false;
          rec.attrs = EditAttrs(store, rec.attrs, {"t", "ref", "si"});
        } else if (rec.hasRef && (rec.kind == FormulaKind::Array || rec.dynamic) && moves) {
          rec.ref = Range{to.row, to.col, to.row, to.col};
          rec.attrs = EditAttrs(store, rec.attrs, {}, RangeName(rec.ref));
        }
        rec.text = texts.Add(text);
        recs.push_back(rec);
        own[key(c.row, c.col)] = static_cast<uint32_t>(recs.size() - 1);
      }

      // ---- Cells: the blocks holding the range's rows laid out again, the rest kept.
      struct Item {
        uint32_t row = 0;
        CellRec cell;
        uint32_t formula = kNone;
        bool hasSide = false;
        CellSide side;
      };
      const auto& oldBlocks = A::Blocks(s);
      const auto& oldFirst = A::FirstRows(s);
      std::vector<std::shared_ptr<RowBlock>> blocks;
      std::vector<uint32_t> firstRows;
      std::vector<Item> items;
      std::vector<RowRec> rowRecs;
      auto flush = [&]() {  // the gathered rows, sorted, into new blocks
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.row != b.row ? a.row < b.row : a.cell.col < b.cell.col; });
        bool open = false;
        for (size_t ri = 0, ci = 0; ri < rowRecs.size() || ci < items.size();) {
          uint32_t row = std::min(ri < rowRecs.size() ? rowRecs[ri].row : kNone, ci < items.size() ? items[ci].row : kNone);
          RowRec rec;
          rec.row = row;
          if (ri < rowRecs.size() && rowRecs[ri].row == row) rec = rowRecs[ri++];
          if (!open || blocks.back()->rows.size() >= kBlockRows || blocks.back()->cells.size() >= SheetBuilder::kBlockCells) {
            blocks.push_back(std::make_shared<RowBlock>());
            firstRows.push_back(row);
            open = true;
          }
          RowBlock& b = *blocks.back();
          rec.first = static_cast<uint32_t>(b.cells.size());
          rec.count = 0;
          for (; ci < items.size() && items[ci].row == row; ++ci) {
            uint32_t index = static_cast<uint32_t>(b.cells.size());
            b.cells.push_back(items[ci].cell);
            if (items[ci].formula != kNone && (items[ci].cell.flags & detail::kHasFormula))
              b.formulas.push_back({index, items[ci].formula});
            if (items[ci].hasSide) {
              b.sides.push_back(items[ci].side);
              b.sides.back().cell = index;
            }
            ++rec.count;
          }
          b.rows.push_back(rec);
        }
        items.clear();
        rowRecs.clear();
      };
      for (size_t bi = 0; bi < oldBlocks.size(); ++bi) {
        const RowBlock& b = *oldBlocks[bi];
        if (b.rows.empty() || b.rows.back().row < r.firstRow || b.rows.front().row > r.lastRow) {
          flush();
          blocks.push_back(oldBlocks[bi]);
          firstRows.push_back(oldFirst[bi]);
          continue;
        }
        size_t fi = 0, si = 0;
        for (const RowRec& r0 : b.rows) {
          for (uint32_t k = r0.first; k < r0.first + r0.count; ++k) {
            while (fi < b.formulas.size() && b.formulas[fi].first < k) ++fi;
            while (si < b.sides.size() && b.sides[si].cell < k) ++si;
            const CellRec& c0 = b.cells[k];
            const CellAddr to = moved(r0.row, c0.col);
            Item it;
            it.row = to.row;
            it.cell = c0;
            it.cell.col = static_cast<uint16_t>(to.col);
            it.formula = fi < b.formulas.size() && b.formulas[fi].first == k ? b.formulas[fi].second : kNone;
            if (auto o = own.find(key(r0.row, c0.col)); o != own.end()) it.formula = o->second;
            if (si < b.sides.size() && b.sides[si].cell == k) {
              it.hasSide = true;
              it.side = b.sides[si];
              if (!(to == CellAddr{r0.row, c0.col})) it.side.attrs = EditAttrs(store, it.side.attrs, {"r"});
            }
            items.push_back(it);
          }
          RowRec rr = r0;
          if (r0.row >= r.firstRow && r0.row <= r.lastRow) rr.attrs = EditAttrs(store, rr.attrs, {"spans"});
          rowRecs.push_back(rr);
        }
      }
      flush();
      size_t cellCount = 0, formulaCells = 0;
      for (const auto& b : blocks) {
        cellCount += b->cells.size();
        formulaCells += b->formulas.size();
      }
      A::Blocks(s) = std::move(blocks);
      A::FirstRows(s) = std::move(firstRows);
      A::CellCount(s) = cellCount;
      A::FormulaCells(s) = formulaCells;
      std::vector<detail::Kept> kept;
      for (detail::Kept k : A::Kept(s)) {
        if (k.row != kNone && k.col != detail::Kept::kBeforeRow && k.col != detail::Kept::kRowEnd) {
          CellAddr to = moved(k.row, k.col);
          k.row = to.row;
          k.col = to.col;
        }
        kept.push_back(std::move(k));
      }
      std::stable_sort(kept.begin(), kept.end(), [](const detail::Kept& a, const detail::Kept& b) { return a.row < b.row; });
      A::Kept(s) = std::move(kept);

      // ---- Links on cells that moved go with them.
      {
        std::vector<Hyperlink> links = s.Hyperlinks();
        bool changed = false;
        for (Hyperlink& h : links) {
          bool one = rows ? h.ref.firstRow == h.ref.lastRow : h.ref.firstCol == h.ref.lastCol;
          if (!one || !inside(h.ref.firstRow, h.ref.firstCol) || !inside(h.ref.lastRow, h.ref.lastCol)) continue;
          CellAddr to = moved(h.ref.firstRow, h.ref.firstCol);
          if (to == CellAddr{h.ref.firstRow, h.ref.firstCol}) continue;
          h.ref = Range{to.row, to.col, to.row + (h.ref.lastRow - h.ref.firstRow), to.col + (h.ref.lastCol - h.ref.firstCol)};
          changed = true;
        }
        if (changed) {
          st.SaveLinks(sv);
          A::Links(s) = std::move(links);
        }
      }

      // ---- The sort's record (sortState), as Excel writes it.
      if (!keys.empty()) {
        st.SaveText(sv);
        detail::PartText& t = A::Text(s);
        const std::string p = xlsx::SheetPrefix(t);
        std::string x = "<" + p + "sortState";
        if (!rows) x += " columnSort=\"1\"";
        x += " ref=\"" + RangeName(r) + "\">";
        for (const SortKey& k : keys) {
          Range kr = rows ? Range{r.firstRow, k.line, r.lastRow, k.line} : Range{k.line, r.firstCol, k.line, r.lastCol};
          x += "<" + p + "sortCondition";
          if (k.descending) x += " descending=\"1\"";
          x += " ref=\"" + RangeName(kr) + "\"/>";
        }
        x += "</" + p + "sortState>";
        xlsx::PutSheetElement(t, "sortState", x);
      }

      // ---- Notes (and threaded comments, VML boxes) and drawings inside one row go with their cells.
      if (package) {
        refs::Context ctx{m, sheet, s.Name(), {}, {}};
        ctx.formula = [](std::string_view text, int, uint32_t, uint32_t) { return std::string(text); };
        ctx.copy = [](std::string_view text, int, CellAddr, CellAddr) { return std::string(text); };
        for (const auto& [type, part] : sheetParts) {
          std::vector<uint8_t> bytes;
          if (!package->ReadPart(part, bytes).Ok()) continue;
          std::string xml(bytes.begin(), bytes.end());
          bool changed = false;
          if (EndsWith(type, "/comments") || EndsWith(type, "/threadedComment")) changed = refs::MoveComments(xml, ctx);
          else if (EndsWith(type, "/vmlDrawing")) changed = refs::MoveVml(xml, sheet, ctx);
          else if (EndsWith(type, "/drawing")) changed = refs::MoveDrawing(xml, sheet, ctx);
          if (changed) st.SetPart(part, std::move(xml));
        }
      }
      st.Change(&s, kValues | kFormulas | kStyles | kObjects, r);
      return Result{};
    });
  }());
}

namespace {

// "#REF!A1", "#REF!$A$1:$B$2" (a reference to a deleted sheet as SheetsChanged writes it) become
// "#REF!", as Excel writes them in the file.
std::string StripRefTails(const std::string& s) {
  std::string out;
  size_t i = 0;
  auto ref = [&](size_t k) {
    if (k < s.size() && s[k] == '$') ++k;
    while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) ++k;
    if (k < s.size() && s[k] == '$') ++k;
    while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
    return k;
  };
  for (size_t at = s.find("#REF!"); at != std::string::npos; at = s.find("#REF!", i)) {
    out.append(s, i, at + 5 - i);
    size_t j = at + 5, e = ref(j);
    if (e > j && e < s.size() && s[e] == ':') {
      size_t e2 = ref(e + 1);
      if (e2 > e + 1) e = e2;
    }
    bool name = e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_' || s[e] == '(');
    i = e > j && !name ? e : j;
  }
  out.append(s, i, std::string::npos);
  return out;
}

}  // namespace

void Editor::RewriteSheetRefs(int skip, const std::function<std::string(std::string_view, int)>& rewrite,
                              const std::function<bool(std::string_view)>& touches, std::string_view from,
                              std::string_view to) {
  State& st = *s_;
  refs::Context ctx{refs::Mover(Shift{}), -1, std::string(), {}, {}};
  ctx.formula = [&](std::string_view text, int at, uint32_t, uint32_t) {
    return touches(text) ? rewrite(text, at) : std::string(text);
  };
  ctx.copy = [](std::string_view text, int, CellAddr, CellAddr) { return std::string(text); };
  pkg::Package* package = book_.GetPackage();
  auto edit = [&](const std::string& part, const std::function<bool(std::string&)>& fn) {
    std::vector<uint8_t> bytes;
    if (!package || !package->ReadPart(part, bytes).Ok()) return;
    std::string xml(bytes.begin(), bytes.end());
    if (touches(xml) && fn(xml)) st.SetPart(part, std::move(xml));
  };
  for (size_t i = 0; i < book_.SheetCount(); ++i) {
    if (static_cast<int>(i) == skip) continue;
    Sheet& x = book_.GetSheet(i);
    const int xi = static_cast<int>(i);
    auto& recs = A::Formulas(x);
    SheetSave* sv = nullptr;
    for (uint32_t id = 0; id < recs.size(); ++id) {
      if (recs[id].text == kNone) continue;
      std::string_view text = A::Texts(x).Get(recs[id].text);
      if (!touches(text)) continue;
      std::string now = rewrite(text, xi);
      if (now == text) continue;
      if (!sv) {
        sv = &st.Save(x);
        st.SaveCells(*sv);
      }
      st.SaveFormula(*sv, id);
      recs[id].text = A::Texts(x).Add(now);
    }
    if (sv) st.Change(&x, kFormulas, Range::All());
    std::vector<Table> tables = x.Tables();
    bool tablesChanged = false;
    for (Table& t : tables)
      for (TableColumn& c : t.columns)
        for (std::string* f : {&c.calculatedFormula, &c.totalsFormula})
          if (!f->empty() && touches(*f)) {
            std::string now = rewrite(*f, xi);
            tablesChanged = tablesChanged || now != *f;
            *f = std::move(now);
          }
    if (tablesChanged) {
      st.SaveTables(st.Save(x));
      A::Tables(x) = tables;
    }
    for (const Table& t : x.Tables())
      edit(t.part, [&](std::string& xml) { return refs::MoveTable(xml, t, t, xi, ctx); });
    if (touches(A::Text(x).afterData)) {
      detail::PartText now = A::Text(x);
      refs::MoveSheetXml(now, xi, ctx);
      if (now.afterData != A::Text(x).afterData) {
        st.SaveText(st.Save(x));
        A::Text(x) = std::move(now);
      }
    }
    const pkg::RelationshipSet* rels = package && !x.PartName().empty() ? package->RelationshipsOf(x.PartName()) : nullptr;
    if (rels)
      for (const pkg::Relationship& r : rels->items) {
        if (r.external || r.targetPart.empty()) continue;
        if (EndsWith(r.type, "/drawing"))
          edit(r.targetPart, [&](std::string& xml) { return refs::MoveDrawing(xml, xi, ctx); });
        else if (EndsWith(r.type, "/vmlDrawing"))
          edit(r.targetPart, [&](std::string& xml) { return refs::MoveVml(xml, xi, ctx); });
        else if (EndsWith(r.type, "/ctrlProp"))
          edit(r.targetPart, [&](std::string& xml) { return refs::MoveControl(xml, xi, ctx); });
      }
  }
  std::vector<DefinedName> names = A::Names(book_);
  bool namesChanged = false;
  for (DefinedName& n : names)
    if (touches(n.formula)) {
      std::string now = rewrite(n.formula, n.sheet >= 0 ? n.sheet : 0);
      namesChanged = namesChanged || now != n.formula;
      n.formula = std::move(now);
    }
  if (namesChanged) {
    st.SaveBook(false, true);
    A::Names(book_) = std::move(names);
    st.Change(nullptr, kNames, Range{});
  }
  if (!package) return;
  for (const pkg::Part& part : package->Parts()) {
    if (part.deleted) continue;
    std::string_view ct = package->ContentTypeOf(part.name);
    if (ct == "application/vnd.openxmlformats-officedocument.drawingml.chart+xml" ||
        ct == "application/vnd.ms-office.chartex+xml")
      edit(part.name, [&](std::string& xml) { return refs::MoveChart(xml, 0, ctx, {}); });
    else if (!from.empty() && ct == "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml")
      edit(part.name, [&](std::string& xml) { return refs::RenamePivotSource(xml, from, to); });
  }
}

// ---- Package parts

namespace {

// `target` as a relationship from `source` names it: relative to the source's folder, as Excel
// writes them ("../drawings/drawing1.xml"); from the package ("/"), without the leading slash.
std::string RelativeTarget(std::string_view source, std::string_view target) {
  auto split = [](std::string_view s) {
    std::vector<std::string_view> out;
    for (size_t at = 0; at < s.size();) {
      size_t slash = s.find('/', at);
      if (slash == std::string_view::npos) slash = s.size();
      if (slash > at) out.push_back(s.substr(at, slash - at));
      at = slash + 1;
    }
    return out;
  };
  std::vector<std::string_view> from = split(source), to = split(target);
  if (!from.empty()) from.pop_back();  // the source's own name
  size_t common = 0;
  while (common < from.size() && common + 1 < to.size() && from[common] == to[common]) ++common;
  std::string out;
  for (size_t i = common; i < from.size(); ++i) out += "../";
  for (size_t i = common; i < to.size(); ++i) {
    out += to[i];
    if (i + 1 < to.size()) out += '/';
  }
  return out;
}

}  // namespace

std::string Editor::AddPart(std::string_view source, std::string_view relType, std::string_view name,
                            std::string_view contentType, std::string bytes) {
  State& st = *s_;
  pkg::Package* package = book_.GetPackage();
  if (st.open.empty() || !package || name.empty() || name[0] != '/' || package->FindPart(name)) return {};
  if (source != "/" && !package->FindPart(source)) return {};
  const std::string part(name), from(source);
  st.KeepPart(part);
  st.KeepTypes();
  package->SetPart(part, std::vector<uint8_t>(bytes.begin(), bytes.end()));
  if (package->ContentTypeOf(part) != contentType) {
    pkg::ContentTypes types = package->Types();
    auto& o = types.overrides;
    o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& x) { return x.first == part; }), o.end());
    o.emplace_back(part, std::string(contentType));
    package->SetContentTypes(std::move(types));
  }
  return AddRelationship(from, relType, part);
}

std::string Editor::AddRelationship(std::string_view source, std::string_view relType, std::string_view target) {
  State& st = *s_;
  pkg::Package* package = book_.GetPackage();
  if (st.open.empty() || !package || !package->FindPart(target)) return {};
  if (source != "/" && !package->FindPart(source)) return {};
  const std::string from(source);
  st.KeepRels(from);
  pkg::RelationshipSet set;
  if (const pkg::RelationshipSet* r = package->RelationshipsOf(from)) set = *r;
  else set.source = from;
  std::string id;
  for (size_t n = set.items.size() + 1;; ++n) {
    id = "rId" + std::to_string(n);
    if (std::none_of(set.items.begin(), set.items.end(), [&](const pkg::Relationship& x) { return x.id == id; })) break;
  }
  pkg::Relationship rel;
  rel.id = id;
  rel.type = std::string(relType);
  rel.target = RelativeTarget(from, target);
  set.items.push_back(std::move(rel));
  package->SetRelationships(std::move(set));
  return id;
}

bool Editor::WritePart(std::string_view name, std::string bytes) {
  State& st = *s_;
  pkg::Package* package = book_.GetPackage();
  if (st.open.empty() || !package || !package->FindPart(name)) return false;
  st.SetPart(std::string(name), std::move(bytes));
  return true;
}

bool Editor::RemovePart(std::string_view name) {
  State& st = *s_;
  pkg::Package* package = book_.GetPackage();
  if (st.open.empty() || !package || !package->FindPart(name)) return false;
  const std::string part(name);
  // Relationships to it, from anywhere.
  std::vector<pkg::RelationshipSet> sets;
  for (const pkg::RelationshipSet& set : package->Relationships())
    if (std::any_of(set.items.begin(), set.items.end(),
                    [&](const pkg::Relationship& r) { return !r.external && NameEqualsNoCase(r.targetPart, part); }))
      sets.push_back(set);
  for (pkg::RelationshipSet& set : sets) {
    st.KeepRels(set.source);
    auto& items = set.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const pkg::Relationship& r) { return !r.external && NameEqualsNoCase(r.targetPart, part); }),
                items.end());
    package->SetRelationships(std::move(set));
  }
  // Its own relationships.
  if (const pkg::RelationshipSet* own = package->RelationshipsOf(part)) {
    st.KeepRels(part);
    pkg::RelationshipSet none = *own;
    none.items.clear();
    package->SetRelationships(none);
    package->DeletePart(none.relsPart);
  }
  st.KeepTypes();
  pkg::ContentTypes types = package->Types();
  auto& o = types.overrides;
  const size_t before = o.size();
  o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& x) { return NameEqualsNoCase(x.first, part); }), o.end());
  if (o.size() != before) package->SetContentTypes(std::move(types));
  st.KeepPart(part);
  package->DeletePart(part);
  return true;
}

std::string Editor::NewPartName(std::string_view stem, std::string_view ext) const {
  const pkg::Package* package = book_.GetPackage();
  for (size_t n = 1;; ++n) {
    std::string name = std::string(stem) + std::to_string(n) + std::string(ext);
    if (!package || !package->FindPart(name)) return name;
  }
}

std::string Editor::PartBytes(std::string_view name) const {
  const pkg::Package* package = book_.GetPackage();
  std::vector<uint8_t> bytes;
  if (!package || !package->ReadPart(name, bytes).Ok()) return {};
  return std::string(bytes.begin(), bytes.end());
}

// ---- Drawings and charts

namespace {

constexpr std::string_view kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

}  // namespace

std::string Editor::SheetDrawing(int sheet, bool create) {
  State& st = *s_;
  pkg::Package* package = book_.GetPackage();
  if (!package || sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return {};
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  const std::string sheetPart = s.PartName();
  if (sheetPart.empty() || !package->FindPart(sheetPart)) return {};
  const std::string type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing";
  if (const pkg::RelationshipSet* rels = package->RelationshipsOf(sheetPart))
    for (const pkg::Relationship& r : rels->items)
      if (!r.external && r.type == type && package->FindPart(r.targetPart)) return r.targetPart;
  if (!create || st.open.empty()) return {};
  const std::string drawing = NewPartName("/xl/drawings/drawing", ".xml");
  const std::string id = AddPart(sheetPart, type, drawing, "application/vnd.openxmlformats-officedocument.drawing+xml",
                                 "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<xdr:wsDr "
                                 "xmlns:xdr=\"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing\" "
                                 "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"></xdr:wsDr>");
  if (id.empty()) return {};
  SheetSave& sv = st.Save(s);
  st.SaveText(sv);
  detail::PartText& t = A::Text(s);
  xlsx::PutSheetElement(t, "drawing", "<" + xlsx::SheetPrefix(t) + "drawing xmlns:r=\"" + std::string(kRelNs) + "\" r:id=\"" + id + "\"/>");
  st.Change(&s, kObjects, Range{});
  return drawing;
}

uint32_t Editor::NextShapeId(std::string_view drawing) const { return drawing::NextShapeId(PartBytes(drawing)); }

bool Editor::AddAnchor(std::string_view drawing, std::string_view anchorXml) {
  std::string xml = PartBytes(drawing);
  const size_t end = xml.rfind("</");
  if (xml.empty() || end == std::string::npos) return false;
  xml.insert(end, anchorXml);
  return WritePart(drawing, std::move(xml));
}

std::string Editor::AnchorXml(std::string_view drawing, uint32_t id) const {
  const std::string xml = PartBytes(drawing);
  const std::vector<drawing::Object> objects = drawing::Objects(xml);
  const drawing::Object* o = drawing::Find(objects, id);
  return o ? xml.substr(o->begin, o->end - o->begin) : std::string();
}

bool Editor::ReplaceAnchor(std::string_view drawing, uint32_t id, std::string_view anchorXml) {
  std::string xml = PartBytes(drawing);
  const std::vector<drawing::Object> objects = drawing::Objects(xml);
  const drawing::Object* o = drawing::Find(objects, id);
  if (!o) return false;
  xml.replace(o->begin, o->end - o->begin, anchorXml);
  return WritePart(drawing, std::move(xml));
}

Result Editor::MoveObject(int sheet, uint32_t id, const drawing::Place& p) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::MoveObject, [&](JournalRecord& w) {
    w.I(sheet).U(id);
    for (const drawing::Marker* m : {&p.from, &p.to}) w.U(m->col).U(m->row).I(m->colOff).I(m->rowOff);
    w.I(p.x).I(p.y).I(p.cx).I(p.cy);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (p.from.col >= kMaxCols || p.to.col >= kMaxCols || p.from.row >= kMaxRows || p.to.row >= kMaxRows ||
        p.to.col < p.from.col || p.to.row < p.from.row || p.cx < 0 || p.cy < 0)
      return BadRange();
    const std::string d = SheetDrawing(sheet, false);
    const std::string anchor = d.empty() ? std::string() : AnchorXml(d, id);
    if (anchor.empty()) return Result::Fail(u"The object isn't on this sheet any more.");
    const std::string placed = drawing::Placed(anchor, p);
    if (placed.empty()) return Result::Fail(u"This object can't be moved.");
    return Run(*this, u"Move Object", [&]() -> Result {
      if (!ReplaceAnchor(d, id, placed)) return Result::Fail(u"This object can't be moved.");
      s_->Change(&book_.GetSheet(static_cast<size_t>(sheet)), kObjects, Range{p.from.row, p.from.col, p.to.row, p.to.col});
      return Result{};
    });
  }());
}

Result Editor::DeleteObjects(int sheet, const std::vector<uint32_t>& ids) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::DeleteObjects, [&](JournalRecord& w) {
    w.I(sheet).U(ids.size());
    for (uint32_t id : ids) w.U(id);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    const std::string d = SheetDrawing(sheet, false);
    std::string xml = d.empty() ? std::string() : PartBytes(d);
    const std::vector<drawing::Object> objects = drawing::Objects(xml);
    std::vector<const drawing::Object*> gone;
    for (uint32_t id : ids)
      if (const drawing::Object* o = drawing::Find(objects, id); o && std::find(gone.begin(), gone.end(), o) == gone.end()) gone.push_back(o);
    if (gone.empty()) return Result::Fail(u"The object isn't on this sheet any more.");
    return Run(*this, u"Delete", [&]() -> Result {
      State& st = *s_;
      pkg::Package& package = *book_.GetPackage();
      Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
      std::vector<std::string> rels;
      std::sort(gone.begin(), gone.end(), [](auto* a, auto* b) { return a->begin > b->begin; });
      for (const drawing::Object* o : gone) {
        for (std::string& r : drawing::RelIds(std::string_view(xml).substr(o->begin, o->end - o->begin))) rels.push_back(std::move(r));
        xml.erase(o->begin, o->end - o->begin);
      }
      // A part nothing refers to any more goes, and then what only it referred to.
      auto unused = [&](const std::string& part) {
        for (const pkg::RelationshipSet& set : package.Relationships())
          for (const pkg::Relationship& r : set.items)
            if (!r.external && NameEqualsNoCase(r.targetPart, part)) return false;
        return true;
      };
      auto ownTargets = [&](const std::string& part) {
        std::vector<std::string> own;
        if (const pkg::RelationshipSet* set = package.RelationshipsOf(part))
          for (const pkg::Relationship& r : set->items)
            if (!r.external) own.push_back(r.targetPart);
        return own;
      };
      auto drop = [&](auto&& self, const std::string& part) -> void {
        if (!package.FindPart(part) || !unused(part)) return;
        const std::vector<std::string> own = ownTargets(part);
        RemovePart(part);
        for (const std::string& o : own) self(self, o);
      };
      if (drawing::Objects(xml).empty()) {
        // The last object: the drawing goes, with the sheet's <drawing r:id>.
        st.SaveText(st.Save(s));
        xlsx::PutSheetElement(A::Text(s), "drawing", {});
        const std::vector<std::string> own = ownTargets(d);
        RemovePart(d);
        for (const std::string& o : own) drop(drop, o);
      } else {
        WritePart(d, xml);
        const std::vector<std::string> left = drawing::RelIds(xml);
        if (const pkg::RelationshipSet* set = package.RelationshipsOf(d)) {
          pkg::RelationshipSet kept = *set;
          std::vector<std::string> targets;
          auto& items = kept.items;
          items.erase(std::remove_if(items.begin(), items.end(), [&](const pkg::Relationship& r) {
                        const bool off = std::find(rels.begin(), rels.end(), r.id) != rels.end() &&
                                         std::find(left.begin(), left.end(), r.id) == left.end();
                        if (off && !r.external) targets.push_back(r.targetPart);
                        return off;
                      }), items.end());
          if (items.size() != set->items.size()) {
            st.KeepRels(d);
            package.SetRelationships(std::move(kept));
            for (const std::string& t : targets) drop(drop, t);
          }
        }
      }
      st.Change(&s, kObjects, Range{});
      return Result{};
    });
  }());
}

Result Editor::InsertChart(int sheet, const NewChart& c) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects, c.data); !no) return no;
  Rec rec(*this, JournalOp::InsertChart, [&](JournalRecord& w) {
    w.I(sheet).U(static_cast<uint8_t>(c.kind)).R(c.data).A(c.at).U(c.cols).U(c.rows).S(c.title);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(c.data) || c.at.row >= kMaxRows || c.at.col >= kMaxCols || !c.cols || !c.rows) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    pkg::Package* package = book_.GetPackage();
    if (!package || s.PartName().empty() || !package->FindPart(s.PartName()))
      return Result::Fail(u"Save the workbook as an Excel workbook first, then insert the chart.");
    std::string xml = charts::NewChartXml(s, c.data, c.kind, c.title);
    if (xml.empty()) return Result::Fail(u"Select the cells with the numbers to chart.");
    return Run(*this, u"Insert Chart", [&]() -> Result {
      const std::string drawing = SheetDrawing(sheet, true);
      if (drawing.empty()) return Result::Fail(u"The chart couldn't be added to this sheet.");
      const std::string chartPart = NewPartName("/xl/charts/chart", ".xml");
      const std::string rel = AddPart(drawing, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart", chartPart,
                                      "application/vnd.openxmlformats-officedocument.drawingml.chart+xml", std::move(xml));
      const CellAddr to{std::min(c.at.row + c.rows, kMaxRows - 1), std::min(c.at.col + c.cols, kMaxCols - 1)};
      if (rel.empty() || !AddAnchor(drawing, charts::ChartAnchor(c.at, to, rel, NextShapeId(drawing))))
        return Result::Fail(u"The chart couldn't be added to this sheet.");
      s_->Change(&s, kObjects, Range{c.at.row, c.at.col, c.at.row + c.rows, c.at.col + c.cols});
      return Result{};
    });
  }());
}

// ---- Cut and paste

namespace {

// References after cells `from` of sheet `fromSheet` were cut and pasted over `to` of sheet
// `toSheet` (the same size), as Excel rewrites them (measured): one wholly inside the cut cells
// follows them, absolute or not, from any sheet or name (to the other sheet: calc names it so);
// covered cells that weren't cut drop out of a reference as deleted cells do (lines at its edges;
// when some are still inside, it stays as it was), and one left with none is #REF!; the rest stay,
// whole rows and columns and 3-D references too. Formulas are compiled and printed for A1, where a
// reference's numbers are the cells it names.
class MoveMap : public calc::RefMap {
 public:
  MoveMap(int fromSheet, const Range& from, int toSheet, const Range& to)
      : src_(fromSheet), dst_(toSheet), from_(from), to_(to) {}
  mutable bool changed = false;

  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
           bool wholeCols) const override {
    int at = sheet;
    return MapTo(sheet, at, r1, c1, r2, c2, abs, wholeRows, wholeCols);
  }

  bool MapTo(int sheet, int& toSheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t, bool wholeRows,
             bool wholeCols) const override {
    if ((sheet != src_ && sheet != dst_) || wholeRows || wholeCols || r1 < 0 || c1 < 0 || r2 < r1 || c2 < c1) return true;
    auto range = [](int64_t a1, int64_t b1, int64_t a2, int64_t b2) {
      return Range{static_cast<uint32_t>(a1), static_cast<uint32_t>(b1), static_cast<uint32_t>(a2), static_cast<uint32_t>(b2)};
    };
    const bool same = src_ == dst_;
    int64_t t1 = r1, t2 = r2, l1 = c1, l2 = c2;
    if (sheet == dst_ && Overlaps(range(t1, l1, t2, l2), to_)) {
      // A line of it inside `to` with none of it cut drops off.
      auto over = [&](int64_t a1, int64_t b1, int64_t a2, int64_t b2) {
        const Range line = range(a1, b1, a2, b2);
        return Inside(line, to_) && !(same && Overlaps(line, from_));
      };
      while (t1 <= t2 && l1 <= l2) {
        if (over(t1, l1, t1, l2)) ++t1;
        else if (over(t2, l1, t2, l2)) --t2;
        else if (over(t1, l1, t2, l1)) ++l1;
        else if (over(t1, l2, t2, l2)) --l2;
        else break;
      }
      if (t1 > t2 || l1 > l2) {
        changed = true;
        return false;
      }
      const Range left = range(t1, l1, t2, l2);
      if (Overlaps(left, to_)) {
        const Range in{std::max(left.firstRow, to_.firstRow), std::max(left.firstCol, to_.firstCol),
                       std::min(left.lastRow, to_.lastRow), std::min(left.lastCol, to_.lastCol)};
        if (!same || !Inside(in, from_)) t1 = r1, t2 = r2, l1 = c1, l2 = c2;  // covered cells still inside
      }
    }
    if (sheet == src_ && Inside(range(t1, l1, t2, l2), from_)) {
      const int64_t dr = static_cast<int64_t>(to_.firstRow) - from_.firstRow;
      const int64_t dc = static_cast<int64_t>(to_.firstCol) - from_.firstCol;
      t1 += dr, t2 += dr, l1 += dc, l2 += dc;
      toSheet = dst_;
    }
    if (toSheet != sheet || t1 != r1 || t2 != r2 || l1 != c1 || l2 != c2) changed = true;
    r1 = static_cast<int32_t>(t1), r2 = static_cast<int32_t>(t2);
    c1 = static_cast<int32_t>(l1), c2 = static_cast<int32_t>(l2);
    return true;
  }

 private:
  int src_, dst_;
  Range from_, to_;
};

// Whether a shared formula's references, as they read in any cell of its group `group`, reach area
// `a` of sheet `sa` or `b` of sheet `sb`: then a move changes them differently from cell to cell,
// and the group can't stay shared. Printed for the group's master cell.
class Reaches : public calc::RefMap {
 public:
  Reaches(int sa, const Range& a, int sb, const Range& b, const Range& group) : sa_(sa), sb_(sb), a_(a), b_(b), g_(group) {}
  mutable bool hit = false;

  bool Map(int sheet, int32_t& r1, int32_t& c1, int32_t& r2, int32_t& c2, uint8_t abs, bool wholeRows,
           bool wholeCols) const override {
    if ((sheet != sa_ && sheet != sb_) || wholeRows || wholeCols) return true;
    // A relative part is an offset each cell of the group adds to its own row or column.
    auto lo = [](int32_t v, bool fixed, uint32_t first) { return fixed ? int64_t{v} : int64_t{first} + v; };
    const int64_t top = std::min(lo(r1, abs & kRow1, g_.firstRow), lo(r2, abs & kRow2, g_.firstRow));
    const int64_t bottom = std::max(lo(r1, abs & kRow1, g_.lastRow), lo(r2, abs & kRow2, g_.lastRow));
    const int64_t leftmost = std::min(lo(c1, abs & kCol1, g_.firstCol), lo(c2, abs & kCol2, g_.firstCol));
    const int64_t rightmost = std::max(lo(c1, abs & kCol1, g_.lastCol), lo(c2, abs & kCol2, g_.lastCol));
    if (bottom < 0 || rightmost < 0) return true;
    const Range swept{static_cast<uint32_t>(std::max<int64_t>(top, 0)), static_cast<uint32_t>(std::max<int64_t>(leftmost, 0)),
                      static_cast<uint32_t>(bottom), static_cast<uint32_t>(rightmost)};
    hit = hit || (sheet == sa_ && Overlaps(swept, a_)) || (sheet == sb_ && Overlaps(swept, b_));
    return true;
  }

 private:
  int sa_, sb_;
  Range a_, b_, g_;
};

// A cell as typed again: its value (a formula's: its result), its formula as it reads in the cell,
// its format.
CellInput InputOf(const CellInfo& cell, const FormulaInfo* f, int sheet, calc::Names& names) {
  CellInput in;
  in.type = cell.value.type;
  in.number = cell.value.number;
  in.boolean = cell.value.boolean;
  in.error = cell.value.error;
  in.text = std::string(cell.value.text);
  if (f && f->origin.row == cell.row && f->origin.col == cell.col) {
    in.formula = std::string(f->text);
  } else if (f) {
    calc::CompileOptions a, b;
    a.sheet = b.sheet = sheet;
    a.row = static_cast<int32_t>(f->origin.row);
    a.col = static_cast<int32_t>(f->origin.col);
    b.row = static_cast<int32_t>(cell.row);
    b.col = static_cast<int32_t>(cell.col);
    std::string out = calc::CopyFormula("=" + std::string(f->text), a, b, names);
    in.formula = !out.empty() && out[0] == '=' ? out.substr(1) : out;
  }
  in.style = cell.style;
  return in;
}

}  // namespace

Result Editor::MoveCells(int sheet, const Range& from, int toSheet, CellAddr to, std::u16string stepName) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Content, from); !no) return no;
  if (from.lastRow >= from.firstRow && from.lastCol >= from.firstCol)
    if (Result no = guard.Check(toSheet, ProtectAct::Content,
                                Range{to.row, to.col, to.row + (from.lastRow - from.firstRow), to.col + (from.lastCol - from.firstCol)});
        !no)
      return no;
  Rec rec(*this, JournalOp::MoveCells, [&](JournalRecord& w) { w.I(sheet).R(from).A(to).S16(stepName).I(toSheet); });
  return rec.Done([&]() -> Result {
    const int n = static_cast<int>(book_.SheetCount());
    if (sheet < 0 || sheet >= n || toSheet < 0 || toSheet >= n) return NoSheet();
    if (!Valid(from) || to.row >= kMaxRows || to.col >= kMaxCols) return BadRange();
    const Range dest{to.row, to.col, to.row + (from.lastRow - from.firstRow), to.col + (from.lastCol - from.firstCol)};
    if (!Valid(dest)) return BadRange();
    if (dest == from && toSheet == sheet) return {};
    for (size_t i = 0; i < book_.SheetCount(); ++i)
      if (!book_.LoadSheet(i).Ok()) return Result::Fail(u"A sheet of this workbook couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    Sheet& d = book_.GetSheet(static_cast<size_t>(toSheet));
    if (s.Kind() != SheetKind::Worksheet || d.Kind() != SheetKind::Worksheet) return Result::Fail(u"This sheet has no cells.");

    // ---- What Excel refuses, and what isn't done here yet: on each sheet, its area.
    std::vector<Range> spills;  // dynamic arrays moving: the cells they spill into aren't typed again
    auto check = [&](Sheet& x, const Range& area, bool cut) -> Result {
      auto part = [&](const Range& r) { return Overlaps(r, area) && !Inside(r, area); };
      for (const Range& g : x.Merges())
        if (part(g)) return Result::Fail(u"We can't do that to a merged cell.");
      for (const Range& a : State::Arrays(x))
        if (part(a)) return Result::Fail(u"You can't change part of an array.");
      for (const FCell& c : FormulaCellsOf(x)) {
        if (c.id >= A::Formulas(x).size()) continue;
        const FormulaRec& f = A::Formulas(x)[c.id];
        if (!f.dynamic || !f.hasRef) {
          if (f.kind == FormulaKind::DataTable && Overlaps(Range::Cell(c.row, c.col), area))
            return Result::Fail(u"Featherweight can't move a what-if data table yet.");
          continue;
        }
        if (part(f.ref)) return Result::Fail(u"You can't change part of an array.");
        if (cut && Inside(f.ref, area)) spills.push_back(f.ref);
      }
      for (const Table& t : x.Tables())
        if (Overlaps(t.ref, area)) return Result::Fail(u"Featherweight can't move cells into or out of a table yet.");
      if (pkg::Package* package = book_.GetPackage(); package && !x.PartName().empty())
        if (const pkg::RelationshipSet* rels = package->RelationshipsOf(x.PartName()))
          for (const pkg::Relationship& rel : rels->items) {
            std::vector<uint8_t> bytes;
            Range where;
            if (!rel.external && EndsWith(rel.type, "/pivotTable") && package->ReadPart(rel.targetPart, bytes).Ok() &&
                PivotLocation(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where) &&
                Overlaps(where, area))
              return Result::Fail(u"We can't make this change for the selected cells because it will affect a PivotTable.");
          }
      return Result{};
    };
    if (Result r = check(s, from, true); !r) return r;
    if (Result r = check(d, dest, false); !r) return r;

    return Run(*this, stepName.empty() ? std::u16string(u"Paste") : stepName, [&]() -> Result {
      BookNames names(book_);

      // ---- Shared formulas whose cells' references would change differently: each its own.
      for (size_t i = 0; i < book_.SheetCount(); ++i) {
        Sheet& x = book_.GetSheet(i);
        const int xi = static_cast<int>(i);
        const std::vector<FCell> fcells = FormulaCellsOf(x);
        const auto& recs = A::Formulas(x);
        std::unordered_set<uint32_t> split;
        for (const FCell& c : fcells) {
          if (c.id >= recs.size()) continue;
          const FormulaRec& f = recs[c.id];
          if (f.kind != FormulaKind::Shared || f.text == kNone || f.sharedIndex == kNone || !f.hasRef) continue;
          bool hit = (xi == sheet && Overlaps(f.ref, from)) || (xi == toSheet && Overlaps(f.ref, dest));
          if (!hit) {
            calc::CompileOptions at;
            at.sheet = xi;
            at.row = static_cast<int32_t>(c.row);
            at.col = static_cast<int32_t>(c.col);
            if (auto compiled = calc::Compile(A::Texts(x).Get(f.text), at, names)) {
              Reaches probe(sheet, from, toSheet, dest, f.ref);
              calc::PrintOptions how;
              how.map = &probe;
              (void)calc::Print(*compiled, at, names, how);
              hit = probe.hit;
            }
          }
          if (hit) split.insert(f.sharedIndex);
        }
        if (split.empty()) continue;
        std::vector<std::pair<CellAddr, CellInput>> own;
        for (const FCell& c : fcells) {
          if (c.id >= recs.size() || recs[c.id].kind != FormulaKind::Shared || !split.count(recs[c.id].sharedIndex)) continue;
          FormulaInfo f;
          CellInfo cell;
          if (x.GetFormula(c.row, c.col, f) && x.GetCell(c.row, c.col, cell))
            own.emplace_back(CellAddr{c.row, c.col}, InputOf(cell, &f, xi, names));
        }
        if (Result r = SetCells(xi, own); !r) return r;
      }

      // ---- The cut cells as they'll be typed where they go: their formulas read there.
      const MoveMap map(sheet, from, toSheet, dest);
      const uint32_t dr = dest.firstRow - from.firstRow, dc = dest.firstCol - from.firstCol;
      auto moved = [&](const Range& r) { return Range{r.firstRow + dr, r.firstCol + dc, r.lastRow + dr, r.lastCol + dc}; };
      std::vector<std::pair<CellAddr, CellInput>> cells;
      std::vector<std::pair<CellAddr, uint32_t>> styles;  // cells inside an array: their formats alone
      std::vector<Range> merges;
      for (const Range& g : s.Merges())
        if (Inside(g, from)) merges.push_back(moved(g));
      s.ForEachCell(from, [&](const CellInfo& cell) {
        const CellAddr there{cell.row + dr, cell.col + dc};
        FormulaInfo f;
        const bool formula = cell.hasFormula && s.GetFormula(cell.row, cell.col, f);
        const bool array = formula && f.kind == FormulaKind::Array && !f.dynamic;
        const bool spilled = !formula && std::any_of(spills.begin(), spills.end(), [&](const Range& sp) {
          return Inside(Range::Cell(cell.row, cell.col), sp);
        });
        if ((array && !f.master) || spilled) {
          styles.emplace_back(there, cell.style);
          return true;
        }
        CellInput in = InputOf(cell, formula ? &f : nullptr, sheet, names);
        if (!in.formula.empty()) {
          calc::CompileOptions o;
          o.sheet = sheet;
          if (auto compiled = calc::Compile(in.formula, o, names)) {
            map.changed = false;
            calc::CompileOptions print;
            print.sheet = toSheet;
            calc::PrintOptions how;
            how.map = &map;
            how.homeSheet = sheet;
            std::string out = calc::Print(*compiled, print, names, how);
            // On its own sheet, as written when nothing moved; on another, references to cells
            // left behind take their sheet's name.
            if (map.changed || toSheet != sheet) in.formula = std::move(out);
          }
        }
        if (array) {
          in.array = true;
          in.arrayRange = moved(f.ref);
        }
        cells.emplace_back(there, std::move(in));
        return true;
      });

      // ---- References everywhere: cells, names, tables, rules, charts, drawings, controls.
      RewriteSheetRefs(
          -1,
          [&](std::string_view text, int at) {
            calc::CompileOptions o;
            o.sheet = at;
            auto f = calc::Compile(text, o, names);
            if (!f) return std::string(text);
            map.changed = false;
            calc::PrintOptions how;
            how.map = &map;
            std::string out = calc::Print(*f, o, names, how);
            return map.changed ? out : std::string(text);  // as written when nothing moved
          },
          [](std::string_view text) { return text.find_first_of("0123456789") != std::string_view::npos; });

      // ---- Cleared where they were and where they go, then typed there.
      for (const auto& [x, r] : {std::pair<int, Range>{sheet, from}, std::pair<int, Range>{toSheet, dest}}) {
        if (Result e = Unmerge(x, r); !e) return e;
        if (Result e = ClearAll(x, r); !e) return e;
      }
      if (Result x = SetCells(toSheet, cells); !x) return x;
      for (const auto& [at, style] : styles)
        if (style)
          if (Result x = SetStyle(toSheet, Range::Cell(at.row, at.col), style); !x) return x;
      for (const Range& g : merges)
        if (Result x = Merge(toSheet, g); !x) return x;
      return Result{};
    });
  }());
}

// ---- Merged cells

Result Editor::Merge(int sheet, const Range& r) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::Merge, [&](JournalRecord& w) { w.I(sheet).R(r); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r) || (r.firstRow == r.lastRow && r.firstCol == r.lastCol)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    // The merges it overlaps go into it.
    Range m = r;
    for (bool grew = true; grew;) {
      grew = false;
      for (const Range& x : s.Merges())
        if (Overlaps(x, m) && !Inside(x, m)) {
          m = Range{std::min(m.firstRow, x.firstRow), std::min(m.firstCol, x.firstCol), std::max(m.lastRow, x.lastRow),
                    std::max(m.lastCol, x.lastCol)};
          grew = true;
        }
    }
    for (const Range& arr : State::Arrays(s))
      if (Overlaps(arr, m)) return Result::Fail(u"You can't change part of an array.");
    // Excel offers no merging in tables and pivot tables (a file with one is repaired).
    for (const Table& t : s.Tables())
      if (Overlaps(t.ref, m)) return Result::Fail(u"Cells in a table can't be merged.");
    if (pkg::Package* package = book_.GetPackage(); package && !s.PartName().empty())
      if (const pkg::RelationshipSet* rels = package->RelationshipsOf(s.PartName()))
        for (const pkg::Relationship& rel : rels->items) {
          std::vector<uint8_t> bytes;
          Range where;
          if (!rel.external && EndsWith(rel.type, "/pivotTable") && package->ReadPart(rel.targetPart, bytes).Ok() &&
              PivotLocation(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where) &&
              Overlaps(where, m))
            return Result::Fail(u"We can't make this change for the selected cells because it will affect a PivotTable.");
        }
    return Run(*this, u"Merge Cells", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      std::u16string why;
      for (const CellAddr& a : State::StoredIn(s, m))
        if (!(a.row == m.firstRow && a.col == m.firstCol) && !st.Put(sv, a.row, a.col, nullptr, true, std::nullopt, why))
          return Result::Fail(why);
      st.SaveMerges(sv);
      auto& merges = A::Merges(s);
      merges.erase(std::remove_if(merges.begin(), merges.end(), [&](const Range& x) { return Overlaps(x, m); }), merges.end());
      merges.push_back(m);
      st.RecountMerges(s);
      st.Change(&s, kMerges | kValues | kFormulas, m);
      return Result{};
    });
  }());
}

Result Editor::Unmerge(int sheet, const Range& r) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::Unmerge, [&](JournalRecord& w) { w.I(sheet).R(r); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (std::none_of(s.Merges().begin(), s.Merges().end(), [&](const Range& x) { return Overlaps(x, r); })) return {};
    return Run(*this, u"Unmerge Cells", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveMerges(sv);
      auto& merges = A::Merges(s);
      Range all = r;
      for (const Range& x : merges)
        if (Overlaps(x, r))
          all = Range{std::min(all.firstRow, x.firstRow), std::min(all.firstCol, x.firstCol),
                      std::max(all.lastRow, x.lastRow), std::max(all.lastCol, x.lastCol)};
      merges.erase(std::remove_if(merges.begin(), merges.end(), [&](const Range& x) { return Overlaps(x, r); }), merges.end());
      st.RecountMerges(s);
      st.Change(&s, kMerges, all);
      return Result{};
    });
  }());
}

// ---- Sheets

std::u16string Editor::CheckSheetName(std::string_view name, int except) const {
  if (name.empty()) return u"A sheet needs a name.";
  if (Utf16Length(name) > 31) return u"A sheet name can be at most 31 characters long.";
  if (name.find_first_of(":\\/?*[]") != std::string_view::npos || !XmlSafe(name) ||
      name.find_first_of("\t\r\n") != std::string_view::npos)
    return u"A sheet name can't contain : \\ / ? * [ or ].";
  if (name.front() == '\'' || name.back() == '\'') return u"A sheet name can't begin or end with an apostrophe.";
  if (NameEqualsNoCase(name, "History")) return u"\"History\" is a name Excel keeps for itself.";
  int other = book_.FindSheet(name);
  if (other >= 0 && other != except) return u"Another sheet already has that name.";
  return {};
}

Result Editor::AddSheet(int at, std::string name) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::AddSheet, [&](JournalRecord& w) { w.I(at).S(name); });
  return rec.Done([&]() -> Result {
    if (at < 0 || static_cast<size_t>(at) > book_.SheetCount()) at = static_cast<int>(book_.SheetCount());
    if (name.empty()) {
      for (size_t n = book_.SheetCount() + 1;; ++n) {
        name = "Sheet" + std::to_string(n);
        if (book_.FindSheet(name) < 0) break;
      }
    }
    if (std::u16string why = CheckSheetName(name); !why.empty()) return Result::Fail(why);
    return Run(*this, u"Insert Sheet", [&]() -> Result {
      State& st = *s_;
      st.SaveBook(true, true);
      uint32_t id = 0;
      for (size_t i = 0; i < book_.SheetCount(); ++i) id = std::max(id, A::SheetId(book_.GetSheet(i)));
      auto sheet = std::make_unique<Sheet>(book_, name);
      WorkbookLoader::SetSheetInfo(*sheet, SheetKind::Worksheet, SheetState::Visible, id + 1, {}, {});
      WorkbookLoader::MarkLoaded(*sheet, true);
      SheetView view;
      A::Views(*sheet).push_back(view);
      Sheet* added = sheet.get();
      auto& list = A::Sheets(book_);
      std::vector<int> remap(list.size());
      for (size_t i = 0; i < list.size(); ++i) remap[i] = static_cast<int>(i) + (static_cast<int>(i) >= at ? 1 : 0);
      list.insert(list.begin() + at, std::move(sheet));
      st.RemapNameSheets(remap);
      uint32_t& active = A::Active(book_);
      if (static_cast<int>(active) >= at) ++active;
      st.SaveViews(st.Save(*added));
      st.Change(added, kSheets, Range{});
      return Result{};
    });
  }());
}

Result Editor::RenameSheet(int sheet, std::string name) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::RenameSheet, [&](JournalRecord& w) { w.I(sheet).S(name); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (name == s.Name()) return {};
    if (std::u16string why = CheckSheetName(name, sheet); !why.empty()) return Result::Fail(why);
    if (!book_.LoadAll().Ok()) return Result::Fail(u"A sheet couldn't be read.");
    std::string old = s.Name();
    return Run(*this, u"Rename Sheet", [&]() -> Result {
      State& st = *s_;
      SheetSave& own = st.Save(s);
      own.dirty |= kPieceOther;
      if (!(own.saved & kSaveName)) {
        own.saved |= kSaveName;
        own.name = A::Name(s);
      }
      A::Name(s) = name;
      st.SaveBook(false, true);
      // Formulas, names, rules, sparklines, charts and the rest; links to places keep their text (Excel).
      RewriteSheetRefs(
          -1, [&](std::string_view t, int) { return pkg::RenameSheetInFormula(t, old, name); },
          [&](std::string_view t) { return Mentions(t, old); }, old, name);
      st.Change(&s, kSheets | kFormulas, Range::All());
      return Result{};
    });
  }());
}

Result Editor::MoveSheet(int sheet, int to) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::MoveSheet, [&](JournalRecord& w) { w.I(sheet).I(to); });
  return rec.Done([&]() -> Result {
    int n = static_cast<int>(book_.SheetCount());
    if (sheet < 0 || sheet >= n) return NoSheet();
    to = std::clamp(to, 0, n - 1);
    if (to == sheet) return {};
    return Run(*this, u"Move Sheet", [&]() -> Result {
      State& st = *s_;
      st.SaveBook(true, true);
      // 3-D references whose end sheets move (before the list changes: indexes as they are).
      std::vector<std::string> names;
      for (size_t i = 0; i < book_.SheetCount(); ++i) names.push_back(book_.GetSheet(i).Name());
      RewriteSheetRefs(
          -1,
          [&](std::string_view t, int at) {
            calc::CompileOptions o;
            o.sheet = at;
            return calc::SheetsChanged(t, o, names, sheet, to);
          },
          [](std::string_view t) { return t.find(':') != std::string_view::npos && t.find('!') != std::string_view::npos; });
      auto& list = A::Sheets(book_);
      std::vector<Sheet*> before;
      for (auto& p : list) before.push_back(p.get());
      auto moved = std::move(list[static_cast<size_t>(sheet)]);
      list.erase(list.begin() + sheet);
      list.insert(list.begin() + to, std::move(moved));
      std::vector<int> remap(before.size());
      for (size_t i = 0; i < before.size(); ++i) remap[i] = st.IndexOf(before[i]);
      st.RemapNameSheets(remap);
      uint32_t& active = A::Active(book_);
      if (active < remap.size()) active = static_cast<uint32_t>(remap[active]);
      A::FirstTab(book_) = 0;
      st.Change(&book_.GetSheet(static_cast<size_t>(to)), kSheets, Range{});
      return Result{};
    });
  }());
}

Result Editor::DeleteSheet(int sheet) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::DeleteSheet, [&](JournalRecord& w) { w.I(sheet); });
  return rec.Done([&]() -> Result {
    int n = static_cast<int>(book_.SheetCount());
    if (sheet < 0 || sheet >= n) return NoSheet();
    if (n == 1) return Result::Fail(u"A workbook needs at least one sheet.");
    int visible = 0;
    for (int i = 0; i < n; ++i) visible += book_.GetSheet(static_cast<size_t>(i)).State() == SheetState::Visible;
    if (visible == 1 && book_.GetSheet(static_cast<size_t>(sheet)).State() == SheetState::Visible)
      return Result::Fail(u"A workbook needs at least one visible sheet.");
    if (!book_.LoadAll().Ok()) return Result::Fail(u"A sheet couldn't be read.");
    return Run(*this, u"Delete Sheet", [&]() -> Result {
      State& st = *s_;
      st.SaveBook(true, true);
      // References to it become #REF! as Excel writes them (Sheet2!A1+1 -> #REF!+1; 3-D ranges give
      // way to the neighbour inside), and so do structured references to its tables; its charts and
      // other parts go with it when the workbook is saved.
      const Sheet& doomed = book_.GetSheet(static_cast<size_t>(sheet));
      std::vector<std::string> sheetNames, tables;
      for (size_t i = 0; i < book_.SheetCount(); ++i) sheetNames.push_back(book_.GetSheet(i).Name());
      for (const Table& t : doomed.Tables()) tables.push_back(t.displayName);
      const std::string goneName = doomed.Name();
      RewriteSheetRefs(
          sheet,
          [&](std::string_view t, int at) {
            calc::CompileOptions o;
            o.sheet = at;
            std::string r = StripRefTails(calc::SheetsChanged(t, o, sheetNames, sheet, -1));
            for (const std::string& table : tables) r = refs::DropTableColumns(r, table, {});
            return r;
          },
          [&](std::string_view t) {
            if (Mentions(t, goneName)) return true;
            for (const std::string& table : tables)
              if (Mentions(t, table)) return true;
            return false;
          });
      auto& list = A::Sheets(book_);
      std::unique_ptr<Sheet> gone = std::move(list[static_cast<size_t>(sheet)]);
      list.erase(list.begin() + sheet);
      st.Cur().book.held.push_back(std::move(gone));
      auto& names = A::Names(book_);
      names.erase(std::remove_if(names.begin(), names.end(), [&](const DefinedName& x) { return x.sheet == sheet; }), names.end());
      for (DefinedName& x : names)
        if (x.sheet > sheet) --x.sheet;
      uint32_t& active = A::Active(book_);
      if (static_cast<int>(active) > sheet || static_cast<size_t>(active) >= list.size())
        active = active > 0 ? active - 1 : 0;
      while (active < list.size() && list[active]->State() != SheetState::Visible) ++active;
      if (active >= list.size()) active = 0;
      A::FirstTab(book_) = 0;
      st.Change(nullptr, kSheets, Range{});
      return Result{};
    });
  }());
}

namespace {

// "$A$1:$C$9"
std::string AbsoluteName(const Range& r) {
  auto cell = [](uint32_t row, uint32_t col) {
    std::string name = CellName(row, col);
    const size_t digits = name.find_first_of("0123456789");
    return "$" + name.substr(0, digits) + "$" + name.substr(digits);
  };
  return cell(r.firstRow, r.firstCol) + ":" + cell(r.lastRow, r.lastCol);
}

// A filter's autoFilter element, with the sheet's prefix `p`.
std::string FilterXml(const SheetFilter& f, const std::string& p) {
  std::string out;
  auto open = [&](const char* name) {
    out += '<';
    out += p;
    out += name;
  };
  auto close = [&](const char* name) {
    out += "</";
    out += p;
    out += name;
    out += '>';
  };
  auto attr = [&](const char* name, std::string_view value) {
    out += ' ';
    out += name;
    out += "=\"";
    pkg::AppendEscapedAttribute(out, value);
    out += '"';
  };
  auto num = [&](const char* name, double v) {
    char buf[48];
    attr(name, std::string_view(buf, xlsx::FormatNumber(v, 0, buf)));
  };
  open("autoFilter");
  attr("ref", RangeName(f.ref));
  if (f.columns.empty()) {
    out += "/>";
    return out;
  }
  out += '>';
  for (const FilterColumn& c : f.columns) {
    if (c.kind == FilterColumn::Kind::Other) {
      out += c.xml;
      continue;
    }
    open("filterColumn");
    attr("colId", std::to_string(c.colId));
    out += '>';
    switch (c.kind) {
      case FilterColumn::Kind::Values:
        open("filters");
        if (c.blank) attr("blank", "1");
        if (c.values.empty()) {
          out += "/>";
          break;
        }
        out += '>';
        for (const std::string& v : c.values) {
          open("filter");
          attr("val", v);
          out += "/>";
        }
        close("filters");
        break;
      case FilterColumn::Kind::Custom:
        open("customFilters");
        if (c.both) attr("and", "1");
        out += '>';
        for (const FilterColumn::Condition& k : c.conditions) {
          open("customFilter");
          if (k.op != "equal") attr("operator", k.op);
          attr("val", k.val);
          out += "/>";
        }
        close("customFilters");
        break;
      case FilterColumn::Kind::Top10:
        open("top10");
        if (c.bottom) attr("top", "0");
        if (c.percent) attr("percent", "1");
        num("val", c.count);
        num("filterVal", c.threshold);
        out += "/>";
        break;
      default:
        open("dynamicFilter");
        attr("type", c.kind == FilterColumn::Kind::AboveAverage ? "aboveAverage" : "belowAverage");
        num("val", c.threshold);
        out += "/>";
        break;
    }
    close("filterColumn");
  }
  close("autoFilter");
  return out;
}

}  // namespace

Result Editor::SetFilter(int sheet, const std::optional<SheetFilter>& filter) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount() && book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) {
    const bool both = filter && book_.GetSheet(static_cast<size_t>(sheet)).Filter();
    if (Result no = guard.Check(sheet, both ? ProtectAct::Filter : ProtectAct::Never); !no) return no;
  }
  Rec rec(*this, JournalOp::SetFilter, [&](JournalRecord& w) { w.I(sheet).Filter(filter); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (filter && !Valid(filter->ref)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (filter)
      for (const Table& t : s.Tables())
        if (Overlaps(t.ref, filter->ref)) return Result::Fail(u"A table has its own filter.");
    return Run(*this, filter ? u"Filter" : u"Remove Filter", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveFilter(sv);
      std::optional<SheetFilter> now = filter;
      if (now) now->filtered = !now->columns.empty();
      A::Filter(s) = now;
      // The element in the sheet's part, which saving writes as it is.
      st.SaveText(sv);
      detail::PartText& t = A::Text(s);
      xlsx::PutSheetElement(t, "autoFilter", now ? FilterXml(*now, xlsx::SheetPrefix(t)) : std::string());
      const bool mode = now && now->filtered;
      if (s.Props().filterMode != mode) {
        st.SaveProps(sv);
        A::Props(s).filterMode = mode;
      }
      // Excel's hidden name for the range, local to the sheet.
      std::vector<DefinedName> names = book_.Names();
      auto it = std::find_if(names.begin(), names.end(), [&](const DefinedName& n) {
        return n.sheet == sheet && NameEqualsNoCase(n.name, "_xlnm._FilterDatabase");
      });
      if (now) {
        DefinedName n{"_xlnm._FilterDatabase", sheet, pkg::QuoteSheetName(s.Name()) + "!" + AbsoluteName(now->ref), true};
        if (it != names.end()) *it = n;
        else names.push_back(n);
      } else if (it != names.end()) {
        names.erase(it);
      }
      if (names.size() != book_.Names().size() || !std::equal(names.begin(), names.end(), book_.Names().begin(),
                                                               [](const DefinedName& a, const DefinedName& b) {
                                                                 return a.name == b.name && a.sheet == b.sheet &&
                                                                        a.formula == b.formula && a.hidden == b.hidden;
                                                               })) {
        st.SaveBook(false, true);
        A::Names(book_) = std::move(names);
        st.Change(nullptr, kNames, Range{});
      }
      st.Change(&s, kSheetProps, now ? now->ref : Range{});
      return Result{};
    });
  }());
}

Result Editor::SetSheetRules(int sheet, RuleKind kind, const std::string& xml, const std::string& ext,
                             std::u16string stepName) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount())
    if (Result no = guard.Check(sheet, kind == RuleKind::Validations ? ProtectAct::Never : ProtectAct::FormatCells); !no) return no;
  Rec rec(*this, JournalOp::SetSheetRules,
          [&](JournalRecord& w) { w.I(sheet).U(static_cast<uint8_t>(kind)).S(xml).S(ext).S16(stepName); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet || s.PartName().empty())
      return Result::Fail(u"Only a worksheet of an Excel workbook keeps these rules.");
    const bool validations = kind == RuleKind::Validations;
    std::string oldXml, oldExt;
    xlsx::GetSheetRules(A::Text(s), validations, oldXml, oldExt);
    if (oldXml == xml && oldExt == ext) return Result{};
    return Run(*this, std::move(stepName), [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveText(sv);
      xlsx::PutSheetRules(A::Text(s), validations, xml, ext);
      st.Change(&s, kRules, Range{});
      return Result{};
    });
  }());
}

namespace {
constexpr std::string_view kTableRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/table";
constexpr std::string_view kTableType = "application/vnd.openxmlformats-officedocument.spreadsheetml.table+xml";

// A table on the sheet that `ref` overlaps, other than `except`, as Excel's message names it.
Result TableOverlap(const Sheet& s, const Range& ref, size_t except) {
  for (size_t i = 0; i < s.Tables().size(); ++i)
    if (i != except && Overlaps(s.Tables()[i].ref, ref)) return Result::Fail(u"A table can't overlap another table.");
  for (const Range& m : s.Merges())
    if (Overlaps(m, ref)) return Result::Fail(u"A table can't overlap a range that contains merged cells.");
  if (s.Filter() && Overlaps(s.Filter()->ref, ref)) return Result::Fail(u"A table can't overlap the sheet's filter.");
  return {};
}

Result TableShape(const Table& t) {
  if (!Valid(t.ref)) return BadRange();
  if (t.ref.lastRow < t.ref.firstRow + t.headerRows + t.totalsRows) return Result::Fail(u"A table needs a row of data.");
  if (t.columns.size() != t.ref.lastCol - t.ref.firstCol + 1) return Result::Fail(u"A table needs a name for each column.");
  return {};
}
}  // namespace

Result Editor::AddTable(int sheet, Table t) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount())
    if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::AddTable, [&](JournalRecord& w) { w.I(sheet).S(xlsx::TableXml(t)).S(t.part); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet || s.PartName().empty() || !book_.GetPackage())
      return Result::Fail(u"Tables need an Excel workbook: save this one as .xlsx first.");
    if (Result r = TableShape(t); !r) return r;
    if (Result r = TableOverlap(s, t.ref, SIZE_MAX); !r) return r;
    return Run(*this, u"Create Table", [&]() -> Result {
      State& st = *s_;
      // Ids and names unique in the workbook: Excel's "Table1", "Table2"...
      uint32_t maxId = 0;
      bool idTaken = false;
      std::vector<std::string> names;
      for (size_t i = 0; i < book_.SheetCount(); ++i)
        for (const Table& x : book_.GetSheet(i).Tables()) {
          maxId = std::max(maxId, x.id);
          idTaken |= x.id == t.id;
          names.push_back(x.name);
        }
      for (const DefinedName& n : book_.Names()) names.push_back(n.name);
      auto taken = [&](const std::string& name) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& x) { return NameEqualsNoCase(x, name); });
      };
      if (t.id == 0 || idTaken) t.id = maxId + 1;
      if (t.name.empty() || taken(t.name))
        for (uint32_t n = 1;; ++n)
          if (!taken(t.name = "Table" + std::to_string(n))) break;
      t.displayName = t.name;
      for (size_t i = 0; i < t.columns.size(); ++i)
        if (t.columns[i].id == 0) t.columns[i].id = static_cast<uint32_t>(i + 1);
      t.part = NewPartName("/xl/tables/table", ".xml");
      const std::string id = AddPart(s.PartName(), kTableRel, t.part, kTableType, xlsx::TableXml(t));
      if (id.empty()) return Result::Fail(u"The table couldn't be added to the workbook.");
      SheetSave& sv = st.Save(s);
      st.SaveText(sv);
      xlsx::PutTablePart(A::Text(s), id, true);
      st.SaveTables(sv);
      A::Tables(s).push_back(t);
      st.Change(&s, kStyles | kRules | kTables, t.ref);
      return Result{};
    });
  }());
}

Result Editor::SetTable(int sheet, size_t index, const Table& t) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount())
    if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::SetTable, [&](JournalRecord& w) { w.I(sheet).U(index).S(xlsx::TableXml(t)).S(t.part); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (index >= s.Tables().size()) return Result::Fail(u"There's no such table.");
    if (Result r = TableShape(t); !r) return r;
    if (Result r = TableOverlap(s, t.ref, index); !r) return r;
    const Table before = s.Tables()[index];
    if (!t.name.empty() && !NameEqualsNoCase(t.name, before.name)) {
      for (size_t i = 0; i < book_.SheetCount(); ++i)
        for (const Table& x : book_.GetSheet(i).Tables())
          if (NameEqualsNoCase(x.name, t.name)) return Result::Fail(u"The name entered already exists. Enter a unique name.");
      for (const DefinedName& n : book_.Names())
        if (NameEqualsNoCase(n.name, t.name)) return Result::Fail(u"The name entered already exists. Enter a unique name.");
    }
    return Run(*this, u"Table", [&]() -> Result {
      State& st = *s_;
      Table now = t;
      now.part = before.part;
      now.id = before.id;
      if (now.name.empty()) now.name = before.name;
      now.displayName = now.name;
      uint32_t maxId = 0;
      for (const TableColumn& c : before.columns) maxId = std::max(maxId, c.id);
      for (size_t i = 0; i < now.columns.size(); ++i)
        if (now.columns[i].id == 0) now.columns[i].id = ++maxId;
      std::string xml = PartBytes(before.part);
      if (xml.empty() || !xlsx::UpdateTableXml(xml, now)) return Result::Fail(u"The table's part couldn't be written.");
      SheetFilter kept;
      now.criteria.clear();
      if (now.autoFilter && xlsx::ReadTableFilter(xml, kept)) now.criteria = std::move(kept.columns);
      now.filtered = !now.criteria.empty();
      if (!WritePart(before.part, std::move(xml))) return Result::Fail(u"The table's part couldn't be written.");
      SheetSave& sv = st.Save(s);
      st.SaveTables(sv);
      A::Tables(s)[index] = now;
      const Range all{std::min(before.ref.firstRow, now.ref.firstRow), std::min(before.ref.firstCol, now.ref.firstCol),
                      std::max(before.ref.lastRow, now.ref.lastRow), std::max(before.ref.lastCol, now.ref.lastCol)};
      st.Change(&s, kStyles | kRules | kTables, all);
      return Result{};
    });
  }());
}

Result Editor::RemoveTable(int sheet, size_t index) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount())
    if (Result no = guard.Check(sheet, ProtectAct::Never); !no) return no;
  Rec rec(*this, JournalOp::RemoveTable, [&](JournalRecord& w) { w.I(sheet).U(index); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (index >= s.Tables().size() || !book_.GetPackage()) return Result::Fail(u"There's no such table.");
    return Run(*this, u"Convert to Range", [&]() -> Result {
      State& st = *s_;
      const Table t = s.Tables()[index];
      std::string relId;
      if (const pkg::RelationshipSet* set = book_.GetPackage()->RelationshipsOf(s.PartName()))
        for (const pkg::Relationship& r : set->items)
          if (!r.external && NameEqualsNoCase(r.targetPart, t.part)) relId = r.id;
      if (!RemovePart(t.part)) return Result::Fail(u"The table's part couldn't be removed.");
      SheetSave& sv = st.Save(s);
      st.SaveText(sv);
      if (!relId.empty()) xlsx::PutTablePart(A::Text(s), relId, false);
      st.SaveTables(sv);
      A::Tables(s).erase(A::Tables(s).begin() + static_cast<ptrdiff_t>(index));
      st.Change(&s, kStyles | kRules | kTables, t.ref);
      return Result{};
    });
  }());
}

Result Editor::SetTableFilter(int sheet, size_t index, const SheetFilter& f) {
  Guard guard(*this);
  if (sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount())
    if (Result no = guard.Check(sheet, ProtectAct::Filter); !no) return no;
  Rec rec(*this, JournalOp::SetTableFilter, [&](JournalRecord& w) { w.I(sheet).U(index).Filter(f); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (index >= s.Tables().size()) return Result::Fail(u"There's no such table.");
    const Table& t = s.Tables()[index];
    if (!t.autoFilter || !t.headerRows) return Result::Fail(u"The table has no filter buttons.");
    SheetFilter now = f;
    now.ref = t.ref;
    if (t.totalsRows && now.ref.lastRow > now.ref.firstRow) --now.ref.lastRow;
    const uint32_t width = t.ref.lastCol - t.ref.firstCol + 1;
    std::erase_if(now.columns, [&](const FilterColumn& c) { return c.colId >= width; });
    now.filtered = !now.columns.empty();
    return Run(*this, u"Filter", [&]() -> Result {
      State& st = *s_;
      std::string xml = PartBytes(t.part);
      if (xml.empty() || !xlsx::PutTableFilter(xml, FilterXml(now, "")) || !WritePart(t.part, std::move(xml)))
        return Result::Fail(u"The table's part couldn't be written.");
      SheetSave& sv = st.Save(s);
      st.SaveTables(sv);
      A::Tables(s)[index].filtered = now.filtered;
      A::Tables(s)[index].criteria = now.columns;
      st.Change(&s, kSheetProps, now.ref);
      return Result{};
    });
  }());
}

namespace {
constexpr std::string_view kImageRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";

void PutPlace(JournalRecord& w, const drawing::Place& p) {
  for (const drawing::Marker* m : {&p.from, &p.to}) w.U(m->col).U(m->row).I(m->colOff).I(m->rowOff);
  w.I(p.x).I(p.y).I(p.cx).I(p.cy);
}

bool PlaceOk(const drawing::Place& p) {
  return p.from.col < kMaxCols && p.to.col < kMaxCols && p.from.row < kMaxRows && p.to.row < kMaxRows &&
         p.to.col >= p.from.col && p.to.row >= p.from.row && p.cx >= 0 && p.cy >= 0;
}
}  // namespace

Result Editor::InsertPicture(int sheet, const std::string& bytes, const drawing::Place& p, const std::string& altText,
                             uint32_t* id) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::InsertPicture, [&](JournalRecord& w) {
    w.I(sheet).S(bytes);
    PutPlace(w, p);
    w.S(altText);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!PlaceOk(p)) return BadRange();
    std::string extension, type;
    if (!data::PictureType(bytes, extension, type)) return Result::Fail(u"That isn't a picture Featherweight adds: PNG, JPEG, GIF or BMP.");
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    pkg::Package* package = book_.GetPackage();
    if (!package || s.PartName().empty() || !package->FindPart(s.PartName()))
      return Result::Fail(u"Save the workbook as an Excel workbook first, then insert the picture.");
    return Run(*this, u"Insert Picture", [&]() -> Result {
      const std::string d = SheetDrawing(sheet, true);
      if (d.empty()) return Result::Fail(u"The picture couldn't be added to this sheet.");
      // Stored once: a media part with the same bytes, and the drawing's own relationship to it.
      std::string media;
      for (const pkg::Part& part : package->Parts()) {
        if (part.deleted || part.name.size() < 10 || !NameEqualsNoCase(std::string_view(part.name).substr(0, 10), "/xl/media/")) continue;
        const uint64_t size = part.modified || part.entry == pkg::Part::kNoEntry
                                  ? part.data.size()
                                  : (part.entry < package->Zip().Entries().size() ? package->Zip().Entries()[part.entry].uncompressedSize : 0);
        if (size == bytes.size() && PartBytes(part.name) == bytes) {
          media = part.name;
          break;
        }
      }
      std::string rel;
      if (!media.empty()) {
        if (const pkg::RelationshipSet* set = package->RelationshipsOf(d))
          for (const pkg::Relationship& r : set->items)
            if (!r.external && r.type == kImageRel && NameEqualsNoCase(r.targetPart, media)) rel = r.id;
        if (rel.empty()) rel = AddRelationship(d, kImageRel, media);
      } else {
        rel = AddPart(d, kImageRel, NewPartName("/xl/media/image", extension), type, bytes);
      }
      const uint32_t n = NextShapeId(d);
      const std::string anchor = rel.empty() ? std::string() : drawing::Placed(data::PictureAnchor(n, rel, altText), p);
      if (anchor.empty() || !AddAnchor(d, anchor)) return Result::Fail(u"The picture couldn't be added to this sheet.");
      if (id) *id = n;
      s_->Change(&s, kObjects, Range{p.from.row, p.from.col, p.to.row, p.to.col});
      return Result{};
    });
  }());
}

Result Editor::InsertShape(int sheet, data::ShapeKind kind, const drawing::Place& p, const std::string& text, uint32_t* id) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::InsertShape, [&](JournalRecord& w) {
    w.I(sheet).U(static_cast<uint8_t>(kind)).S(text);
    PutPlace(w, p);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!PlaceOk(p)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    pkg::Package* package = book_.GetPackage();
    if (!package || s.PartName().empty() || !package->FindPart(s.PartName()))
      return Result::Fail(u"Save the workbook as an Excel workbook first, then insert the shape.");
    return Run(*this, u"Insert Shape", [&]() -> Result {
      const std::string d = SheetDrawing(sheet, true);
      if (d.empty()) return Result::Fail(u"The shape couldn't be added to this sheet.");
      const uint32_t n = NextShapeId(d);
      const std::string anchor = drawing::Placed(data::ShapeAnchor(kind, n, text), p);
      if (anchor.empty() || !AddAnchor(d, anchor)) return Result::Fail(u"The shape couldn't be added to this sheet.");
      if (id) *id = n;
      s_->Change(&s, kObjects, Range{p.from.row, p.from.col, p.to.row, p.to.col});
      return Result{};
    });
  }());
}

Result Editor::SetObjectXml(int sheet, uint32_t id, const std::string& xml, std::u16string stepName) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::SetObjectXml, [&](JournalRecord& w) { w.I(sheet).U(id).S(xml).S16(stepName); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    const std::string d = SheetDrawing(sheet, false);
    if (d.empty() || AnchorXml(d, id).empty()) return Result::Fail(u"The object isn't on this sheet any more.");
    // Never damage the drawing: the new bytes read, as one object with the same id.
    const std::vector<drawing::Object> objects = drawing::Objects("<r>" + xml + "</r>");
    pugi::xml_document parsed;
    if (objects.size() != 1 || objects[0].id != id || !parsed.load_buffer(xml.data(), xml.size()))
      return Result::Fail(u"The object's new format couldn't be written.");
    return Run(*this, stepName, [&]() -> Result {
      if (!ReplaceAnchor(d, id, xml)) return Result::Fail(u"The object's new format couldn't be written.");
      s_->Change(&book_.GetSheet(static_cast<size_t>(sheet)), kObjects, Range{0, 0, kMaxRows - 1, kMaxCols - 1});
      return Result{};
    });
  }());
}

Result Editor::SetTabColor(int sheet, const Color& color) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::SetTabColor, [&](JournalRecord& w) { w.I(sheet).C(color); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, u"Tab Color", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveProps(sv);
      A::Props(s).tabColor = color;
      st.Change(&s, kSheetProps, Range{});
      return Result{};
    });
  }());
}

Result Editor::SetSheetState(int sheet, SheetState state) {
  Guard guard(*this);
  if (Result no = guard.Check(-1, ProtectAct::Structure); !no) return no;
  Rec rec(*this, JournalOp::SetSheetState, [&](JournalRecord& w) { w.I(sheet).U(static_cast<uint8_t>(state)); });
  return rec.Done([&]() -> Result {
    int n = static_cast<int>(book_.SheetCount());
    if (sheet < 0 || sheet >= n) return NoSheet();
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.State() == state) return {};
    if (state != SheetState::Visible) {
      int visible = 0;
      for (int i = 0; i < n; ++i) visible += book_.GetSheet(static_cast<size_t>(i)).State() == SheetState::Visible;
      if (visible <= 1 && s.State() == SheetState::Visible)
        return Result::Fail(u"A workbook needs at least one visible sheet.");
    }
    return Run(*this, state == SheetState::Visible ? u"Unhide Sheet" : u"Hide Sheet", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveBook(false, false);
      if (!(sv.saved & kSaveState)) {
        sv.saved |= kSaveState;
        sv.state = A::State(s);
      }
      A::State(s) = state;
      // The active tab moves off a sheet that's hidden.
      uint32_t& active = A::Active(book_);
      if (state != SheetState::Visible && static_cast<int>(active) == sheet) {
        for (int i = 1; i < n; ++i) {
          int j = (sheet + i) % n;
          if (book_.GetSheet(static_cast<size_t>(j)).State() == SheetState::Visible) {
            active = static_cast<uint32_t>(j);
            break;
          }
        }
      }
      st.Change(&s, kSheetProps | kSheets, Range{});
      return Result{};
    });
  }());
}

Result Editor::FreezePanes(int sheet, uint32_t rows, uint32_t cols) {
  Rec rec(*this, JournalOp::FreezePanes, [&](JournalRecord& w) { w.I(sheet).U(rows).U(cols); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    return Run(*this, rows || cols ? u"Freeze Panes" : u"Unfreeze Panes", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveViews(sv);
      auto& views = A::Views(s);
      if (views.empty()) views.emplace_back();
      SheetView& v = views[0];
      if (v.topLeft.row + rows >= kMaxRows || v.topLeft.col + cols >= kMaxCols) return BadRange();
      if (rows == 0 && cols == 0) {
        v.paneState = PaneState::None;
        v.xSplit = v.ySplit = 0;
        v.activePane = PaneId::TopLeft;
      } else {
        v.paneState = PaneState::Frozen;
        v.xSplit = cols;
        v.ySplit = rows;
        v.paneTopLeft = {v.topLeft.row + rows, v.topLeft.col + cols};
        v.activePane = rows && cols ? PaneId::BottomRight : rows ? PaneId::BottomLeft : PaneId::TopRight;
      }
      for (Selection& sel : v.selections) sel.pane = v.activePane;
      st.Change(&s, kSheetProps, Range{});
      return Result{};
    });
  }());
}

// ---- Protection

Result Editor::SetSheetProtection(int sheet, const std::optional<SheetProtection>& protection) {
  const std::string xml = protection ? SheetProtectionXml(*protection) : std::string();
  Rec rec(*this, JournalOp::SetSheetProtection, [&](JournalRecord& w) { w.I(sheet).S(xml); });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet) return Result::Fail(u"Only worksheets can be protected here.");
    const bool on = protection && protection->sheet;
    return Run(*this, on ? u"Protect Sheet" : u"Unprotect Sheet", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveText(sv);
      detail::PartText& t = A::Text(s);
      xlsx::PutSheetElement(t, "sheetProtection", protection ? SheetProtectionXml(*protection, xlsx::SheetPrefix(t)) : std::string());
      st.Change(&s, kSheetProps, Range{});
      return Result{};
    });
  }());
}

Result Editor::SetWorkbookProtection(const std::optional<WorkbookProtection>& protection) {
  const std::string xml = protection ? WorkbookProtectionXml(*protection) : std::string();
  Rec rec(*this, JournalOp::SetWorkbookProtection, [&](JournalRecord& w) { w.S(xml); });
  return rec.Done([&]() -> Result {
    if (book_.ProtectionXml() == xml) return Result{};
    return Run(*this, protection && protection->lockStructure ? u"Protect Workbook" : u"Unprotect Workbook",
               [&]() -> Result {
                 State& st = *s_;
                 st.SaveBook(false, false);
                 Step& step = st.Cur();
                 if (!step.book.protection) {
                   step.book.protection = true;
                   step.book.protectionXml = A::ProtectionXml(book_);
                 }
                 A::ProtectionXml(book_) = xml;
                 st.Change(nullptr, kSheetProps, Range{});
                 return Result{};
               });
  }());
}

// ---- Links

Result Editor::SetHyperlink(int sheet, const Range& r, const std::optional<Hyperlink>& link) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::InsertLink, r); !no) return no;
  Rec rec(*this, JournalOp::SetHyperlink, [&](JournalRecord& w) {
    w.I(sheet).R(r).B(link.has_value());
    if (link) w.S(link->target).S(link->location).S(link->display).S(link->tooltip).S(link->relId);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!Valid(r)) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (link) {
      if (link->target.empty() && link->location.empty()) return Result::Fail(u"Type an address, or choose a place in this workbook.");
      for (const std::string* text : {&link->target, &link->location, &link->display, &link->tooltip})
        if (!XmlSafe(*text)) return Result::Fail(u"The link holds characters a workbook can't store.");
      if (Utf16Length(link->target) > 2079) return Result::Fail(u"An address can be at most 2,079 characters long.");
      if (Utf16Length(link->tooltip) > 255) return Result::Fail(u"A ScreenTip can be at most 255 characters long.");
    }
    return Run(*this, link ? u"Insert Hyperlink" : u"Remove Hyperlinks", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveLinks(sv);
      auto& links = A::Links(s);
      links.erase(std::remove_if(links.begin(), links.end(), [&](const Hyperlink& h) { return Overlaps(h.ref, r); }),
                  links.end());
      if (link) {
        Hyperlink h = *link;
        h.ref = r;
        if (h.target.empty()) h.relId.clear();
        links.push_back(std::move(h));
      }
      st.Change(&s, kObjects, r);
      return Result{};
    });
  }());
}

// ---- Notes

namespace {

// The parts a sheet's notes and threads are in: its comments, its VML drawing (the
// legacyDrawing's) and its threaded comments.
struct NoteParts {
  std::string comments, vml, threads;
};

NoteParts NotePartsOf(const pkg::Package& package, const Sheet& s) {
  NoteParts p;
  if (const pkg::RelationshipSet* set = package.RelationshipsOf(s.PartName()))
    for (const pkg::Relationship& r : set->items) {
      if (r.external) continue;
      if (EndsWith(r.type, "/comments") && p.comments.empty()) p.comments = r.targetPart;
      if (EndsWith(r.type, "/vmlDrawing") && (p.vml.empty() || r.id == s.LegacyDrawingRelId())) p.vml = r.targetPart;
      if (EndsWith(r.type, "/threadedComment") && p.threads.empty()) p.threads = r.targetPart;
    }
  return p;
}

std::string PersonsPartOf(const pkg::Package& package, const std::string& workbookPart) {
  if (const pkg::RelationshipSet* set = package.RelationshipsOf(workbookPart))
    for (const pkg::Relationship& r : set->items)
      if (!r.external && EndsWith(r.type, "/person")) return r.targetPart;
  return {};
}

// Now in UTC as threaded comments keep their times: "2026-11-18T09:25:49.80".
std::string ThreadTime() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto day = floor<days>(now);
  const year_month_day ymd{day};
  const hh_mm_ss t{floor<milliseconds>(now - day)};
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%02d", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<int>(t.hours().count()),
                static_cast<int>(t.minutes().count()), static_cast<int>(t.seconds().count()),
                static_cast<int>(t.subseconds().count() / 10));
  return buf;
}

}  // namespace

std::optional<Editor::Thread> Editor::ThreadAt(int sheet, CellAddr cell) const {
  const pkg::Package* package = book_.GetPackage();
  if (!package || sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return std::nullopt;
  const Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  if (s.PartName().empty()) return std::nullopt;
  const NoteParts parts = NotePartsOf(*package, s);
  if (parts.threads.empty()) return std::nullopt;
  const std::string persons = PersonsPartOf(*package, book_.WorkbookPart());
  std::optional<notes::ThreadText> t =
      notes::ReadThread(PartBytes(parts.threads), persons.empty() ? std::string() : PartBytes(persons), cell);
  if (!t) return std::nullopt;
  Thread out;
  out.resolved = t->resolved;
  for (notes::ThreadEntry& e : t->comments) out.comments.push_back({std::move(e.author), std::move(e.text), std::move(e.id), std::move(e.date)});
  return out;
}

Result Editor::SetThread(int sheet, CellAddr cell, const std::optional<Thread>& in) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  // New comments get their id and time here, so the journal replays the same file.
  std::optional<Thread> thread = in;
  if (thread)
    for (ThreadComment& c : thread->comments) {
      if (c.id.empty()) c.id = pkg::crypto::NewGuid();
      if (c.date.empty()) c.date = ThreadTime();
    }
  Rec rec(*this, JournalOp::SetThread, [&](JournalRecord& w) {
    w.I(sheet).A(cell).B(thread.has_value());
    if (thread) {
      w.U(thread->comments.size());
      for (const ThreadComment& c : thread->comments) w.S(c.author).S(c.text).S(c.id).S(c.date);
      w.B(thread->resolved);
    }
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (cell.row >= kMaxRows || cell.col >= kMaxCols) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    pkg::Package* package = book_.GetPackage();
    if (!package || s.PartName().empty() || s.Kind() != SheetKind::Worksheet)
      return Result::Fail(u"Comments can be added to this sheet once the workbook is saved.");
    if (thread) {
      if (thread->comments.empty()) return Result::Fail(u"A comment thread needs a comment.");
      for (const ThreadComment& c : thread->comments) {
        if (c.text.empty() || c.id.empty()) return Result::Fail(u"Type a comment first.");
        for (const std::string* t : {&c.author, &c.text, &c.id, &c.date})
          if (!XmlSafe(*t)) return Result::Fail(u"The comment holds characters a workbook can't store.");
        if (Utf16Length(c.text) > 32767) return Result::Fail(u"A comment can hold at most 32,767 characters.");
      }
    }
    const NoteParts parts = NotePartsOf(*package, s);
    const std::string personsPart = PersonsPartOf(*package, book_.WorkbookPart());
    notes::ThreadParts old, now;
    old.threads = parts.threads.empty() ? std::string() : PartBytes(parts.threads);
    old.persons = personsPart.empty() ? std::string() : PartBytes(personsPart);
    old.comments = parts.comments.empty() ? std::string() : PartBytes(parts.comments);
    old.vml = parts.vml.empty() ? std::string() : PartBytes(parts.vml);
    const bool had = notes::ReadThread(old.threads, {}, cell).has_value();
    if (!had && !thread) return Result{};
    if (!had && notes::HasEntry(old.comments, cell))
      return Result::Fail(u"This cell has a note. Edit or delete the note first.");
    std::optional<notes::ThreadText> text;
    if (thread) {
      text.emplace();
      text->resolved = thread->resolved;
      for (const ThreadComment& c : thread->comments) text->comments.push_back({c.author, c.text, c.id, c.date});
    }
    if (!notes::EditThread(old, cell, text, now)) return Result::Fail(u"This sheet's comments couldn't be read.");
    if (now.threads == old.threads && now.persons == old.persons && now.comments == old.comments && now.vml == old.vml)
      return Result{};
    return Run(*this, !thread ? u"Delete Comment" : had ? u"Edit Comment" : u"New Comment", [&]() -> Result {
      const Result failed = Result::Fail(u"The comment couldn't be stored in the workbook.");
      const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
      const std::string ms = "http://schemas.microsoft.com/office/2017/10/relationships/";
      // A part written, added (from `source`) or removed as its new bytes say.
      auto put = [&](const std::string& part, const std::string& was, const std::string& bytes, std::string_view source,
                     const std::string& type, const std::string& name, std::string_view contentType) -> std::string {
        if (bytes == was) return part.empty() ? std::string() : "same";
        if (bytes.empty()) return part.empty() || RemovePart(part) ? "gone" : std::string();
        if (!part.empty()) return WritePart(part, bytes) ? "written" : std::string();
        return AddPart(source, type, name, contentType, bytes);
      };
      if (now.persons != old.persons &&
          put(personsPart, old.persons, now.persons, book_.WorkbookPart(), ms + "person",
              PartBytes("/xl/persons/person.xml").empty() ? "/xl/persons/person.xml" : NewPartName("/xl/persons/person", ".xml"),
              "application/vnd.ms-excel.person+xml")
              .empty())
        return failed;
      if (now.threads != old.threads &&
          put(parts.threads, old.threads, now.threads, s.PartName(), ms + "threadedComment",
              NewPartName("/xl/threadedComments/threadedComment", ".xml"), "application/vnd.ms-excel.threadedcomments+xml")
              .empty())
        return failed;
      if (now.comments != old.comments &&
          put(parts.comments, old.comments, now.comments, s.PartName(), rel + "comments", NewPartName("/xl/comments", ".xml"),
              "application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml")
              .empty())
        return failed;
      State& st = *s_;
      if (now.vml != old.vml) {
        if (now.vml.empty() || parts.vml.empty()) {
          // The sheet's legacyDrawing names the VML drawing (or goes with it).
          std::string id;
          if (!now.vml.empty() &&
              (id = AddPart(s.PartName(), rel + "vmlDrawing", NewPartName("/xl/drawings/vmlDrawing", ".vml"),
                            "application/vnd.openxmlformats-officedocument.vmlDrawing", now.vml))
                  .empty())
            return failed;
          if (now.vml.empty() && !parts.vml.empty() && !RemovePart(parts.vml)) return failed;
          st.SaveText(st.Save(s));
          detail::PartText& t = A::Text(s);
          xlsx::PutSheetElement(t, "legacyDrawing",
                                id.empty() ? std::string() : "<" + xlsx::SheetPrefix(t) + "legacyDrawing r:id=\"" + id + "\"/>");
        } else if (!WritePart(parts.vml, now.vml)) {
          return failed;
        }
      }
      st.Change(&s, kObjects, Range{cell.row, cell.col, cell.row, cell.col});
      return Result{};
    });
  }());
}

Result Editor::SetNote(int sheet, CellAddr cell, const std::optional<Note>& note) {
  Guard guard(*this);
  if (Result no = guard.Check(sheet, ProtectAct::Objects); !no) return no;
  Rec rec(*this, JournalOp::SetNote, [&](JournalRecord& w) {
    w.I(sheet).A(cell).B(note.has_value());
    if (note) {
      w.S(note->author).S(note->text).B(note->shown).D(note->width).D(note->height);
      for (uint32_t v : note->anchor) w.U(v);
    }
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (cell.row >= kMaxRows || cell.col >= kMaxCols) return BadRange();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    pkg::Package* package = book_.GetPackage();
    if (!package || s.PartName().empty() || s.Kind() != SheetKind::Worksheet)
      return Result::Fail(u"Notes can be added to this sheet once the workbook is saved.");
    if (note && (!XmlSafe(note->text) || !XmlSafe(note->author)))
      return Result::Fail(u"The note holds characters a workbook can't store.");
    if (note && Utf16Length(note->text) > 32767) return Result::Fail(u"A note can hold at most 32,767 characters.");
    const NoteParts parts = NotePartsOf(*package, s);
    const std::string &commentsPart = parts.comments, &vmlPart = parts.vml;
    if (!parts.threads.empty() && notes::ReadThread(PartBytes(parts.threads), {}, cell))
      return Result::Fail(u"This cell has a comment thread. Reply to it, or delete it first.");
    const std::string comments = commentsPart.empty() ? std::string() : PartBytes(commentsPart);
    const std::string vml = vmlPart.empty() ? std::string() : PartBytes(vmlPart);
    std::string newComments, newVml;
    std::optional<notes::NoteText> text;
    if (note) text = notes::NoteText{note->author, note->text, note->shown, note->width, note->height, note->anchor};
    if (!notes::Edit(comments, vml, cell, text, newComments, newVml)) return Result::Fail(u"This sheet's notes couldn't be read.");
    if (newComments == comments && newVml == vml) return Result{};
    return Run(*this, note ? u"Edit Note" : u"Delete Note", [&]() -> Result {
      const Result failed = Result::Fail(u"The note couldn't be stored in the workbook.");
      const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
      if (newComments.empty()) {
        if (!commentsPart.empty() && !RemovePart(commentsPart)) return failed;
      } else if (commentsPart.empty()) {
        if (AddPart(s.PartName(), rel + "comments", NewPartName("/xl/comments", ".xml"),
                    "application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml", newComments)
                .empty())
          return failed;
      } else if (!WritePart(commentsPart, newComments)) {
        return failed;
      }
      State& st = *s_;
      if (newVml.empty() || vmlPart.empty()) {
        // The sheet's legacyDrawing names the VML drawing (or goes with it).
        std::string id;
        if (!newVml.empty() &&
            (id = AddPart(s.PartName(), rel + "vmlDrawing", NewPartName("/xl/drawings/vmlDrawing", ".vml"),
                          "application/vnd.openxmlformats-officedocument.vmlDrawing", newVml))
                .empty())
          return failed;
        if (newVml.empty() && !vmlPart.empty() && !RemovePart(vmlPart)) return failed;
        st.SaveText(st.Save(s));
        detail::PartText& t = A::Text(s);
        xlsx::PutSheetElement(t, "legacyDrawing", id.empty() ? std::string() : "<" + xlsx::SheetPrefix(t) + "legacyDrawing r:id=\"" + id + "\"/>");
      } else if (!WritePart(vmlPart, newVml)) {
        return failed;
      }
      st.Change(&s, kObjects, Range{cell.row, cell.col, cell.row, cell.col});
      return Result{};
    });
  }());
}

// ---- Page setup

Result Editor::SetPageSetup(int sheet, const PageSetup& page, const HeaderFooter& hf, bool fitToPage) {
  Rec rec(*this, JournalOp::SetPageSetup, [&](JournalRecord& w) {
    w.I(sheet).U(page.paperSize).S(page.paperWidth).S(page.paperHeight).U(page.scale).U(page.fitToWidth);
    w.U(page.fitToHeight).I(page.firstPageNumber).B(page.useFirstPageNumber).U(static_cast<uint8_t>(page.orientation));
    w.U(static_cast<uint8_t>(page.pageOrder)).B(page.blackAndWhite).B(page.draft).B(page.usePrinterDefaults);
    w.U(static_cast<uint8_t>(page.cellComments)).U(static_cast<uint8_t>(page.errors)).U(page.horizontalDpi);
    w.U(page.verticalDpi).U(page.copies).B(page.horizontalCentered).B(page.verticalCentered).B(page.headings);
    w.B(page.gridLines).B(page.gridLinesSet);
    const PageMargins& m = page.margins;
    w.D(m.left).D(m.right).D(m.top).D(m.bottom).D(m.header).D(m.footer);
    w.B(hf.differentOddEven).B(hf.differentFirst).B(hf.scaleWithDoc).B(hf.alignWithMargins).S(hf.oddHeader);
    w.S(hf.oddFooter).S(hf.evenHeader).S(hf.evenFooter).S(hf.firstHeader).S(hf.firstFooter).B(fitToPage);
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet) return Result::Fail(u"A chart sheet's page setup can't be changed yet.");
    // Excel's limits (its Page Setup refuses the rest).
    if (page.scale < 10 || page.scale > 400) return Result::Fail(u"The scale must be between 10 and 400 percent.");
    if (page.fitToWidth > 32767 || page.fitToHeight > 32767) return Result::Fail(u"Fit to: at most 32,767 pages.");
    const PageMargins& m = page.margins;
    for (double v : {m.left, m.right, m.top, m.bottom, m.header, m.footer})
      if (!(v >= 0 && v < 49)) return Result::Fail(u"Margins must be between 0 and 49 inches.");
    if (page.firstPageNumber < -32765 || page.firstPageNumber > 32767) return Result::Fail(u"The first page number is too large.");
    for (const std::string* t : {&hf.oddHeader, &hf.oddFooter, &hf.evenHeader, &hf.evenFooter, &hf.firstHeader, &hf.firstFooter}) {
      if (!XmlSafe(*t)) return Result::Fail(u"The header or footer holds characters a workbook can't store.");
      if (Utf16Length(*t) > 255)
        return Result::Fail(u"The text string you entered is too long. Reduce the number of characters used.");
    }
    for (const std::string* t : {&page.paperWidth, &page.paperHeight})
      if (!XmlSafe(*t)) return Result::Fail(u"The paper size can't be stored.");
    detail::PartText& t = A::Text(s);
    const std::string p = xlsx::SheetPrefix(t);
    const std::pair<const char*, std::string> parts[] = {
        {"printOptions", pagexml::PrintOptions(page, xlsx::SheetElement(t, "printOptions"), p)},
        {"pageMargins", pagexml::Margins(m, p)},
        {"pageSetup", pagexml::Setup(page, xlsx::SheetElement(t, "pageSetup"), p)},
        {"headerFooter", pagexml::HeadersFooters(hf, xlsx::SheetElement(t, "headerFooter"), p)},
        {"sheetPr", pagexml::SheetPrFit(xlsx::SheetElement(t, "sheetPr"), fitToPage, p)}};
    bool same = s.Props().fitToPage == fitToPage;
    for (const auto& [local, xml] : parts) same = same && xml == xlsx::SheetElement(t, local);
    if (same) return Result{};
    return Run(*this, u"Page Setup", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SavePage(sv);
      st.SaveText(sv);
      st.SaveProps(sv);
      A::Page(s) = page;
      A::HF(s) = hf;
      A::Props(s).fitToPage = fitToPage;
      for (const auto& [local, xml] : parts) xlsx::PutSheetElement(A::Text(s), local, xml);
      st.Change(&s, kSheetProps, Range{});
      return Result{};
    });
  }());
}

Result Editor::SetPageBreaks(int sheet, const std::vector<PageBreak>& rowsIn, const std::vector<PageBreak>& colsIn) {
  Rec rec(*this, JournalOp::SetPageBreaks, [&](JournalRecord& w) {
    w.I(sheet);
    for (const auto* list : {&rowsIn, &colsIn}) {
      w.U(list->size());
      for (const PageBreak& b : *list) w.U(b.id).U(b.min).U(b.max).B(b.manual).B(b.pivot);
    }
  });
  return rec.Done([&]() -> Result {
    if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
    if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
    Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
    if (s.Kind() != SheetKind::Worksheet) return NoSheet();
    // In order, one per row or column, and only where a page can start (not before the first).
    std::vector<PageBreak> rows = rowsIn, cols = colsIn;
    for (auto* list : {&rows, &cols}) {
      const uint32_t limit = list == &rows ? kMaxRows : kMaxCols;
      std::sort(list->begin(), list->end(), [](const PageBreak& a, const PageBreak& b) { return a.id < b.id; });
      list->erase(std::unique(list->begin(), list->end(), [](const PageBreak& a, const PageBreak& b) { return a.id == b.id; }),
                  list->end());
      for (const PageBreak& b : *list)
        if (b.id == 0 || b.id >= limit) return BadRange();
      if (list->size() > 1026) return Result::Fail(u"A sheet can have at most 1,026 page breaks each way.");
    }
    const auto same = [](const std::vector<PageBreak>& a, const std::vector<PageBreak>& b) {
      return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const PageBreak& x, const PageBreak& y) {
        return x.id == y.id && x.min == y.min && x.max == y.max && x.manual == y.manual && x.pivot == y.pivot;
      });
    };
    if (same(rows, s.RowBreaks()) && same(cols, s.ColBreaks())) return Result{};
    return Run(*this, u"Page Breaks", [&]() -> Result {
      State& st = *s_;
      SheetSave& sv = st.Save(s);
      st.SaveBreaks(sv);
      st.SaveText(sv);
      A::Breaks(s, true) = std::move(rows);
      A::Breaks(s, false) = std::move(cols);
      detail::PartText& t = A::Text(s);
      const std::string p = xlsx::SheetPrefix(t);
      xlsx::PutSheetElement(t, "rowBreaks", pagexml::Breaks(s.RowBreaks(), true, p));
      xlsx::PutSheetElement(t, "colBreaks", pagexml::Breaks(s.ColBreaks(), false, p));
      st.Change(&s, kSheetProps, Range{});
      return Result{};
    });
  }());
}

// ---- View state

void Editor::SetActiveSheet(int sheet) {
  Rec rec(*this, JournalOp::SetActiveSheet, [&](JournalRecord& w) { w.I(sheet).Key(0); });
  rec.commit = true;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  uint32_t& active = A::Active(book_);
  if (active == static_cast<uint32_t>(sheet)) return;
  if (active < book_.SheetCount()) s_->ViewChanged(&book_.GetSheet(active));
  active = static_cast<uint32_t>(sheet);
  s_->ViewChanged(&book_.GetSheet(active));
  s_->BookViewChanged();
  for (auto& l : listeners_) l({edit::Change{sheet, kView, Range{}, {}}});
}

namespace {
SheetView& FirstView(Sheet& s) {
  auto& views = A::Views(s);
  if (views.empty()) views.emplace_back();
  return views[0];
}
}  // namespace

void Editor::SetSelection(int sheet, CellAddr active, const std::vector<Range>& ranges) {
  Rec rec(*this, JournalOp::SetSelection, [&](JournalRecord& w) { w.I(sheet).A(active).U(ranges.size()); for (const Range& x : ranges) w.R(x); w.Key(sheet); });
  rec.commit = true;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  SheetView& v = FirstView(s);
  Selection sel;
  sel.pane = v.paneState == PaneState::None ? PaneId::TopLeft : v.activePane;
  sel.active = active;
  sel.ranges = ranges.empty() ? std::vector<Range>{Range::Cell(active.row, active.col)} : ranges;
  for (uint32_t i = 0; i < sel.ranges.size(); ++i)
    if (sel.ranges[i].Contains(active.row, active.col)) sel.activeRange = i;
  auto it = std::find_if(v.selections.begin(), v.selections.end(), [&](const Selection& x) { return x.pane == sel.pane; });
  if (it != v.selections.end()) *it = sel;
  else v.selections.push_back(sel);
  s_->ViewChanged(&s);
}

void Editor::SetTopLeft(int sheet, CellAddr topLeft) {
  Rec rec(*this, JournalOp::SetTopLeft, [&](JournalRecord& w) { w.I(sheet).A(topLeft).Key(sheet); });
  rec.commit = true;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  SheetView& v = FirstView(s);
  if (v.paneState == PaneState::Frozen) v.paneTopLeft = topLeft;
  else v.topLeft = topLeft;
  s_->ViewChanged(&s);
}

void Editor::SetZoom(int sheet, uint32_t percent) {
  Rec rec(*this, JournalOp::SetZoom, [&](JournalRecord& w) { w.I(sheet).U(percent).Key(sheet); });
  rec.commit = true;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  FirstView(s).zoomScale = std::clamp(percent, 10u, 400u);
  s_->ViewChanged(&s);
}

void Editor::SetViewMode(int sheet, ViewMode mode) {
  Rec rec(*this, JournalOp::SetViewMode, [&](JournalRecord& w) { w.I(sheet).U(static_cast<uint8_t>(mode)).Key(sheet); });
  rec.commit = true;
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  FirstView(s).view = mode;
  s_->ViewChanged(&s);
}

// ---- Names

namespace {
bool ValidName(std::string_view n) {
  if (n.empty() || n.size() > 255) return false;
  auto c0 = static_cast<unsigned char>(n[0]);
  if (!(std::isalpha(c0) || c0 == '_' || c0 == '\\' || c0 >= 0x80)) return false;
  for (char ch : n) {
    auto c = static_cast<unsigned char>(ch);
    if (!(std::isalnum(c) || c == '_' || c == '\\' || c == '.' || c == '?' || c >= 0x80)) return false;
  }
  if (n.size() <= 3 && (n == "R" || n == "r" || n == "C" || n == "c")) return false;
  Range r;
  return !ParseRange(n, r);  // not a cell reference
}
}  // namespace

Result Editor::SetName(const DefinedName& name) {
  Rec rec(*this, JournalOp::SetName, [&](JournalRecord& w) { w.S(name.name).I(name.sheet).S(name.formula).B(name.hidden); });
  return rec.Done([&]() -> Result {
    bool builtin = name.name.rfind("_xlnm.", 0) == 0;
    if (!builtin && !ValidName(name.name)) return Result::Fail(u"That isn't a valid name.");
    if (name.sheet >= static_cast<int>(book_.SheetCount())) return NoSheet();
    if (name.formula.empty() || !XmlSafe(name.formula)) return Result::Fail(u"The name needs a reference or a formula.");
    return Run(*this, u"Define Name", [&]() -> Result {
      State& st = *s_;
      st.SaveBook(false, true);
      auto& names = A::Names(book_);
      auto it = std::find_if(names.begin(), names.end(),
                             [&](const DefinedName& x) { return x.sheet == name.sheet && NameEqualsNoCase(x.name, name.name); });
      if (it != names.end()) *it = name;
      else names.push_back(name);
      st.Change(nullptr, kNames, Range{});
      return Result{};
    });
  }());
}

Result Editor::DeleteName(std::string_view name, int sheet) {
  Rec rec(*this, JournalOp::DeleteName, [&](JournalRecord& w) { w.S(name).I(sheet); });
  return rec.Done([&]() -> Result {
    auto& names = A::Names(book_);
    auto it = std::find_if(names.begin(), names.end(),
                           [&](const DefinedName& x) { return x.sheet == sheet && NameEqualsNoCase(x.name, name); });
    if (it == names.end()) return Result::Fail(u"There's no such name.");
    size_t index = static_cast<size_t>(it - names.begin());
    return Run(*this, u"Delete Name", [&]() -> Result {
      State& st = *s_;
      st.SaveBook(false, true);
      A::Names(book_).erase(A::Names(book_).begin() + static_cast<ptrdiff_t>(index));
      st.Change(nullptr, kNames, Range{});
      return Result{};
    });
  }());
}

// ---- Steps

void Editor::BeginGroup(std::u16string name) {
  Rec rec(*this, JournalOp::BeginGroup, [&](JournalRecord& w) { w.S16(name); });
  rec.commit = true;
  s_->open.emplace_back();
  s_->open.back().name = std::move(name);
}

void Editor::EndGroup() {
  Rec rec(*this, JournalOp::EndGroup, [](JournalRecord&) {});
  rec.commit = true;
  State& st = *s_;
  if (st.open.empty()) return;
  Step step = std::move(st.open.back());
  st.open.pop_back();
  if (!st.open.empty()) {  // an inner group: part of the outer one
    std::u16string outer = std::move(st.open.back().name);
    st.open.back().Absorb(std::move(step));
    st.open.back().name = std::move(outer);
    return;
  }
  if (step.Empty()) return;
  step.serial = ++st.serial;
  st.Count(step, 1);
  std::vector<edit::Change> changes = st.Resolve(step, false);
  st.undo.push_back(std::move(step));
  st.redo.clear();
  if (st.undo.size() > kMaxSteps) st.undo.erase(st.undo.begin());
  for (auto& l : listeners_) l(changes);
}

void Editor::CancelGroup() {
  Rec rec(*this, JournalOp::CancelGroup, [](JournalRecord&) {});
  rec.commit = true;
  State& st = *s_;
  if (st.open.empty()) return;
  Step step = std::move(st.open.back());
  st.open.pop_back();
  step.Swap(book_);  // back as it was; nothing was announced yet
}

bool Editor::CanUndo() const { return !s_->undo.empty(); }
bool Editor::CanRedo() const { return !s_->redo.empty(); }
std::u16string Editor::UndoName() const { return s_->undo.empty() ? std::u16string() : s_->undo.back().name; }
std::u16string Editor::RedoName() const { return s_->redo.empty() ? std::u16string() : s_->redo.back().name; }

std::vector<std::u16string> Editor::UndoNames() const {
  std::vector<std::u16string> out;
  for (auto it = s_->undo.rbegin(); it != s_->undo.rend(); ++it) out.push_back(it->name);
  return out;
}

std::vector<std::u16string> Editor::RedoNames() const {
  std::vector<std::u16string> out;
  for (auto it = s_->redo.rbegin(); it != s_->redo.rend(); ++it) out.push_back(it->name);
  return out;
}

Result Editor::Undo(size_t steps) {
  Rec rec(*this, JournalOp::Undo, [&](JournalRecord& w) { w.U(steps); });
  return rec.Done([&]() -> Result {
    State& st = *s_;
    if (st.saving) return Result::Fail(u"The workbook is being saved. Try again in a moment.");
    if (!st.open.empty()) return Result::Fail(u"An edit is still going on.");
    if (st.undo.empty()) return Result::Fail(u"There's nothing to undo.");
    std::vector<edit::Change> changes;
    for (size_t i = 0; i < steps && !st.undo.empty(); ++i) {
      Step step = std::move(st.undo.back());
      st.undo.pop_back();
      step.Swap(book_);
      st.Count(step, -1, true);
      auto c = st.Resolve(step, true);
      changes.insert(changes.end(), c.begin(), c.end());
      st.redo.push_back(std::move(step));
    }
    for (auto& l : listeners_) l(changes);
    return {};
  }());
}

Result Editor::Redo(size_t steps) {
  Rec rec(*this, JournalOp::Redo, [&](JournalRecord& w) { w.U(steps); });
  return rec.Done([&]() -> Result {
    State& st = *s_;
    if (st.saving) return Result::Fail(u"The workbook is being saved. Try again in a moment.");
    if (!st.open.empty()) return Result::Fail(u"An edit is still going on.");
    if (st.redo.empty()) return Result::Fail(u"There's nothing to redo.");
    std::vector<edit::Change> changes;
    for (size_t i = 0; i < steps && !st.redo.empty(); ++i) {
      Step step = std::move(st.redo.back());
      st.redo.pop_back();
      step.Swap(book_);
      st.Count(step, 1, true);
      auto c = st.Resolve(step, false);
      changes.insert(changes.end(), c.begin(), c.end());
      st.undo.push_back(std::move(step));
    }
    for (auto& l : listeners_) l(changes);
    return {};
  }());
}

bool Editor::Modified() const {
  const State& st = *s_;
  return (st.undo.empty() ? 0 : st.undo.back().serial) != st.savedSerial || !st.viewDirty.empty() || st.bookViewDirty;
}

void Editor::MarkSaved() {
  State& st = *s_;
  st.savedSerial = st.undo.empty() ? 0 : st.undo.back().serial;
  st.dirty.clear();
  st.bookDirty = 0;
  st.viewDirty.clear();
  st.bookViewDirty = false;
  st.Saved();
}

void Editor::SavedOutside(FileFormat format) {
  State& st = *s_;
  st.savedSerial = st.undo.empty() ? 0 : st.undo.back().serial;
  st.viewDirty.clear();
  st.bookViewDirty = false;
  A::Format(book_) = format;
  st.saving = false;
}

void Editor::BeginSave() {
  State& st = *s_;
  st.saving = true;
  st.viewsWhileSaving.clear();
  st.bookViewWhileSaving = false;
  const Styles& styles = book_.GetStyles();
  st.xfsSaving = styles.CellXfs().size();
  st.fontsSaving = styles.Fonts().size();
  st.fillsSaving = styles.Fills().size();
  st.bordersSaving = styles.Borders().size();
  st.numFmtsSaving = styles.CustomFormats().size();
  st.stringsSaving = book_.Strings().Size();
  st.cellStylesSaving = styles.CellStyleXfs().size() + styles.CellStyles().size() + styles.Dxfs().size();
}

bool Editor::Saving() const { return s_->saving; }

void Editor::EndSave() { s_->saving = false; }

void Editor::Saved(std::vector<Written> sheets, bool stringsWritten, std::string newStringsPart, FileFormat format,
                   bool sheetsRemoved) {
  State& st = *s_;
  for (Written& w : sheets) {
    for (size_t i = 0; i < book_.SheetCount(); ++i) {
      Sheet& s = book_.GetSheet(i);
      if (&s != w.sheet) continue;
      A::Part(s) = std::move(w.part);
      A::RelId(s) = std::move(w.relId);
      A::Text(s) = std::move(w.text);
      st.mustWrite.insert(&s);
    }
  }
  if (!newStringsPart.empty()) A::StringsPart(book_) = std::move(newStringsPart);
  st.mustWriteStrings = st.mustWriteStrings || stringsWritten;
  A::Format(book_) = format;
  if (sheetsRemoved) {
    st.undo.clear();
    st.redo.clear();
    Rec rec(*this, JournalOp::DropHistory, [](JournalRecord&) {});
    rec.commit = true;
  }
  // The file holds the workbook as it was when the save began; view changes and formats added
  // since are still to save.
  st.savedSerial = st.undo.empty() ? 0 : st.undo.back().serial;
  st.dirty.clear();
  st.bookDirty = 0;
  st.viewDirty = std::move(st.viewsWhileSaving);
  st.viewsWhileSaving.clear();
  st.bookViewDirty = st.bookViewWhileSaving;
  st.xfsAtSave = st.xfsSaving;
  st.fontsAtSave = st.fontsSaving;
  st.fillsAtSave = st.fillsSaving;
  st.bordersAtSave = st.bordersSaving;
  st.numFmtsAtSave = st.numFmtsSaving;
  st.stringsAtSave = st.stringsSaving;
  st.cellStylesAtSave = st.cellStylesSaving;
  st.saving = false;
}

bool Editor::MustWrite(int sheet) const {
  return sheet >= 0 && static_cast<size_t>(sheet) < book_.SheetCount() &&
         s_->mustWrite.count(&book_.GetSheet(static_cast<size_t>(sheet)));
}

bool Editor::MustWriteStrings() const { return s_->mustWriteStrings; }

uint32_t Editor::DirtyPieces(int sheet) const {
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return 0;
  const Sheet* s = &book_.GetSheet(static_cast<size_t>(sheet));
  uint32_t out = s_->viewDirty.count(s) ? kPieceViews : 0;
  if (auto it = s_->dirty.find(s); it != s_->dirty.end())
    for (int p = 0; p < kPieces; ++p)
      if (it->second[p] != 0) out |= 1u << p;
  return out;
}

bool Editor::WorkbookDirty() const { return s_->bookDirty != 0 || s_->bookViewDirty; }

bool Editor::StylesDirty() const {
  const Styles& st = book_.GetStyles();
  return st.CellXfs().size() != s_->xfsAtSave || st.Fonts().size() != s_->fontsAtSave ||
         st.Fills().size() != s_->fillsAtSave || st.Borders().size() != s_->bordersAtSave ||
         st.CustomFormats().size() != s_->numFmtsAtSave ||
         st.CellStyleXfs().size() + st.CellStyles().size() + st.Dxfs().size() != s_->cellStylesAtSave;
}

bool Editor::StringsDirty() const { return book_.Strings().Size() != s_->stringsAtSave; }

}  // namespace fw::sheet::edit
