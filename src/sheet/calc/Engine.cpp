// The dependency graph and recalculation.
//
// Graph: every distinct area a formula reads is one range; ranges a few columns wide are indexed
// per column, wider ones per sheet, each as intervals of rows (an implicit interval tree over an
// array sorted by first row), so finding what depends on a cell costs a few lookups however many
// formulas there are.
//
// Recalculation: a depth-first walk with an explicit stack. Before a formula is calculated, the
// dirty formulas among its precedents are calculated (found through per-column bitmaps of dirty
// cells); a precedent already on the walk is a circular reference. References made at run time
// (OFFSET, INDIRECT) are checked when read: a dirty cell there stops the evaluation, which is
// retried after that cell.

#include "sheet/calc/Engine.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

#include "sheet/calc/Bytecode.h"

namespace fw::sheet::calc {

using namespace bc;

namespace {

int32_t Wrap(int64_t v, int32_t max) {
  int64_t r = v % max;
  return static_cast<int32_t>(r < 0 ? r + max : r);
}

char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

std::string UpperKey(std::string_view s) {
  std::string k(s);
  for (char& c : k) c = Upper(c);
  return k;
}

// Rows [lo, hi] with an id; queries find every interval overlapping a span.
struct Intervals {
  struct E {
    int32_t lo, hi;
    uint32_t id;
  };
  std::vector<E> e;
  std::vector<int32_t> maxHi;

  void Build() {
    std::sort(e.begin(), e.end(), [](const E& a, const E& b) { return a.lo < b.lo; });
    maxHi.assign(e.size(), INT_MIN);
    if (!e.empty()) Fill(0, static_cast<int32_t>(e.size()) - 1);
  }
  int32_t Fill(int32_t l, int32_t r) {
    if (l > r) return INT_MIN;
    int32_t m = l + (r - l) / 2;
    int32_t mx = std::max({e[m].hi, Fill(l, m - 1), Fill(m + 1, r)});
    maxHi[m] = mx;
    return mx;
  }
  template <class F>
  void Query(int32_t lo, int32_t hi, F& fn) const {
    if (!e.empty()) Query(lo, hi, fn, 0, static_cast<int32_t>(e.size()) - 1);
  }
  template <class F>
  void Query(int32_t lo, int32_t hi, F& fn, int32_t l, int32_t r) const {
    while (l <= r) {
      int32_t m = l + (r - l) / 2;
      if (maxHi[m] < lo) return;
      Query(lo, hi, fn, l, m - 1);
      if (e[m].lo > hi) return;
      if (e[m].hi >= lo) fn(e[m].id);
      l = m + 1;  // the right half, without recursing
    }
  }
};

double Seconds() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
}

}  // namespace

// --- cells of dirty formulas: a bitmap per column, with a summary bit per 64 rows ---

class Engine::DirtyCells {
 public:
  size_t count = 0;

  void Set(int s, int32_t r, int32_t c, bool on) {
    uint32_t key = (static_cast<uint32_t>(s) << 16) | static_cast<uint32_t>(c);
    auto it = cols_.find(key);
    if (it == cols_.end()) {
      if (!on) return;
      it = cols_.emplace(key, Bits()).first;
      if (sheetCols_.size() <= static_cast<size_t>(s)) sheetCols_.resize(static_cast<size_t>(s) + 1);
      auto& v = sheetCols_[static_cast<size_t>(s)];
      v.insert(std::upper_bound(v.begin(), v.end(), c), c);
    }
    Bits& b = it->second;
    size_t w = static_cast<size_t>(r) >> 6;
    if (w >= b.w.size()) {
      if (!on) return;
      b.w.resize(w + 1, 0);
      b.sum.resize((w >> 6) + 1, 0);
    }
    uint64_t bit = 1ull << (r & 63);
    bool was = (b.w[w] & bit) != 0;
    if (was == on) return;
    if (on) {
      b.w[w] |= bit;
      b.sum[w >> 6] |= 1ull << (w & 63);
      ++count;
    } else {
      b.w[w] &= ~bit;
      if (!b.w[w]) b.sum[w >> 6] &= ~(1ull << (w & 63));
      --count;
    }
  }

  // fn(row, col) for each marked cell of `a` on sheet s; false from fn stops (and returns false).
  template <class F>
  bool Each(int s, const Area& a, F&& fn) const {
    if (count == 0 || static_cast<size_t>(s) >= sheetCols_.size()) return true;
    const auto& cols = sheetCols_[static_cast<size_t>(s)];
    for (auto ci = std::lower_bound(cols.begin(), cols.end(), static_cast<int32_t>(a.c1));
         ci != cols.end() && *ci <= a.c2; ++ci) {
      const Bits& b = cols_.at((static_cast<uint32_t>(s) << 16) | static_cast<uint32_t>(*ci));
      if (b.w.empty()) continue;
      int32_t last = static_cast<int32_t>(b.w.size()) - 1;
      int32_t w1 = a.r1 >> 6, w2 = std::min(a.r2 >> 6, last);
      for (int32_t wi = w1; wi <= w2;) {
        if ((wi & 63) == 0 && b.sum[static_cast<size_t>(wi) >> 6] == 0) {
          wi += 64;
          continue;
        }
        uint64_t bits = b.w[static_cast<size_t>(wi)];
        if (wi == w1) bits &= ~0ull << (a.r1 & 63);
        if (wi == (a.r2 >> 6) && (a.r2 & 63) != 63) bits &= (1ull << ((a.r2 & 63) + 1)) - 1;
        while (bits) {
          int k = std::countr_zero(bits);
          bits &= bits - 1;
          if (!fn(wi * 64 + k, *ci)) return false;
        }
        ++wi;
      }
    }
    return true;
  }

