// Content controls and legacy form fields in the document tab (plan milestone D4), filled in as in
// Word: a click ticks a check box or opens a list's menu, a click in a placeholder selects it and
// typing replaces it, a control emptied shows its placeholder again, and in a form protected for
// filling in, Tab and Shift+Tab go from field to field. Insert ▾ adds the basic controls. Every
// change is fw::doc::edit's (Forms.h), one undo step each.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Commands.h"
#include "doc/edit/Forms.h"
#include "ui/Theme.h"
#include "word/DocView.h"

namespace edit = fw::doc::edit;
namespace layout = fw::doc::layout;
using edit::ContentControl;
using edit::ControlKind;
using edit::Result;

namespace {

fw::doc::Pos ToDoc(layout::TextPos p) { return {p.para, p.offset}; }
layout::TextPos ToText(fw::doc::Pos p) { return {p.para, p.offset}; }
bool Before(fw::doc::Pos a, fw::doc::Pos b) { return a.para != b.para ? a.para < b.para : a.offset < b.offset; }
bool Within(fw::doc::Pos from, fw::doc::Pos to, const edit::Range& r) { return !Before(from, r.from) && !Before(r.to, to); }

// Controls with a menu of choices: Alt+Down opens it, and a click in one that shows its placeholder.
bool HasMenu(ControlKind k) { return k == ControlKind::DropDown || k == ControlKind::ComboBox || k == ControlKind::Date; }
// Controls that are chosen, not typed in.
bool Chosen(ControlKind k) { return k == ControlKind::CheckBox || k == ControlKind::DropDown; }

struct Insertable {
  int cmd;
  ControlKind kind;
  const wchar_t* label;
};
constexpr Insertable kInserts[] = {
    {CmdInsertRichText, ControlKind::RichText, L"Rich text content control"},
    {CmdInsertPlainText, ControlKind::PlainText, L"Plain text content control"},
    {CmdInsertCheckBox, ControlKind::CheckBox, L"Check box content control"},
    {CmdInsertDropDown, ControlKind::DropDown, L"Drop-down list content control"},
    {CmdInsertDatePicker, ControlKind::Date, L"Date picker content control"},
};

std::wstring Wide(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }

// Midnight of a day `days` from today, local time.
fw::doc::DateTime Day(int days) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const int64_t now = static_cast<int64_t>(static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime);
  fw::doc::DateTime d = fw::doc::LocalDateTime(now + days * 864000000000LL);
  d.hour = d.minute = d.second = 0;
  return d;
}

}  // namespace

std::vector<int> DocView::FormCommands() {
  std::vector<int> ids;
  for (const Insertable& i : kInserts) ids.push_back(i.cmd);
  return ids;
}

std::optional<CommandState> DocView::QueryForms(int cmd) {
  for (const Insertable& i : kInserts) {
    if (i.cmd != cmd) continue;
    CommandState s;
    s.label = i.label;
    s.enabled = Editing() && editor_->CanEdit() && !edit::FormsProtected(*doc_);
    return s;
  }
  return std::nullopt;
}

bool DocView::ExecuteForms(int cmd) {
  for (const Insertable& i : kInserts) {
    if (i.cmd != cmd) continue;
    if (!Editing()) return true;
    editor_->BreakTyping();
    // A drop-down list over selected text ("Red, Green, Blue", or one per paragraph): its items, in
    // its place. Word adds them in its Properties dialog.
    std::vector<std::u16string> items;
    if (i.kind == ControlKind::DropDown && HasSelection()) {
      std::u16string item;
      auto take = [&] {
        while (!item.empty() && item.back() == u' ') item.pop_back();
        if (!item.empty()) items.push_back(item);
        item.clear();
      };
      for (wchar_t ch : TextOf(anchor_, focus_, 4000)) {
        if (ch == L',' || ch == L';' || ch == L'\r' || ch == L'\n' || ch == L'\v' || ch == L'\t') take();
        else if (ch != L' ' || !item.empty()) item += static_cast<char16_t>(ch);
      }
      take();
    }
    // Otherwise where the selection starts. The new control shows its placeholder, selected, as in
    // Word (the caret goes after a check box).
    editor_->BeginGroup(u"Content control");
    Result r;
    r.caret = SelFrom();
    if (!items.empty()) r = editor_->Delete(SelFrom(), SelTo());
    if (r) r = edit::InsertControl(*editor_, r.caret, i.kind, items);
    if (r) editor_->EndGroup();
    else editor_->CancelGroup();
    if (Apply(r)) {
      if (auto c = edit::ControlAt(*doc_, {focus_.para, focus_.offset + 1})) {
        anchor_ = ToText(c->kind == ControlKind::CheckBox ? c->content.to : c->content.from);
        focus_ = ToText(c->content.to);
        CaretMoved(true);
        host_.ViewInvalidate();
      }
    }
    return true;
  }
  return false;
}

