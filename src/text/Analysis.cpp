#include "text/Analysis.h"

#include <algorithm>

namespace fw::text::detail {
namespace {

class Sink final : public IDWriteTextAnalysisSink {
 public:
  explicit Sink(Analysis& out) : out_(out) {}
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (riid == __uuidof(IDWriteTextAnalysisSink) || riid == __uuidof(IUnknown)) {
      *object = static_cast<IDWriteTextAnalysisSink*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 position, UINT32 length,
                                              const DWRITE_SCRIPT_ANALYSIS* script) override {
    out_.scripts.push_back({position, position + length, *script});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32 position, UINT32 length,
                                               const DWRITE_LINE_BREAKPOINT* breaks) override {
    if (position + length > out_.breaks.size()) return E_INVALIDARG;
    std::copy(breaks, breaks + length, out_.breaks.begin() + position);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 position, UINT32 length, UINT8, UINT8 resolvedLevel) override {
    if (position + length > out_.levels.size()) return E_INVALIDARG;
    std::fill(out_.levels.begin() + position, out_.levels.begin() + position + length, resolvedLevel);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32 position, UINT32 length, IDWriteNumberSubstitution*) override {
    if (position + length > out_.substituted.size()) return E_INVALIDARG;
    std::fill(out_.substituted.begin() + position, out_.substituted.begin() + position + length, uint8_t{1});
    return S_OK;
  }

 private:
  Analysis& out_;
};

}  // namespace

TextSource::TextSource(std::wstring_view text, bool rightToLeft, const std::vector<LocaleRange>& locales,
                       IDWriteNumberSubstitution* numbers)
    : text_(text), rtl_(rightToLeft), locales_(locales), numbers_(numbers) {}

HRESULT TextSource::QueryInterface(REFIID riid, void** object) {
  if (riid == __uuidof(IDWriteTextAnalysisSource) || riid == __uuidof(IUnknown)) {
    *object = static_cast<IDWriteTextAnalysisSource*>(this);
    return S_OK;
  }
  *object = nullptr;
  return E_NOINTERFACE;
}

HRESULT TextSource::GetTextAtPosition(UINT32 position, const WCHAR** text, UINT32* length) {
  if (position >= text_.size()) {
    *text = nullptr;
    *length = 0;
  } else {
    *text = text_.data() + position;
    *length = static_cast<UINT32>(text_.size() - position);
  }
  return S_OK;
}

HRESULT TextSource::GetTextBeforePosition(UINT32 position, const WCHAR** text, UINT32* length) {
  if (position == 0 || position > text_.size()) {
    *text = nullptr;
    *length = 0;
  } else {
    *text = text_.data();
    *length = position;
  }
  return S_OK;
}

DWRITE_READING_DIRECTION TextSource::GetParagraphReadingDirection() {
  return rtl_ ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

HRESULT TextSource::GetLocaleName(UINT32 position, UINT32* length, const WCHAR** locale) {
  for (const auto& r : locales_) {
    if (position >= r.start && position < r.end) {
      *length = r.end - position;
      *locale = r.locale;
      return S_OK;
    }
  }
  *length = static_cast<UINT32>(text_.size() > position ? text_.size() - position : 0);
  *locale = L"";
  return S_OK;
}

HRESULT TextSource::GetNumberSubstitution(UINT32 position, UINT32* length, IDWriteNumberSubstitution** numbers) {
  *length = static_cast<UINT32>(text_.size() > position ? text_.size() - position : 0);
  *numbers = numbers_;
  if (numbers_) numbers_->AddRef();
  return S_OK;
}

bool NeedsBidi(wchar_t c) {
  return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) ||
         c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
         // Surrogates of the right-to-left planes' blocks (U+10800-U+10FFF, U+1E800-U+1EFFF).
         (c >= 0xD802 && c <= 0xD803) || (c >= 0xD83A && c <= 0xD83B);
}

Analysis Analyze(IDWriteTextAnalyzer* analyzer, std::wstring_view text, bool rightToLeft,
                 const std::vector<LocaleRange>& locales, IDWriteNumberSubstitution* numbers) {
  Analysis out;
  const auto n = static_cast<UINT32>(text.size());
  out.levels.assign(n, rightToLeft ? 1 : 0);
  out.breaks.assign(n, DWRITE_LINE_BREAKPOINT{});
  out.substituted.assign(n, 0);
  if (n == 0) return out;
  TextSource source(text, rightToLeft, locales, numbers);
  Sink sink(out);
  if (FAILED(analyzer->AnalyzeScript(&source, 0, n, &sink))) out.scripts.clear();
  std::sort(out.scripts.begin(), out.scripts.end(), [](const ScriptRun& a, const ScriptRun& b) { return a.start < b.start; });
  if (out.scripts.empty()) out.scripts.push_back({0, n, DWRITE_SCRIPT_ANALYSIS{}});
  analyzer->AnalyzeLineBreakpoints(&source, 0, n, &sink);
  if (rightToLeft || std::any_of(text.begin(), text.end(), NeedsBidi)) analyzer->AnalyzeBidi(&source, 0, n, &sink);
  if (numbers) analyzer->AnalyzeNumberSubstitution(&source, 0, n, &sink);
  return out;
}

}  // namespace fw::text::detail