 private:
  struct Bits {
    std::vector<uint64_t> w, sum;
  };
  std::unordered_map<uint32_t, Bits> cols_;
  std::vector<std::vector<int32_t>> sheetCols_;
};

// --- the dependency graph ---

class Engine::Graph {
 public:
  // Builds the graph from the engine's formulas, until `deadline` (Seconds()): true once built,
  // false to go on in the next call. Abandon() makes the next call start over.
  bool Build(Engine& e, double deadline = 1e300) {
    if (!building_) {
      *this = Graph();
      building_ = true;
    }
    auto& ids = ids_;
    auto& edges = edges_;
    std::vector<Area> areas;
    const bool started = next_ < e.nodes_.size();
    for (uint32_t& n = next_; n < e.nodes_.size(); ++n) {
      if ((n & 255) == 0 && n && Seconds() > deadline) return false;
      Node& node = e.nodes_[n];
      if (node.flags & kFree) continue;
      areas.clear();
      bool volatileName = false;
      e.Precedents(node, areas, &volatileName);
      if (volatileName) node.flags |= kVolatileNode;
      for (const Area& a0 : areas)
        for (int s = a0.s1; s <= a0.s2; ++s) {
          Area a = a0;
          a.s1 = a.s2 = static_cast<uint16_t>(s);
          std::pair<uint64_t, uint64_t> key{(static_cast<uint64_t>(s) << 48) | (static_cast<uint64_t>(a.c1) << 32) |
                                                static_cast<uint32_t>(a.r1),
                                            (static_cast<uint64_t>(a.c2) << 32) | static_cast<uint32_t>(a.r2)};
          auto [it, added] = ids.emplace(key, static_cast<uint32_t>(ranges_.size()));
          if (added) ranges_.push_back(a);
          edges.emplace_back(it->second, n);
        }
    }
    // Indexing takes a slice of its own (a few ms for 50,000 formulas).
    if (started && deadline < 1e300 && e.nodes_.size() > 4096) return false;
    building_ = false;
    next_ = 0;
    ids = {};
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    start_.assign(ranges_.size() + 1, 0);
    for (const auto& ed : edges) ++start_[ed.first + 1];
    for (size_t i = 1; i < start_.size(); ++i) start_[i] += start_[i - 1];
    deps_.resize(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) deps_[i] = edges[i].second;
    edges = {};
    seen_.assign(ranges_.size(), 0);
    // Index the ranges.
    for (uint32_t id = 0; id < ranges_.size(); ++id) {
      const Area& a = ranges_[id];
      if (a.Cols() <= 8) {
        for (int32_t c = a.c1; c <= a.c2; ++c) narrow_[(static_cast<uint32_t>(a.s1) << 16) | static_cast<uint32_t>(c)].e.push_back({a.r1, a.r2, id});
      } else {
        if (wide_.size() <= a.s1) wide_.resize(a.s1 + 1u);
        wide_[a.s1].e.push_back({a.r1, a.r2, id});
      }
    }
    for (auto& [key, iv] : narrow_) {
      iv.Build();
      uint32_t s = key >> 16;
      if (narrowCols_.size() <= s) narrowCols_.resize(s + 1);
      narrowCols_[s].push_back(static_cast<int32_t>(key & 0xFFFF));
    }
    for (auto& v : narrowCols_) std::sort(v.begin(), v.end());
    for (auto& iv : wide_) iv.Build();
    built_ = true;
    return true;
  }
  void Abandon() { building_ = false, built_ = false; }

  // A formula set after the graph was built (an edit): its precedents go to a list searched as
  // well, until there are so many that rebuilding is cheaper. False when the graph must be rebuilt.
  bool Add(Engine& e, uint32_t node) {
    if (!built_ || extra_.size() > 4096) return false;
    std::vector<Area> areas;
    bool volatileName = false;
    e.Precedents(e.nodes_[node], areas, &volatileName);
    if (volatileName) e.nodes_[node].flags |= kVolatileNode;
    for (const Area& a0 : areas)
      for (int s = a0.s1; s <= a0.s2; ++s) {
        Area a = a0;
        a.s1 = a.s2 = static_cast<uint16_t>(s);
        extra_.emplace_back(a, node);
      }
    return true;
  }

