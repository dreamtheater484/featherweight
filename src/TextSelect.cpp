#include "TextSelect.h"

#include <fpdf_text.h>

#include <algorithm>
#include <cwctype>

#include "Document.h"

namespace textsel {

int CharCount(Document& doc, int page) {
  FPDF_TEXTPAGE tp = doc.TextPage(page);
  return tp ? FPDFText_CountChars(tp) : 0;
}

std::wstring Text(Document& doc, const TextSelection& sel, int maxChars) {
  if (sel.Empty()) return {};
  TextPos a = sel.Start(), b = sel.End();
  std::wstring out;
  for (int p = a.page; p <= b.page && static_cast<int>(out.size()) < maxChars; ++p) {
    FPDF_TEXTPAGE tp = doc.TextPage(p);
    if (!tp) continue;
    int count = FPDFText_CountChars(tp);
    if (count <= 0) continue;
    int from = p == a.page ? a.index : 0;
    int to = p == b.page ? std::min(b.index, count - 1) : count - 1;
    if (to < from) continue;
    int n = to - from + 1;
    std::wstring chunk(static_cast<size_t>(n) + 1, L'\0');
    int written = FPDFText_GetText(tp, from, n, reinterpret_cast<unsigned short*>(chunk.data()));
    chunk.resize(written > 0 ? written - 1 : 0);
    if (!out.empty() && p != a.page) out += L"\r\n";
    out += chunk;
  }
  // PDFium may emit soft hyphens and NUL characters; tidy them up.
  out.erase(std::remove(out.begin(), out.end(), L'\0'), out.end());
  return out;
}

std::vector<FS_RECTF> Rects(Document& doc, const TextSelection& sel, int page) {
  std::vector<FS_RECTF> rects;
  if (!sel.Covers(page)) return rects;
  FPDF_TEXTPAGE tp = doc.TextPage(page);
  if (!tp) return rects;
  int count = FPDFText_CountChars(tp);
  if (count <= 0) return rects;
  TextPos a = sel.Start(), b = sel.End();
  int from = page == a.page ? a.index : 0;
  int to = page == b.page ? std::min(b.index, count - 1) : count - 1;
  if (to < from) return rects;
  int n = FPDFText_CountRects(tp, from, to - from + 1);
  for (int i = 0; i < n; ++i) {
    double l, t, r, bt;
    if (FPDFText_GetRect(tp, i, &l, &t, &r, &bt))
      rects.push_back(FS_RECTF{static_cast<float>(l), static_cast<float>(t), static_cast<float>(r),
                               static_cast<float>(bt)});
  }
  return rects;
}

void WordAt(Document& doc, int page, int index, int& start, int& end) {
  start = end = index;
  FPDF_TEXTPAGE tp = doc.TextPage(page);
  if (!tp) return;
  int count = FPDFText_CountChars(tp);
  auto isWord = [&](int i) {
    unsigned int c = FPDFText_GetUnicode(tp, i);
    return c > 32 && (iswalnum(static_cast<wint_t>(c)) || c == '_' || c == '\'' || c == 0x2019 || c > 0x2E7F);
  };
  if (index < 0 || index >= count || !isWord(index)) return;
  while (start > 0 && isWord(start - 1)) --start;
  while (end + 1 < count && isWord(end + 1)) ++end;
}

}  // namespace textsel
