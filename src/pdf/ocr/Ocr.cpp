#include "pdf/ocr/Ocr.h"

#include <windows.h>

#include <MemoryBuffer.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.imaging.h>
#include <windows.media.ocr.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cstring>
#include <thread>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Implements;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Wrappers::Event;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;
namespace gi = ABI::Windows::Graphics::Imaging;
namespace wocr = ABI::Windows::Media::Ocr;

namespace pdfocr {

namespace {

std::wstring Text(HSTRING s) {
  UINT32 n = 0;
  const wchar_t* p = WindowsGetStringRawBuffer(s, &n);
  return std::wstring(p ? p : L"", n);
}

// Waits for a Windows Runtime operation: its completion comes on a thread pool thread.
template <typename T>
bool Await(wf::IAsyncOperation<T>* op) {
  Event done(CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
  if (!done.IsValid()) return false;
  auto handler = Callback<Implements<RuntimeClassFlags<ClassicCom>, wf::IAsyncOperationCompletedHandler<T>, FtmBase>>(
      [&done](wf::IAsyncOperation<T>*, wf::AsyncStatus) {
        SetEvent(done.Get());
        return S_OK;
      });
  if (!handler || FAILED(op->put_Completed(handler.Get()))) return false;
  WaitForSingleObjectEx(done.Get(), INFINITE, FALSE);
  wf::AsyncStatus status = wf::AsyncStatus::Error;
  ComPtr<wf::IAsyncInfo> info;
  return SUCCEEDED(ComPtr<wf::IAsyncOperation<T>>(op).As(&info)) && SUCCEEDED(info->get_Status(&status)) &&
         status == wf::AsyncStatus::Completed;
}

ComPtr<wocr::IOcrEngineStatics> Statics() {
  ComPtr<wocr::IOcrEngineStatics> statics;
  RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_Ocr_OcrEngine).Get(), IID_PPV_ARGS(&statics));
  return statics;
}

// A SoftwareBitmap holding a copy of the pixels.
ComPtr<gi::ISoftwareBitmap> MakeBitmap(const uint8_t* bgra, int width, int height, int stride) {
  ComPtr<gi::ISoftwareBitmapFactory> factory;
  ComPtr<gi::ISoftwareBitmap> bmp;
  if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Imaging_SoftwareBitmap).Get(),
                                    IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateWithAlpha(gi::BitmapPixelFormat_Bgra8, width, height, gi::BitmapAlphaMode_Ignore, &bmp)))
    return nullptr;
  ComPtr<gi::IBitmapBuffer> buffer;
  if (FAILED(bmp->LockBuffer(gi::BitmapBufferAccessMode_Write, &buffer))) return nullptr;
  gi::BitmapPlaneDescription plane{};
  ComPtr<wf::IMemoryBuffer> memory;
  ComPtr<wf::IMemoryBufferReference> ref;
  ComPtr<::Windows::Foundation::IMemoryBufferByteAccess> access;
  BYTE* data = nullptr;
  UINT32 capacity = 0;
  bool ok = SUCCEEDED(buffer->GetPlaneDescription(0, &plane)) && SUCCEEDED(buffer.As(&memory)) &&
            SUCCEEDED(memory->CreateReference(&ref)) && SUCCEEDED(ref.As(&access)) &&
            SUCCEEDED(access->GetBuffer(&data, &capacity)) && plane.Stride >= width * 4;
  if (ok) {
    for (int y = 0; y < height && ok; ++y) {
      const size_t at = static_cast<size_t>(plane.StartIndex) + static_cast<size_t>(y) * plane.Stride;
      ok = at + static_cast<size_t>(width) * 4 <= capacity;
      if (ok) memcpy(data + at, bgra + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
    }
  }
  ComPtr<wf::IClosable> close;
  if (ref && SUCCEEDED(ref.As(&close))) close->Close();
  if (SUCCEEDED(buffer.As(&close))) close->Close();
  return ok ? bmp : nullptr;
}