  // fn(node) for each formula reading a cell of `a` (on sheet s); a node can come more than once.
  template <class F>
  void Dependents(int s, const Area& a, F&& fn) {
    if (++epoch_ == 0) {
      std::fill(seen_.begin(), seen_.end(), 0);
      epoch_ = 1;
    }
    auto range = [&](uint32_t id) {
      if (seen_[id] == epoch_) return;
      seen_[id] = epoch_;
      const Area& r = ranges_[id];
      if (r.c2 < a.c1 || r.c1 > a.c2) return;
      for (uint32_t k = start_[id]; k < start_[id + 1]; ++k) fn(deps_[k]);
    };
    if (static_cast<size_t>(s) < narrowCols_.size()) {
      const auto& cols = narrowCols_[static_cast<size_t>(s)];
      for (auto ci = std::lower_bound(cols.begin(), cols.end(), static_cast<int32_t>(a.c1));
           ci != cols.end() && *ci <= a.c2; ++ci)
        narrow_.at((static_cast<uint32_t>(s) << 16) | static_cast<uint32_t>(*ci)).Query(a.r1, a.r2, range);
    }
    if (static_cast<size_t>(s) < wide_.size()) wide_[static_cast<size_t>(s)].Query(a.r1, a.r2, range);
    for (const auto& [r, node] : extra_)
      if (r.s1 == s && r.r1 <= a.r2 && r.r2 >= a.r1 && r.c1 <= a.c2 && r.c2 >= a.c1) fn(node);
  }

 private:
  struct KeyHash {
    size_t operator()(const std::pair<uint64_t, uint64_t>& k) const {
      return std::hash<uint64_t>()(k.first * 0x9E3779B97F4A7C15ull ^ k.second);
    }
  };
  // While building: the next node, each range's id, the (range, node) edges.
  bool building_ = false;
  uint32_t next_ = 0;
  std::unordered_map<std::pair<uint64_t, uint64_t>, uint32_t, KeyHash> ids_;
  std::vector<std::pair<uint32_t, uint32_t>> edges_;
  std::vector<Area> ranges_;
  std::vector<uint32_t> start_, deps_;
  std::unordered_map<uint32_t, Intervals> narrow_;
  std::vector<std::vector<int32_t>> narrowCols_;
  std::vector<Intervals> wide_;
  std::vector<uint32_t> seen_;
  bool built_ = false;
  std::vector<std::pair<Area, uint32_t>> extra_;  // formulas set since the build: (precedent, node)
  uint32_t epoch_ = 0;
};

// --- Engine ---

Engine::Engine(Host& host)
    : host_(host), ev_(host, this), graph_(std::make_unique<Graph>()), dirtyCells_(std::make_unique<DirtyCells>()) {}
Engine::~Engine() = default;

void Engine::SetSheets(std::vector<std::string> names) { sheets_ = std::move(names); }

int Engine::FindSheet(std::string_view name) const {
  for (size_t i = 0; i < sheets_.size(); ++i)
    if (EqualText(sheets_[i], name)) return static_cast<int>(i);
  return -1;
}

std::string Engine::SheetName(int sheet) const {
  return sheet >= 0 && static_cast<size_t>(sheet) < sheets_.size() ? sheets_[static_cast<size_t>(sheet)] : "#REF";
}

uint32_t Engine::NameSlot(std::string_view name) {
  auto [it, added] = nameSlots_.emplace(UpperKey(name), static_cast<uint32_t>(names_.size()));
  if (added) names_.push_back({std::string(name), {}});
  return it->second;
}

std::string Engine::SlotName(uint32_t slot) const { return slot < names_.size() ? names_[slot].name : "#NAME?"; }

bool Engine::DefineName(std::string_view name, int scope, std::string_view text) {
  CompileOptions o;
  o.sheet = scope >= 0 ? scope : 0;
  auto f = Compile(text, o, *this);
  if (!f) return false;
  uint32_t slot = NameSlot(name);
  auto& scopes = names_[slot].scopes;
  for (auto& [s, def] : scopes)
    if (s == scope) {
      def = std::move(f);
      GraphChanged();
      return true;
    }
  scopes.emplace_back(scope, std::move(f));
  GraphChanged();
  return true;
}

const Formula* Engine::Name(uint32_t slot, int sheet) {
  if (slot >= names_.size()) return nullptr;
  const Formula* global = nullptr;
  for (const auto& [s, f] : names_[slot].scopes) {
    if (s == sheet) return f.get();
    if (s < 0) global = f.get();
  }
  return global;
}

std::shared_ptr<const Formula> Engine::CompileAt(std::string_view text, const Cell& at, bool array, ParseError* error) {
  CompileOptions o;
  o.sheet = at.sheet, o.row = at.row, o.col = at.col, o.array = array;
  return Compile(text, o, *this, error);
}

uint32_t Engine::NodeAt(int sheet, int32_t row, int32_t col) const {
  auto it = at_.find(Key(sheet, row, col));
  return it == at_.end() ? UINT32_MAX : it->second;
}

