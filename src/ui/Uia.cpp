#include "ui/Uia.h"

#include <oleauto.h>

#include <algorithm>
#include <atomic>
#include <tuple>

#include "ui/Widgets.h"

#pragma comment(lib, "uiautomationcore.lib")

namespace uia {

namespace {

constexpr Key kRootKey = 1;

Key Mix(Key parent, int64_t id) {
  uint64_t z = parent * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(id) + 0x632BE59BD9B4E019ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return z > kRootKey ? z : z + 2;
}

bool IsEmpty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }

bool IsKeyName(const std::wstring& part) {
  static const wchar_t* const kNames[] = {L"Ctrl",  L"Shift",   L"Alt",       L"Esc",    L"Enter", L"Tab",
                                          L"Space", L"Home",    L"End",       L"Delete", L"Up",    L"Down",
                                          L"Left",  L"Right",   L"Page Up",   L"Page Down", L"Backspace"};
  for (const wchar_t* n : kNames)
    if (part == n) return true;
  if (part.size() == 1) return true;  // a letter, digit or symbol: "O", "=", "−"
  if (part.size() >= 2 && part.size() <= 3 && part[0] == L'F' && iswdigit(part[1])) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tree

Tree::Tree() { Clear(); }

void Tree::Clear() {
  nodes_.clear();
  index_.clear();
  Node root;
  root.key = kRootKey;
  root.type = UIA_WindowControlTypeId;
  nodes_.push_back(std::move(root));
  index_[kRootKey] = 0;
}

int Tree::Add(int parent, int64_t id, CONTROLTYPEID type, std::wstring name, D2D1_RECT_F rc) {
  Node n;
  n.key = Mix(nodes_[parent].key, id);
  n.parent = parent;
  n.type = type;
  n.name = std::move(name);
  n.rc = rc;
  n.layer = nodes_[parent].layer;
  const int i = Size();
  index_.emplace(n.key, i);  // a repeated id keeps the first element findable
  nodes_.push_back(std::move(n));
  nodes_[parent].children.push_back(i);
  return i;
}

int Tree::Find(Key key) const {
  auto it = index_.find(key);
  return it == index_.end() ? -1 : it->second;
}

int Tree::Focused() const {
  int found = -1;
  for (int i = 1; i < Size(); ++i)
    if (nodes_[i].focused) found = i;
  return found;
}

int Tree::HitTest(float x, float y) const {
  int best = -1;
  std::tuple<int, int, int> bestRank{-1, -1, -1};
  // Nodes are added parents first, so a node's depth is its parent's plus one.
  std::vector<int> depth(nodes_.size(), 0);
  for (int i = 1; i < Size(); ++i) {
    const Node& n = nodes_[i];
    depth[i] = depth[n.parent] + 1;
    if (n.offscreen || IsEmpty(n.rc) || !gfx::Contains(n.rc, x, y)) continue;
    auto rank = std::make_tuple(n.layer, depth[i], i);
    if (rank > bestRank) {
      bestRank = rank;
      best = i;
    }
  }
  return best;
}

int Tree::ContainerOf(int i) const {
  for (int p = nodes_[i].parent; p >= 0; p = nodes_[p].parent)
    if (nodes_[p].selectionContainer) return p;
  return -1;
}

bool Tree::HasFocusBelow(int i) const {
  for (int c : nodes_[i].children)
    if (nodes_[c].focused || HasFocusBelow(c)) return true;
  return false;
}

std::pair<std::wstring, std::wstring> SplitShortcut(const std::wstring& tip) {
  if (tip.size() > 3 && tip.back() == L')') {
    size_t open = tip.rfind(L" (");
    if (open != std::wstring::npos) {
      std::wstring keys = tip.substr(open + 2, tip.size() - open - 3);
      bool allKeys = !keys.empty();
      size_t start = 0;
      while (allKeys && start <= keys.size()) {
        size_t plus = keys.find(L'+', start + 1);  // "Ctrl++" has a key named "+"
        if (plus == std::wstring::npos) plus = keys.size();
        allKeys = IsKeyName(keys.substr(start, plus - start));
        start = plus + 1;
      }
      if (allKeys) return {tip.substr(0, open), keys};
    }
  }
  return {tip, L""};
}

int AddButton(Tree& t, int parent, int64_t id, const Button& b, std::function<void()> invoke) {
  auto [name, keys] = SplitShortcut(b.tooltip);
  int i = t.Add(parent, id, UIA_ButtonControlTypeId, b.text.empty() ? name : b.text, b.rc);
  Node& n = t[i];
  n.accelerator = keys;
  if (!b.text.empty() && !name.empty() && name != b.text) n.help = name;
  n.enabled = b.enabled;
  n.invoke = std::move(invoke);
  return i;
}

int AddTextBox(Tree& t, int parent, int64_t id, TextBox& box, const std::wstring& name, std::function<void()> focus) {
  auto [label, keys] = SplitShortcut(name);
  int i = t.Add(parent, id, UIA_EditControlTypeId, label, box.rc);
  Node& n = t[i];
  n.accelerator = keys;
  if (!box.placeholder.empty() && box.placeholder != label) n.help = box.placeholder;
  n.focusable = true;
  n.focused = box.focused;
  n.focus = std::move(focus);
  n.password = box.password;
  n.hasValue = true;
  if (!box.password) n.value = box.Text();
  TextBox* p = &box;
  n.setValue = [p](const std::wstring& v) {
    p->SetText(v);
    if (p->onChange) p->onChange();
    else if (p->onEnter) p->onEnter(false);
  };
  return i;
}

// ---------------------------------------------------------------------------
// Providers

struct State {
  Root* root = nullptr;
};

class Element : public IRawElementProviderSimple,
                public IRawElementProviderFragment,
                public IRawElementProviderFragmentRoot,
                public IInvokeProvider,
                public IToggleProvider,
                public IExpandCollapseProvider,
                public IValueProvider,
                public IRangeValueProvider,
                public ISelectionProvider,
                public ISelectionItemProvider,
                public IScrollProvider,
                public IScrollItemProvider {
 public:
  Element(std::shared_ptr<State> state, Key key) : state_(std::move(state)), key_(key) {}
  virtual ~Element() = default;

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
      *out = static_cast<IRawElementProviderSimple*>(this);
    else if (iid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
    else if (iid == __uuidof(IRawElementProviderFragmentRoot) && key_ == kRootKey)
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    else if (iid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
    else if (iid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
    else if (iid == __uuidof(IExpandCollapseProvider)) *out = static_cast<IExpandCollapseProvider*>(this);
    else if (iid == __uuidof(IValueProvider)) *out = static_cast<IValueProvider*>(this);
    else if (iid == __uuidof(IRangeValueProvider)) *out = static_cast<IRangeValueProvider*>(this);
    else if (iid == __uuidof(ISelectionProvider)) *out = static_cast<ISelectionProvider*>(this);
    else if (iid == __uuidof(ISelectionItemProvider)) *out = static_cast<ISelectionItemProvider*>(this);
    else if (iid == __uuidof(IScrollProvider)) *out = static_cast<IScrollProvider*>(this);
    else if (iid == __uuidof(IScrollItemProvider)) *out = static_cast<IScrollItemProvider*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }

  // IRawElementProviderSimple
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
    // COM threading: every call arrives on the UI thread, where the UI can be read safely.
    *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** out) override {
    *out = nullptr;
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    IUnknown* p = nullptr;
    switch (id) {
      case UIA_InvokePatternId: if (n->invoke) p = static_cast<IInvokeProvider*>(this); break;
      case UIA_TogglePatternId: if (n->toggle >= 0) p = static_cast<IToggleProvider*>(this); break;
      case UIA_ExpandCollapsePatternId: if (n->expand >= 0) p = static_cast<IExpandCollapseProvider*>(this); break;
      case UIA_ValuePatternId: if (n->hasValue) p = static_cast<IValueProvider*>(this); break;
      case UIA_RangeValuePatternId: if (n->hasRange) p = static_cast<IRangeValueProvider*>(this); break;
      case UIA_SelectionPatternId: if (n->selectionContainer) p = static_cast<ISelectionProvider*>(this); break;
      case UIA_SelectionItemPatternId: if (n->selectable) p = static_cast<ISelectionItemProvider*>(this); break;
      case UIA_ScrollPatternId: if (n->scrollable) p = static_cast<IScrollProvider*>(this); break;
      case UIA_ScrollItemPatternId: if (n->scrollIntoView) p = static_cast<IScrollItemProvider*>(this); break;
      default: break;
    }
    if (p) {
      p->AddRef();
      *out = p;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
    v->vt = VT_EMPTY;
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (key_ == kRootKey) return S_OK;  // the window: its host provides name, role and position
    auto str = [&](const std::wstring& s) {
      if (s.empty()) return;
      v->vt = VT_BSTR;
      v->bstrVal = SysAllocString(s.c_str());
    };
    auto boolean = [&](bool b) {
      v->vt = VT_BOOL;
      v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
    };
    auto integer = [&](int i) {
      v->vt = VT_I4;
      v->lVal = i;
    };
    switch (id) {
      case UIA_ControlTypePropertyId: integer(n->type); break;
      case UIA_NamePropertyId: str(n->name); break;
      case UIA_AutomationIdPropertyId: str(n->automationId); break;
      case UIA_AcceleratorKeyPropertyId: str(n->accelerator); break;
      case UIA_HelpTextPropertyId: str(n->help); break;
      case UIA_ItemStatusPropertyId: str(n->status); break;
      case UIA_IsEnabledPropertyId: boolean(n->enabled); break;
      case UIA_IsKeyboardFocusablePropertyId: boolean(n->focusable && n->enabled); break;
      case UIA_HasKeyboardFocusPropertyId: boolean(n->focused && ::GetFocus() == Hwnd()); break;
      case UIA_IsOffscreenPropertyId: boolean(n->offscreen || IsEmpty(n->rc)); break;
      case UIA_IsPasswordPropertyId: boolean(n->password); break;
      case UIA_OrientationPropertyId:
        if (n->orientation >= 0) integer(n->orientation);
        break;
      case UIA_LiveSettingPropertyId:
        if (n->live) integer(Polite);
        break;
      case UIA_PositionInSetPropertyId:
        if (n->positionInSet > 0) integer(n->positionInSet);
        break;
      case UIA_SizeOfSetPropertyId:
        if (n->sizeOfSet > 0) integer(n->sizeOfSet);
        break;
      default: break;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
    *out = nullptr;
    if (key_ != kRootKey) return S_OK;
    HWND hwnd = Hwnd();
    if (!hwnd) return UIA_E_ELEMENTNOTAVAILABLE;
    return UiaHostProviderFromHwnd(hwnd, out);
  }

  // IRawElementProviderFragment
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection dir, IRawElementProviderFragment** out) override {
    *out = nullptr;
    int i = -1;
    const Node* n = Find(&i);
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    const Tree& t = state_->root->Current();
    int target = -1;
    switch (dir) {
      case NavigateDirection_Parent: target = n->parent; break;
      case NavigateDirection_FirstChild: target = n->children.empty() ? -1 : n->children.front(); break;
      case NavigateDirection_LastChild: target = n->children.empty() ? -1 : n->children.back(); break;
      case NavigateDirection_NextSibling:
      case NavigateDirection_PreviousSibling: {
        if (n->parent < 0) break;
        const auto& sib = t[n->parent].children;
        auto it = std::find(sib.begin(), sib.end(), i);
        if (it == sib.end()) break;
        if (dir == NavigateDirection_NextSibling && it + 1 != sib.end()) target = *(it + 1);
        if (dir == NavigateDirection_PreviousSibling && it != sib.begin()) target = *(it - 1);
        break;
      }
    }
    if (target >= 0) *out = Make(t[target].key);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
    *out = nullptr;
    if (!Find()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (key_ == kRootKey) return S_OK;  // the host's id is used
    int id[3] = {UiaAppendRuntimeId, static_cast<int>(key_ >> 32), static_cast<int>(key_ & 0xFFFFFFFF)};
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!sa) return E_OUTOFMEMORY;
    for (LONG k = 0; k < 3; ++k) SafeArrayPutElement(sa, &k, &id[k]);
    *out = sa;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
    *out = UiaRect{0, 0, 0, 0};
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (key_ == kRootKey || IsEmpty(n->rc)) return S_OK;
    RECT r{static_cast<LONG>(std::floor(n->rc.left)), static_cast<LONG>(std::floor(n->rc.top)),
           static_cast<LONG>(std::ceil(n->rc.right)), static_cast<LONG>(std::ceil(n->rc.bottom))};
    MapWindowPoints(Hwnd(), nullptr, reinterpret_cast<POINT*>(&r), 2);
    *out = UiaRect{static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right - r.left),
                   static_cast<double>(r.bottom - r.top)};
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    if (!n->focus) return n->focusable ? S_OK : UIA_E_INVALIDOPERATION;
    state_->root->Run(n->focus);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
    *out = nullptr;
    if (!Find()) return UIA_E_ELEMENTNOTAVAILABLE;
    Element* root = state_->root->RootElement();
    if (!root) return UIA_E_ELEMENTNOTAVAILABLE;
    root->AddRef();
    *out = static_cast<IRawElementProviderFragmentRoot*>(root);
    return S_OK;
  }

  // IRawElementProviderFragmentRoot
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!Find()) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
    ScreenToClient(Hwnd(), &pt);
    const Tree& t = state_->root->Current();
    int i = t.HitTest(static_cast<float>(pt.x), static_cast<float>(pt.y));
    if (i > 0) *out = Make(t[i].key);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!Find()) return UIA_E_ELEMENTNOTAVAILABLE;
    const Tree& t = state_->root->Current();
    int i = t.Focused();
    if (i > 0) *out = Make(t[i].key);
    return S_OK;
  }

