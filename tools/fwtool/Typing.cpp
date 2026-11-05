// fwtool typing: the typing budget (plan §11, "keystroke to screen, 300-page document: < 16 ms
// typical, < 50 ms worst"). Opens a Word document as the app does, lays it all out, then types
// into it a key at a time at its start, middle and end: characters, Enter and Backspace. Each
// keystroke is timed from the edit to the caret's page laid out and the caret found, which is
// what the screen needs (drawing the page is the same as when scrolling). Then formats there, as
// Word's keys and buttons do (bold, centered, Heading 1, bullets) and undoes each, timed the same.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "doc/docx/Docx.h"
#include "doc/edit/Editor.h"
#include "doc/edit/Format.h"
#include "doc/edit/Lists.h"
#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/layout/Layout.h"
#include "text/Fonts.h"

namespace fwtool {
namespace {

using Clock = std::chrono::steady_clock;

const fw::doc::Document* g_doc = nullptr;
std::wstring Substitute(std::wstring_view family) {
  return g_doc ? fw::doc::layout::SubstituteFamily(*g_doc, family) : std::wstring();
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

}  // namespace

int RunTyping(const Options& options) {
  constexpr double kTypical = 16, kWorst = 50;
  static fw::text::Fonts fonts(Substitute);
  int failed = 0;
  std::printf("Typing: each keystroke from the edit to its page laid out and the caret found, in ms.\n"
              "The budget (plan §11) is < %.0f typical (the median) and < %.0f worst, on the typical consumer PC.\n\n",
              kTypical, kWorst);
  std::printf("%-40s %6s %6s %8s %8s %8s %8s %9s %8s %9s %9s\n", "Document", "pages", "keys", "median", "p95", "worst",
              "budget", "rejoined", "idle/key", "format", "worst");
  for (const auto& file : CollectFiles(options.inputs)) {
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    std::unique_ptr<fw::doc::Document> doc = pkg ? fw::doc::docx::Read(std::move(pkg), st) : nullptr;
    if (!doc) {
      std::printf("%-40s can't open: %s\n", Display(file).c_str(), st.message.c_str());
      ++failed;
      continue;
    }
    g_doc = doc.get();
    fonts.ClearSubstitutions();
    fw::doc::layout::Layout layout(*doc, fonts);
    layout.LayOutAll();
    const size_t pages = layout.PageCount();
    fw::doc::edit::Editor ed(*doc);
    ed.OnChange([&](const fw::doc::edit::Change& c) { layout.Edited(c.story, c.first, c.removed, c.added); });
    std::vector<double> ms, formatMs;
    size_t rejoined = 0;  // keystrokes after which one more page rejoined the old ones
    double idle = 0;      // what laying out the rest took, in idle time
    auto key = [&](const std::function<fw::doc::edit::Result()>& edit, fw::doc::Pos& caret) {
      const auto t0 = Clock::now();
      fw::doc::edit::Result r = edit();
      if (!r) return false;
      caret = r.caret;
      layout.CaretAt({caret.para, caret.offset});
      ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
      // Idle time between keys lays the rest out, as the app does (not in the keystroke's time).
      const auto t1 = Clock::now();
      layout.Continue(0);  // a page more: usually where the new layout meets the old pages
      if (layout.Done()) ++rejoined;
      while (layout.Continue(8)) {
      }
      idle += std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
      return true;
    };
    const std::vector<uint32_t> paras = doc->StoryParagraphs(doc->MainStory());
    const std::u16string words = u"The quick brown fox jumps over the lazy dog. ";
    for (double at : {0.02, 0.5, 0.97}) {
      // A paragraph with text near that place in the document.
      size_t i = static_cast<size_t>(at * static_cast<double>(paras.size()));
      while (i < paras.size() && ed.ParaLength(paras[i]) < 20) ++i;
      if (i >= paras.size()) continue;
      fw::doc::Pos caret{paras[i], ed.ParaLength(paras[i]) / 2};
      for (int k = 0; k < 60; ++k) {
        const std::u16string ch(1, words[static_cast<size_t>(k) % words.size()]);
        if (!key([&] { return ed.InsertText(caret, ch); }, caret)) break;
      }
      key([&] { return ed.SplitParagraph(caret); }, caret);
      for (int k = 0; k < 20; ++k) {
        const std::u16string ch(1, words[static_cast<size_t>(k) % words.size()]);
        if (!key([&] { return ed.InsertText(caret, ch); }, caret)) break;
      }
      for (int k = 0; k < 25 && (caret.offset > 0 || k == 20); ++k) {
        fw::doc::Pos from = caret.offset > 0 ? fw::doc::Pos{caret.para, caret.offset - 1}
                                             : fw::doc::Pos{ed.PrevPara(caret.para), ed.ParaLength(ed.PrevPara(caret.para))};
        if (!key([&] { return ed.Delete(from, caret); }, caret)) break;
      }
      // Formatting there, each undone after: a word bold, the paragraph centered, Heading 1, bullets.
      const size_t typed = ms.size();
      const fw::doc::edit::Range word{{caret.para, 0}, {caret.para, std::min<uint32_t>(4, ed.ParaLength(caret.para))}};
      const fw::doc::edit::Range here{caret, caret};
      fw::doc::edit::ParaFormat center;
      center.jc = fw::doc::Justification::Center;
      const std::function<void(fw::doc::edit::TreeEdit&)> commands[] = {
          [&](fw::doc::edit::TreeEdit& te) { fw::doc::edit::ToggleRuns(te, word, fw::doc::edit::Toggle::Bold); },
          [&](fw::doc::edit::TreeEdit& te) { fw::doc::edit::FormatParagraphs(te, here, center); },
          [&](fw::doc::edit::TreeEdit& te) { fw::doc::edit::ApplyStyle(te, here, "heading 1"); },
          [&](fw::doc::edit::TreeEdit& te) { fw::doc::edit::ToggleList(te, here, fw::doc::edit::ListKind::Bullet); },
      };
      for (const auto& command : commands) {
        key(
            [&] {
              fw::doc::edit::TreeEdit te(ed, caret);
              command(te);
              fw::doc::edit::Result r;
              r.caret = caret;
              return r;
            },
            caret);
        key([&] { return ed.Undo(); }, caret);
      }
      formatMs.insert(formatMs.end(), ms.begin() + static_cast<ptrdiff_t>(typed), ms.end());
      ms.resize(typed);
    }
    g_doc = nullptr;
    const double median = Percentile(ms, 0.5), p95 = Percentile(ms, 0.95), worst = Percentile(ms, 1.0);
    // Formatting is a keystroke too (Ctrl+B): the same budget.
    const double formatMedian = Percentile(formatMs, 0.5), formatWorst = Percentile(formatMs, 1.0);
    const bool met = median < kTypical && worst < kWorst && formatMedian < kTypical && formatWorst < kWorst;
    if (!met) ++failed;
    const double keys = ms.empty() ? 1.0 : static_cast<double>(ms.size());
    std::printf("%-40s %6zu %6zu %8.2f %8.2f %8.2f %8s %8.0f%% %8.2f %9.2f %9.2f\n", Display(file).c_str(), pages, ms.size(),
                median, p95, worst, met ? "met" : "MISSED", 100.0 * static_cast<double>(rejoined) / keys, idle / keys,
                formatMedian, formatWorst);
  }
  return options.budgets && failed ? 1 : 0;
}

}  // namespace fwtool
