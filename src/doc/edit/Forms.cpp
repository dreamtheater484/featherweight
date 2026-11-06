#include "doc/edit/Forms.h"

#include <windows.h>

#include <algorithm>
#include <unordered_set>

#include "doc/edit/Styles.h"
#include "doc/edit/TreeEdit.h"
#include "doc/model/Internal.h"
#include "doc/model/Utf.h"

namespace fw::doc::edit {
using namespace names;
namespace {

constexpr uint32_t kSdtPr = Name("sdtPr"), kAlias = Name("alias"), kTag = Name("tag"), kLock = Name("lock");
constexpr uint32_t kShowing = Name("showingPlcHdr"), kBinding = Name("dataBinding"), kText = Name("text");
constexpr uint32_t kRichText = Name("richText"), kDate = Name("date"), kDropDown = Name("dropDownList");
constexpr uint32_t kCombo = Name("comboBox"), kPicture = Name("picture"), kListItem = Name("listItem");
constexpr uint32_t kDisplayText = Name("displayText"), kValue = Name("value"), kLastValue = Name("lastValue");
constexpr uint32_t kFullDate = Name("fullDate"), kDateFormat = Name("dateFormat"), kLid = Name("lid");
constexpr uint32_t kCheckbox = Name("w14:checkbox"), kW14Checked = Name("w14:checked");
constexpr uint32_t kCheckedState = Name("w14:checkedState"), kUncheckedState = Name("w14:uncheckedState");
constexpr uint32_t kW14Val = Name("w14:val"), kW14Font = Name("w14:font");
constexpr uint32_t kStoreAs = Name("storeMappedDataAs"), kCalendar = Name("calendar");
constexpr uint32_t kFFData = Name("ffData"), kFFName = Name("name"), kEnabled = Name("enabled");
constexpr uint32_t kCheckBox = Name("checkBox"), kDefault = Name("default"), kChecked = Name("checked");
constexpr uint32_t kDDList = Name("ddList"), kResult = Name("result"), kListEntry = Name("listEntry");
constexpr uint32_t kTextInput = Name("textInput"), kMaxLength = Name("maxLength");
constexpr uint32_t kRStyle = Name("rStyle"), kRFonts = Name("rFonts");

constexpr std::u16string_view kProtected = kFormsProtected;

std::u16string U16(std::string_view s) { return ToUtf16(s); }

std::string XmlEscapeAttr(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// The user's short date picture ("d-M-yyyy") and language ("nl-NL"), for a new date control.
std::u16string ShortDatePicture() {
  wchar_t b[80] = {};
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTDATE, b, 80) > 1) return std::u16string(b, b + wcslen(b));
  return u"M/d/yyyy";
}
std::string UserLanguage() {
  wchar_t b[LOCALE_NAME_MAX_LENGTH] = {};
  if (GetUserDefaultLocaleName(b, LOCALE_NAME_MAX_LENGTH) > 1) return ToUtf8(std::u16string(b, b + wcslen(b)));
  return "en-US";
}

bool Before(Pos a, Pos b) { return a.para < b.para || (a.para == b.para && a.offset < b.offset); }
bool Holds(const Range& r, Pos p) { return !Before(p, r.from) && !Before(r.to, p); }

bool IsOn(std::string_view v) { return v == "1" || v == "true" || v == "on"; }

// The w:p elements in `n`, in order: a block control's paragraphs.
void Paragraphs(Node n, uint32_t& first, uint32_t& last) {
  for (Node c : n.Kids()) {
    if (c.Is(kP)) {
      if (first == kNone) first = c.Raw().start;
      last = c.Raw().start;
    } else if (c.FirstChild()) {
      Paragraphs(c, first, last);
    }
  }
}

// The inline w:sdt elements of a paragraph (and those inside them), in order.
void InlineSdts(Node n, std::vector<Node>& out) {
  for (Node c : n.Kids()) {
    if (c.Is(kPPr) || c.Is(kR) || c.Is(kSdtPr)) continue;
    if (c.Is(kSdt)) out.push_back(c);
    if (c.FirstChild()) InlineSdts(c, out);
  }
}

// Kept content in a control's properties that makes it something else (a repeating section).
bool KeptKind(const Document& doc, Node pr) {
  auto has = [&](Node n) {
    for (const pkg::Fragment& f : doc.Store().FragmentsCopy(n.Raw().node))
      if (f.xml.find("repeatingSection") != std::string::npos) return true;
    return false;
  };
  if (has(pr)) return true;
  for (Node c : pr.Kids())
    if (has(c)) return true;
  return false;
}

ContentControl Describe(const Document& doc, Node sdt, bool block, Range content) {
  ContentControl c;
  c.sdt = sdt.Id();
  c.block = block;
  c.content = content;
  const Node pr = sdt.Child(kSdtPr);
  c.placeholder = pr.Child(kShowing).On();
  const std::string_view lock = pr.Child(kLock).AttrOr(kVal);
  c.contentLocked = lock == "contentLocked" || lock == "sdtContentLocked";
  c.controlLocked = lock == "sdtLocked" || lock == "sdtContentLocked";
  c.bound = static_cast<bool>(pr.Child(kBinding));
  c.title = U16(pr.Child(kAlias).AttrOr(kVal));
  c.tag = U16(pr.Child(kTag).AttrOr(kVal));
  if (Node cb = pr.Child(kCheckbox)) {
    c.kind = ControlKind::CheckBox;
    c.checked = IsOn(cb.Child(kW14Checked).AttrOr(kW14Val));
    if (auto v = cb.Child(kCheckedState).Hex(kW14Val); v && *v > 0 && *v < 0xFFFF) c.checkedSymbol = static_cast<char16_t>(*v);
    if (auto v = cb.Child(kUncheckedState).Hex(kW14Val); v && *v > 0 && *v < 0xFFFF) c.uncheckedSymbol = static_cast<char16_t>(*v);
  } else if (Node list = pr.Child(kDropDown) ? pr.Child(kDropDown) : pr.Child(kCombo)) {
    c.kind = list.Is(kDropDown) ? ControlKind::DropDown : ControlKind::ComboBox;
    for (Node item : list.Kids()) {
      if (!item.Is(kListItem)) continue;
      ListItem li;
      li.value = std::string(item.AttrOr(kValue));
      li.text = U16(item.Attr(kDisplayText) ? *item.Attr(kDisplayText) : li.value);
      c.items.push_back(std::move(li));
    }
  } else if (Node date = pr.Child(kDate)) {
    c.kind = ControlKind::Date;
    c.dateFormat = U16(date.Child(kDateFormat).AttrOr(kVal));
    c.dateLang = std::string(date.Child(kLid).AttrOr(kVal));
    c.date = ParseFullDate(date.AttrOr(kFullDate));
  } else if (pr.Child(kText)) {
    c.kind = ControlKind::PlainText;
  } else if (pr.Child(kPicture)) {
    c.kind = ControlKind::Picture;
  } else if (pr.Child(Name("docPartObj")) || pr.Child(Name("docPartList")) || pr.Child(Name("group")) ||
             pr.Child(Name("citation")) || pr.Child(Name("bibliography")) || pr.Child(Name("equation")) || KeptKind(doc, pr)) {
    c.kind = ControlKind::Other;
  }
  return c;
}

// The block control (innermost) a paragraph is in, as an element and its paragraphs.
Node BlockSdt(const Document& doc, uint32_t para, Node* outer = nullptr) {
  Node found;
  for (Node n = doc.ParaNode(para).Parent(); n; n = n.Parent()) {
    if (n.Is(kSdtContent) && n.Parent().Is(kSdt)) {
      if (!found) found = n.Parent();
      if (outer) *outer = n.Parent();
    }
  }
  return found;
}

ContentControl Block(const Document& doc, Node sdt) {
  uint32_t first = kNone, last = kNone;
  Paragraphs(sdt.Child(kSdtContent), first, last);
  Range r;
  if (first != kNone) r = {{first, 0}, {last, static_cast<uint32_t>(doc.Para(last).text.size())}};
  return Describe(doc, sdt, true, r);
}

ContentControl Inline(const Document& doc, uint32_t para, Node sdt) {
  const Node content = sdt.Child(kSdtContent);
  const uint32_t s = content ? content.TextStart() : sdt.TextStart();
  const uint32_t len = content ? content.TextLength() : 0;
  return Describe(doc, sdt, false, {{para, s}, {para, s + len}});
}

// The placeholder text style's id, added (Word's built-in) when `add`; inside the editor's group.
std::string PlaceholderStyle(Editor& ed, bool add) {
  for (const Style& s : ed.Doc().Styles())
    if (s.type == StyleType::Character && s.name == "Placeholder Text") return std::string(s.id);
  if (!add) return {};
  TreeEdit te(ed);
  te.FormFill();
  if (te.EnsurePart(PartKind::Styles) == kNoElem) return {};
  te.Begin(u"Style");
  std::string id = EnsureStyle(te, "Placeholder Text");
  te.Commit();
  return te.Refused().empty() ? id : std::string();
}

// The runs of [from, to) in one paragraph.
std::vector<ElemId> RunsIn(const Document& doc, uint32_t para, uint32_t from, uint32_t to) {
  std::vector<ElemId> out;
  for (const TextRun& r : doc.Runs(para))
    if (r.run && r.start < to && r.end > from && std::find(out.begin(), out.end(), r.run.Id()) == out.end())
      out.push_back(r.run.Id());
  return out;
}

// The control's text replaced by `text`: it goes in after the old text, in the formatting of its
// last run (text typed at a control's end stays in it), then the old text goes. The new range.
Result Replace(Editor& ed, const ContentControl& c, std::u16string_view text, Range& made) {
  const Pos from = c.content.from, to = c.content.to;
  ed.FillingIn(true);
  Result r = ed.InsertText(to, text);
  ed.FillingIn(false);
  if (!r) return r;
  const Pos start = to;
  if (auto in = ControlAt(ed.Doc(), {start.para, start.offset + (text.empty() ? 0u : 1u)}); !in || in->sdt != c.sdt) {
    r.ok = false;
    r.message = u"This content control is empty, and its text can't be filled in yet.";
    return r;
  }
  if (Before(from, to)) {
    r = ed.Delete(from, to);
    if (!r) return r;
  }
  made = {from, {from.para, from.offset + static_cast<uint32_t>(text.size())}};
  r.caret = made.to;
  return r;
}

// One undo step: `change` inside the editor's group, cancelled as a whole when it fails.
Result Group(Editor& ed, std::u16string name, const std::function<Result()>& change) {
  ed.BeginGroup(std::move(name));
  Result r = change();
  if (!r) {
    ed.CancelGroup();
    return r;
  }
  ed.EndGroup();
  return r;
}

Result Refused(std::u16string_view why) {
  Result r;
  r.ok = false;
  r.message = std::u16string(why);
  return r;
}

// A transaction on a control's properties, allowed in a protected form; its layout reaches the
// control's paragraphs only.
template <class Fn>
Result Properties(Editor& ed, const ContentControl& c, const Fn& fn) {
  TreeEdit te(ed, c.content.from);
  te.FormFill();
  te.Begin(u"Fill in");
  te.Affects(c.content.from.para, c.content.to.para);
  fn(te);
  te.Commit();
  Result r;
  r.ok = te.Refused().empty();
  r.message = te.Refused();
  r.caret = c.content.to;
  return r;
}

std::u16string Why(const ContentControl& c) {
  if (c.bound) return u"This content control takes its text from data in the document, which can't be changed here yet.";
  if (c.contentLocked) return u"The text of this content control is locked.";
  return u"This content control can't be filled in here.";
}

}  // namespace

