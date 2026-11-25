#include "text/Fonts.h"

#include "draw/FontSubstitutes.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace fw::text {
namespace {

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
int16_t S16(const uint8_t* p) { return static_cast<int16_t>(U16(p)); }

// A font table's bytes, released when this goes away.
class Table {
 public:
  Table(IDWriteFontFace* face, uint32_t tag) : face_(face) {
    BOOL exists = FALSE;
    const void* data = nullptr;
    UINT32 size = 0;
    if (SUCCEEDED(face->TryGetFontTable(tag, &data, &size, &context_, &exists)) && exists) {
      data_ = static_cast<const uint8_t*>(data);
      size_ = size;
    }
  }
  ~Table() {
    if (context_) face_->ReleaseFontTable(context_);
  }
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  bool Has(size_t bytes) const { return data_ && size_ >= bytes; }
  const uint8_t* Data() const { return data_; }

 private:
  IDWriteFontFace* face_;
  void* context_ = nullptr;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

std::wstring Lower(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
  return out;
}

std::wstring EnglishName(IDWriteLocalizedStrings* names) {
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
  UINT32 length = 0;
  if (FAILED(names->GetStringLength(index, &length))) return {};
  std::wstring name(length + 1, L'\0');
  if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
  name.resize(length);
  return name;
}

std::wstring FamilyName(IDWriteFontFamily* family) {
  ComPtr<IDWriteLocalizedStrings> names;
  return SUCCEEDED(family->GetFamilyNames(&names)) ? EnglishName(names.Get()) : std::wstring();
}

}  // namespace

Face::Face(ComPtr<IDWriteFontFace> face, std::wstring family, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style)
    : face_(std::move(face)), family_(std::move(family)), weight_(weight), style_(style) {
  DWRITE_FONT_METRICS fm{};
  face_->GetMetrics(&fm);
  upem_ = fm.designUnitsPerEm ? fm.designUnitsPerEm : 2048.0f;
  ascent_ = dwAscent_ = fm.ascent;
  descent_ = dwDescent_ = fm.descent;
  lineGap_ = dwLineGap_ = fm.lineGap;
  underlinePos_ = -static_cast<float>(fm.underlinePosition);
  underlineThick_ = fm.underlineThickness;
  strikePos_ = fm.strikethroughPosition;
  strikeThick_ = fm.strikethroughThickness;
  supSize_ = upem_ * 2 / 3;
  supRaise_ = upem_ / 3;
  subSize_ = upem_ * 2 / 3;
  subLower_ = upem_ / 7;

  // GDI's metrics, which Word uses: see FontMetrics.
  Table os2(face_.Get(), DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'));
  Table hhea(face_.Get(), DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'));
  if (os2.Has(78)) {
    const uint8_t* p = os2.Data();
    float winAscent = U16(p + 74), winDescent = U16(p + 76);
    if (winAscent + winDescent > 0) {
      ascent_ = winAscent;
      descent_ = winDescent;
      lineGap_ = 0;
      if (hhea.Has(10)) {
        const uint8_t* h = hhea.Data();
        float hheaHeight = static_cast<float>(S16(h + 4)) - S16(h + 6);
        lineGap_ = std::max(0.0f, S16(h + 8) - ((winAscent + winDescent) - hheaHeight));
      }
    }
    if (S16(p + 20) > 0 && S16(p + 24) > 0) {
      supSize_ = S16(p + 20);
      supRaise_ = S16(p + 24);
    }
    if (S16(p + 12) > 0 && S16(p + 16) > 0) {
      subSize_ = S16(p + 12);
      subLower_ = S16(p + 16);
    }
  }
}

IDWriteFontFace* Face::Plain() const {
  if (face_->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE) return face_.Get();
  if (!plain_) {
    ComPtr<IDWriteFontFace3> face3;
    ComPtr<IDWriteFontFaceReference> ref;
    ComPtr<IDWriteFontFace3> plain;
    if (SUCCEEDED(face_.As(&face3)) && SUCCEEDED(face3->GetFontFaceReference(&ref)) &&
        SUCCEEDED(ref->CreateFontFaceWithSimulations(DWRITE_FONT_SIMULATIONS_NONE, &plain)))
      plain_ = plain;
    else
      plain_ = face_;
  }
  return plain_.Get();
}

FontMetrics Face::Metrics(float size, bool directWrite) const {
  float k = size / upem_;
  FontMetrics m;
  m.ascent = (directWrite ? dwAscent_ : ascent_) * k;
  m.descent = (directWrite ? dwDescent_ : descent_) * k;
  m.lineGap = (directWrite ? dwLineGap_ : lineGap_) * k;
  m.underlinePosition = underlinePos_ * k;
  m.underlineThickness = underlineThick_ * k;
  m.strikethroughPosition = strikePos_ * k;
  m.strikethroughThickness = strikeThick_ * k;
  m.superscriptSize = supSize_ * k;
  m.superscriptRaise = supRaise_ * k;
  m.subscriptSize = subSize_ * k;
  m.subscriptLower = subLower_ * k;
  return m;
}

Face::Face(const Face& base, float widthScale, float height) : Face(base.face_, base.family_, base.weight_, base.style_) {
  widthScale_ = widthScale > 0 ? widthScale : 1;
  const float total = ascent_ + descent_ + lineGap_;
  if (height > 0 && total > 0) {
    const float k = height * upem_ / total;
    ascent_ *= k, descent_ *= k, lineGap_ *= k;
    dwAscent_ *= k, dwDescent_ *= k, dwLineGap_ *= k;
  }
}

float Face::Advance(wchar_t c, float size) const {
  UINT32 cp = c;
  UINT16 glyph = 0;
  if (FAILED(face_->GetGlyphIndices(&cp, 1, &glyph)) || glyph == 0) return 0;
  DWRITE_GLYPH_METRICS gm{};
  if (FAILED(face_->GetDesignGlyphMetrics(&glyph, 1, &gm, FALSE))) return 0;
  return gm.advanceWidth * size / upem_ * widthScale_;
}

Fonts::Fonts(SubstituteFn substitute, IDWriteFontCollection* collection) : substitute_(std::move(substitute)) {
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                 reinterpret_cast<IUnknown**>(factory_.GetAddressOf())))) {
    return;
  }
  if (collection) collection_ = collection;
  else if (FAILED(factory_->GetSystemFontCollection(&collection_))) return;
  if (FAILED(factory_->GetSystemFontFallback(&fallback_))) return;
  ComPtr<IDWriteTextAnalyzer> analyzer;
  if (FAILED(factory_->CreateTextAnalyzer(&analyzer)) || FAILED(analyzer.As(&analyzer_))) analyzer_.Reset();
}