void Engine::SetFormula(const Cell& at, std::shared_ptr<const Formula> f, bool cached, const Area* range, bool dynamic) {
  ClearFormula(at);
  uint32_t id;
  if (!free_.empty()) {
    id = free_.back();
    free_.pop_back();
  } else {
    id = static_cast<uint32_t>(nodes_.size());
    nodes_.emplace_back();
  }
  Node& n = nodes_[id];
  n = Node();
  n.f = std::move(f);
  n.at = at;
  if (range) {
    n.out = *range;
  } else {
    n.out.r1 = n.out.r2 = at.row;
    n.out.c1 = n.out.c2 = static_cast<int16_t>(at.col);
  }
  n.out.s1 = n.out.s2 = static_cast<uint16_t>(at.sheet);
  n.flags = static_cast<uint8_t>((cached ? kCached : kFresh) | (dynamic ? kDynamicArray : 0));
  for (int32_t r = n.out.r1; r <= n.out.r2; ++r)
    for (int32_t c = n.out.c1; c <= n.out.c2; ++c) at_[Key(at.sheet, r, c)] = id;
  if (graphStale_ || !graph_->Add(*this, id)) GraphChanged();
}

void Engine::ClearFormula(const Cell& at) {
  uint32_t id = NodeAt(at.sheet, at.row, at.col);
  if (id == UINT32_MAX) return;
  Node& n = nodes_[id];
  if (n.state != kClean) Clean(id);
  for (int32_t r = n.out.r1; r <= n.out.r2; ++r)
    for (int32_t c = n.out.c1; c <= n.out.c2; ++c) at_.erase(Key(n.at.sheet, r, c));
  n = Node();
  n.flags = kFree;
  free_.push_back(id);
  // The graph still names it; a freed node is never marked dirty, and one reused gets its own edges.
  if (graphStale_) GraphChanged();
}

void Engine::ClearFormulas(int sheet, const Area& a) {
  const int64_t cells = static_cast<int64_t>(a.Rows()) * a.Cols();
  if (cells <= static_cast<int64_t>(nodes_.size())) {
    for (int32_t r = a.r1; r <= a.r2; ++r)
      for (int32_t c = a.c1; c <= a.c2; ++c) ClearFormula({sheet, r, c});
    return;
  }
  std::vector<Cell> found;
  for (const Node& n : nodes_)
    if (!(n.flags & kFree) && n.at.sheet == sheet && n.at.row >= a.r1 && n.at.row <= a.r2 && n.at.col >= a.c1 &&
        n.at.col <= a.c2)
      found.push_back(n.at);
  for (const Cell& at : found) ClearFormula(at);
}

void Engine::Touch(const Cell& at) {
  uint32_t id = NodeAt(at.sheet, at.row, at.col);
  if (id != UINT32_MAX) MarkDirty(id);
}

bool Engine::FirstCircular(Cell& out) const {
  for (const Node& n : nodes_)
    if (!(n.flags & kFree) && (n.flags & kCircularNode)) {
      out = n.at;
      return true;
    }
  return false;
}

void Engine::GraphChanged() {
  graphStale_ = true;
  graph_->Abandon();
}

void Engine::EnsureGraph() {
  if (!graphStale_) return;
  graph_->Build(*this);
  graphStale_ = false;
}

bool Engine::BuildGraph(double budgetMs) {
  if (!graphStale_) return true;
  if (!graph_->Build(*this, Seconds() + budgetMs / 1000)) return false;
  graphStale_ = false;
  return true;
}

void Engine::Collect(const uint8_t* code, size_t size, const Cell& at, std::vector<Area>& out, bool* volatileName,
                     int depth) {
  const uint8_t* p = code;
  const uint8_t* end = code + size;
  while (p < end) {
    size_t len = InstrLength(p);
    switch (p[0]) {
      case kRef:
      case kArea: {
        uint8_t fl = p[1];
        const uint8_t* q = p + 2;
        int s1 = at.sheet, s2 = at.sheet;
        if (fl & kSheet) {
          s1 = s2 = Read<uint16_t>(q);
          q += 2;
          if (fl & kSheet3D) {
            s2 = Read<uint16_t>(q);
            q += 2;
          }
        }
        int32_t r1 = Read<int32_t>(q), c1 = Read<int16_t>(q + 4), r2 = r1, c2 = c1;
        bool r2Abs = fl & kRowAbs, c2Abs = fl & kColAbs;
        if (p[0] == kArea) {
          r2 = Read<int32_t>(q + 6), c2 = Read<int16_t>(q + 10);
          r2Abs = fl & kRow2Abs, c2Abs = fl & kCol2Abs;
        }
        if (!(fl & kRowAbs)) r1 = Wrap(static_cast<int64_t>(at.row) + r1, kMaxRows);
        if (!(fl & kColAbs)) c1 = Wrap(static_cast<int64_t>(at.col) + c1, kMaxCols);
        if (!r2Abs) r2 = Wrap(static_cast<int64_t>(at.row) + r2, kMaxRows);
        if (!c2Abs) c2 = Wrap(static_cast<int64_t>(at.col) + c2, kMaxCols);
        if (s1 > s2 || s2 >= static_cast<int>(std::max<size_t>(sheets_.size(), host_.SheetCount()))) break;
        Area a;
        a.r1 = std::min(r1, r2), a.r2 = std::max(r1, r2);
        a.c1 = static_cast<int16_t>(std::min(c1, c2)), a.c2 = static_cast<int16_t>(std::max(c1, c2));
        a.s1 = static_cast<uint16_t>(s1), a.s2 = static_cast<uint16_t>(s2);
        out.push_back(a);
        break;
      }
      case kName: {
        if (depth > 16) break;
        int scope = (p[5] & kSheet) ? Read<uint16_t>(p + 6) : at.sheet;
        if (const Formula* f = Name(Read<uint32_t>(p + 1), scope)) {
          if ((f->flags & Formula::kVolatile) && volatileName) *volatileName = true;
          Collect(f->code.data(), f->code.size(), at, out, volatileName, depth + 1);
        }
        break;
      }
      case kTable: {
        size_t k = 2, l;
        std::string_view table = ReadStr(p + k, &l);
        k += l;
        std::string_view c1 = ReadStr(p + k, &l);
        k += l;
        std::string_view c2 = ReadStr(p + k, &l);
        Area a;
        Err e = Err::None;
        if (host_.Table(table, at.sheet, at.row, at.col, p[1], c1, c2, a, e)) out.push_back(a);
        break;
      }
      default: break;
    }
    p += len;
  }
}