bool ContentControl::Typable() const {
  return !contentLocked && !bound &&
         (kind == ControlKind::RichText || kind == ControlKind::PlainText || kind == ControlKind::ComboBox ||
          kind == ControlKind::Date);
}

bool ContentControl::Fillable() const {
  return Typable() || (!contentLocked && !bound && (kind == ControlKind::CheckBox || kind == ControlKind::DropDown));
}

std::vector<ContentControl> ContentControls(const Document& doc, uint32_t story) {
  std::vector<ContentControl> out;
  std::unordered_set<ElemId> seen;
  for (uint32_t p : doc.StoryParagraphs(story)) {
    std::vector<Node> blocks;
    for (Node n = doc.ParaNode(p).Parent(); n; n = n.Parent())
      if (n.Is(kSdtContent) && n.Parent().Is(kSdt)) blocks.push_back(n.Parent());
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
      if (seen.insert(it->Id()).second) out.push_back(Block(doc, *it));
    std::vector<Node> inl;
    InlineSdts(doc.ParaNode(p), inl);
    for (Node s : inl)
      if (seen.insert(s.Id()).second) out.push_back(Inline(doc, p, s));
  }
  return out;
}

std::optional<ContentControl> ControlAt(const Document& doc, Pos at) {
  if (at.para >= doc.Paragraphs().size()) return std::nullopt;
  std::vector<Node> inl;
  InlineSdts(doc.ParaNode(at.para), inl);
  std::optional<ContentControl> best;
  for (Node s : inl) {
    ContentControl c = Inline(doc, at.para, s);
    if (!Holds(c.content, at)) continue;
    if (!best || c.content.to.offset - c.content.from.offset <= best->content.to.offset - best->content.from.offset)
      best = std::move(c);
  }
  if (best) return best;
  if (Node sdt = BlockSdt(doc, at.para)) return Block(doc, sdt);
  return std::nullopt;
}