  // IInvokeProvider
  HRESULT STDMETHODCALLTYPE Invoke() override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    if (!n->invoke) return UIA_E_INVALIDOPERATION;
    state_->root->PostInvoke(key_);
    return S_OK;
  }

  // IToggleProvider
  HRESULT STDMETHODCALLTYPE Toggle() override { return Act([](const Node& n) { return n.doToggle; }); }
  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = static_cast<ToggleState>(std::max(0, n->toggle));
    return S_OK;
  }

  // IExpandCollapseProvider
  HRESULT STDMETHODCALLTYPE Expand() override { return ExpandTo(true); }
  HRESULT STDMETHODCALLTYPE Collapse() override { return ExpandTo(false); }
  HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = static_cast<ExpandCollapseState>(std::max(0, n->expand));
    return S_OK;
  }

  // IValueProvider and IRangeValueProvider
  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    if (!n->setValue) return UIA_E_INVALIDOPERATION;
    std::wstring v = value ? value : L"";
    auto set = n->setValue;
    state_->root->Run([set, v] { set(v); });
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
    *out = nullptr;
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (n->password) return E_ACCESSDENIED;
    *out = SysAllocString(n->value.c_str());
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = (n->hasRange ? !n->setRange : !n->setValue) || !n->enabled;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetValue(double value) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    if (!n->setRange) return UIA_E_INVALIDOPERATION;
    if (value < n->rangeMin || value > n->rangeMax) return E_INVALIDARG;
    auto set = n->setRange;
    state_->root->Run([set, value] { set(value); });
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Value(double* out) override { return Range(out, &Node::rangeValue); }
  HRESULT STDMETHODCALLTYPE get_Maximum(double* out) override { return Range(out, &Node::rangeMax); }
  HRESULT STDMETHODCALLTYPE get_Minimum(double* out) override { return Range(out, &Node::rangeMin); }
  HRESULT STDMETHODCALLTYPE get_LargeChange(double* out) override { return Range(out, &Node::rangeLarge); }
  HRESULT STDMETHODCALLTYPE get_SmallChange(double* out) override { return Range(out, &Node::rangeSmall); }

  // ISelectionProvider
  HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override {
    *out = nullptr;
    int i = -1;
    if (!Find(&i)) return UIA_E_ELEMENTNOTAVAILABLE;
    const Tree& t = state_->root->Current();
    std::vector<Key> selected;
    for (int k = 1; k < t.Size(); ++k)
      if (t[k].selectable && t[k].selected && t.ContainerOf(k) == i) selected.push_back(t[k].key);
    SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(selected.size()));
    if (!sa) return E_OUTOFMEMORY;
    for (LONG k = 0; k < static_cast<LONG>(selected.size()); ++k) {
      IRawElementProviderFragment* e = Make(selected[k]);
      SafeArrayPutElement(sa, &k, static_cast<IUnknown*>(e));
      e->Release();
    }
    *out = sa;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* out) override {
    *out = FALSE;
    return Find() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = n->selectionRequired;
    return S_OK;
  }

  // ISelectionItemProvider
  HRESULT STDMETHODCALLTYPE Select() override { return Act([](const Node& n) { return n.select; }); }
  HRESULT STDMETHODCALLTYPE AddToSelection() override {
    int i = -1;
    const Node* n = Find(&i);
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (n->selected) return S_OK;
    // Single selection: adding is only possible while nothing else is selected.
    const Tree& t = state_->root->Current();
    int c = t.ContainerOf(i);
    for (int k = 1; k < t.Size(); ++k)
      if (k != i && t[k].selected && t.ContainerOf(k) == c) return UIA_E_INVALIDOPERATION;
    return Select();
  }
  HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    return n->selected ? UIA_E_INVALIDOPERATION : S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = n->selected;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** out) override {
    *out = nullptr;
    int i = -1;
    if (!Find(&i)) return UIA_E_ELEMENTNOTAVAILABLE;
    const Tree& t = state_->root->Current();
    int c = t.ContainerOf(i);
    if (c >= 0) *out = static_cast<IRawElementProviderSimple*>(static_cast<Element*>(Make(t[c].key)));
    return S_OK;
  }

  // IScrollProvider (vertical only)
  HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (horizontal != ScrollAmount_NoAmount || !n->setScroll || n->scrollPercent < 0) return UIA_E_INVALIDOPERATION;
    double step = 0;
    switch (vertical) {
      case ScrollAmount_LargeDecrement: step = -n->viewPercent; break;
      case ScrollAmount_SmallDecrement: step = -std::max(1.0, n->viewPercent / 8); break;
      case ScrollAmount_LargeIncrement: step = n->viewPercent; break;
      case ScrollAmount_SmallIncrement: step = std::max(1.0, n->viewPercent / 8); break;
      default: break;
    }
    double target = std::clamp(n->scrollPercent + step, 0.0, 100.0);
    auto set = n->setScroll;
    state_->root->Run([set, target] { set(target); });
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal, double vertical) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (horizontal != UIA_ScrollPatternNoScroll || !n->setScroll || n->scrollPercent < 0) return UIA_E_INVALIDOPERATION;
    if (vertical == UIA_ScrollPatternNoScroll) return S_OK;
    if (vertical < 0 || vertical > 100) return E_INVALIDARG;
    auto set = n->setScroll;
    state_->root->Run([set, vertical] { set(vertical); });
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* out) override {
    *out = UIA_ScrollPatternNoScroll;
    return Find() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* out) override {
    return Range(out, &Node::scrollPercent);
  }
  HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* out) override {
    *out = 100;
    return Find() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* out) override { return Range(out, &Node::viewPercent); }
  HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* out) override {
    *out = FALSE;
    return Find() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* out) override {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = n->scrollPercent >= 0;
    return S_OK;
  }

  // IScrollItemProvider
  HRESULT STDMETHODCALLTYPE ScrollIntoView() override { return Act([](const Node& n) { return n.scrollIntoView; }); }

  // An element for `key`, as a fragment, with one reference for the caller.
  IRawElementProviderFragment* Make(Key key) const {
    if (key == kRootKey) {
      Element* root = state_->root->RootElement();
      root->AddRef();
      return static_cast<IRawElementProviderFragment*>(root);
    }
    return static_cast<IRawElementProviderFragment*>(new Element(state_, key));
  }

 private:
  HWND Hwnd() const { return state_->root ? state_->root->Hwnd() : nullptr; }

  // The element in the current tree, or null when it is gone (or the window is).
  const Node* Find(int* index = nullptr) const {
    if (!state_->root || !state_->root->Hwnd()) return nullptr;
    const Tree& t = state_->root->Current();
    int i = t.Find(key_);
    if (i < 0) return nullptr;
    if (index) *index = i;
    return &t[i];
  }

  template <typename Pick>
  HRESULT Act(Pick pick) {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    std::function<void()> action = pick(*n);
    if (!action) return UIA_E_INVALIDOPERATION;
    state_->root->Run(std::move(action));
    return S_OK;
  }

  HRESULT ExpandTo(bool expand) {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!n->enabled) return UIA_E_ELEMENTNOTENABLED;
    if (!n->doExpand || n->expand == ExpandCollapseState_LeafNode) return UIA_E_INVALIDOPERATION;
    auto act = n->doExpand;
    state_->root->Run([act, expand] { act(expand); });
    return S_OK;
  }

  HRESULT Range(double* out, double Node::*field) const {
    const Node* n = Find();
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    *out = n->*field;
    return S_OK;
  }

  std::atomic<ULONG> refs_{1};
  std::shared_ptr<State> state_;
  Key key_;
};