void Fonts::SetPowerPointFallback(bool on) {
  if (on == powerPointFallback_ || !factory_) return;
  ComPtr<IDWriteFontFallback> system, custom;
  if (FAILED(factory_->GetSystemFontFallback(&system))) return;
  if (on) {
    ComPtr<IDWriteFontFallbackBuilder> builder;
    static const DWRITE_UNICODE_RANGE kArabic[] = {
        {0x0600, 0x06FF}, {0x0750, 0x077F}, {0x08A0, 0x08FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
    const wchar_t* tahoma[] = {L"Tahoma"};
    if (!Win32Family(L"Tahoma") || FAILED(factory_->CreateFontFallbackBuilder(&builder)) ||
        FAILED(builder->AddMapping(kArabic, static_cast<UINT32>(std::size(kArabic)), tahoma, 1)) ||
        FAILED(builder->AddMappings(system.Get())) || FAILED(builder->CreateFontFallback(&custom))) {
      return;
    }
  }
  fallback_ = on ? custom : system;
  powerPointFallback_ = on;
}

Fonts::~Fonts() {
  if (memory_) factory_->UnregisterFontFileLoader(memory_.Get());
}

void Fonts::UseDocumentFonts(const std::vector<std::vector<uint8_t>>& files,
                             const std::vector<std::wstring>& documentNames) {
  if (files.empty() && !installed_) return;  // none before, none now
  std::vector<std::pair<std::wstring, std::wstring>> aliases;  // the document's name, the font's family
  if (!installed_) installed_ = collection_;
  ComPtr<IDWriteFontCollection> next = installed_;
  ComPtr<IDWriteFactory5> factory;
  ComPtr<IDWriteFontCollection1> installed;
  ComPtr<IDWriteFontSet> installedSet;
  ComPtr<IDWriteFontSetBuilder1> builder;
  if (!files.empty() && SUCCEEDED(factory_.As(&factory)) && SUCCEEDED(installed_.As(&installed)) &&
      SUCCEEDED(installed->GetFontSet(&installedSet)) && SUCCEEDED(factory->CreateFontSetBuilder(&builder))) {
    if (!memory_ && SUCCEEDED(factory->CreateInMemoryFontFileLoader(&memory_)) &&
        FAILED(factory->RegisterFontFileLoader(memory_.Get())))
      memory_.Reset();
    bool added = false;
    for (size_t k = 0; k < files.size(); ++k) {
      const std::vector<uint8_t>& file = files[k];
      // DirectWrite copies the bytes. A family the PC has keeps its installed fonts, as in Word: a
      // document's copy is often a subset of it.
      ComPtr<IDWriteFontFile> font;
      ComPtr<IDWriteFontSetBuilder1> one;
      ComPtr<IDWriteFontSet> faces;
      if (!memory_ || file.empty() || file.size() > UINT32_MAX ||
          FAILED(memory_->CreateInMemoryFontFileReference(factory.Get(), file.data(), static_cast<UINT32>(file.size()),
                                                          nullptr, &font)) ||
          FAILED(factory->CreateFontSetBuilder(&one)) || FAILED(one->AddFontFile(font.Get())) ||
          FAILED(one->CreateFontSet(&faces)) || faces->GetFontCount() == 0)
        continue;
      bool installedFamily = false;
      for (UINT32 i = 0; i < faces->GetFontCount() && !installedFamily; ++i) {
        ComPtr<IDWriteLocalizedStrings> names;
        BOOL exists = FALSE;
        if (SUCCEEDED(faces->GetPropertyValues(i, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &exists, &names)) && exists) {
          const std::wstring family = EnglishName(names.Get());
          if (i == 0 && k < documentNames.size() && !documentNames[k].empty() && Lower(documentNames[k]) != Lower(family))
            aliases.emplace_back(documentNames[k], family);
          UINT32 index = 0;
          BOOL has = FALSE;
          const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, family.c_str(), L""};
          ComPtr<IDWriteFontSet> matches;
          installedFamily = (SUCCEEDED(installed_->FindFamilyName(family.c_str(), &index, &has)) && has) ||
                            (SUCCEEDED(installedSet->GetMatchingFonts(&property, 1, &matches)) && matches &&
                             matches->GetFontCount() > 0);
        }
      }
      if (!installedFamily && SUCCEEDED(builder->AddFontSet(faces.Get()))) added = true;
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (added && SUCCEEDED(builder->AddFontSet(installedSet.Get())) && SUCCEEDED(builder->CreateFontSet(&set)) &&
        SUCCEEDED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection)))
      next = collection;
  }
  collection_ = next;
  if (collection_ == installed_) installed_.Reset();
  // Everything found in the old collection goes.
  win32_.Reset();
  win32Tried_ = false;
  resolved_.clear();
  adjust_.clear();
  adjusted_.clear();
  faces_.clear();
  faceNames_.clear();
  substitutions_.clear();
  for (const auto& [name, family] : aliases)
    if (!Installed(name) && Installed(family)) resolved_.emplace(Lower(name), family);
}