std::vector<FormField> FormFields(const Document& doc, uint32_t story) {
  std::vector<FormField> out;
  for (const Field& f : doc.Fields(story)) {
    if (f.simple || !f.beginNode || !f.complete) continue;
    const Node ff = f.beginNode.Child(kFFData);
    const std::u16string_view type = f.Type();
    if (!ff || (type != u"FORMTEXT" && type != u"FORMCHECKBOX" && type != u"FORMDROPDOWN")) continue;
    FormField x;
    x.story = story;
    x.kind = type == u"FORMCHECKBOX" ? FormFieldKind::CheckBox : type == u"FORMDROPDOWN" ? FormFieldKind::DropDown : FormFieldKind::Text;
    x.begin = f.beginNode.Id();
    x.start = f.begin;
    x.end = {f.end.para, f.end.offset + 1};
    x.hasResult = f.separator.para != kNone;
    x.result = x.hasResult ? Range{{f.separator.para, f.separator.offset + 1}, f.end} : Range{f.end, f.end};
    const Node enabled = ff.Child(kEnabled);
    x.enabled = !enabled || enabled.On();
    x.name = U16(ff.Child(kFFName).AttrOr(kVal));
    if (Node cb = ff.Child(kCheckBox)) x.checked = cb.Child(kChecked) ? cb.Child(kChecked).On() : cb.Child(kDefault).On();
    if (Node dd = ff.Child(kDDList)) {
      for (Node e : dd.Kids())
        if (e.Is(kListEntry)) x.entries.push_back(U16(e.AttrOr(kVal)));
      const auto pick = dd.Child(kResult) ? dd.Child(kResult).Int(kVal) : dd.Child(kDefault).Int(kVal);
      x.selected = pick && *pick >= 0 && *pick < static_cast<int64_t>(x.entries.size()) ? static_cast<int>(*pick) : 0;
    }
    if (Node ti = ff.Child(kTextInput)) x.maxLength = static_cast<int>(ti.Child(kMaxLength).Int(kVal).value_or(0));
    out.push_back(std::move(x));
  }
  return out;
}

