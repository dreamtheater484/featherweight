#include "sheet/edit/Edit.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "pkg/Formula.h"
#include "sheet/calc/Formula.h"

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
    for (auto& f : later.formulas)
      if (std::none_of(formulas.begin(), formulas.end(), [&](const auto& x) { return x.first == f.first; }))
        formulas.push_back(std::move(f));
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
    saved |= later.saved;
    dirty |= later.dirty;
  }
};

// The workbook's own state as it was: the list of sheets, the names, the active tab.
struct BookSave {
  bool sheets = false, names = false, active = false;
  std::vector<Sheet*> order;
  std::vector<std::unique_ptr<Sheet>> held;  // sheets of `order` the workbook doesn't hold now
  std::vector<DefinedName> nameList;
  uint32_t activeSheet = 0, firstTab = 0;

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
  uint64_t serial = 0;

  bool Empty() const { return sheets.empty() && !book.sheets && !book.names && !book.active && changes.empty(); }

  // Undo or redo: everything back the other way.
  void Swap(Workbook& b) {
    for (auto it = sheets.rbegin(); it != sheets.rend(); ++it) it->Swap();
    book.Swap(b);
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

std::string Number(double v) {
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
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
        if (content->array) {
          rec.kind = FormulaKind::Array;
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
  if (in.array && !Valid(in.arrayRange)) return BadRange();
  return {};
}

Result Editor::SetCell(int sheet, uint32_t row, uint32_t col, const CellInput& in) {
  return SetCells(sheet, {{CellAddr{row, col}, in}});
}

Result Editor::SetCells(int sheet, const std::vector<std::pair<CellAddr, CellInput>>& cells) {
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
  if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  const std::vector<Range> arrays = State::Arrays(s);
  for (const auto& [at, in] : cells) {
    if (at.row >= kMaxRows || at.col >= kMaxCols) return BadRange();
    if (Result r = CheckInput(in); !r) return r;
    if (in.array && (in.arrayRange.firstRow != at.row || in.arrayRange.firstCol != at.col)) return BadRange();
    if (const Range* arr = State::ArrayAt(arrays, at.row, at.col); arr && !(in.array && in.arrayRange == *arr))
      return Result::Fail(u"You can't change part of an array.");
  }
  return Run(*this, u"Typing", [&]() -> Result {
    State& st = *s_;
    SheetSave& sv = st.Save(s);
    std::u16string why;
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
    if (!cells.empty())
      st.Change(&s, kValues | kFormulas | (std::any_of(cells.begin(), cells.end(), [](const auto& c) { return c.second.style.has_value(); }) ? kStyles : 0u),
                touched);
    (void)formulas;
    return Result{};
  });
}

Result Editor::ClearContents(int sheet, const Range& r) {
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
    st.Change(&s, kValues | kFormulas, r);
    return Result{};
  });
}

Result Editor::ClearFormats(int sheet, const Range& r) { return MapStyles(sheet, r, [](uint32_t) { return 0u; }); }

Result Editor::ClearAll(int sheet, const Range& r) {
  return Run(*this, u"Clear All", [&]() -> Result {
    if (Result x = ClearContents(sheet, r); !x) return x;
    return ClearFormats(sheet, r);
  });
}

Result Editor::SetStyle(int sheet, const Range& r, uint32_t style) {
  return MapStyles(sheet, r, [style](uint32_t) { return style; });
}

Result Editor::MapStyles(int sheet, const Range& r, const std::function<uint32_t(uint32_t)>& map) {
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return NoSheet();
  if (!Valid(r)) return BadRange();
  if (!book_.LoadSheet(static_cast<size_t>(sheet)).Ok()) return Result::Fail(u"This sheet couldn't be read.");
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  return Run(*this, u"Format Cells", [&]() -> Result {
    State& st = *s_;
    SheetSave& sv = st.Save(s);
    std::u16string why;
    auto mapped = [&](uint32_t old) {
      uint32_t x = map(old);
      return x < book_.GetStyles().CellXfs().size() ? x : 0u;
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
    st.Change(&s, kStyles, r);
    return Result{};
  });
}

uint32_t Editor::AddXf(const Xf& xf) {
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
}

uint32_t Editor::AddFont(const Font& font) {
  StylesBuilder sb(book_.GetStyles());
  auto& v = sb.Fonts();
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == font) return static_cast<uint32_t>(i);
  v.push_back(font);
  return static_cast<uint32_t>(v.size() - 1);
}

uint32_t Editor::AddFill(const Fill& fill) {
  StylesBuilder sb(book_.GetStyles());
  auto& v = sb.Fills();
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == fill) return static_cast<uint32_t>(i);
  v.push_back(fill);
  return static_cast<uint32_t>(v.size() - 1);
}