bool DocView::FormClick(TextPos pos, int x, int y) {
  const fw::doc::Pos at = ToDoc(pos);
  // Whether (x, y) is over a range's text on its line, not beside it (where the caret lands at its edge).
  auto over = [&](const edit::Range& r) {
    auto a = pagination_->CaretAt(ToText(r.from), CaretPage()), b = pagination_->CaretAt(ToText(r.to), CaretPage());
    float px = 0, py = 0;
    if (!a || !b || a->page != b->page || !ToPagePoint(static_cast<int>(a->page), x, y, px, py)) return false;
    return px >= std::min(a->x, b->x) && px <= std::max(a->x, b->x) && py >= a->top && py <= a->bottom;
  };
  auto select = [&](const edit::Range& r) {
    anchor_ = ToText(r.from);
    focus_ = ToText(r.to);
    drag_ = Drag::None;
    goalX_ = -1;
    editor_->BreakTyping();
    CaretMoved(false);
    host_.ViewInvalidate();
  };
  if (auto c = edit::ControlAt(*doc_, at)) {
    if (!c->Fillable() || (Chosen(c->kind) && !over(c->content))) return false;
    if (c->kind == ControlKind::CheckBox) {
      drag_ = Drag::None;
      editor_->BreakTyping();
      Apply(edit::SetChecked(*editor_, *c, !c->checked));
      return true;
    }
    if (!c->placeholder && c->kind != ControlKind::DropDown) return false;  // text in it: the caret goes there
    select(c->content);
    if (HasMenu(c->kind)) FormMenu(c->content.from);
    return true;
  }
  if (!edit::FormsProtected(*doc_)) return false;  // legacy fields are filled in only in a protected form
  auto f = edit::FormFieldAt(*doc_, at);
  if (!f || !f->enabled || f->kind == edit::FormFieldKind::Text || !over({f->start, f->end})) return false;
  if (f->kind == edit::FormFieldKind::CheckBox) {
    drag_ = Drag::None;
    editor_->BreakTyping();
    Apply(edit::SetFormCheckBox(*editor_, *f, !f->checked));
    return true;
  }
  select(f->result);
  FormMenu(f->result.from);
  return true;
}

bool DocView::FormKey(UINT vk, UINT mods) {
  const bool ctrl = (mods & kModCtrl) != 0, alt = (mods & kModAlt) != 0, shift = (mods & kModShift) != 0;
  if (vk == VK_TAB && !ctrl && !alt && edit::FormsProtected(*doc_)) {
    // From field to field, each selected whole; nothing else takes a tab in a protected form.
    if (auto next = edit::NextFillIn(*doc_, SelFrom(), shift)) {
      anchor_ = ToText(next->from);
      focus_ = ToText(next->to);
      goalX_ = -1;
      editor_->BreakTyping();
      CaretMoved(true);
      host_.ViewInvalidate();
    }
    return true;
  }
  if (vk == VK_DOWN && alt && !ctrl && !shift) {
    const fw::doc::Pos at = ToDoc(focus_);
    if (auto c = edit::ControlAt(*doc_, at); c && HasMenu(c->kind) && c->Fillable()) {
      FormMenu(c->content.from);
      return true;
    }
    if (auto f = edit::FormsProtected(*doc_) ? edit::FormFieldAt(*doc_, at) : std::nullopt;
        f && f->enabled && f->kind == edit::FormFieldKind::DropDown) {
      FormMenu(f->result.from);
      return true;
    }
  }
  return false;
}

bool DocView::FormType(std::u16string_view text) {
  const fw::doc::Pos from = SelFrom(), to = SelTo();
  if (auto c = edit::ControlAt(*doc_, from); c && c->Fillable() && Within(from, to, c->content)) {
    if (Chosen(c->kind)) {
      // At its edge, beside it: typed there as anywhere (the editor puts it outside).
      if (!HasSelection() && (from == c->content.from || from == c->content.to)) return false;
      if (c->kind == ControlKind::CheckBox && text == u" ") Apply(edit::SetChecked(*editor_, *c, !c->checked));
      return true;  // a check box or a list isn't typed in
    }
    if (c->placeholder) {
      editor_->BreakTyping();
      Apply(edit::FillControl(*editor_, *c, text));
      return true;
    }
    return false;
  }
  if (!edit::FormsProtected(*doc_)) return false;
  auto f = edit::FormFieldAt(*doc_, from);
  if (!f || !f->enabled || f->kind == edit::FormFieldKind::Text) return false;
  if (f->kind == edit::FormFieldKind::CheckBox && text == u" ") Apply(edit::SetFormCheckBox(*editor_, *f, !f->checked));
  return true;
}