std::optional<FormField> FormFieldAt(const Document& doc, Pos at) {
  if (at.para >= doc.Paragraphs().size()) return std::nullopt;
  for (FormField& f : FormFields(doc, doc.Para(at.para).story))
    if (Holds(f.result, at) || (!Before(at, f.start) && Before(at, f.end))) return std::move(f);
  return std::nullopt;
}

bool FormsProtected(const Document& doc) {
  const Part* settings = doc.FindPart(PartKind::Settings);
  if (!settings) return false;
  const Node prot = Node(&doc.GetTree(), settings->root).Child(Name("documentProtection"));
  if (!prot) return false;
  std::string_view edit, enforcement;
  for (const pkg::XmlAttr& a : doc.Store().Attributes(doc.Store().NodeAttributes(prot.Raw().node))) {
    const size_t colon = a.qname.find(':');
    const std::string_view local = colon == std::string::npos ? std::string_view(a.qname) : std::string_view(a.qname).substr(colon + 1);
    if (local == "edit") edit = a.value;
    else if (local == "enforcement") enforcement = a.value;
  }
  return edit == "forms" && IsOn(enforcement);
}

bool InFillIn(const Document& doc, Pos from, Pos to, std::u16string* why, bool typing) {
  auto say = [&](std::u16string_view w) {
    if (why) *why = std::u16string(w);
    return false;
  };
  if (auto c = ControlAt(doc, from); c && Holds(c->content, to)) {
    if (c->Typable() || (!typing && c->Fillable())) return true;
    if (c->kind == ControlKind::CheckBox && c->Fillable()) return say(u"Click the check box, or press Space, to tick it.");
    if (c->kind == ControlKind::DropDown && c->Fillable()) return say(u"Choose an item from this list.");
    return say(Why(*c));
  }
  if (auto f = FormFieldAt(doc, from); f && Holds(f->result, from) && Holds(f->result, to)) {
    if (f->enabled && (f->kind == FormFieldKind::Text || !typing)) return true;
    if (f->kind == FormFieldKind::CheckBox) return say(u"Click the check box, or press Space, to tick it.");
    if (f->kind == FormFieldKind::DropDown) return say(u"Choose an item from this list.");
    return say(u"This form field can't be filled in.");
  }
  return say(kProtected);
}

