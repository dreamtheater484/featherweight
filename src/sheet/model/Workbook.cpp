#include "sheet/model/Workbook.h"

#include <windows.h>

#include <algorithm>

#include "core/Strings.h"
#include "draw/Theme.h"

namespace fw::sheet {
namespace {

bool SameNameNoCase(std::string_view a, std::string_view b) {
  if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
      }))
    return true;
  bool ascii = std::all_of(a.begin(), a.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }) &&
               std::all_of(b.begin(), b.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
  if (ascii) return false;
  std::wstring wa = Utf8ToWide(a), wb = Utf8ToWide(b);
  return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()), wb.c_str(), static_cast<int>(wb.size()), TRUE) ==
         CSTR_EQUAL;
}

std::string FontKey(const Font& f) {
  std::string k = f.name;
  k += '\0';
  k += std::to_string(f.size);
  k += static_cast<char>('0' + f.bold + 2 * f.italic + 4 * f.strike + 8 * f.outline + 16 * f.shadow + 32 * f.condense);
  k += static_cast<char>('0' + f.extend);
  k += static_cast<char>('0' + static_cast<int>(f.underline));
  k += static_cast<char>('0' + static_cast<int>(f.vertAlign));
  k += static_cast<char>('0' + static_cast<int>(f.scheme));
  k += static_cast<char>('0' + static_cast<int>(f.color.kind));
  k += std::to_string(f.color.value) + ',' + std::to_string(f.color.tint) + ',' + std::to_string(f.family) + ',' +
       std::to_string(f.charset);
  return k;
}

}  // namespace

// ---- StringTable

std::span<const RichRun> StringTable::Runs(uint32_t id) const {
  auto it = std::lower_bound(rich_.begin(), rich_.end(), id, [](const Rich& r, uint32_t v) { return r.id < v; });
  if (it == rich_.end() || it->id != id || it->runCount == 0) return {};
  return std::span<const RichRun>(runs_.data() + it->firstRun, it->runCount);
}

std::string_view StringTable::RawXml(uint32_t id) const {
  auto it = std::lower_bound(rich_.begin(), rich_.end(), id, [](const Rich& r, uint32_t v) { return r.id < v; });
  if (it == rich_.end() || it->id != id || it->raw == kNone) return {};
  return raw_.Get(it->raw);
}

uint32_t StringTable::Add(std::string_view text) { return AddRead(text, 0, {}, {}); }

uint32_t StringTable::AddRead(std::string_view text, uint8_t flags, std::string_view rawXml, std::vector<RichRun> runs) {
  uint32_t id = text_.Add(text);
  flags_.push_back(flags);
  if (!rawXml.empty() || !runs.empty()) {
    Rich r;
    r.id = id;
    r.raw = rawXml.empty() ? kNone : raw_.Add(rawXml);
    r.firstRun = static_cast<uint32_t>(runs_.size());
    r.runCount = static_cast<uint32_t>(runs.size());
    runs_.insert(runs_.end(), runs.begin(), runs.end());
    rich_.push_back(r);
  }
  return id;
}

uint32_t StringTable::AddFont(const Font& f) {
  auto [it, added] = fontIndex_.try_emplace(FontKey(f), static_cast<uint32_t>(fonts_.size()));
  if (added) fonts_.push_back(f);
  return it->second;
}

size_t StringTable::Bytes() const {
  return text_.Bytes() + flags_.capacity() + rich_.capacity() * sizeof(Rich) + runs_.capacity() * sizeof(RichRun) +
         raw_.Bytes() + fonts_.capacity() * sizeof(Font);
}

// ---- Workbook

Workbook::Workbook() = default;
Workbook::~Workbook() = default;

int Workbook::FindSheet(std::string_view name) const {
  for (size_t i = 0; i < sheets_.size(); ++i)
    if (SameNameNoCase(sheets_[i]->Name(), name)) return static_cast<int>(i);
  return -1;
}

const DefinedName* Workbook::FindName(std::string_view name, int sheet) const {
  if (sheet >= 0)
    for (const auto& n : names_)
      if (n.sheet == sheet && SameNameNoCase(n.name, name)) return &n;
  for (const auto& n : names_)
    if (n.sheet < 0 && SameNameNoCase(n.name, name)) return &n;
  return nullptr;
}

const std::shared_ptr<const draw::Theme>& Workbook::Theme() const {
  return theme_ ? theme_ : draw::Theme::Default();
}

pkg::Status Workbook::LoadSheet(size_t i) {
  if (i >= sheets_.size() || sheets_[i]->Loaded() || !loader_) return {};
  return loader_(*this, i);
}

pkg::Status Workbook::LoadAll() {
  for (size_t i = 0; i < sheets_.size(); ++i) {
    pkg::Status st = LoadSheet(i);
    if (!st.Ok()) return st;
  }
  return {};
}

Sheet& Workbook::AddSheet(std::string name) {
  sheets_.push_back(std::make_unique<Sheet>(*this, std::move(name)));
  sheets_.back()->loaded_ = true;
  return *sheets_.back();
}

std::string Workbook::ResolveRel(std::string_view sourcePart, std::string_view relId) const {
  if (!package_) return {};
  const pkg::RelationshipSet* rels = package_->RelationshipsOf(sourcePart);
  const pkg::Relationship* r = rels ? rels->Find(relId) : nullptr;
  return r && !r->external ? r->targetPart : std::string();
}

size_t Workbook::ModelBytes() const {
  size_t n = sizeof(Workbook) + strings_.Bytes();
  for (const auto& s : sheets_) n += s->ModelBytes();
  const Styles& st = styles_;
  n += (st.CellXfs().capacity() + st.CellStyleXfs().capacity()) * sizeof(Xf) + st.Fonts().capacity() * sizeof(Font) +
       st.Fills().capacity() * sizeof(Fill) + st.Borders().capacity() * sizeof(Border);
  return n;
}

void WorkbookLoader::SetSheetInfo(Sheet& s, SheetKind kind, SheetState state, uint32_t sheetId, std::string part,
                                  std::string relId) {
  s.kind_ = kind;
  s.state_ = state;
  s.sheetId_ = sheetId;
  s.part_ = std::move(part);
  s.relId_ = std::move(relId);
}

void WorkbookLoader::MarkLoaded(Sheet& s, bool loaded) { s.loaded_ = loaded; }

}  // namespace fw::sheet