Status RecognizeHere(const uint8_t* bgra, int width, int height, int stride, Result& out) {
  ComPtr<wocr::IOcrEngineStatics> statics = Statics();
  if (!statics) return Status::NoEngine;
  ComPtr<wocr::IOcrEngine> engine;
  if (FAILED(statics->TryCreateFromUserProfileLanguages(&engine)) || !engine) return Status::NoLanguage;
  UINT32 most = 0;
  statics->get_MaxImageDimension(&most);
  if (width <= 0 || height <= 0 || (most && (static_cast<UINT32>(width) > most || static_cast<UINT32>(height) > most)))
    return Status::Failed;
  ComPtr<gi::ISoftwareBitmap> bmp = MakeBitmap(bgra, width, height, stride);
  if (!bmp) return Status::Failed;
  ComPtr<wf::IAsyncOperation<wocr::OcrResult*>> op;
  if (FAILED(engine->RecognizeAsync(bmp.Get(), &op)) || !Await(op.Get())) return Status::Failed;
  ComPtr<wocr::IOcrResult> result;
  if (FAILED(op->GetResults(&result)) || !result) return Status::Failed;

  ComPtr<ABI::Windows::Globalization::ILanguage> language;
  HSTRING tag = nullptr;
  if (SUCCEEDED(engine->get_RecognizerLanguage(&language)) && language && SUCCEEDED(language->get_LanguageTag(&tag))) {
    out.language = Text(tag);
    WindowsDeleteString(tag);
  }
  ComPtr<wf::IReference<double>> angle;
  if (SUCCEEDED(result->get_TextAngle(&angle)) && angle) angle->get_Value(&out.angle);

  ComPtr<wfc::IVectorView<wocr::OcrLine*>> lines;
  UINT32 lineCount = 0;
  if (FAILED(result->get_Lines(&lines)) || FAILED(lines->get_Size(&lineCount))) return Status::Failed;
  for (UINT32 i = 0; i < lineCount; ++i) {
    ComPtr<wocr::IOcrLine> line;
    ComPtr<wfc::IVectorView<wocr::OcrWord*>> words;
    UINT32 wordCount = 0;
    if (FAILED(lines->GetAt(i, &line)) || FAILED(line->get_Words(&words)) || FAILED(words->get_Size(&wordCount))) continue;
    Line l;
    for (UINT32 k = 0; k < wordCount; ++k) {
      ComPtr<wocr::IOcrWord> word;
      HSTRING text = nullptr;
      wf::Rect box{};
      if (FAILED(words->GetAt(k, &word)) || FAILED(word->get_Text(&text)) || FAILED(word->get_BoundingRect(&box))) continue;
      Word w;
      w.text = Text(text);
      WindowsDeleteString(text);
      w.x = box.X;
      w.y = box.Y;
      w.w = box.Width;
      w.h = box.Height;
      if (!w.text.empty() && w.w > 0 && w.h > 0) l.words.push_back(std::move(w));
    }
    if (!l.words.empty()) out.lines.push_back(std::move(l));
  }
  return Status::Ok;
}

// Runs `fn` where the Windows Runtime is (or can be made) multithreaded: on this thread, or on a
// thread of its own when this one is a single-threaded apartment (the UI thread), whose blocking
// wait could hold up the operation.
template <typename Fn>
auto InMta(Fn fn) {
  const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  if (hr == RPC_E_CHANGED_MODE) {
    decltype(fn()) result{};
    std::thread worker([&] {
      const HRESULT inner = RoInitialize(RO_INIT_MULTITHREADED);
      result = fn();
      if (SUCCEEDED(inner)) RoUninitialize();
    });
    worker.join();
    return result;
  }
  auto result = fn();
  if (SUCCEEDED(hr)) RoUninitialize();
  return result;
}

}  // namespace

uint32_t MaxDimension() {
  return InMta([] {
    UINT32 most = 0;
    if (ComPtr<wocr::IOcrEngineStatics> statics = Statics()) statics->get_MaxImageDimension(&most);
    return static_cast<uint32_t>(most);
  });
}

Status Recognize(const uint8_t* bgra, int width, int height, int stride, Result& out) {
  out = Result{};
  return InMta([&] { return RecognizeHere(bgra, width, height, stride, out); });
}

}  // namespace pdfocr