std::optional<Range> NextFillIn(const Document& doc, Pos at, bool back) {
  if (at.para >= doc.Paragraphs().size()) return std::nullopt;
  const uint32_t story = doc.Para(at.para).story;
  std::vector<Range> targets;
  for (const ContentControl& c : ContentControls(doc, story))
    if (c.Fillable()) targets.push_back(c.content);
  for (const FormField& f : FormFields(doc, story))
    if (f.enabled) targets.push_back(f.kind == FormFieldKind::Text ? f.result : Range{f.start, f.end});
  if (targets.empty()) return std::nullopt;
  std::stable_sort(targets.begin(), targets.end(), [](const Range& a, const Range& b) { return Before(a.from, b.from); });
  const size_t n = targets.size();
  if (!back) {
    for (const Range& r : targets)
      if (Before(at, r.from)) return r;
    return targets.front();
  }
  size_t k = 0;  // the target `at` is in, or the number before it
  while (k < n && Before(targets[k].from, at)) ++k;
  for (size_t i = 0; i < n; ++i)
    if (Holds(targets[i], at)) k = i;
  return targets[(k + n - 1) % n];
}

std::u16string_view DefaultPlaceholder(ControlKind kind) {
  switch (kind) {
    case ControlKind::Date: return u"Click or tap to enter a date.";
    case ControlKind::DropDown:
    case ControlKind::ComboBox: return u"Choose an item.";
    default: return u"Click or tap here to enter text.";
  }
}

