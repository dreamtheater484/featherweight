// Export PDF for Word documents (plan §4.6, milestone D4): the pages drawn through draw::PdfDocument
// with the same Painter code as the screen, plus what a PDF reader shows besides the pages: links,
// an outline from the headings, and the title and author.
#include <wincodec.h>
#include <wrl/client.h>

#include <pugixml.hpp>

#include "Util.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Pdf.h"
#include "pkg/Package.h"
#include "ui/Gfx.h"
#include "word/DocView.h"

using Microsoft::WRL::ComPtr;
namespace layout = fw::doc::layout;

namespace {

std::u16string U16(const std::wstring& s) { return std::u16string(s.begin(), s.end()); }

// dc:title and dc:creator from the core properties.
void CoreProperties(const fw::pkg::Package& pkg, std::wstring& title, std::wstring& author) {
  std::string name;
  if (const fw::pkg::RelationshipSet* rels = pkg.RelationshipsOf("/"))
    for (const fw::pkg::Relationship& r : rels->items)
      if (!r.external && r.type.ends_with("/metadata/core-properties")) name = r.targetPart;
  std::vector<uint8_t> bytes;
  if (name.empty() || !pkg.ReadPart(name, bytes).Ok()) return;
  pugi::xml_document xml;
  if (!xml.load_buffer(bytes.data(), bytes.size(), pugi::parse_default, pugi::encoding_utf8)) return;
  for (pugi::xml_node n : xml.document_element().children()) {
    std::string_view tag = n.name();
    tag = tag.substr(tag.find(':') == std::string_view::npos ? 0 : tag.find(':') + 1);
    std::wstring value = fw::Trim(fw::Utf8ToWide(n.text().get()));
    if (tag == "title") title = value;
    else if (tag == "creator") author = value;
  }
}

bool WriteAtomically(const std::wstring& path, const std::vector<uint8_t>& bytes, std::wstring& error) {
  const std::wstring tmp = fw::DirOf(path) + L"\\~" + fw::FileNameOf(path) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = GetLastError() == ERROR_ACCESS_DENIED ? L"Featherweight can't save in this folder."
                                                  : L"The PDF couldn't be saved. " + util::LastErrorMessage(GetLastError());
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() &&
            FlushFileBuffers(h);
  CloseHandle(h);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!ok) {
    const DWORD e = GetLastError();
    DeleteFileW(tmp.c_str());
    error = (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED)
                ? L"The PDF couldn't be saved: another program has " + fw::FileNameOf(path) + L" open."
                : L"The PDF couldn't be saved. " + util::LastErrorMessage(e);
  }
  return ok;
}

}  // namespace