uint32_t Editor::AddBorder(const Border& border) {
  StylesBuilder sb(book_.GetStyles());
  auto& v = sb.Borders();
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == border) return static_cast<uint32_t>(i);
  v.push_back(border);
  return static_cast<uint32_t>(v.size() - 1);
}

uint32_t Editor::AddNumFmt(std::string_view code) {
  StylesBuilder sb(book_.GetStyles());
  auto& fmts = sb.NumFmts();
  for (const auto& [id, c] : fmts)
    if (c == code) return id;
  uint32_t id = 164;
  for (const auto& [i, c] : fmts) id = std::max(id, i + 1);
  fmts.emplace(id, std::string(code));
  return id;
}

// ---- Rows and columns

Result Editor::SetRowHeight(int sheet, uint32_t first, uint32_t last, double points) {
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
}

Result Editor::HideRows(int sheet, uint32_t first, uint32_t last, bool hidden) {
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
}

Result Editor::SetRowOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed) {
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
}

Result Editor::SetColumnWidth(int sheet, uint32_t first, uint32_t last, double chars) {
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
}

Result Editor::HideColumns(int sheet, uint32_t first, uint32_t last, bool hidden) {
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
}

Result Editor::SetColumnOutline(int sheet, uint32_t first, uint32_t last, uint8_t level, bool collapsed) {
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
}

// Structural edits arrive with the reference registry: until then they're refused, so nothing
// half-done ever reaches a file.
static Result NotYet() {
  return Result::Fail(u"Inserting and deleting rows, columns and cells isn't available yet.");
}
Result Editor::InsertRows(int, uint32_t, uint32_t) { return NotYet(); }
Result Editor::DeleteRows(int, uint32_t, uint32_t) { return NotYet(); }
Result Editor::InsertColumns(int, uint32_t, uint32_t) { return NotYet(); }
Result Editor::DeleteColumns(int, uint32_t, uint32_t) { return NotYet(); }
Result Editor::InsertCells(int, const Range&, Direction) { return NotYet(); }
Result Editor::DeleteCells(int, const Range&, Direction) { return NotYet(); }

// ---- Merged cells

Result Editor::Merge(int sheet, const Range& r) {
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
}

Result Editor::Unmerge(int sheet, const Range& r) {
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
}

Result Editor::RenameSheet(int sheet, std::string name) {
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
    for (DefinedName& n : A::Names(book_)) n.formula = pkg::RenameSheetInFormula(n.formula, old, name);
    // Formulas on every sheet, and links to places in the workbook.
    for (size_t i = 0; i < book_.SheetCount(); ++i) {
      Sheet& x = book_.GetSheet(i);
      auto& formulas = A::Formulas(x);
      SheetSave* sv = nullptr;
      for (uint32_t id = 0; id < formulas.size(); ++id) {
        if (formulas[id].text == kNone) continue;
        bool changed = false;
        std::string text = pkg::RenameSheetInFormula(A::Texts(x).Get(formulas[id].text), old, name, &changed);
        if (!changed) continue;
        if (!sv) sv = &st.Save(x);
        st.SaveCells(*sv);
        st.SaveFormula(*sv, id);
        formulas[id].text = A::Texts(x).Add(text);
      }
      bool links = std::any_of(x.Hyperlinks().begin(), x.Hyperlinks().end(), [&](const Hyperlink& h) {
        bool changed = false;
        pkg::RenameSheetInFormula(h.location, old, name, &changed);
        return changed;
      });
      if (links) {
        SheetSave& lsv = st.Save(x);
        st.SaveLinks(lsv);
        for (Hyperlink& h : A::Links(x)) h.location = pkg::RenameSheetInFormula(h.location, old, name);
      }
    }
    st.Change(&s, kSheets | kFormulas, Range::All());
    return Result{};
  });
}