// ---------------------------------------------------------------------------
// Root

Root::Root() : state_(std::make_shared<State>()) {}

Root::~Root() { Detach(); }

void Root::Attach(HWND hwnd, std::function<void(Tree&)> describe) {
  hwnd_ = hwnd;
  describe_ = std::move(describe);
  state_->root = this;
  root_ = new Element(state_, kRootKey);
  stale_ = true;
}

void Root::Detach() {
  if (!root_) return;
  if (hwnd_) UiaReturnRawElementProvider(hwnd_, 0, 0, nullptr);
  UiaDisconnectProvider(root_);
  state_->root = nullptr;
  root_->Release();
  root_ = nullptr;
  hwnd_ = nullptr;
  seen_.clear();
  haveSeen_ = false;
}

LRESULT Root::OnGetObject(WPARAM wp, LPARAM lp) {
  if (!root_ || static_cast<long>(lp) != static_cast<long>(UiaRootObjectId)) return 0;
  return UiaReturnRawElementProvider(hwnd_, wp, lp, root_);
}

const Tree& Root::Current() {
  if (stale_ && !building_ && describe_) {
    building_ = true;
    tree_.Clear();
    describe_(tree_);
    building_ = false;
    stale_ = false;
  }
  return tree_;
}

void Root::AfterMessage(UINT msg) {
  if (!hwnd_ || msg == WM_GETOBJECT || msg == WM_NCHITTEST || msg == WM_SETCURSOR) return;
  stale_ = true;
  switch (msg) {
    case WM_PAINT:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_COMMAND:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
    case WM_SIZE:
    case kRunMessage:
      Sync();
      break;
    default: break;
  }
}

