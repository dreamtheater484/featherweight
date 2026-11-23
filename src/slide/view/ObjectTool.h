#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TabContent.h"
#include "slide/objects/Frame.h"

struct ToolItem;
#include "slide/objects/Guides.h"
#include "slide/objects/Selection.h"
#include "ui/Toolbar.h"

namespace gfx {
class Canvas;
}

namespace fw::slide::view {

class Deck;

// Objects on the slide in the presentation tab (P2, p2-objects; plan §8.2 "Objects"): selecting
// (click, Shift/Ctrl+click, a marquee, Tab, a double-click into a group), moving, resizing and
// turning with smart guides and snapping (Alt: none), arrow-key nudges, align and distribute,
// order, group and ungroup, deleting, inserting pictures, shapes, text boxes, tables and charts,
// crop, and the clipboard. Every change is one undo step of the deck's editor (src/slide/edit).
//
// While an object is dragged, the slide is drawn once without it and once with only it, and each
// frame draws those two bitmaps, the second moved (or turned and scaled), so a drag keeps up with
// the display whatever the slide holds; the edit is made when the button comes up.
//
// The tab hooks it in: the slide's mouse and keys go here first (text editing, p2-text, takes a
// press inside text: see MouseDown), and its overlay is drawn after the slide.
class ObjectTool {
 public:
  struct Host {
    std::function<Deck*()> deck;
    std::function<size_t()> current;               // the slide shown
    std::function<D2D1_RECT_F()> slideRect;        // the slide on screen, client pixels
    std::function<float()> ui;                     // pixels per DIP
    std::function<void()> invalidate;              // repaint
    std::function<void(const std::wstring&)> refused;  // a banner: why an edit can't be made
    std::function<HWND()> window;
    std::function<void(size_t slide)> slideEdited;  // drop what was drawn of the slide
    // Text editing starts in a shape (p2-text's BeginTextEdit): after inserting a text box, so
    // typing goes into it. Unset: the text box is only selected.
    std::function<void(uint32_t shape)> beginText;
    // The cells chosen in table `table` (text editing's: the cell with the caret, or a range), rows
    // and columns inclusive. False when none is: row and column commands then work at the ends.
    std::function<bool(uint32_t table, int& row0, int& col0, int& row1, int& col1)> tableCells;
  };
  explicit ObjectTool(Host host);
  ~ObjectTool();

  // ---- The pointer and keys (client pixels). MouseDown takes the press when it lands on a
  // handle, on an object, or on the empty slide (a marquee); `textTakes` tells it a press inside
  // a text body goes to text editing instead (then call SelectForText). False: not taken.
  bool MouseDown(float x, float y, UINT mods, bool doubleClick);
  // Whether a press here is the objects' before text editing looks at it: a handle of the
  // selection, a selected object's frame edge, or an object without text.
  bool WantsPress(float x, float y) const;
  void MouseMove(float x, float y, UINT mods);
  void MouseUp(float x, float y, UINT mods);
  bool Dragging() const { return drag_.kind != DragKind::None; }
  bool KeyDown(UINT vk, UINT mods);
  HCURSOR Cursor(float x, float y) const;  // null: not over anything of the objects'
  std::wstring Tooltip(float x, float y) const;

  // ---- Drawing: after the slide (the drag's layers over it, then frames, handles, guides).
  void Draw(gfx::Canvas& c, ID2D1DeviceContext* dc);
  void ReleaseDeviceResources();

  // ---- Commands (CmdGroupObjects..., and the shared CmdCut/CmdCopy/CmdPaste/CmdDeleteObject/
  // CmdSelectAll/CmdInsertPicture/CmdInsertTable/CmdInsertChart when objects are selected).
  int Shortcut(UINT vk, UINT mods) const;
  bool Query(int cmd, CommandState& s);
  bool Execute(int cmd);
  std::vector<int> Commands() const;
  void AddTools(ToolbarModel& m);
  // The control for one of the objects' commands, as the ribbon and toolbar show it: the Insert
  // dropdowns (shapes, table, chart), Arrange, fill and outline colors, Weight, Crop.
  ToolItem Tool(int cmd);
  void ContextMenu(HMENU menu, float x, float y);
  // A table's items (insert and delete rows and columns, merge, split) when the selected object is
  // a table: the view adds them after text editing's own menu in a cell.
  void TableContextMenu(HMENU menu);

  // Scripts (--script, the build-from-scratch deck): insert:table|<rows>x<cols>,
  // insert:chart|column|bar|line|pie, insert:shape|<preset>|x,y,w,h, insert:textbox|x,y,w,h (points),
  // shapefill:<RRGGBB>, tablemerge:<row0>,<col0>,<row1>,<col1> and tablesplit:<row>,<col> (the selected
  // table, 0-based). False for other ops; what an op of these can't do goes to stderr.
  bool ScriptOp(const std::wstring& op, const std::wstring& arg);

  // Pictures dropped on the slide (files), at client pixels (-1: centred).
  bool TakesPictures(const std::vector<std::wstring>& files) const;
  void DropPictures(const std::vector<std::wstring>& files, float x, float y);