bool Fonts::Installed(std::wstring_view family) {
  if (family.empty() || !collection_) return false;
  std::wstring name(family);
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (gdiFamilies_) {
    if (Win32Family(name)) return true;
  } else if ((SUCCEEDED(collection_->FindFamilyName(name.c_str(), &index, &exists)) && exists) || Win32Family(name)) {
    return true;
  }
  return false;
}

ComPtr<IDWriteFontSet> Fonts::Win32Family(const std::wstring& family) {
  if (!win32Tried_) {
    win32Tried_ = true;
    ComPtr<IDWriteFontCollection1> collection;
    if (collection_ && SUCCEEDED(collection_.As(&collection))) collection->GetFontSet(&win32_);
  }
  ComPtr<IDWriteFontSet> matches;
  if (!win32_ || family.empty()) return matches;
  const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, family.c_str(), L""};
  if (FAILED(win32_->GetMatchingFonts(&property, 1, &matches)) || !matches || matches->GetFontCount() == 0) matches.Reset();
  return matches;
}

bool Fonts::FullName(const std::wstring& name, FaceName& out) {
  Win32Family(name);  // makes the font set
  ComPtr<IDWriteFontSet> matches;
  ComPtr<IDWriteFontFaceReference> ref;
  ComPtr<IDWriteFontFace3> face;
  ComPtr<IDWriteLocalizedStrings> names;
  const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_FULL_NAME, name.c_str(), L""};
  if (!win32_ || name.empty() || FAILED(win32_->GetMatchingFonts(&property, 1, &matches)) || !matches ||
      matches->GetFontCount() == 0 || FAILED(matches->GetFontFaceReference(0, &ref)) ||
      FAILED(ref->CreateFontFace(&face)) || FAILED(face->GetFamilyNames(&names)))
    return false;
  out.family = EnglishName(names.Get());
  out.weight = face->GetWeight();
  out.italic = face->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
  return !out.family.empty() && Installed(out.family);
}

