#pragma once

// The Signature panel (F1, f1-pdf): a pad to sign on with a pen or the mouse, or a picture of a
// signature, then Place: the next click on a page puts it there as a stamp (pdf/sign/Stamp).
// What was drawn stays for the rest of the session, so signing again is Place and a click.

#include <utility>
#include <vector>

#include "pdf/PdfView.h"
#include "pdf/sign/Stamp.h"
#include "ui/SidePanel.h"

class PdfView::SignaturePanel : public PanelContent {
 public:
  explicit SignaturePanel(PdfView& view) : v_(view) {}

  std::wstring Title() const override;
  std::vector<Control*> Layout(const D2D1_RECT_F& body, float ui) override;
  void Draw(gfx::Canvas& c) override;
  bool OnMouseDown(float x, float y, bool dbl) override;
  bool OnMouseMove(float x, float y) override;
  void OnMouseUp(float x, float y) override;
  void OnMouseLeave() override { hover_ = -1; }
  std::wstring TooltipAt(float x, float y) override;

  // The signature to place: the strokes in their own box, or the picture.
  pdfsign::Ink Ink() const;
  bool Empty() const { return strokes_.empty() && picture_.empty(); }
  // A signature-like scribble, for the screen tests.
  void DrawForTesting();

 private:
  enum Button { kClear, kPicture, kPlace, kButtonCount };
  int ButtonAt(float x, float y) const;
  bool Enabled(int b) const;
  void Press(int b);
  bool PickPicture();

  PdfView& v_;
  float ui_ = 1;
  D2D1_RECT_F body_{}, pad_{}, hint_{};
  D2D1_RECT_F buttons_[kButtonCount]{};
  std::vector<std::vector<std::pair<float, float>>> strokes_;  // pad pixels from its corner, / ui
  bool drawing_ = false;
  int hover_ = -1, pressed_ = -1;
  // A picture instead of strokes: top-down BGRA.
  std::vector<uint8_t> picture_;
  int pictureW_ = 0, pictureH_ = 0;
  ComPtr<ID2D1Bitmap> pictureBmp_;
  ID2D1DeviceContext* pictureDc_ = nullptr;
};