Result FillControl(Editor& ed, const ContentControl& c, std::u16string_view text) {
  if (!c.Fillable()) return Refused(Why(c));
  return Group(ed, u"Fill in", [&]() -> Result {
    Range made;
    Result r = Replace(ed, c, text, made);
    if (!r || !c.placeholder) return r;
    // The placeholder goes: its state, and its style on the new text.
    const std::string style = PlaceholderStyle(ed, false);
    const Document& doc = ed.Doc();
    Result p = Properties(ed, c, [&](TreeEdit& te) {
      te.Remove(Node(&doc.GetTree(), c.sdt).Child(kSdtPr).Id(), kShowing);
      if (style.empty() || made.from.para != made.to.para) return;
      for (ElemId run : RunsIn(doc, made.from.para, made.from.offset, made.to.offset)) {
        const Node rs = Node(&doc.GetTree(), run).Path({kRPr, kRStyle});
        if (rs && rs.AttrOr(kVal) == style) te.Remove(Node(&doc.GetTree(), run).Child(kRPr).Id(), kRStyle);
      }
    });
    if (!p) return p;
    return r;
  });
}

Result ShowPlaceholder(Editor& ed, const ContentControl& c) {
  if (!c.Fillable() || c.kind == ControlKind::CheckBox) return Refused(Why(c));
  return Group(ed, u"Fill in", [&]() -> Result {
    const std::string style = PlaceholderStyle(ed, true);
    Range made;
    Result r = Replace(ed, c, DefaultPlaceholder(c.kind), made);
    if (!r) return r;
    const Document& doc = ed.Doc();
    Result p = Properties(ed, c, [&](TreeEdit& te) {
      te.Child(Node(&doc.GetTree(), c.sdt).Child(kSdtPr).Id(), kShowing);
      if (style.empty() || made.from.para != made.to.para) return;
      for (ElemId run : RunsIn(doc, made.from.para, made.from.offset, made.to.offset))
        te.SetAttr(te.Path(run, {kRPr, kRStyle}), kVal, style);
    });
    if (!p) return p;
    r.caret = made.from;
    return r;
  });
}

Result SetChecked(Editor& ed, const ContentControl& c, bool checked) {
  if (c.kind != ControlKind::CheckBox || !c.Fillable()) return Refused(Why(c));
  return Group(ed, u"Check box", [&]() -> Result {
    const Document& doc = ed.Doc();
    Result p = Properties(ed, c, [&](TreeEdit& te) {
      const ElemId box = te.Path(Node(&doc.GetTree(), c.sdt).Child(kSdtPr).Id(), {kCheckbox, kW14Checked});
      te.SetAttr(box, kW14Val, checked ? "1" : "0");
    });
    if (!p) return p;
    const std::u16string symbol(1, checked ? c.checkedSymbol : c.uncheckedSymbol);
    const Pos from = c.content.from, to = c.content.to;
    if (from.para == to.para && doc.Para(from.para).text.substr(from.offset, to.offset - from.offset) == symbol) return p;
    Range made;
    return Replace(ed, c, symbol, made);
  });
}

Result PickItem(Editor& ed, const ContentControl& c, size_t index) {
  if ((c.kind != ControlKind::DropDown && c.kind != ControlKind::ComboBox) || index >= c.items.size()) return Refused(Why(c));
  // Its "Choose an item." entry (no value): the placeholder again, as in Word.
  if (c.items[index].value.empty() && c.items[index].text == DefaultPlaceholder(c.kind))
    return c.placeholder ? Result{true, {}, c.content.from} : ShowPlaceholder(ed, c);
  return Group(ed, u"Choose an item", [&]() -> Result {
    const ListItem& item = c.items[index];
    Result r = FillControl(ed, c, item.text.empty() ? U16(item.value) : item.text);
    if (!r) return r;
    const Document& doc = ed.Doc();
    const Node list = Node(&doc.GetTree(), c.sdt).Child(kSdtPr).Child(c.kind == ControlKind::DropDown ? kDropDown : kCombo);
    if (list && list.Has(kLastValue)) {
      ContentControl now = c;
      now.content = {c.content.from, r.caret};
      Result p = Properties(ed, now, [&](TreeEdit& te) { te.SetAttr(list.Id(), kLastValue, item.value); });
      if (!p) return p;
    }
    return r;
  });
}