void Engine::Precedents(const Node& n, std::vector<Area>& out, bool* volatileName) {
  Collect(n.f->code.data(), n.f->code.size(), n.at, out, volatileName, 0);
}

void Engine::MarkDirty(uint32_t id) {
  Node& n = nodes_[id];
  if (n.state != kClean || (n.flags & kFree)) return;
  n.state = kDirty;
  ++dirtyCount_;
  for (int32_t r = n.out.r1; r <= n.out.r2; ++r)
    for (int32_t c = n.out.c1; c <= n.out.c2; ++c) dirtyCells_->Set(n.at.sheet, r, c, true);
  pending_.push_back(id);
  changed_.push_back(n.out);
}

void Engine::Clean(uint32_t id) {
  Node& n = nodes_[id];
  if (n.state == kClean) return;
  n.state = kClean;
  --dirtyCount_;
  for (int32_t r = n.out.r1; r <= n.out.r2; ++r)
    for (int32_t c = n.out.c1; c <= n.out.c2; ++c) dirtyCells_->Set(n.at.sheet, r, c, false);
}

void Engine::Propagate() {
  EnsureGraph();
  while (!changed_.empty()) {
    Area a = changed_.back();
    changed_.pop_back();
    for (int s = a.s1; s <= a.s2; ++s) graph_->Dependents(s, a, [&](uint32_t n) { MarkDirty(n); });
  }
}

void Engine::Opened(const CalcSettings& settings) {
  settings_ = settings;
  if (settings.manual) return;
  EnsureGraph();
  for (uint32_t id = 0; id < nodes_.size(); ++id) {
    const Node& n = nodes_[id];
    if (n.flags & kFree) continue;
    bool vol = (n.f->flags & Formula::kVolatile) || (n.flags & kVolatileNode);
    bool sheet = std::find(settings.fullCalcSheets.begin(), settings.fullCalcSheets.end(), n.at.sheet) !=
                 settings.fullCalcSheets.end();
    if (settings.fullCalcOnLoad || sheet || !(n.flags & kCached) || vol) MarkDirty(id);
  }
  Propagate();
}

void Engine::Changed(int sheet, const Area& cells) {
  EnsureGraph();
  Area a = cells;
  a.s1 = a.s2 = static_cast<uint16_t>(sheet);
  for (int32_t r = a.r1; r <= a.r2 && a.Rows() * static_cast<int64_t>(a.Cols()) <= 65536; ++r)
    for (int32_t c = a.c1; c <= a.c2; ++c) {
      uint32_t id = NodeAt(sheet, r, c);
      if (id != UINT32_MAX) MarkDirty(id);
    }
  changed_.push_back(a);
  if (!settings_.manual)
    for (uint32_t id = 0; id < nodes_.size(); ++id) {
      const Node& n = nodes_[id];
      if (!(n.flags & kFree) && ((n.f->flags & Formula::kVolatile) || (n.flags & kVolatileNode))) MarkDirty(id);
    }
  Propagate();
}

void Engine::MarkAllDirty() {
  for (uint32_t id = 0; id < nodes_.size(); ++id) MarkDirty(id);
  changed_.clear();
}

bool Engine::Ready(const Area& a) {
  if (dirtyCells_->count == 0) return true;
  bool ready = true;
  uint32_t self = stack_.empty() ? UINT32_MAX : stack_.back().node;
  for (int s = a.s1; s <= a.s2 && ready; ++s) {
    if (onlySheet_ >= 0 && s != onlySheet_) continue;
    dirtyCells_->Each(s, a, [&](int32_t r, int32_t c) {
      uint32_t id = NodeAt(s, r, c);
      if (id == UINT32_MAX || id == self) {
        if (id == self && id != UINT32_MAX) circular_ = true;
        return true;
      }
      if (nodes_[id].state == kDirty) {
        needed_ = id;
        ready = false;
        return false;
      }
      // On the walk already: a circular reference through a run-time reference.
      circular_ = true;
      if (self != UINT32_MAX) {
        nodes_[self].flags |= kCircularNode;
        circularNodes_.push_back(self);
        loopNodes_.push_back(self);
      }
      return true;
    });
  }
  return ready;
}

