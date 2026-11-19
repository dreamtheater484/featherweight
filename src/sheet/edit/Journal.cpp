#include "sheet/edit/Journal.h"

#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "sheet/xlsx/Internal.h"

namespace fw::sheet::edit {

// A record in the journal: its kind, the length of its fields, the fields. Numbers are LEB128
// varints (signed ones zigzagged), doubles 8 bytes little-endian, strings a length and their bytes
// (UTF-8; UTF-16 code units for step names).

JournalRecord& JournalRecord::U(uint64_t v) {
  do {
    uint8_t b = v & 0x7F;
    v >>= 7;
    bytes_ += static_cast<char>(v ? b | 0x80 : b);
  } while (v);
  return *this;
}

JournalRecord& JournalRecord::I(int64_t v) {
  return U((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
}

JournalRecord& JournalRecord::D(double v) {
  char b[8];
  std::memcpy(b, &v, 8);
  bytes_.append(b, 8);
  return *this;
}

JournalRecord& JournalRecord::S(std::string_view s) {
  U(s.size());
  bytes_.append(s);
  return *this;
}

JournalRecord& JournalRecord::S16(std::u16string_view s) {
  U(s.size());
  for (char16_t c : s) U(c);
  return *this;
}

JournalRecord& JournalRecord::C(const Color& c) { return U(static_cast<uint8_t>(c.kind)).U(c.value).D(c.tint); }

JournalRecord& JournalRecord::Input(const CellInput& in) {
  U(static_cast<uint8_t>(in.type)).D(in.number).B(in.boolean).U(static_cast<uint8_t>(in.error)).S(in.text).S(in.formula);
  U(in.array ? 1 : in.dynamic ? 2 : 0).R(in.arrayRange).B(in.style.has_value());  // 2: dynamic (older: 0, 1)
  if (in.style) U(*in.style);
  return *this;
}

JournalRecord& JournalRecord::Filter(const std::optional<SheetFilter>& f) {
  B(f.has_value());
  if (!f) return *this;
  R(f->ref).U(f->columns.size());
  for (const FilterColumn& c : f->columns) {
    U(c.colId).U(static_cast<uint8_t>(c.kind)).U(c.values.size());
    for (const std::string& v : c.values) S(v);
    B(c.blank).U(c.conditions.size());
    for (const FilterColumn::Condition& k : c.conditions) S(k.op).S(k.val);
    B(c.both).D(c.count).B(c.bottom).B(c.percent).D(c.threshold).S(c.xml);
  }
  return *this;
}

Journal::Journal() { Add(JournalRecord(JournalOp::Version).U(kVersion)); }

void Journal::Add(const JournalRecord& r) {
  // Not a record the recovery file has already.
  if (r.CoalesceKey() >= 0 && lastAt_ != std::string::npos && lastAt_ >= written_ && lastOp_ == r.Op() &&
      lastKey_ == r.CoalesceKey())
    bytes_.resize(lastAt_);
  lastAt_ = bytes_.size();
  lastOp_ = r.Op();
  lastKey_ = r.CoalesceKey();
  JournalRecord head(r.Op());
  head.U(static_cast<uint8_t>(r.Op())).U(r.Bytes().size());
  bytes_ += head.Bytes();
  bytes_ += r.Bytes();
}

namespace {

// Reads a record's fields, refusing to go past its end.
class Fields {
 public:
  explicit Fields(std::string_view data) : d_(data) {}
  bool Ok() const { return ok_; }
  bool AtEnd() const { return at_ == d_.size(); }
  size_t Left() const { return d_.size() - at_; }

  uint64_t U() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at_ >= d_.size()) break;
      const auto b = static_cast<uint8_t>(d_[at_++]);
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok_ = false;
    return 0;
  }
  uint32_t U32() {
    uint64_t v = U();
    if (v > UINT32_MAX) ok_ = false;
    return static_cast<uint32_t>(v);
  }
  int64_t I() {
    uint64_t v = U();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }
  int Int() {
    int64_t v = I();
    if (v < INT32_MIN || v > INT32_MAX) ok_ = false;
    return static_cast<int>(v);
  }
  double D() {
    double v = 0;
    if (Left() < 8) {
      ok_ = false;
      return v;
    }
    std::memcpy(&v, d_.data() + at_, 8);
    at_ += 8;
    return v;
  }
  bool B() { return U() != 0; }
  std::string S() {
    uint64_t n = U();
    if (!ok_ || n > Left()) {
      ok_ = false;
      return {};
    }
    std::string s(d_.substr(at_, static_cast<size_t>(n)));
    at_ += static_cast<size_t>(n);
    return s;
  }
  std::u16string S16() {
    uint64_t n = U();
    if (!ok_ || n > Left()) {
      ok_ = false;
      return {};
    }
    std::u16string s;
    s.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n && ok_; ++i) s += static_cast<char16_t>(U());
    return s;
  }
  Range R() {
    Range r;
    r.firstRow = U32(), r.firstCol = U32(), r.lastRow = U32(), r.lastCol = U32();
    return r;
  }
  CellAddr A() {
    CellAddr a;
    a.row = U32(), a.col = U32();
    return a;
  }
  Color C() {
    Color c;
    c.kind = static_cast<Color::Kind>(U());
    c.value = U32();
    c.tint = D();
    return c;
  }
  CellInput Input() {
    CellInput in;
    in.type = static_cast<ValueType>(U());
    in.number = D();
    in.boolean = B();
    in.error = static_cast<ErrorCode>(U());
    in.text = S();
    in.formula = S();
    const uint64_t array = U();
    in.array = array == 1;
    in.dynamic = array == 2;
    in.arrayRange = R();
    if (B()) in.style = U32();
    return in;
  }
  // A count of items that each take at least one byte: no more than what's left.
  size_t Count() {
    uint64_t n = U();
    if (n > Left()) ok_ = false;
    return ok_ ? static_cast<size_t>(n) : 0;
  }
  std::optional<SheetFilter> Filter() {
    if (!B()) return std::nullopt;
    SheetFilter f;
    f.ref = R();
    f.columns.resize(Count());
    for (FilterColumn& c : f.columns) {
      c.colId = U32();
      c.kind = static_cast<FilterColumn::Kind>(U());
      c.values.resize(Count());
      for (std::string& v : c.values) v = S();
      c.blank = B();
      c.conditions.resize(Count());
      for (FilterColumn::Condition& k : c.conditions) k.op = S(), k.val = S();
      c.both = B();
      c.count = D();
      c.bottom = B();
      c.percent = B();
      c.threshold = D();
      c.xml = S();
    }
    return f;
  }
  // The next `n` bytes, skipped.
  std::string_view Bytes(uint64_t n) {
    if (!ok_ || n > Left()) {
      ok_ = false;
      return {};
    }
    std::string_view s = d_.substr(at_, static_cast<size_t>(n));
    at_ += static_cast<size_t>(n);
    return s;
  }

 private:
  std::string_view d_;
  size_t at_ = 0;
  bool ok_ = true;
};

}  // namespace