Result PickDate(Editor& ed, const ContentControl& c, const DateTime& date) {
  if (c.kind != ControlKind::Date) return Refused(Why(c));
  return Group(ed, u"Date", [&]() -> Result {
    Result r = FillControl(ed, c, FormatDateTime(date, c.dateFormat, false, c.dateLang));
    if (!r) return r;
    const Document& doc = ed.Doc();
    ContentControl now = c;
    now.content = {c.content.from, r.caret};
    Result p = Properties(ed, now, [&](TreeEdit& te) {
      te.SetAttr(te.Child(Node(&doc.GetTree(), c.sdt).Child(kSdtPr).Id(), kDate), kFullDate, FullDate(date));
    });
    return p ? r : p;
  });
}

Result SetFormCheckBox(Editor& ed, const FormField& f, bool checked) {
  if (f.kind != FormFieldKind::CheckBox || !f.enabled) return Refused(u"This form field can't be filled in.");
  TreeEdit te(ed, f.start);
  te.FormFill();
  te.Begin(u"Check box");
  te.Affects(f.start.para, f.start.para);
  const ElemId box = te.Path(f.begin, {kFFData, kCheckBox, kChecked});
  if (checked) te.RemoveAttr(box, kVal);
  else te.SetAttr(box, kVal, "0");
  te.Commit();
  Result r;
  r.ok = te.Refused().empty();
  r.message = te.Refused();
  r.caret = f.end;
  return r;
}

Result PickFormEntry(Editor& ed, const FormField& f, size_t index) {
  if (f.kind != FormFieldKind::DropDown || !f.enabled || index >= f.entries.size())
    return Refused(u"This form field can't be filled in.");
  return Group(ed, u"Choose an item", [&]() -> Result {
    TreeEdit te(ed, f.start);
    te.FormFill();
    te.Begin(u"Choose an item");
    te.Affects(f.start.para, f.end.para);
    te.SetAttr(te.Path(f.begin, {kFFData, kDDList, kResult}), kVal, std::to_string(index));
    te.Commit();
    Result r;
    r.ok = te.Refused().empty();
    r.message = te.Refused();
    r.caret = f.end;
    // The result Word shows, when the field has one: the entry.
    if (r && f.hasResult && Before(f.result.from, f.result.to) && f.result.from.para == f.result.to.para) {
      r = ed.InsertText(f.result.to, f.entries[index]);
      if (r) r = ed.Delete(f.result.from, f.result.to);
    }
    return r;
  });
}