bool Engine::Spill(int sheet, int32_t row, int32_t col, Area& out) {
  uint32_t id = NodeAt(sheet, row, col);
  if (id == UINT32_MAX || !(nodes_[id].f->flags & Formula::kArray)) return false;
  out = nodes_[id].out;
  return true;
}

bool Engine::SpillRangeAt(int sheet, int32_t row, int32_t col, Area& out) {
  const uint32_t id = NodeAt(sheet, row, col);
  if (id == UINT32_MAX || !(nodes_[id].flags & kDynamicArray)) return false;
  out = nodes_[id].out;
  return out.r2 > out.r1 || out.c2 > out.c1;
}

bool Engine::Subtotal(int sheet, int32_t row, int32_t col) {
  uint32_t id = NodeAt(sheet, row, col);
  return id != UINT32_MAX && (nodes_[id].f->flags & Formula::kSubtotal);
}

void Engine::Store(uint32_t id, Value v) {
  const Node& n = nodes_[id];
  int s = n.at.sheet;
  if (n.flags & kDynamicArray) return StoreSpill(id, std::move(v));
  auto shown = [](Value x) { return x.IsEmpty() || x.IsMissing() ? Value::Number(0) : x; };
  if (n.out.r1 == n.out.r2 && n.out.c1 == n.out.c2) {
    if (v.IsArray()) v = Value(v.array().at(0, 0));
    host_.Store(s, n.at.row, n.at.col, shown(std::move(v)));
    return;
  }
  // Ctrl+Shift+Enter over a range: one row or column repeats, cells past the result show #N/A.
  for (int32_t r = n.out.r1; r <= n.out.r2; ++r)
    for (int32_t c = n.out.c1; c <= n.out.c2; ++c) {
      int32_t i = r - n.out.r1, j = c - n.out.c1;
      Value x;
      if (v.IsArray()) {
        const Array& a = v.array();
        int32_t ii = a.rows() == 1 ? 0 : i, jj = a.cols() == 1 ? 0 : j;
        x = ii < a.rows() && jj < a.cols() ? shown(a.at(ii, jj)) : Value::Error(Err::NA);
      } else {
        x = shown(v);
      }
      host_.Store(s, r, c, x);
    }
}

namespace {

bool Inside(const Area& a, int32_t r, int32_t c) { return r >= a.r1 && r <= a.r2 && c >= a.c1 && c <= a.c2; }

}  // namespace

// A dynamic array's result spills from its cell over as many cells as it has values, when those are
// free (no value, no other formula); otherwise the cell shows #SPILL! alone. Cells it no longer
// covers are cleared, and formulas reading the cells it covers now or did before are recalculated.
void Engine::StoreSpill(uint32_t id, Value v) {
  Node& n = nodes_[id];
  const int s = n.at.sheet;
  auto shown = [](const Value& x) { return x.IsEmpty() || x.IsMissing() ? Value::Number(0) : x; };
  int32_t rows = 1, cols = 1;
  if (v.IsArray()) rows = v.array().rows(), cols = v.array().cols();
  Area want = n.out;
  want.r1 = want.r2 = n.at.row;
  want.c1 = want.c2 = static_cast<int16_t>(n.at.col);
  bool blocked = n.at.row + static_cast<int64_t>(rows) > kMaxRows || n.at.col + static_cast<int64_t>(cols) > kMaxCols;
  if (!blocked) {
    want.r2 = n.at.row + rows - 1;
    want.c2 = static_cast<int16_t>(n.at.col + cols - 1);
    for (int32_t r = want.r1; r <= want.r2 && !blocked; ++r)
      for (int32_t c = want.c1; c <= want.c2; ++c) {
        if (Inside(n.out, r, c)) continue;
        if (NodeAt(s, r, c) != UINT32_MAX || host_.Cell(s, r, c).type != Type::Empty) {
          blocked = true;
          break;
        }
      }
  }
  if (blocked) {
    want.r2 = want.r1, want.c2 = want.c1;
    v = Value::Error(Err::Spill);
  }
  const Area old = n.out;
  bool moved = old.r1 != want.r1 || old.r2 != want.r2 || old.c1 != want.c1 || old.c2 != want.c2;
  if (moved) {
    bool dirty = n.state != kClean;
    for (int32_t r = old.r1; r <= old.r2; ++r)
      for (int32_t c = old.c1; c <= old.c2; ++c) {
        if (dirty) dirtyCells_->Set(s, r, c, false);
        if (Inside(want, r, c)) continue;
        at_.erase(Key(s, r, c));
        host_.Store(s, r, c, Value());
      }
    for (int32_t r = want.r1; r <= want.r2; ++r)
      for (int32_t c = want.c1; c <= want.c2; ++c) {
        if (dirty) dirtyCells_->Set(s, r, c, true);
        at_[Key(s, r, c)] = id;
      }
    n.out = want;
  }
  for (int32_t r = want.r1; r <= want.r2; ++r)
    for (int32_t c = want.c1; c <= want.c2; ++c) {
      int32_t i = r - want.r1, j = c - want.c1;
      Value x = v.IsArray() ? shown(v.array().at(i, j)) : (i || j) ? Value() : shown(v);
      host_.Store(s, r, c, x);
    }
  if (moved) {
    Area u = want;
    u.r1 = std::min(old.r1, want.r1), u.r2 = std::max(old.r2, want.r2);
    u.c1 = std::min(old.c1, want.c1), u.c2 = std::max(old.c2, want.c2);
    changed_.push_back(u);
    Propagate();
  }
}

