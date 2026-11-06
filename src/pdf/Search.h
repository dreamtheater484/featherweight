#pragma once

#include <fpdfview.h>

#include <string>
#include <vector>

class Document;

// Incremental full-text search. Pages are scanned a few at a time from the current page,
// wrapping around, so typing in the search box never blocks the window.
class Search {
 public:
  struct Match {
    int page = 0;
    int index = 0;  // first character
    int count = 0;  // number of characters
    std::vector<FS_RECTF> rects;  // page coordinates, filled lazily
    bool hasRects = false;
  };

  void Start(const std::wstring& query, bool matchCase, int fromPage, int pageCount);
  void Clear();
  bool Active() const { return !query_.empty(); }
  bool Done() const { return scanned_ >= pageCount_; }
  const std::wstring& Query() const { return query_; }
  bool MatchCase() const { return matchCase_; }

  // Scans pages until the deadline. Returns true if new matches were found.
  bool Work(Document& doc, double deadlineMs);

  std::vector<Match>& Matches() { return matches_; }
  int Current() const { return current_; }
  void SetCurrent(int i) { current_ = i; }
  // Moves to the next/previous match; returns its index or -1.
  int Step(bool forward);
  // Picks the first match at or after `page` (used when results first arrive).
  int FirstFrom(int page) const;
  const std::vector<FS_RECTF>& RectsFor(Document& doc, Match& m);
  bool PageScanned(int page) const;

 private:
  std::wstring query_;
  bool matchCase_ = false;
  int pageCount_ = 0;
  int startPage_ = 0;
  int scanned_ = 0;
  std::vector<Match> matches_;
  std::vector<bool> scannedPages_;
  int current_ = -1;
  bool userMoved_ = false;
};