Result InsertControl(Editor& ed, Pos at, ControlKind kind, const std::vector<std::u16string>& items) {
  const Document& doc = ed.Doc();
  if (at.para >= doc.Paragraphs().size()) return Refused(u"That place isn't in the document.");
  if (kind == ControlKind::Picture || kind == ControlKind::Other) return Refused(u"That kind of content control can't be inserted.");
  if (FormsProtected(doc)) return Refused(kProtected);
  // Not inside a link, field, content control or tracked change.
  for (const TextRun& r : doc.Runs(at.para))
    if (r.start < at.offset && at.offset < r.end && (r.hyperlink || r.fieldDepth > 0 || r.simpleField || r.contentControl || r.revision))
      return Refused(u"A content control can't go here.");
  if (ControlAt(doc, at) || FormFieldAt(doc, at)) return Refused(u"A content control can't go inside another, or in a form field.");
  // A new id: one more than the document's largest.
  int64_t id = 0;
  const Tree& tree = doc.GetTree();
  for (ElemId e = 0; e < tree.elems.size(); ++e) {
    const Node n(&tree, e);
    if (n.Is(kId) && n.Parent().Is(kSdtPr)) id = std::max<int64_t>(id, n.Int(kVal).value_or(0) < 0 ? -*n.Int(kVal) : n.Int(kVal).value_or(0));
  }
  const bool box = kind == ControlKind::CheckBox;
  const std::u16string text = box ? std::u16string(1, u'☐') : std::u16string(DefaultPlaceholder(kind));
  return Group(ed, u"Content control", [&]() -> Result {
    const std::string style = box ? std::string() : PlaceholderStyle(ed, true);
    Result r = ed.InsertText(at, text);
    if (!r) return r;
    const uint32_t p = at.para, a = at.offset, b = at.offset + static_cast<uint32_t>(text.size());
    TreeEdit te(ed, at);
    te.Begin(u"Content control");
    te.Local();
    if (!te.SplitRunAt(p, a) || !te.SplitRunAt(p, b)) {
      te.Abort();
      return Refused(u"A content control can't go here.");
    }
    const std::vector<ElemId> runs = RunsIn(doc, p, a, b);
    const ElemId sdt = runs.empty() ? kNoElem : te.Wrap(runs.front(), runs.back(), {kSdt, kSdtContent});
    if (sdt == kNoElem) {
      te.Abort();
      return Refused(u"A content control can't go here.");
    }
    const ElemId pr = te.Child(sdt, kSdtPr);
    te.SetAttr(te.Child(pr, kId), kVal, std::to_string(id + 1));
    if (!box) te.Child(pr, kShowing);
    switch (kind) {
      case ControlKind::PlainText: te.Child(pr, kText); break;
      case ControlKind::CheckBox: {
        const ElemId cb = te.Child(pr, kCheckbox);
        te.SetAttr(te.Child(cb, kW14Checked), kW14Val, "0");
        for (auto [state, hex] : {std::pair{kCheckedState, "2612"}, std::pair{kUncheckedState, "2610"}}) {
          const ElemId s = te.Child(cb, state);
          te.SetAttr(s, kW14Val, hex);
          te.SetAttr(s, kW14Font, "MS Gothic");
        }
        for (ElemId run : runs) {
          const ElemId fonts = te.Path(run, {kRPr, kRFonts});
          for (uint32_t n : {Name("ascii"), Name("eastAsia"), Name("hAnsi")}) te.SetAttr(fonts, n, "MS Gothic");
          te.SetAttr(fonts, Name("hint"), "eastAsia");
        }
        break;
      }
      case ControlKind::DropDown:
      case ControlKind::ComboBox: {
        const ElemId list = te.Child(pr, kind == ControlKind::DropDown ? kDropDown : kCombo);
        std::vector<std::u16string> all = {std::u16string(DefaultPlaceholder(kind))};
        all.insert(all.end(), items.begin(), items.end());
        for (size_t i = 0; i < all.size(); ++i) {
          const std::string shown = ToUtf8(all[i]);
          std::string xml = "<w:listItem w:displayText=\"" + XmlEscapeAttr(shown) + "\" w:value=\"" +
                            (i == 0 ? std::string() : XmlEscapeAttr(shown)) + "\"/>";
          te.AddXml(list, xml);
        }
        break;
      }
      case ControlKind::Date: {
        const ElemId date = te.Child(pr, kDate);
        // The user's short date, as Word's date picker starts.
        te.SetAttr(te.Child(date, kDateFormat), kVal, ToUtf8(ShortDatePicture()));
        te.SetAttr(te.Child(date, kLid), kVal, UserLanguage());
        te.SetAttr(te.Child(date, kStoreAs), kVal, "dateTime");
        te.SetAttr(te.Child(date, kCalendar), kVal, "gregorian");
        break;
      }
      default: break;
    }
    if (!style.empty())
      for (ElemId run : runs) te.SetAttr(te.Path(run, {kRPr, kRStyle}), kVal, style);
    te.Commit();
    if (!te.Refused().empty()) return Refused(te.Refused());
    r.caret = {p, a};
    return r;
  });
}

}  // namespace fw::doc::edit