void Editor::SetJournal(Journal* journal) { journal_ = journal; }

Result Editor::Replay(std::string_view records, size_t* applied) {
  const Result damaged = Result::Fail(u"The recovery journal is damaged.");
  const Result differs = Result::Fail(u"The recovery journal doesn't match this workbook.");
  if (applied) *applied = 0;
  Fields in(records);
  for (size_t done = 0; !in.AtEnd(); ++done) {
    const uint64_t kind = in.U();
    Fields f(in.Bytes(in.U()));
    if (!in.Ok()) return damaged;
    const auto op = static_cast<JournalOp>(kind);
    if ((done == 0) != (op == JournalOp::Version)) return damaged;
    // A format added must land where it did: the records after it name it by index.
    auto same = [&](uint32_t got) { return got == f.U32() ? Result{} : differs; };
    Result r;
    switch (op) {
      case JournalOp::Version: {
        const uint64_t v = f.U();
        if (v == 0 || v > Journal::kVersion)
          return Result::Fail(u"The recovery journal was written by a newer Featherweight.");
        break;
      }
      case JournalOp::SetCells: {
        const int sheet = f.Int();
        std::vector<std::pair<CellAddr, CellInput>> cells(f.Count());
        for (auto& [a, input] : cells) a = f.A(), input = f.Input();
        if (f.Ok()) r = SetCells(sheet, cells);
        break;
      }
      case JournalOp::ClearContents: {
        const int sheet = f.Int();
        const Range range = f.R();
        if (f.Ok()) r = ClearContents(sheet, range);
        break;
      }
      case JournalOp::ClearFormats: {
        const int sheet = f.Int();
        const Range range = f.R();
        if (f.Ok()) r = ClearFormats(sheet, range);
        break;
      }
      case JournalOp::MapStyles: {
        const int sheet = f.Int();
        const Range range = f.R();
        std::map<uint32_t, uint32_t> map;
        for (size_t n = f.Count(); n > 0 && f.Ok(); --n) {
          const uint32_t from = f.U32();
          map[from] = f.U32();
        }
        if (f.Ok())
          r = MapStyles(sheet, range, [&](uint32_t s) {
            auto it = map.find(s);
            return it == map.end() ? s : it->second;
          });
        break;
      }
      case JournalOp::AddXf: {
        Xf x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddXf(x));
        break;
      }
      case JournalOp::AddFont: {
        Font x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddFont(x));
        break;
      }
      case JournalOp::AddFill: {
        Fill x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddFill(x));
        break;
      }
      case JournalOp::AddBorder: {
        Border x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddBorder(x));
        break;
      }
      case JournalOp::AddCellStyle: {
        CellStyle style;
        style.name = f.S();
        style.builtinId = f.Int();
        style.outlineLevel = f.Int();
        style.hidden = f.B();
        style.customBuiltin = f.B();
        Xf x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddCellStyle(style, x));
        break;
      }
      case JournalOp::AddNumFmt: {
        const std::string code = f.S();
        r = same(AddNumFmt(code));
        break;
      }
      case JournalOp::SetRowHeight:
      case JournalOp::SetColumnWidth: {
        const int sheet = f.Int();
        const uint32_t first = f.U32(), last = f.U32();
        const double size = f.D();
        if (f.Ok())
          r = op == JournalOp::SetRowHeight ? SetRowHeight(sheet, first, last, size) : SetColumnWidth(sheet, first, last, size);
        break;
      }
      case JournalOp::HideRows:
      case JournalOp::HideColumns: {
        const int sheet = f.Int();
        const uint32_t first = f.U32(), last = f.U32();
        const bool hidden = f.B();
        if (f.Ok()) r = op == JournalOp::HideRows ? HideRows(sheet, first, last, hidden) : HideColumns(sheet, first, last, hidden);
        break;
      }
      case JournalOp::SetRowOutline:
      case JournalOp::SetColumnOutline: {
        const int sheet = f.Int();
        const uint32_t first = f.U32(), last = f.U32();
        const auto level = static_cast<uint8_t>(f.U32());
        const bool collapsed = f.B();
        if (f.Ok())
          r = op == JournalOp::SetRowOutline ? SetRowOutline(sheet, first, last, level, collapsed)
                                             : SetColumnOutline(sheet, first, last, level, collapsed);
        break;
      }
      case JournalOp::ShiftCells: {
        const int sheet = f.Int();
        Shift sh;
        sh.axis = f.U() ? Shift::Axis::Columns : Shift::Axis::Rows;
        sh.at = f.U32();
        sh.count = static_cast<int32_t>(f.Int());
        sh.band = f.R();
        const std::u16string name = f.S16();
        if (f.Ok()) r = ShiftCells(sheet, sh, name);
        break;
      }
      case JournalOp::SortLines: {
        const int sheet = f.Int();
        const Range range = f.R();
        std::vector<uint32_t> order(f.Count());
        for (uint32_t& o : order) o = f.U32();
        std::vector<SortKey> keys(f.Count());
        for (SortKey& k : keys) k.line = f.U32(), k.descending = f.B();
        const bool rows = f.B();
        if (f.Ok()) r = SortLines(sheet, range, order, keys, rows);
        break;
      }
      case JournalOp::Merge:
      case JournalOp::Unmerge: {
        const int sheet = f.Int();
        const Range range = f.R();
        if (f.Ok()) r = op == JournalOp::Merge ? Merge(sheet, range) : Unmerge(sheet, range);
        break;
      }
      case JournalOp::AddSheet: {
        const int at = f.Int();
        std::string name = f.S();
        if (f.Ok()) r = AddSheet(at, std::move(name));
        break;
      }
      case JournalOp::RenameSheet: {
        const int sheet = f.Int();
        std::string name = f.S();
        if (f.Ok()) r = RenameSheet(sheet, std::move(name));
        break;
      }
      case JournalOp::MoveSheet: {
        const int sheet = f.Int(), to = f.Int();
        if (f.Ok()) r = MoveSheet(sheet, to);
        break;
      }
      case JournalOp::DeleteSheet: {
        const int sheet = f.Int();
        if (f.Ok()) r = DeleteSheet(sheet);
        break;
      }
      case JournalOp::SetTabColor: {
        const int sheet = f.Int();
        const Color color = f.C();
        if (f.Ok()) r = SetTabColor(sheet, color);
        break;
      }
      case JournalOp::SetSheetState: {
        const int sheet = f.Int();
        const auto state = static_cast<SheetState>(f.U());
        if (f.Ok()) r = SetSheetState(sheet, state);
        break;
      }
      case JournalOp::MoveCells: {
        const int sheet = f.Int();
        const Range from = f.R();
        const CellAddr to = f.A();
        std::u16string name = f.S16();
        const int toSheet = f.AtEnd() ? sheet : f.Int();  // the same sheet in the first records
        if (f.Ok()) r = MoveCells(sheet, from, toSheet, to, std::move(name));
        break;
      }
      case JournalOp::SetSheetRules: {
        const int sheet = f.Int();
        const uint32_t ruleKind = f.U32();
        std::string xml(f.S()), ext(f.S());
        std::u16string name = f.S16();
        if (f.Ok()) r = SetSheetRules(sheet, ruleKind ? RuleKind::Validations : RuleKind::ConditionalFormats, xml, ext, std::move(name));
        break;
      }
      case JournalOp::AddTable:
      case JournalOp::SetTable: {
        const int sheet = f.Int();
        const uint64_t index = op == JournalOp::SetTable ? f.U() : 0;
        Table t;
        const std::string xml(f.S());
        const std::string part(f.S());
        // A new table's part is chosen when it's added: read it under a stand-in name.
        if (!f.Ok() || !xlsx::ReadTable(xml, pkg::Limits::Default(), part.empty() ? "/xl/tables/new.xml" : part, t).Ok())
          return damaged;
        t.part = part;
        r = op == JournalOp::AddTable ? AddTable(sheet, std::move(t)) : SetTable(sheet, static_cast<size_t>(index), t);
        break;
      }
      case JournalOp::RemoveTable: {
        const int sheet = f.Int();
        const uint64_t index = f.U();
        if (!f.Ok()) break;
        // The look's formats the step put (Convert to Range), when the record has them.
        BakedStyles baked;
        const uint64_t n = f.U();
        bool given = f.Ok();
        for (uint64_t k = 0; given && k < n; ++k) {
          const uint64_t row = f.U(), col = f.U(), xf = f.U();
          given = f.Ok() && row < kMaxRows && col < kMaxCols && xf <= UINT32_MAX;
          if (given) baked.push_back({CellAddr{static_cast<uint32_t>(row), static_cast<uint32_t>(col)}, static_cast<uint32_t>(xf)});
        }
        r = RemoveTableAs(sheet, static_cast<size_t>(index), given ? &baked : nullptr);
        break;
      }
      case JournalOp::InsertPicture:
      case JournalOp::InsertShape: {
        const int sheet = f.Int();
        const uint64_t shape = op == JournalOp::InsertShape ? f.U() : 0;
        const std::string bytes = f.S();
        drawing::Place p;
        for (drawing::Marker* m : {&p.from, &p.to}) {
          m->col = f.U32();
          m->row = f.U32();
          m->colOff = f.I();
          m->rowOff = f.I();
        }
        p.x = f.I();
        p.y = f.I();
        p.cx = f.I();
        p.cy = f.I();
        if (!f.Ok() || shape > static_cast<uint64_t>(data::ShapeKind::TextBox)) return damaged;
        r = op == JournalOp::InsertPicture ? InsertPicture(sheet, bytes, p, f.S())
                                           : InsertShape(sheet, static_cast<data::ShapeKind>(shape), p, bytes);
        break;
      }
      case JournalOp::SetObjectXml: {
        const int sheet = f.Int();
        const uint32_t id = f.U32();
        const std::string xml = f.S();
        const std::u16string name = f.S16();
        if (f.Ok()) r = SetObjectXml(sheet, id, xml, name);
        break;
      }
      case JournalOp::SetTableFilter: {
        const int sheet = f.Int();
        const uint64_t index = f.U();
        const std::optional<SheetFilter> filter = f.Filter();
        if (f.Ok() && filter) r = SetTableFilter(sheet, static_cast<size_t>(index), *filter);
        else if (f.Ok()) return damaged;
        break;
      }
      case JournalOp::AddDxf: {
        Dxf x;
        if (!xlsx::ReadStyleItem(f.S(), x)) return damaged;
        r = same(AddDxf(x));
        break;
      }
      case JournalOp::InsertChart: {
        Editor::NewChart c;
        const int sheet = f.Int();
        c.kind = static_cast<Editor::ChartKind>(f.U());
        c.data = f.R();
        c.at = f.A();
        c.cols = f.U32();
        c.rows = f.U32();
        c.title = f.S();
        if (f.Ok()) r = InsertChart(sheet, c);
        break;
      }
      case JournalOp::MoveObject: {
        const int sheet = f.Int();
        const uint32_t id = f.U32();
        drawing::Place p;
        for (drawing::Marker* m : {&p.from, &p.to}) {
          m->col = f.U32();
          m->row = f.U32();
          m->colOff = f.I();
          m->rowOff = f.I();
        }
        p.x = f.I();
        p.y = f.I();
        p.cx = f.I();
        p.cy = f.I();
        if (f.Ok()) r = MoveObject(sheet, id, p);
        break;
      }
      case JournalOp::EditChart: {
        const int sheet = f.Int();
        const uint32_t id = f.U32();
        ChartEdit e;
        {
          const bool has = f.U() != 0;
          const uint64_t v = f.U();
          if (has) e.kind = static_cast<ChartKind>(v);
        }
        for (std::optional<ChartText>* t : {&e.title, &e.xTitle, &e.yTitle}) {
          const bool has = f.U() != 0;
          ChartText x;
          x.shown = f.U() != 0;
          x.text = f.S();
          if (has) *t = std::move(x);
        }
        {
          const bool has = f.U() != 0;
          const uint64_t v = f.U();
          if (has) e.legend = static_cast<char>(v);
        }
        {
          const bool has = f.U() != 0;
          const uint64_t v = f.U();
          if (has) e.labels = v != 0;
        }
        e.colors.resize(std::min<size_t>(f.U32(), f.Left()));
        for (ChartColor& c : e.colors) {
          c.series = f.U32();
          c.point = static_cast<int32_t>(f.I());
          c.rgb = f.U32();
        }
        {
          const bool has = f.U() != 0;
          std::vector<ChartSeries> list(std::min<size_t>(f.U32(), f.Left()));
          for (ChartSeries& s : list) {
            s.name = f.S();
            s.values = f.S();
            s.categories = f.S();
          }
          if (has) e.series = std::move(list);
        }
        if (f.Ok()) r = EditChart(sheet, id, e);
        break;
      }
      case JournalOp::MoveChartToSheet: {
        const int sheet = f.Int();
        const uint32_t id = f.U32();
        std::string name = f.S();
        if (f.Ok()) r = MoveChartToSheet(sheet, id, std::move(name));
        break;
      }
      case JournalOp::MoveChartToObject: {
        const int chartSheet = f.Int();
        const int toSheet = f.Int();
        const CellAddr at = f.A();
        if (f.Ok()) r = MoveChartToObject(chartSheet, toSheet, at);
        break;
      }
      case JournalOp::PasteObject: {
        const int sheet = f.Int();
        Editor::ObjectClip clip;
        clip.anchor = f.S();
        clip.book = f.U() ? &book_ : nullptr;
        auto parts = [&](auto&& self, std::vector<Editor::ObjectClip::Part>& out, int depth) -> void {
          out.resize(depth > 4 ? 0 : std::min<size_t>(f.U32(), f.Left()));
          for (Editor::ObjectClip::Part& p : out) {
            p.rel = f.S();
            p.type = f.S();
            p.name = f.S();
            p.contentType = f.S();
            p.bytes = f.S();
            p.external = f.U() != 0;
            p.target = f.S();
            self(self, p.parts, depth + 1);
          }
        };
        parts(parts, clip.parts, 0);
        drawing::Place p;
        for (drawing::Marker* m : {&p.from, &p.to}) {
          m->col = f.U32();
          m->row = f.U32();
          m->colOff = f.I();
          m->rowOff = f.I();
        }
        p.x = f.I();
        p.y = f.I();
        p.cx = f.I();
        p.cy = f.I();
        if (f.Ok()) r = PasteObject(sheet, clip, p);
        break;
      }
      case JournalOp::DeleteObjects: {
        const int sheet = f.Int();
        std::vector<uint32_t> ids(std::min<size_t>(f.U32(), f.Left()));
        for (uint32_t& id : ids) id = f.U32();
        if (f.Ok()) r = DeleteObjects(sheet, ids);
        break;
      }
      case JournalOp::SetFilter: {
        const int sheet = f.Int();
        const std::optional<SheetFilter> filter = f.Filter();
        if (f.Ok()) r = SetFilter(sheet, filter);
        break;
      }
      case JournalOp::SetSheetProtection: {
        const int sheet = f.Int();
        const std::string xml = f.S();
        const std::optional<SheetProtection> p = ReadSheetProtection(xml);
        if (f.Ok() && (xml.empty() || p)) r = SetSheetProtection(sheet, p);
        else if (f.Ok()) return damaged;
        break;
      }
      case JournalOp::SetWorkbookProtection: {
        const std::string xml = f.S();
        const std::optional<WorkbookProtection> p = ReadWorkbookProtection(xml);
        if (f.Ok() && (xml.empty() || p)) r = SetWorkbookProtection(p);
        else if (f.Ok()) return damaged;
        break;
      }
      case JournalOp::SetHyperlink: {
        const int sheet = f.Int();
        const Range at = f.R();
        std::optional<Hyperlink> link;
        if (f.B()) {
          link.emplace();
          link->target = f.S(), link->location = f.S(), link->display = f.S(), link->tooltip = f.S(), link->relId = f.S();
        }
        if (f.Ok()) r = SetHyperlink(sheet, at, link);
        break;
      }
      case JournalOp::SetNote: {
        const int sheet = f.Int();
        const CellAddr cell = f.A();
        std::optional<Note> note;
        if (f.B()) {
          note.emplace();
          note->author = f.S(), note->text = f.S(), note->shown = f.B();
          note->width = f.D(), note->height = f.D();
          for (uint32_t& v : note->anchor) v = f.U32();
        }
        if (f.Ok()) r = SetNote(sheet, cell, note);
        break;
      }
      case JournalOp::SetThread: {
        const int sheet = f.Int();
        const CellAddr cell = f.A();
        std::optional<Thread> thread;
        if (f.B()) {
          thread.emplace();
          thread->comments.resize(f.Count());
          for (ThreadComment& c : thread->comments) c.author = f.S(), c.text = f.S(), c.id = f.S(), c.date = f.S();
          thread->resolved = f.B();
        }
        if (f.Ok()) r = SetThread(sheet, cell, thread);
        break;
      }
      case JournalOp::SetPageSetup: {
        const int sheet = f.Int();
        PageSetup p;
        HeaderFooter h;
        p.paperSize = f.U32(), p.paperWidth = f.S(), p.paperHeight = f.S(), p.scale = f.U32(), p.fitToWidth = f.U32();
        p.fitToHeight = f.U32(), p.firstPageNumber = static_cast<int>(f.Int()), p.useFirstPageNumber = f.B();
        p.orientation = static_cast<Orientation>(f.U32()), p.pageOrder = static_cast<PageOrder>(f.U32());
        p.blackAndWhite = f.B(), p.draft = f.B(), p.usePrinterDefaults = f.B();
        p.cellComments = static_cast<CellComments>(f.U32()), p.errors = static_cast<PrintErrors>(f.U32());
        p.horizontalDpi = f.U32(), p.verticalDpi = f.U32(), p.copies = f.U32(), p.horizontalCentered = f.B();
        p.verticalCentered = f.B(), p.headings = f.B(), p.gridLines = f.B(), p.gridLinesSet = f.B();
        PageMargins& m = p.margins;
        m.left = f.D(), m.right = f.D(), m.top = f.D(), m.bottom = f.D(), m.header = f.D(), m.footer = f.D();
        h.differentOddEven = f.B(), h.differentFirst = f.B(), h.scaleWithDoc = f.B(), h.alignWithMargins = f.B();
        h.oddHeader = f.S(), h.oddFooter = f.S(), h.evenHeader = f.S(), h.evenFooter = f.S(), h.firstHeader = f.S();
        h.firstFooter = f.S();
        const bool fit = f.B();
        if (f.Ok()) r = SetPageSetup(sheet, p, h, fit);
        break;
      }
      case JournalOp::SetPageBreaks: {
        const int sheet = f.Int();
        std::vector<PageBreak> lists[2];
        for (auto& list : lists) {
          list.resize(f.Count());
          for (PageBreak& b : list) b.id = f.U32(), b.min = f.U32(), b.max = f.U32(), b.manual = f.B(), b.pivot = f.B();
        }
        if (f.Ok()) r = SetPageBreaks(sheet, lists[0], lists[1]);
        break;
      }
      case JournalOp::FreezePanes: {
        const int sheet = f.Int();
        const uint32_t rows = f.U32(), cols = f.U32();
        if (f.Ok()) r = FreezePanes(sheet, rows, cols);
        break;
      }
      case JournalOp::SetActiveSheet: {
        const int sheet = f.Int();
        if (f.Ok()) SetActiveSheet(sheet);
        break;
      }
      case JournalOp::SetSelection: {
        const int sheet = f.Int();
        const CellAddr active = f.A();
        std::vector<Range> ranges(f.Count());
        for (Range& x : ranges) x = f.R();
        if (f.Ok()) SetSelection(sheet, active, ranges);
        break;
      }
      case JournalOp::SetTopLeft: {
        const int sheet = f.Int();
        const CellAddr topLeft = f.A();
        if (f.Ok()) SetTopLeft(sheet, topLeft);
        break;
      }
      case JournalOp::SetViewMode: {
        const int sheet = f.Int();
        const uint32_t mode = f.U32();
        if (f.Ok() && mode <= static_cast<uint32_t>(ViewMode::PageLayout)) SetViewMode(sheet, static_cast<ViewMode>(mode));
        break;
      }
      case JournalOp::SetZoom: {
        const int sheet = f.Int();
        const uint32_t percent = f.U32();
        if (f.Ok()) SetZoom(sheet, percent);
        break;
      }
      case JournalOp::SetName: {
        DefinedName n;
        n.name = f.S();
        n.sheet = f.Int();
        n.formula = f.S();
        n.hidden = f.B();
        if (f.Ok()) r = SetName(n);
        break;
      }
      case JournalOp::DeleteName: {
        const std::string name = f.S();
        const int sheet = f.Int();
        if (f.Ok()) r = DeleteName(name, sheet);
        break;
      }
      case JournalOp::BeginGroup: {
        std::u16string name = f.S16();
        if (f.Ok()) BeginGroup(std::move(name));
        break;
      }
      case JournalOp::EndGroup: EndGroup(); break;
      case JournalOp::CancelGroup: CancelGroup(); break;
      case JournalOp::Undo:
      case JournalOp::Redo: {
        const uint64_t steps = f.U();
        if (f.Ok()) r = op == JournalOp::Undo ? Undo(static_cast<size_t>(steps)) : Redo(static_cast<size_t>(steps));
        break;
      }
      case JournalOp::DropHistory: DropHistory(); break;
      default: return damaged;  // a kind this version doesn't know
    }
    if (!f.Ok() || !f.AtEnd()) return damaged;
    if (!r) return r;
    if (applied) *applied = done + 1;
  }
  return {};
}

}  // namespace fw::sheet::edit
