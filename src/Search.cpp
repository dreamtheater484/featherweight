#include "Search.h"

#include <fpdf_text.h>

#include <algorithm>

#include "Anim.h"
#include "Document.h"

void Search::Start(const std::wstring& query, bool matchCase, int fromPage, int pageCount) {
  query_ = query;
  matchCase_ = matchCase;
  pageCount_ = pageCount;
  startPage_ = std::clamp(fromPage, 0, std::max(0, pageCount - 1));
  scanned_ = 0;
  matches_.clear();
  scannedPages_.assign(pageCount, false);
  current_ = -1;
  userMoved_ = false;
}

void Search::Clear() {
  query_.clear();
  matches_.clear();
  scannedPages_.clear();
  scanned_ = pageCount_ = 0;
  current_ = -1;
}

bool Search::PageScanned(int page) const {
  return page >= 0 && page < static_cast<int>(scannedPages_.size()) && scannedPages_[page];
}

bool Search::Work(Document& doc, double deadlineMs) {
  if (!Active() || Done()) return false;
  bool found = false;
  while (!Done() && anim::Now() < deadlineMs) {
    int page = (startPage_ + scanned_) % pageCount_;
    ++scanned_;
    scannedPages_[page] = true;
    FPDF_TEXTPAGE tp = doc.TextPage(page);
    if (!tp) continue;
    unsigned long flags = matchCase_ ? FPDF_MATCHCASE : 0;
    FPDF_SCHHANDLE h = FPDFText_FindStart(tp, reinterpret_cast<FPDF_WIDESTRING>(query_.c_str()), flags, 0);
    if (!h) continue;
    std::vector<Match> pageMatches;
    while (FPDFText_FindNext(h)) {
      Match m;
      m.page = page;
      m.index = FPDFText_GetSchResultIndex(h);
      m.count = FPDFText_GetSchCount(h);
      if (m.count > 0) pageMatches.push_back(m);
    }
    FPDFText_FindClose(h);
    if (pageMatches.empty()) continue;

    // Keep matches sorted by page; keep the current match stable.
    auto pos = std::lower_bound(matches_.begin(), matches_.end(), page,
                                [](const Match& m, int p) { return m.page < p; });
    int insertAt = static_cast<int>(pos - matches_.begin());
    matches_.insert(pos, pageMatches.begin(), pageMatches.end());
    if (current_ >= insertAt) current_ += static_cast<int>(pageMatches.size());
    if (current_ < 0) current_ = FirstFrom(startPage_);
    found = true;
  }
  return found;
}

int Search::FirstFrom(int page) const {
  if (matches_.empty()) return -1;
  for (int i = 0; i < static_cast<int>(matches_.size()); ++i)
    if (matches_[i].page >= page) return i;
  return 0;
}

int Search::Step(bool forward) {
  if (matches_.empty()) return -1;
  userMoved_ = true;
  int n = static_cast<int>(matches_.size());
  if (current_ < 0) current_ = forward ? 0 : n - 1;
  else current_ = (current_ + (forward ? 1 : n - 1)) % n;
  return current_;
}

const std::vector<FS_RECTF>& Search::RectsFor(Document& doc, Match& m) {
  if (m.hasRects) return m.rects;
  if (FPDF_TEXTPAGE tp = doc.TextPage(m.page)) {
    int n = FPDFText_CountRects(tp, m.index, m.count);
    for (int i = 0; i < n; ++i) {
      double l, t, r, b;
      if (FPDFText_GetRect(tp, i, &l, &t, &r, &b))
        m.rects.push_back(FS_RECTF{static_cast<float>(l), static_cast<float>(t), static_cast<float>(r),
                                   static_cast<float>(b)});
    }
    m.hasRects = true;
  }
  return m.rects;
}