  // ---- Selection, for the tab and text editing.
  const std::vector<uint32_t>& Selection() const { return selection_; }
  uint32_t SelectedShape() const { return selection_.size() == 1 ? selection_[0] : 0; }
  void Select(const std::vector<uint32_t>& ids);
  // Text editing started in `id`: it's the selection, its frame drawn as text editing shows it.
  void SelectForText(uint32_t id);
  void SetTextEditing(bool on) { textEditing_ = on; }
  void ClearSelection();
  // The slide shown changed, or the model did: the map is read again, gone objects unselected.
  void SlideChanged();

  // Tests and scripts: the slide's objects as the tool sees them.
  const objects::ObjectMap& Map();

 private:
  enum class DragKind : uint8_t { None, Pending, Move, Resize, Rotate, LineEnd, Crop, Marquee };
  struct Moving {
    uint32_t id = 0;
    objects::Frame start, now;  // on the slide
  };
  struct DragState {
    DragKind kind = DragKind::None;
    objects::Handle handle = objects::Handle::None;
    uint32_t grabbed = 0;           // the object whose handle (or body) was pressed
    draw::Point from{}, at{};       // slide points
    float downX = 0, downY = 0;     // client pixels
    bool copy = false;              // Ctrl+drag: the objects are copied
    std::vector<Moving> objects;
    draw::Rect marquee{};
    bool additive = false;          // Shift/Ctrl marquee: added to the selection
    objects::Crop cropStart;
  };

  // Coordinates.
  draw::Point ToSlide(float x, float y) const;
  D2D1_POINT_2F ToScreen(draw::Point p) const;
  float Scale() const;  // pixels per point
  float Tolerance() const;  // points: a few pixels
  float RotateOffset() const;  // points: the turn handle above the top edge
  objects::HandleSet HandlesOf(const objects::ObjectInfo& o) const;
  // The selected object and handle under (x, y); Handle::None when none.
  objects::Handle HandleAt(float x, float y, uint32_t* id) const;

  // Dragging.
  void StartDrag(UINT mods);
  void UpdateDrag(UINT mods);
  void FinishDrag(UINT mods);
  std::vector<draw::Rect> OtherBoxes() const;
  objects::SnapSettings Snapping(UINT mods) const;
  void MakeLayers(ID2D1DeviceContext* dc);
  void DropLayers();

  // Drawing.
  void DrawFrames(gfx::Canvas& c);
  void DrawGuides(gfx::Canvas& c);

  // Edits (through the deck's editor; false with a banner when they can't be made).
  bool Editable(bool say = true) const;
  bool CommitFrames(const std::vector<Moving>& moved, const std::u16string& step);
  bool Nudge(float dx, float dy);
  bool DeleteSelection();
  bool Arrange(int cmd);
  bool AlignOrDistribute(int cmd);
  bool GroupSelection();
  bool UngroupSelection();
  bool Turn(float degrees);
  bool Flip(bool horizontal);
  bool Copy(bool cut);
  bool Paste();
  bool DuplicateSelection(float dx, float dy);
  bool InsertPictures(const std::vector<std::wstring>& files, draw::Point at, bool centred);
  bool InsertPictureBytes(std::vector<uint8_t> bytes, draw::Point at, bool centred);
  bool InsertShape(int index);
  bool InsertPreset(const std::string& preset, const draw::Rect& r);
  bool InsertTextBox();
  bool InsertTextBox(const draw::Rect& r);
  bool InsertTable(int rows, int columns);
  bool InsertChart(int type);
  bool TableEdit(int cmd, const int* cells = nullptr);  // cells: row0, col0, row1, col1 (a script's)
  uint32_t SelectedTable() const;                         // the one selected object when it's a table
  bool SetFillColor(std::optional<uint32_t> rgb, bool none);
  bool SetLineColor(std::optional<uint32_t> rgb, bool none);
  bool SetLineWidth(float points);
  bool SetShadow(bool on);
  bool ToggleCrop();
  void Refused(const std::wstring& why) const;
  void Edited();

  Host host_;
  objects::ObjectMap map_;
  size_t mapSlide_ = SIZE_MAX;
  bool mapDirty_ = true;
  std::vector<uint32_t> selection_;
  uint32_t level_ = 0;          // the group being edited (0: the slide)
  bool textEditing_ = false;
  uint32_t textShape_ = 0;      // selected for text editing
  bool cropMode_ = false;
  bool alignToSlide_ = false;   // Align > Align to Slide (always with one object)
  DragState drag_;
  std::vector<objects::Guide> guides_;
  bool listening_ = false;      // registered with the editor

  // The drag's layers: the slide without the dragged objects, and the dragged objects alone.
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> under_, layer_;
  bool layersDue_ = false;
  D2D1_RECT_F layersRect_{};
};

// The open dialog for pictures (PNG, JPEG, GIF, BMP, TIFF, EMF, WMF, SVG): the files chosen, or none
// when cancelled. Shared by inserting pictures and picture fills and backgrounds.
std::vector<std::wstring> PickPictureFiles(HWND owner, bool several);

}  // namespace fw::slide::view
