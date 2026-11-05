#pragma once

#include <dwrite_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The text analyzer's side of line layout: a paragraph's scripts, bidi levels, line-break
// opportunities and number substitution, from IDWriteTextAnalyzer. Internal to fwtext.
namespace fw::text::detail {

struct LocaleRange {
  uint32_t start = 0, end = 0;
  const wchar_t* locale = L"";
};

// Text for the analyzer and for font fallback. Lives on the stack for the calls it's passed to;
// DirectWrite doesn't keep it.
class TextSource final : public IDWriteTextAnalysisSource {
 public:
  TextSource(std::wstring_view text, bool rightToLeft, const std::vector<LocaleRange>& locales,
             IDWriteNumberSubstitution* numbers);
  TextSource(const TextSource&) = delete;
  TextSource& operator=(const TextSource&) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** text, UINT32* length) override;
  HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** text, UINT32* length) override;
  DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override;
  HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* length, const WCHAR** locale) override;
  HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 position, UINT32* length,
                                                  IDWriteNumberSubstitution** numbers) override;

 private:
  std::wstring_view text_;
  bool rtl_;
  const std::vector<LocaleRange>& locales_;
  IDWriteNumberSubstitution* numbers_;
};

struct ScriptRun {
  uint32_t start = 0, end = 0;
  DWRITE_SCRIPT_ANALYSIS script{};
};

struct Analysis {
  std::vector<ScriptRun> scripts;               // in order, covering the text
  std::vector<uint8_t> levels;                  // per character
  std::vector<DWRITE_LINE_BREAKPOINT> breaks;   // per character
  std::vector<uint8_t> substituted;             // per character: digits are substituted
};

// Runs the analyzer over `text`. Bidi levels are only analyzed when the paragraph is right to
// left or holds right-to-left characters; otherwise they're all 0.
Analysis Analyze(IDWriteTextAnalyzer* analyzer, std::wstring_view text, bool rightToLeft,
                 const std::vector<LocaleRange>& locales, IDWriteNumberSubstitution* numbers);

// Whether a character is right to left, or a bidi control, so the bidi algorithm is needed.
bool NeedsBidi(wchar_t c);

}  // namespace fw::text::detail
