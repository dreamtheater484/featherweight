#pragma once

#include <dwrite_2.h>
#include <wrl/client.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Fonts: from the family names documents use to DirectWrite font faces (plan §4.4, decision 11).
// Featherweight bundles no fonts. A family that isn't installed is replaced by a substitute (the
// caller's choice first, then a short built-in table, then Times New Roman, as Word does), and
// every replacement is recorded so the viewer can say in a banner that the document may look
// slightly different. Characters a font lacks are drawn with DirectWrite's system font fallback.
//
// Not thread-safe: use one Fonts per thread. Faces stay cached for the object's lifetime.
namespace fw::text {

// Vertical metrics in points at one size. ascent, descent and lineGap are GDI's, which is what
// Word's "auto" line height uses: the OS/2 table's usWinAscent and usWinDescent, plus the external
// leading the hhea table implies (max(0, hhea line gap - (win height - hhea height))). For
// Times New Roman 12 pt that is 13.8 pt in total, for Calibri 11 pt 13.43 pt.
struct FontMetrics {
  float ascent = 0, descent = 0, lineGap = 0;
  float underlinePosition = 0, underlineThickness = 0;  // position: below the baseline, positive down
  float strikethroughPosition = 0, strikethroughThickness = 0;  // position: above the baseline
  float superscriptSize = 0, superscriptRaise = 0;  // from the OS/2 table, as Word sizes w:vertAlign
  float subscriptSize = 0, subscriptLower = 0;
};

// One face of an installed family.
class Face {
 public:
  Face(Microsoft::WRL::ComPtr<IDWriteFontFace> face, std::wstring family);

  IDWriteFontFace* Get() const { return face_.Get(); }
  const std::wstring& Family() const { return family_; }
  FontMetrics Metrics(float size) const;
  // The glyph's advance for one character at `size` points, without shaping; 0 if it's missing.
  float Advance(wchar_t c, float size) const;

 private:
  Microsoft::WRL::ComPtr<IDWriteFontFace> face_;
  std::wstring family_;
  float upem_ = 2048;
  // In design units, from the font's tables (see FontMetrics).
  float ascent_ = 0, descent_ = 0, lineGap_ = 0;
  float underlinePos_ = 0, underlineThick_ = 0, strikePos_ = 0, strikeThick_ = 0;
  float supSize_ = 0, supRaise_ = 0, subSize_ = 0, subLower_ = 0;
};

// A family that a document asked for but that isn't installed, and what was drawn instead.
struct Substitution {
  std::wstring requested;
  std::wstring used;
  bool operator==(const Substitution&) const = default;
};

class Fonts {
 public:
  // Picks a substitute for a family that isn't installed: an installed family, or empty to try
  // the built-in table. The document model passes one built from the file's font table
  // (w:altName, the panose number, the font family class).
  using SubstituteFn = std::function<std::wstring(std::wstring_view family)>;

  // `collection`: the fonts to use; null for the fonts installed on the PC. Tests pass their own.
  explicit Fonts(SubstituteFn substitute = {}, IDWriteFontCollection* collection = nullptr);
  ~Fonts();
  Fonts(const Fonts&) = delete;
  Fonts& operator=(const Fonts&) = delete;

  // False if DirectWrite couldn't be started (it can't fail on Windows 10 and 11).
  bool Ok() const { return analyzer_ != nullptr; }

  bool Installed(std::wstring_view family);
  // The installed family drawn for `family`: itself, a substitute, or the last resort. Replacements
  // are recorded in Substitutions(). An empty name means the last resort, without recording.
  const std::wstring& Resolve(std::wstring_view family);
  // The face of an installed family closest to the weight and style; DirectWrite simulates bold
  // and italic when the family has no such face, as Word does. Null only if the family is gone.
  const Face* Get(const std::wstring& installedFamily, DWRITE_FONT_WEIGHT weight, bool italic);
  // The face for a font DirectWrite's fallback picked.
  const Face* Get(IDWriteFont* font);

  // Every family replaced so far, in the order first seen.
  const std::vector<Substitution>& Substitutions() const { return substitutions_; }
  void ClearSubstitutions() { substitutions_.clear(); }

  // The family drawn when nothing else is installed.
  static constexpr const wchar_t* kLastResort = L"Times New Roman";
  // The built-in table: common families that ship with Office or macOS but not with Windows,
  // and the Windows family closest in widths. Empty if `family` isn't in it.
  static std::wstring_view BuiltInSubstitute(std::wstring_view family);

  IDWriteFactory2* Factory() const { return factory_.Get(); }
  IDWriteTextAnalyzer1* Analyzer() const { return analyzer_.Get(); }
  IDWriteFontFallback* Fallback() const { return fallback_.Get(); }
  IDWriteFontCollection* Collection() const { return collection_.Get(); }

 private:
  Microsoft::WRL::ComPtr<IDWriteFactory2> factory_;
  Microsoft::WRL::ComPtr<IDWriteTextAnalyzer1> analyzer_;
  Microsoft::WRL::ComPtr<IDWriteFontFallback> fallback_;
  Microsoft::WRL::ComPtr<IDWriteFontCollection> collection_;
  SubstituteFn substitute_;
  std::map<std::wstring, std::wstring, std::less<>> resolved_;  // lower-cased request -> family
  std::map<std::tuple<std::wstring, int, int, int>, std::unique_ptr<Face>> faces_;
  std::vector<Substitution> substitutions_;
  std::wstring lastResort_;
};

}  // namespace fw::text
