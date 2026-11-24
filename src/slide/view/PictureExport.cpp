// File > Export > the current slide or all slides as PNG pictures (P3, p3-files), drawn as the slide
// view and Copy Slide draw them (SlideRender through Direct2D onto a WIC bitmap), at a chosen width
// (1920 pixels by default; the height follows the slide's shape). All slides go in a folder named as
// the file chosen, Slide1.png, Slide2.png..., as PowerPoint exports them.

#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "slide/view/PresentationView.h"
#include "ui/Gfx.h"

using Microsoft::WRL::ComPtr;

namespace fw::slide::view {
namespace {

constexpr unsigned kWidths[] = {1280, 1920, 2560, 3840};  // the dialog's choices; 1920 first chosen
constexpr DWORD kWidthBox = 1;

// One slide at `w` pixels across as a PNG file.
bool WriteSlidePng(Deck& deck, size_t slide, UINT w, const std::wstring& file) {
  const draw::Size s = deck.SlideSize();
  if (s.width <= 0 || s.height <= 0) return false;
  const UINT h = std::clamp(static_cast<UINT>(std::lround(static_cast<double>(w) * s.height / s.width)), 1u, 8192u);
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  ComPtr<ID2D1DeviceContext> dc;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(gfx::WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
      FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc)))
    return false;
  const HRESULT drawn = draw::DrawFrame(dc.Get(), [&](bool fallback) {
    dc->Clear(D2D1::ColorF(1, 1, 1, 1));
    draw::D2DPainter painter(dc.Get());
    painter.UsePdfLikeDrawing();
    painter.SetNoShadows(fallback);
    painter.Transform(draw::Matrix::Scale(w / s.width, h / s.height));
    deck.Paint(slide, painter);
  });
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  return SUCCEEDED(drawn) && SUCCEEDED(gfx::WIC()->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

}  // namespace

int PresentationView::ExportSlidePictures(const std::wstring& path, bool all, unsigned width, std::wstring& message) {
  if (!Ready() || SlideCount() == 0) return 0;
  const UINT w = std::clamp(width, 16u, 8192u);
  if (!all) {
    if (WriteSlidePng(*deck_, current_, w, path)) {
      message = L"Exported slide " + std::to_wstring(current_ + 1) + L" to " + FileNameOf(path);
      return 1;
    }
    message = FileNameOf(path) + L" couldn't be written.";
    return 0;
  }
  // A folder named as the file, beside it.
  std::wstring dir = path;
  if (const size_t dot = dir.rfind(L'.'); dot != std::wstring::npos && dot > dir.find_last_of(L"\\/") + 1) dir.resize(dot);
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    message = L"The folder " + FileNameOf(dir) + L" couldn't be made.";
    return 0;
  }
  int written = 0;
  for (size_t i = 0; i < SlideCount(); ++i)
    written += WriteSlidePng(*deck_, i, w, dir + L"\\Slide" + std::to_wstring(i + 1) + L".png") ? 1 : 0;
  message = written == static_cast<int>(SlideCount())
                ? L"Exported " + std::to_wstring(written) + L" slides to the folder " + FileNameOf(dir)
                : std::to_wstring(SlideCount() - written) + L" slides couldn't be written to the folder " + FileNameOf(dir) + L".";
  return written;
}

bool PresentationView::PictureCommand(int cmd, CommandState* state) {
  if (cmd != CmdExportSlidePng && cmd != CmdExportSlidesPng) return false;
  const bool all = cmd == CmdExportSlidesPng;
  if (state) {
    state->enabled = Ready() && SlideCount() > 0;
    state->label = all ? L"All slides as PNG…" : L"Current slide as PNG…";
    return true;
  }
  if (!Ready() || SlideCount() == 0) return true;
  // Save As for a PNG, with the picture's width beside the file type.
  ComPtr<IFileSaveDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return true;
  const COMDLG_FILTERSPEC png[] = {{L"PNG picture", L"*.png"}};
  dlg->SetFileTypes(1, png);
  dlg->SetDefaultExtension(L"png");
  std::wstring name = Title();
  if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
  dlg->SetFileName((all ? name : L"Slide" + std::to_wstring(current_ + 1)).c_str());
  dlg->SetTitle(all ? L"Export all slides as PNG" : L"Export the current slide as PNG");
  if (!Path().empty()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(DirOf(Path()).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  }
  ComPtr<IFileDialogCustomize> custom;
  if (SUCCEEDED(dlg.As(&custom))) {
    custom->StartVisualGroup(kWidthBox + 10, L"Width:");
    custom->AddComboBox(kWidthBox);
    for (DWORD i = 0; i < std::size(kWidths); ++i) {
      custom->AddControlItem(kWidthBox, i, (std::to_wstring(kWidths[i]) + L" pixels").c_str());
      if (kWidths[i] == pictureWidth_) custom->SetSelectedControlItem(kWidthBox, i);
    }
    custom->EndVisualGroup();
  }
  if (FAILED(dlg->Show(host_.ViewHwnd()))) return true;
  DWORD chosen = 0;
  if (custom && SUCCEEDED(custom->GetSelectedControlItem(kWidthBox, &chosen)) && chosen < std::size(kWidths))
    pictureWidth_ = kWidths[chosen];
  ComPtr<IShellItem> item;
  PWSTR file = nullptr;
  if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) return true;
  const std::wstring path = file;
  CoTaskMemFree(file);
  std::wstring message;
  const int written = ExportSlidePictures(path, all, pictureWidth_, message);
  host_.ViewMessage(written > 0 && (all ? written == static_cast<int>(SlideCount()) : true) ? BannerKind::Success
                                                                                             : BannerKind::Error,
                    message);
  return true;
}

// slidepng:file|width writes the current slide; slidepngs:file|width every slide, in the folder the
// file names (P3 picture export, for the pixel compare against the app's PDF).
bool PresentationView::PictureScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op != L"slidepng" && op != L"slidepngs") return false;
  std::wstring path = arg;
  unsigned width = 1920;
  if (const size_t bar = path.find(L'|'); bar != std::wstring::npos) {
    width = static_cast<unsigned>(std::max(0, _wtoi(path.c_str() + bar + 1)));
    path.resize(bar);
  }
  std::wstring message;
  const bool all = op == L"slidepngs";
  if (ExportSlidePictures(path, all, width, message) < (all ? static_cast<int>(SlideCount()) : 1))
    fwprintf(stderr, L"%s failed: %s\n", op.c_str(), message.c_str());
  return true;
}

}  // namespace fw::slide::view