bool DocView::FormDelete(TextPos from, TextPos to) {
  const fw::doc::Pos a = ToDoc(std::min(from, to)), b = ToDoc(std::max(from, to));
  auto c = edit::ControlAt(*doc_, a);
  if (!c || !c->Fillable() || !Within(a, b, c->content) || !Before(c->content.from, c->content.to)) return false;
  // A placeholder stays while its control is empty; a check box and a list keep their text.
  if (c->placeholder || Chosen(c->kind)) return true;
  // All of its text: its placeholder comes back, in the same step.
  if (a == c->content.from && b == c->content.to) {
    editor_->BreakTyping();
    Apply(edit::ShowPlaceholder(*editor_, *c));
    return true;
  }
  return false;
}

void DocView::FormMenu(fw::doc::Pos at) {
  // Not over a window that isn't shown (the visual checks draw one off screen): nothing would close it.
  if (!IsWindowVisible(host_.ViewHwnd())) return;
  std::optional<ContentControl> c = edit::ControlAt(*doc_, at);
  std::optional<edit::FormField> f;
  if (!c || !HasMenu(c->kind) || !c->Fillable()) {
    c.reset();
    if (edit::FormsProtected(*doc_)) f = edit::FormFieldAt(*doc_, at);
    if (!f || !f->enabled || f->kind != edit::FormFieldKind::DropDown) return;
  }
  HMENU menu = CreatePopupMenu();
  std::vector<fw::doc::DateTime> days;
  if (c && c->kind == ControlKind::Date) {
    const std::u16string picture = c->dateFormat.empty() ? u"d MMMM yyyy" : c->dateFormat;
    const std::pair<int, const wchar_t*> choices[] = {{0, L"Today"}, {-1, L"Yesterday"}, {1, L"Tomorrow"}};
    for (const auto& [offset, name] : choices) {
      days.push_back(Day(offset));
      const std::wstring label = std::wstring(name) + L"\t" + Wide(fw::doc::FormatDateTime(days.back(), picture, false, c->dateLang));
      AppendMenuW(menu, MF_STRING | (c->date == days.back() ? MF_CHECKED : 0), days.size(), label.c_str());
    }
  } else if (c) {
    const std::u16string shown = c->placeholder ? std::u16string() : doc_->Para(c->content.from.para).text.substr(
        c->content.from.offset, c->content.from.para == c->content.to.para ? c->content.to.offset - c->content.from.offset : 0);
    for (size_t i = 0; i < c->items.size(); ++i) {
      const std::u16string& text = c->items[i].text.empty() ? std::u16string(c->items[i].value.begin(), c->items[i].value.end())
                                                            : c->items[i].text;
      AppendMenuW(menu, MF_STRING | (!shown.empty() && text == shown ? MF_CHECKED : 0), i + 1, Wide(text).c_str());
    }
  } else {
    for (size_t i = 0; i < f->entries.size(); ++i)
      AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == f->selected ? MF_CHECKED : 0), i + 1, Wide(f->entries[i]).c_str());
  }
  // Under the control's first character.
  POINT pt;
  GetCursorPos(&pt);
  const fw::doc::Pos start = c ? c->content.from : f->result.from;
  if (auto box = pagination_->CaretAt(ToText(start), CaretPage())) {
    const D2D1_RECT_F r = PageRectToScreen(static_cast<int>(box->page), {box->x, box->top, box->x, box->bottom});
    pt = {static_cast<LONG>(r.left), static_cast<LONG>(r.bottom + 2 * ui_)};
    ClientToScreen(host_.ViewHwnd(), &pt);
  }
  theme::ApplyToMenus();
  const int pick = GetMenuItemCount(menu) > 0
                       ? TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y,
                                          host_.ViewHwnd(), nullptr)
                       : 0;
  DestroyMenu(menu);
  if (pick <= 0) return;
  editor_->BreakTyping();
  const size_t i = static_cast<size_t>(pick - 1);
  if (!days.empty()) Apply(edit::PickDate(*editor_, *c, days[i]));
  else if (c) Apply(edit::PickItem(*editor_, *c, i));
  else Apply(edit::PickFormEntry(*editor_, *f, i));
}