// Whether a precedent of `n` is on or after a circular reference: its precedents' cells looked up,
// or the flagged nodes checked against them, whichever is fewer.
bool Engine::AfterLoop(const Node& n, uint32_t id, std::vector<Area>& areas) {
  areas.clear();
  Precedents(n, areas);
  auto flagged = [&](uint32_t m) {
    return m != UINT32_MAX && m != id && !(nodes_[m].flags & kFree) && (nodes_[m].flags & (kCircularNode | kAfterCircular));
  };
  int64_t cells = 0;
  for (const Area& a : areas) cells += static_cast<int64_t>(a.Rows()) * a.Cols() * (a.s2 - a.s1 + 1);
  if (cells <= static_cast<int64_t>(loopNodes_.size())) {
    for (const Area& a : areas)
      for (int s = a.s1; s <= a.s2; ++s)
        for (int32_t r = a.r1; r <= a.r2; ++r)
          for (int32_t c = a.c1; c <= a.c2; ++c)
            if (flagged(NodeAt(s, r, c))) return true;
    return false;
  }
  std::sort(loopNodes_.begin(), loopNodes_.end());
  loopNodes_.erase(std::unique(loopNodes_.begin(), loopNodes_.end()), loopNodes_.end());
  size_t keep = 0;
  for (uint32_t m : loopNodes_)
    if (flagged(m) || m == id) loopNodes_[keep++] = m;
  loopNodes_.resize(keep);
  for (uint32_t m : loopNodes_) {
    if (!flagged(m)) continue;
    const Cell& c = nodes_[m].at;
    for (const Area& a : areas)
      if (c.sheet >= a.s1 && c.sheet <= a.s2 && c.row >= a.r1 && c.row <= a.r2 && c.col >= a.c1 && c.col <= a.c2)
        return true;
  }
  return false;
}

bool Engine::Iterate() {
  // Iterative calculation (calcPr iterate): the cells on circular references again and again,
  // each from the others' last values, until they change by less than the delta.
  std::vector<uint32_t> cycle;
  cycle.swap(circularNodes_);
  std::sort(cycle.begin(), cycle.end());
  cycle.erase(std::unique(cycle.begin(), cycle.end()), cycle.end());
  for (int it = 1; it < settings_.iterateCount; ++it) {
    double change = 0;
    for (uint32_t id : cycle) {
      Node& n = nodes_[id];
      CellView before = host_.Cell(n.at.sheet, n.at.row, n.at.col);
      Value v;
      if (ev_.Evaluate(*n.f, n.at, v) != EvalResult::kDone) continue;
      ++evaluations_;
      if (v.IsNumber() && before.type == Type::Number) change = std::max(change, std::fabs(v.number() - before.num));
      Store(id, std::move(v));
    }
    if (change < settings_.iterateDelta) break;
  }
  return false;
}