void Root::PostInvoke(Key key) {
  posted_.push_back(key);
  PostMessageW(hwnd_, kRunMessage, 0, 0);
}

void Root::RunPosted() {
  std::vector<Key> keys;
  keys.swap(posted_);
  for (Key key : keys) {
    const Tree& t = Current();
    int i = t.Find(key);
    if (i < 0 || !t[i].enabled || !t[i].invoke) continue;
    Run(t[i].invoke);
  }
}

void Root::Run(std::function<void()> action) {
  if (!hwnd_) return;
  action();
  stale_ = true;
  InvalidateRect(hwnd_, nullptr, FALSE);
  Sync();
}

void Root::Sync() {
  if (!hwnd_ || building_) return;
  if (!UiaClientsAreListening()) {
    seen_.clear();
    haveSeen_ = false;
    return;
  }
  RaiseEvents(Current());
}

void Root::RaiseEvents(const Tree& t) {
  std::unordered_map<Key, Seen> now;
  now.reserve(t.Size());
  for (int i = 0; i < t.Size(); ++i) {
    const Node& n = t[i];
    Seen s;
    s.name = n.name;
    s.value = n.password ? L"" : n.value;
    s.status = n.status;
    s.range = n.rangeValue;
    s.scroll = n.scrollPercent;
    s.toggle = n.toggle;
    s.expand = n.expand;
    s.selected = n.selected;
    s.enabled = n.enabled;
    s.offscreen = n.offscreen;
    s.live = n.live;
    uint64_t h = 1469598103934665603ull;
    for (int c : n.children) h = (h ^ t[c].key) * 1099511628211ull;
    s.children = h;
    now.emplace(n.key, std::move(s));
  }
  const int focused = t.Focused();
  const Key focusKey = focused > 0 ? t[focused].key : 0;

  if (haveSeen_) {
    auto element = [&](Key key) { return static_cast<Element*>(root_->Make(key)); };
    auto changed = [&](Key key, PROPERTYID id, VARIANT before, VARIANT after) {
      Element* e = element(key);
      UiaRaiseAutomationPropertyChangedEvent(static_cast<IRawElementProviderSimple*>(e), id, before, after);
      e->Release();
      VariantClear(&before);
      VariantClear(&after);
    };
    auto bstr = [](const std::wstring& s) {
      VARIANT v;
      v.vt = VT_BSTR;
      v.bstrVal = SysAllocString(s.c_str());
      return v;
    };
    auto i4 = [](int x) {
      VARIANT v;
      v.vt = VT_I4;
      v.lVal = x;
      return v;
    };
    auto r8 = [](double x) {
      VARIANT v;
      v.vt = VT_R8;
      v.dblVal = x;
      return v;
    };
    auto boolean = [](bool b) {
      VARIANT v;
      v.vt = VT_BOOL;
      v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
      return v;
    };
    int structureEvents = 0;
    for (const auto& [key, s] : now) {
      auto it = seen_.find(key);
      if (it == seen_.end()) {
        if (s.live && !s.name.empty()) {
          Element* e = element(key);
          UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(e), UIA_LiveRegionChangedEventId);
          e->Release();
        }
        continue;
      }
      const Seen& o = it->second;
      if (o.name != s.name) {
        changed(key, UIA_NamePropertyId, bstr(o.name), bstr(s.name));
        if (s.live) {
          Element* e = element(key);
          UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(e), UIA_LiveRegionChangedEventId);
          e->Release();
        }
      }
      if (o.value != s.value) changed(key, UIA_ValueValuePropertyId, bstr(o.value), bstr(s.value));
      if (o.status != s.status) changed(key, UIA_ItemStatusPropertyId, bstr(o.status), bstr(s.status));
      if (o.toggle != s.toggle && s.toggle >= 0)
        changed(key, UIA_ToggleToggleStatePropertyId, i4(o.toggle), i4(s.toggle));
      if (o.expand != s.expand && s.expand >= 0)
        changed(key, UIA_ExpandCollapseExpandCollapseStatePropertyId, i4(o.expand), i4(s.expand));
      if (o.range != s.range) changed(key, UIA_RangeValueValuePropertyId, r8(o.range), r8(s.range));
      if (o.scroll != s.scroll)
        changed(key, UIA_ScrollVerticalScrollPercentPropertyId, r8(o.scroll), r8(s.scroll));
      if (o.enabled != s.enabled) changed(key, UIA_IsEnabledPropertyId, boolean(o.enabled), boolean(s.enabled));
      if (o.selected != s.selected) {
        changed(key, UIA_SelectionItemIsSelectedPropertyId, boolean(o.selected), boolean(s.selected));
        if (s.selected) {
          Element* e = element(key);
          UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(e), UIA_SelectionItem_ElementSelectedEventId);
          e->Release();
        }
      }
      if (o.children != s.children && structureEvents++ < 16) {
        Element* e = element(key);
        int id[3] = {UiaAppendRuntimeId, static_cast<int>(key >> 32), static_cast<int>(key & 0xFFFFFFFF)};
        UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(e), StructureChangeType_ChildrenInvalidated,
                                      key == kRootKey ? nullptr : id, key == kRootKey ? 0 : 3);
        e->Release();
      }
    }
    if (focusKey != seenFocus_ && focusKey != 0 && ::GetFocus() == hwnd_) {
      Element* e = element(focusKey);
      UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(e), UIA_AutomationFocusChangedEventId);
      e->Release();
    }
  }
  seen_ = std::move(now);
  seenFocus_ = focusKey;
  haveSeen_ = true;
}

}  // namespace uia