const Face* Fonts::FaceFor(std::wstring_view requested, bool bold, bool italic) {
  const std::wstring& family = Resolve(requested);
  DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
  if (!faceNames_.empty())
    if (auto it = faceNames_.find(Lower(requested)); it != faceNames_.end()) {
      weight = std::max(weight, it->second.weight);
      italic = italic || it->second.italic;
    }
  const Face* face = Get(family, weight, italic);
  if (!face || adjust_.empty()) return face;
  auto it = adjust_.find(Lower(requested));
  if (it == adjust_.end()) return face;
  std::unique_ptr<Face>& made = adjusted_[{face, it->second.first, it->second.second}];
  if (!made) made = std::make_unique<Face>(*face, it->second.first, it->second.second);
  return made.get();
}

std::wstring_view Fonts::BuiltInSubstitute(std::wstring_view family) {
  std::wstring key = Lower(family);
  const std::span<const draw::FontSubstitute> subs = draw::BuiltInSubstitutes(family);
  return subs.empty() ? std::wstring_view() : std::wstring_view(subs.front().family);
}

const std::wstring& Fonts::Resolve(std::wstring_view family) {
  if (lastResort_.empty()) {
    if (Installed(kLastResort)) {
      lastResort_ = kLastResort;
    } else if (collection_ && collection_->GetFontFamilyCount() > 0) {
      ComPtr<IDWriteFontFamily> first;
      if (SUCCEEDED(collection_->GetFontFamily(0, &first))) lastResort_ = FamilyName(first.Get());
    }
  }
  if (family.empty()) return lastResort_;
  std::wstring key = Lower(family);
  if (auto it = resolved_.find(key); it != resolved_.end()) return it->second;

  std::wstring used;
  FaceName face;
  if (Installed(family)) {
    used = family;
  } else if (FullName(std::wstring(family), face)) {
    // "Times New Roman Bold": that face of an installed family, not a replacement.
    used = face.family;
    faceNames_[key] = std::move(face);
  } else {
    if (substitute_) {
      std::wstring s = substitute_(family);
      if (Installed(s)) used = s;
    }
    if (used.empty()) {
      for (const draw::FontSubstitute& s : draw::BuiltInSubstitutes(family)) {
        if (!Installed(s.family)) continue;
        used = s.family;
        // Its width and line height as measured against Word's renders (draw/FontSubstitutes.h).
        if (s.scale != 1 || s.height > 0) adjust_[key] = {s.scale, s.height};
        break;
      }
    }
    if (used.empty()) used = lastResort_;
    Substitution sub{std::wstring(family), used};
    if (std::find(substitutions_.begin(), substitutions_.end(), sub) == substitutions_.end()) substitutions_.push_back(sub);
  }
  return resolved_.emplace(std::move(key), std::move(used)).first->second;
}

void Fonts::ClearSubstitutions() {
  for (const Substitution& s : substitutions_) {
    resolved_.erase(Lower(s.requested));
    adjust_.erase(Lower(s.requested));
  }
  substitutions_.clear();
}