bool DocView::ExportPdf(const std::wstring& path, std::wstring& message) {
  if (!Ready() || !doc_ || !pagination_) {
    message = L"This document can't be exported yet.";
    return false;
  }
  pagination_->LayOutAll();
  TakeNewPages();
  // Formatting marks and hidden text are for the screen: shown, the pages are laid out again
  // without them, as Word exports.
  std::unique_ptr<layout::Layout> plain;
  layout::Layout* pages = pagination_.get();
  if (marks_) {
    layout::Options options = LayoutOptions();
    options.showHiddenText = options.formattingMarks = false;
    plain = std::make_unique<layout::Layout>(*doc_, *fonts_, options);
    plain->LayOutAll();
    pages = plain.get();
  }
  const size_t count = pages->PageCount();
  if (count == 0) {
    message = L"This document has no pages to export.";
    return false;
  }
  draw::PdfDocument pdf;
  auto drawing = [this](draw::Painter& p, const layout::DrawingBox& box) { DrawDrawing(p, box); };
  for (size_t i = 0; i < count; ++i) {
    const layout::Page& page = pages->GetPage(i);
    pdf.AddPage(page.width, page.height, [&](draw::Painter& painter) { pages->Paint(painter, i, drawing); });
  }

  // Links: each hyperlink's text, wherever it's laid out. Web addresses only (as a click in the
  // app opens), and places in the document by their bookmark.
  auto bookmark = [&](std::string_view name) -> std::optional<layout::Layout::Caret> {
    for (const auto& pair : doc_->Pairs()) {
      if (pair.kind != fw::pkg::RefKind::BookmarkId || pair.openPos.para == fw::doc::kNone) continue;
      if (pair.open.AttrOr(fw::doc::Name("name")) != name) continue;
      return pages->CaretAt({pair.openPos.para, pair.openPos.offset});
    }
    return std::nullopt;
  };
  for (uint32_t p = 0; p < doc_->Paragraphs().size(); ++p) {
    const auto& runs = doc_->Runs(p);
    for (size_t i = 0; i < runs.size();) {
      if (!runs[i].hyperlink) {
        ++i;
        continue;
      }
      // The runs of one link.
      size_t j = i + 1;
      while (j < runs.size() && runs[j].hyperlink && runs[j].hyperlink.Id() == runs[i].hyperlink.Id()) ++j;
      const uint32_t from = runs[i].start, to = runs[j - 1].end;
      const std::string target = doc_->HyperlinkTarget(runs[i].hyperlink);
      i = j;
      if (target.empty() || from >= to) continue;
      const auto rects = pages->Ranges({p, from}, {p, to});
      if (rects.empty()) continue;
      if (target[0] == '#') {
        const auto dest = bookmark(std::string_view(target).substr(1));
        if (!dest) continue;
        for (const auto& [page, r] : rects) pdf.AddLink(page, {r.left, r.top, r.right, r.bottom}, dest->page, dest->top);
      } else if (util::IsSafeExternalLink(fw::Utf8ToWide(target))) {
        for (const auto& [page, r] : rects) pdf.AddLink(page, {r.left, r.top, r.right, r.bottom}, target);
      }
    }
  }

  // Bookmarks: the headings, as the navigation pane lists them.
  for (const Heading& h : Headings())
    if (auto at = pages->CaretAt({h.para, 0})) pdf.AddOutline(h.level, U16(h.text), at->page, at->top);

  std::wstring title, author;
  CoreProperties(doc_->Package(), title, author);
  if (title.empty()) {
    title = Title();
    if (const size_t dot = title.rfind(L'.'); dot != std::wstring::npos && dot > 0) title.resize(dot);
  }
  pdf.SetInfo(U16(title), U16(author));
  const std::vector<uint8_t> bytes = pdf.Finish();
  if (!WriteAtomically(path, bytes, message)) return false;
  message = count == 1 ? L"Exported 1 page to " + fw::FileNameOf(path)
                       : L"Exported " + std::to_wstring(count) + L" pages to " + fw::FileNameOf(path);
  return true;
}

int DocView::RenderPagesForTesting(const std::wstring& dir, float dpi) {
  if (!Ready() || !pagination_) return 0;
  pagination_->LayOutAll();
  TakeNewPages();
  auto drawing = [this](draw::Painter& p, const layout::DrawingBox& box) { DrawDrawing(p, box); };
  const float scale = dpi / 72;
  int written = 0;
  for (size_t i = 0; i < pagination_->PageCount(); ++i) {
    const layout::Page& page = pagination_->GetPage(i);
    const UINT w = std::max(1u, static_cast<UINT>(std::ceil(page.width * scale)));
    const UINT h = std::max(1u, static_cast<UINT>(std::ceil(page.height * scale)));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext> dc;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    if (FAILED(gfx::WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc))) {
      break;
    }
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc.Get());
      painter.Transform(draw::Matrix::Scale(scale, scale));
      pagination_->Paint(painter, i, drawing);
    }
    if (FAILED(dc->EndDraw())) break;
    const std::wstring file = dir + L"\\page-" + std::to_wstring(i + 1) + L".png";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(gfx::WIC()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
        FAILED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
      break;
    }
    ++written;
  }
  return written;
}
