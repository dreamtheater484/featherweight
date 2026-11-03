#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "TabContent.h"
#include "ui/Controls.h"

// A tab with every control in it, for trying them by mouse and keyboard and for the visual and
// UI Automation checks (`--widget-gallery`). Its toolbar holds one of each kind too. Nothing in
// it reads or writes files.
class WidgetGallery : public TabContent {
 public:
  explicit WidgetGallery(ViewHost& host);

  // ---- TabContent ----
  TabKind Kind() const override { return TabKind::Gallery; }
  OpenResult Load(const std::wstring&, std::wstring&) override { return OpenResult::Ok; }
  bool Ready() const override { return true; }
  const std::wstring& Path() const override { return path_; }
  std::wstring Title() const override { return L"Widget gallery"; }
  bool Modified() const override { return false; }

  void SetViewport(const RECT& rc, float ui) override;
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc) override;
  bool HasIdleWork() const override { return false; }
  bool IdleWork(ID2D1DeviceContext*, double) override { return false; }
  void ReleaseDeviceResources() override {}
  void OnDeactivated() override;

  void OnMouseDown(int x, int y, int button, UINT mods, bool doubleClick) override;
  void OnMouseMove(int x, int y, UINT mods) override;
  void OnMouseUp(int x, int y, int button, UINT mods) override;
  void OnMouseLeave() override;
  void OnWheel(int delta, bool horizontal, UINT mods, int x, int y) override;
  bool OnKeyDown(UINT vk, UINT mods) override;
  bool OnChar(wchar_t ch, UINT mods) override;
  HCURSOR CursorAt(int x, int y) override;
  std::wstring TooltipAt(int x, int y) override;
  bool Dragging() const override { return group_.Dragging(); }
  bool HasKeyFocus() const override { return group_.Focused() != nullptr; }
  void SetKeyFocus(bool has) override;

  int ShortcutFor(UINT, UINT) const override { return 0; }
  CommandState QueryCommand(int cmd) override;
  bool Execute(int cmd) override;
  void BuildToolbar(ToolbarModel& m, const ToolbarContext& ctx) override;
  void FillContextMenu(HMENU, int, int) override {}

  void StartSearch(const std::wstring&, bool) override {}
  void ClearSearch() override {}
  void SearchStep(bool) override {}
  int SearchCount() const override { return 0; }
  int SearchCurrent() const override { return -1; }
  bool SearchDone() const override { return true; }
  std::wstring SelectedText() override { return {}; }

  bool Save(const std::wstring&, std::wstring& error) override;
  SaveFormat SaveAsFormat() const override { return {L"", L"", L""}; }
  bool CanPrint() const override { return false; }
  PrintOutcome Print(HWND, const std::function<void(int, int)>&, std::wstring&) override {
    return PrintOutcome::NotAllowed;
  }
  FileState State() const override { return {}; }
  void ApplyState(const FileState&) override {}
  void DescribeUia(uia::Tree& t, int parent) override;

 private:
  struct Card {
    std::wstring title, hint;
    D2D1_RECT_F rc{};
    std::vector<std::pair<std::wstring, Control*>> rows;  // label, control
  };
  // Shows what the last change was, under the title (and to screen readers).
  void Note(const std::wstring& what);
  std::wstring ColorName(D2D1_COLOR_F c, bool automatic, const std::wstring& automaticLabel) const;

  ViewHost& host_;
  std::wstring path_;
  RECT vp_{};
  float ui_ = 1;
  D2D1_RECT_F status_{};
  std::wstring last_ = L"Nothing yet";
  Control* lastFocused_ = nullptr;

  ControlGroup group_;  // before the controls, so it outlives them
  Dropdown style_, disabled_;
  ComboBox font_, size_;
  SplitButton fontColor_, bullets_;
  ColorPicker highlight_, pageColor_;
  SpinBox before_, lineSpacing_, columns_;
  ListBox files_;
  std::vector<Card> cards_;

  // What the toolbar's controls show.
  int tbStyle_ = 0;
  std::wstring tbFont_ = L"Segoe UI", tbSize_ = L"11";
  bool tbBold_ = false;
  D2D1_COLOR_F tbFontColor_ = gfx::Rgb(0xC00000), tbHighlight_ = gfx::Rgb(0xFFFF00);
  bool tbNoHighlight_ = false;
  double tbAfter_ = 8;
  int tbBullet_ = 0;
};