bool Engine::Recalculate(double budgetMs) {
  Propagate();
  double start = Seconds();
  int sinceCheck = 0;
  std::vector<Area> areas;
  for (;;) {
    if (stack_.empty()) {
      while (!pending_.empty() && (nodes_[pending_.back()].state != kDirty ||
                                   (onlySheet_ >= 0 && nodes_[pending_.back()].at.sheet != onlySheet_)))
        pending_.pop_back();
      if (pending_.empty()) {
        if (settings_.iterate && !circularNodes_.empty()) Iterate();
        circularNodes_.clear();
        return true;
      }
      stack_.push_back({pending_.back(), false});
      pending_.pop_back();
    }
    uint32_t id = stack_.back().node;
    Node& n = nodes_[id];
    if (n.state == kClean || (n.flags & kFree)) {
      stack_.pop_back();
      continue;
    }
    if (!stack_.back().evaluating) {
      // First its dirty precedents.
      stack_.back().evaluating = true;
      n.state = kVisiting;
      n.flags &= static_cast<uint8_t>(~(kCircularNode | kAfterCircular));
      if (dirtyCells_->count > 0) {
        areas.clear();
        Precedents(n, areas);
        for (const Area& a : areas)
          for (int s = a.s1; s <= a.s2; ++s) {
            if (onlySheet_ >= 0 && s != onlySheet_) continue;
            dirtyCells_->Each(s, a, [&](int32_t r, int32_t c) {
              uint32_t m = NodeAt(s, r, c);
              if (m == UINT32_MAX || m == id) {
                if (m == id && !(n.flags & kCircularNode)) {  // reads its own cell
                  n.flags |= kCircularNode;
                  circularNodes_.push_back(id);
                  loopNodes_.push_back(id);
                  circular_ = true;
                }
                return true;
              }
              Node& p = nodes_[m];
              if (p.state == kDirty) {
                stack_.push_back({m, false});
              } else if (p.state == kVisiting) {  // a circular reference
                circular_ = true;
                if (!(n.flags & kCircularNode)) n.flags |= kCircularNode, circularNodes_.push_back(id);
                if (!(p.flags & kCircularNode)) p.flags |= kCircularNode, circularNodes_.push_back(m);
                loopNodes_.push_back(id), loopNodes_.push_back(m);
              }
              return true;
            });
          }
      }
      continue;
    }
    if (!settings_.iterate) {
      // Excel, iteration off: cells on a circular reference, and after one, aren't calculated;
      // they keep their values, and one just entered shows 0 (measured).
      if (!(n.flags & (kCircularNode | kAfterCircular)) && !loopNodes_.empty() && AfterLoop(n, id, areas))
        n.flags |= kAfterCircular, loopNodes_.push_back(id);
      if (n.flags & (kCircularNode | kAfterCircular)) {
        stack_.pop_back();
        if (n.flags & kFresh) Store(id, Value::Number(0));
        n.flags &= static_cast<uint8_t>(~kFresh);
        Clean(id);
        continue;
      }
    }
    Value v;
    needed_ = UINT32_MAX;
    EvalResult r = ev_.Evaluate(*n.f, n.at, v);
    ++evaluations_;
    n.flags &= static_cast<uint8_t>(~kFresh);
    if (r == EvalResult::kRetry && needed_ != UINT32_MAX && nodes_[needed_].state == kDirty) {
      stack_.push_back({needed_, false});
      continue;
    }
    stack_.pop_back();
    if (r == EvalResult::kDone) {
      n.flags &= static_cast<uint8_t>(~kStale);
      Store(id, std::move(v));
    } else {
      n.flags |= kStale;  // keeps its cached value
    }
    Clean(id);
    if (++sinceCheck >= 64) {
      sinceCheck = 0;
      if ((Seconds() - start) * 1000 > budgetMs) return false;
    }
  }
}

void Engine::CalculateSheet(int sheet) {
  Propagate();
  for (uint32_t id = 0; id < nodes_.size(); ++id) {
    const Node& n = nodes_[id];
    if (n.at.sheet == sheet && !(n.flags & kFree) && ((n.f->flags & Formula::kVolatile) || (n.flags & kVolatileNode)))
      MarkDirty(id);
  }
  Propagate();
  onlySheet_ = sheet;
  Recalculate(1e300);
  onlySheet_ = -1;
  pending_.clear();  // the other sheets' dirty formulas, for F9
  for (uint32_t id = 0; id < nodes_.size(); ++id)
    if (nodes_[id].state == kDirty) pending_.push_back(id);
}

void Engine::CalculateCell(const Cell& at) {
  uint32_t id = NodeAt(at.sheet, at.row, at.col);
  if (id == UINT32_MAX) return;
  Node& n = nodes_[id];
  Value v;
  needed_ = UINT32_MAX;
  EvalResult r = ev_.Evaluate(*n.f, n.at, v);
  ++evaluations_;
  n.flags &= static_cast<uint8_t>(~kFresh);
  if (r == EvalResult::kRetry) return;  // waits for what it needs
  if (r == EvalResult::kDone) {
    n.flags &= static_cast<uint8_t>(~kStale);
    Store(id, std::move(v));
  } else {
    n.flags |= kStale;
  }
  if (n.state != kClean) Clean(id);
}

bool Engine::FormulaText(int sheet, int32_t row, int32_t col, std::string& text) {
  uint32_t id = NodeAt(sheet, row, col);
  if (id == UINT32_MAX) return false;
  const Node& n = nodes_[id];
  if (n.flags & kFree) return false;
  const bool anchor = n.at.sheet == sheet && n.at.row == row && n.at.col == col;
  if (!anchor && (n.flags & kDynamicArray)) return false;
  CompileOptions o;
  o.sheet = n.at.sheet, o.row = n.at.row, o.col = n.at.col;
  o.array = (n.f->flags & Formula::kArray) != 0;
  text = "=" + Print(*n.f, o, *this);
  if (o.array && !(n.flags & kDynamicArray)) text = "{" + text + "}";
  return true;
}

bool Engine::Stale(int sheet, int32_t row, int32_t col) const {
  uint32_t id = NodeAt(sheet, row, col);
  return id != UINT32_MAX && (nodes_[id].flags & kStale);
}

bool Engine::Dirty(int sheet, int32_t row, int32_t col) const {
  uint32_t id = NodeAt(sheet, row, col);
  return id != UINT32_MAX && nodes_[id].state != kClean;
}

size_t Engine::StaleCount() const {
  size_t n = 0;
  for (const Node& node : nodes_) n += (node.flags & kStale) && !(node.flags & kFree) ? 1 : 0;
  return n;
}

}  // namespace fw::sheet::calc