Result Editor::MoveSheet(int sheet, int to) {
  int n = static_cast<int>(book_.SheetCount());
  if (sheet < 0 || sheet >= n) return NoSheet();
  to = std::clamp(to, 0, n - 1);
  if (to == sheet) return {};
  return Run(*this, u"Move Sheet", [&]() -> Result {
    State& st = *s_;
    st.SaveBook(true, true);
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
}

Result Editor::DeleteSheet(int sheet) {
  int n = static_cast<int>(book_.SheetCount());
  if (sheet < 0 || sheet >= n) return NoSheet();
  if (n == 1) return Result::Fail(u"A workbook needs at least one sheet.");
  int visible = 0;
  for (int i = 0; i < n; ++i) visible += book_.GetSheet(static_cast<size_t>(i)).State() == SheetState::Visible;
  if (visible == 1 && book_.GetSheet(static_cast<size_t>(sheet)).State() == SheetState::Visible)
    return Result::Fail(u"A workbook needs at least one visible sheet.");
  // Until the reference registry turns them into #REF!, a sheet other formulas use stays.
  if (!book_.LoadAll().Ok()) return Result::Fail(u"A sheet couldn't be read.");
  const std::string& gone = book_.GetSheet(static_cast<size_t>(sheet)).Name();
  auto uses = [&](std::string_view formula) {
    bool found = false;
    pkg::ForEachSheetToken(formula, [&](const pkg::SheetToken& t) {
      found = found || NameEqualsNoCase(t.first, gone) || (!t.last.empty() && NameEqualsNoCase(t.last, gone));
    });
    return found;
  };
  for (const DefinedName& nm : book_.Names())
    if (nm.sheet != sheet && uses(nm.formula)) return Result::Fail(u"Formulas or names use this sheet.");
  // Anything else in the file that names it (chart series, x14 extensions' xm:f, hyperlink
  // locations, ...), until the registry updates those: Sheet1! or 'My sheet'! in any XML part but
  // the sheet's own, the workbook part and the shared strings.
  if (const pkg::Package* package = book_.GetPackage()) {
    auto lower = [](std::string s) {
      for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
      return s;
    };
    std::string quoted = "'";
    for (char c : gone) quoted += c == '\'' ? std::string("''") : std::string(1, c);
    const std::string plain = lower(gone) + "!", inQuotes = lower(quoted) + "'!";
    const std::string own = pkg::PartKey(book_.GetSheet(static_cast<size_t>(sheet)).PartName());
    for (const pkg::Part& part : package->Parts()) {
      std::string key = pkg::PartKey(part.name);
      if (part.deleted || key == own || key == pkg::PartKey(book_.WorkbookPart()) ||
          key == pkg::PartKey(book_.StringsPart()) || !package->IsXmlPart(part.name) ||
          package->ContentTypeOf(part.name).find("extended-properties") != std::string_view::npos)
        continue;  // (docProps/app.xml lists names as "Sheet1!Print_Area": text, not references)
      std::vector<uint8_t> bytes;
      if (!package->ReadPart(part.name, bytes).Ok()) continue;
      const std::string xml = lower(std::string(bytes.begin(), bytes.end()));
      bool named = xml.find(inQuotes) != std::string::npos;
      for (size_t at = xml.find(plain); !named && at != std::string::npos; at = xml.find(plain, at + 1)) {
        char before = at ? xml[at - 1] : '>';
        named = !(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '.' || before == '\'');
      }
      if (named) return Result::Fail(u"Other parts of the workbook use this sheet.");
    }
  }
  for (size_t i = 0; i < book_.SheetCount(); ++i) {
    if (static_cast<int>(i) == sheet) continue;
    Sheet& x = book_.GetSheet(i);
    for (const FormulaRec& f : A::Formulas(x))
      if (f.text != kNone && uses(A::Texts(x).Get(f.text))) return Result::Fail(u"Formulas or names use this sheet.");
  }
  return Run(*this, u"Delete Sheet", [&]() -> Result {
    State& st = *s_;
    st.SaveBook(true, true);
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
}

Result Editor::SetTabColor(int sheet, const Color& color) {
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
}

Result Editor::SetSheetState(int sheet, SheetState state) {
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
}

Result Editor::FreezePanes(int sheet, uint32_t rows, uint32_t cols) {
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
}

// ---- View state

void Editor::SetActiveSheet(int sheet) {
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
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  SheetView& v = FirstView(s);
  if (v.paneState == PaneState::Frozen) v.paneTopLeft = topLeft;
  else v.topLeft = topLeft;
  s_->ViewChanged(&s);
}

void Editor::SetZoom(int sheet, uint32_t percent) {
  if (sheet < 0 || static_cast<size_t>(sheet) >= book_.SheetCount()) return;
  Sheet& s = book_.GetSheet(static_cast<size_t>(sheet));
  FirstView(s).zoomScale = std::clamp(percent, 10u, 400u);
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
}

Result Editor::DeleteName(std::string_view name, int sheet) {
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
}

// ---- Steps

void Editor::BeginGroup(std::u16string name) {
  s_->open.emplace_back();
  s_->open.back().name = std::move(name);
}

void Editor::EndGroup() {
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
}

Result Editor::Redo(size_t steps) {
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
         st.CustomFormats().size() != s_->numFmtsAtSave;
}

bool Editor::StringsDirty() const { return book_.Strings().Size() != s_->stringsAtSave; }

}  // namespace fw::sheet::edit