const Face* Fonts::Get(const std::wstring& installedFamily, DWRITE_FONT_WEIGHT weight, bool italic) {
  auto key = std::make_tuple(Lower(installedFamily), static_cast<int>(weight), italic ? 1 : 0, -1);
  if (auto it = faces_.find(key); it != faces_.end()) return it->second.get();
  std::unique_ptr<Face> result;
  UINT32 index = 0;
  BOOL exists = FALSE;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  ComPtr<IDWriteFontFace> face;
  if (collection_ && SUCCEEDED(collection_->FindFamilyName(installedFamily.c_str(), &index, &exists)) && exists &&
      SUCCEEDED(collection_->GetFontFamily(index, &family)) &&
      SUCCEEDED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
                                             italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)) &&
      SUCCEEDED(font->CreateFontFace(&face))) {
    result = std::make_unique<Face>(face, FamilyName(family.Get()), weight,
                                    italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL);
  } else if (ComPtr<IDWriteFontSet> set = Win32Family(installedFamily)) {
    // As GDI picks within a Win32 family: the face of the right slant with the nearest weight,
    // simulating bold and italic when there's none (Calibri Light has no bold).
    ComPtr<IDWriteFontFaceReference> best;
    ComPtr<IDWriteFontFace3> bestFace;
    int bestScore = INT_MAX;
    for (UINT32 i = 0; i < set->GetFontCount(); ++i) {
      ComPtr<IDWriteFontFaceReference> ref;
      ComPtr<IDWriteFontFace3> candidate;
      if (FAILED(set->GetFontFaceReference(i, &ref)) || FAILED(ref->CreateFontFace(&candidate))) continue;
      const bool slanted = candidate->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
      const int score = (slanted != italic ? 10000 : 0) + std::abs(static_cast<int>(candidate->GetWeight()) - weight);
      if (score < bestScore) {
        bestScore = score;
        best = ref;
        bestFace = candidate;
      }
    }
    if (best) {
      // Fallback finds the face under its DirectWrite family and its own weight and style.
      ComPtr<IDWriteLocalizedStrings> names;
      std::wstring dwFamily = SUCCEEDED(bestFace->GetFamilyNames(&names)) ? EnglishName(names.Get()) : installedFamily;
      const DWRITE_FONT_WEIGHT dwWeight = bestFace->GetWeight();
      const DWRITE_FONT_STYLE dwStyle = bestFace->GetStyle();
      int simulations = DWRITE_FONT_SIMULATIONS_NONE;
      if (weight >= DWRITE_FONT_WEIGHT_SEMI_BOLD && bestFace->GetWeight() < DWRITE_FONT_WEIGHT_SEMI_BOLD)
        simulations |= DWRITE_FONT_SIMULATIONS_BOLD;
      if (italic && bestFace->GetStyle() == DWRITE_FONT_STYLE_NORMAL) simulations |= DWRITE_FONT_SIMULATIONS_OBLIQUE;
      ComPtr<IDWriteFontFace3> simulated;
      if (simulations && SUCCEEDED(best->CreateFontFaceWithSimulations(static_cast<DWRITE_FONT_SIMULATIONS>(simulations), &simulated)))
        bestFace = simulated;
      result = std::make_unique<Face>(bestFace, std::move(dwFamily), dwWeight, dwStyle);
    }
  }
  return faces_.emplace(std::move(key), std::move(result)).first->second.get();
}

const Face* Fonts::Get(IDWriteFont* font) {
  if (!font) return nullptr;
  ComPtr<IDWriteFontFamily> family;
  if (FAILED(font->GetFontFamily(&family))) return nullptr;
  std::wstring name = FamilyName(family.Get());
  int flags = static_cast<int>(font->GetStyle()) | (static_cast<int>(font->GetSimulations()) << 4) |
              (static_cast<int>(font->GetStretch()) << 8);
  auto key = std::make_tuple(Lower(name), static_cast<int>(font->GetWeight()), 2, flags);
  if (auto it = faces_.find(key); it != faces_.end()) return it->second.get();
  std::unique_ptr<Face> result;
  ComPtr<IDWriteFontFace> face;
  if (SUCCEEDED(font->CreateFontFace(&face))) result = std::make_unique<Face>(face, name, font->GetWeight(), font->GetStyle());
  return faces_.emplace(std::move(key), std::move(result)).first->second.get();
}

}  // namespace fw::text
