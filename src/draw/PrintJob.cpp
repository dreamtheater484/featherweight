#include "draw/PrintJob.h"

#include <DocumentTarget.h>
#include <ocidl.h>
#include <prntvpt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>

#include "draw/D2DPainter.h"

using Microsoft::WRL::ComPtr;

namespace draw {
namespace {

constexpr float kDipsPerPoint = 96.f / 72.f;

// Receives the spooler's progress reports for one job.
class StatusSink : public IPrintDocumentPackageStatusEvent {
 public:
  StatusSink() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~StatusSink() { CloseHandle(done_); }

  HANDLE Done() const { return done_; }
  PrintJob::Outcome Outcome() const { return outcome_; }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IDispatch) || iid == __uuidof(IPrintDocumentPackageStatusEvent)) {
      *out = static_cast<IPrintDocumentPackageStatusEvent*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }

  // IDispatch: the spooler calls PackageStatusUpdated directly.
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override {
    *n = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE PackageStatusUpdated(PrintDocumentPackageStatus* status) override {
    PrintJob::Outcome o = PrintJob::Outcome::InProgress;
    switch (status->Completion) {
      case PrintDocumentPackageCompletion_Completed: o = PrintJob::Outcome::Completed; break;
      case PrintDocumentPackageCompletion_Canceled: o = PrintJob::Outcome::Canceled; break;
      case PrintDocumentPackageCompletion_Failed: o = PrintJob::Outcome::Failed; break;
      default: break;
    }
    if (o != PrintJob::Outcome::InProgress) {
      outcome_ = o;
      SetEvent(done_);
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
  std::atomic<PrintJob::Outcome> outcome_{PrintJob::Outcome::InProgress};
  HANDLE done_;
};

// Forwards to the caller's output stream and tells when the print pipeline has let go of it. The
// printer driver can still be writing its output after the job reports completion.
class OutputStream : public IStream {
 public:
  explicit OutputStream(IStream* inner) : inner_(inner), released_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~OutputStream() { CloseHandle(released_); }

  HANDLE Released() const { return released_; }
  // Only the job's own reference is left.
  bool Free() const { return refs_ == 1; }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) || iid == __uuidof(IStream)) {
      *out = static_cast<IStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 1) SetEvent(released_);
    if (n == 0) delete this;
    return n;
  }

  // ISequentialStream and IStream
  HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* read) override { return inner_->Read(pv, cb, read); }
  HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* written) override {
    return inner_->Write(pv, cb, written);
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* pos) override {
    return inner_->Seek(move, origin, pos);
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override { return inner_->SetSize(size); }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream* to, ULARGE_INTEGER cb, ULARGE_INTEGER* read,
                                   ULARGE_INTEGER* written) override {
    return inner_->CopyTo(to, cb, read, written);
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override { return inner_->Commit(flags); }
  HRESULT STDMETHODCALLTYPE Revert() override { return inner_->Revert(); }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER off, ULARGE_INTEGER cb, DWORD type) override {
    return inner_->LockRegion(off, cb, type);
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER off, ULARGE_INTEGER cb, DWORD type) override {
    return inner_->UnlockRegion(off, cb, type);
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override { return inner_->Stat(stat, flags); }
  HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
    *out = nullptr;
    return E_NOTIMPL;
  }

 private:
  std::atomic<ULONG> refs_{1};
  ComPtr<IStream> inner_;
  HANDLE released_;
};

// The print dialog's settings as a print ticket, the XPS print path's form of them.
HRESULT TicketFromDevmode(const std::wstring& printer, const DEVMODEW* devmode, ComPtr<IStream>& ticket) {
  HPTPROVIDER provider = nullptr;
  HRESULT hr = PTOpenProvider(printer.c_str(), 1, &provider);
  if (FAILED(hr)) return hr;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, &ticket);
  if (SUCCEEDED(hr)) {
    hr = PTConvertDevModeToPrintTicket(provider, devmode->dmSize + devmode->dmDriverExtra,
                                       const_cast<DEVMODEW*>(devmode), kPTJobScope, ticket.Get());
  }
  PTCloseProvider(provider);
  if (SUCCEEDED(hr)) {
    LARGE_INTEGER zero{};
    hr = ticket->Seek(zero, STREAM_SEEK_SET, nullptr);
  }
  return hr;
}

}  // namespace

struct PrintJob::Impl {
  ComPtr<ID2D1Device> device;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<ID2D1PrintControl> control;
  ComPtr<IPrintDocumentPackageTarget> target;
  ComPtr<IConnectionPoint> events;
  DWORD cookie = 0;
  StatusSink* sink = nullptr;
  OutputStream* output = nullptr;
  D2DBitmapCache bitmaps;  // pictures used on several pages are uploaded once
  int pages = 0;
  bool finished = false;

  // Lets go of the print control and the package target, which hold on to the output stream.
  void ReleaseTarget() {
    if (events && cookie) events->Unadvise(cookie);
    cookie = 0;
    events.Reset();
    control.Reset();
    target.Reset();
  }

  ~Impl() {
    ReleaseTarget();
    if (sink) sink->Release();
    if (output) output->Release();
  }
};

PrintJob::PrintJob() = default;
PrintJob::~PrintJob() = default;

HRESULT PrintJob::Start(ID2D1Device* device, const std::wstring& printer, const DEVMODEW* devmode,
                        const std::wstring& jobName, IStream* output) {
  if (!device) return E_INVALIDARG;
  auto impl = std::make_unique<Impl>();
  impl->device = device;

  ComPtr<IStream> ticket;
  if (devmode) {
    HRESULT hr = TicketFromDevmode(printer, devmode, ticket);
    if (FAILED(hr)) return hr;
  }
  if (output) impl->output = new OutputStream(output);
  ComPtr<IPrintDocumentPackageTargetFactory> factory;
  HRESULT hr = CoCreateInstance(__uuidof(PrintDocumentPackageTargetFactory), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr))
    hr = factory->CreateDocumentPackageTargetForPrintJob(printer.c_str(), jobName.c_str(), impl->output,
                                                         ticket.Get(), &impl->target);
  if (FAILED(hr)) return hr;

  // Progress reports, so Wait can tell when the spooler has the whole job.
  ComPtr<IConnectionPointContainer> container;
  if (SUCCEEDED(impl->target.As(&container)) &&
      SUCCEEDED(container->FindConnectionPoint(__uuidof(IPrintDocumentPackageStatusEvent), &impl->events))) {
    impl->sink = new StatusSink();
    if (FAILED(impl->events->Advise(impl->sink, &impl->cookie))) impl->cookie = 0;
  }

  ComPtr<IWICImagingFactory> wic;
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  if (FAILED(hr)) return hr;
  // Pictures and effects that can't be vectors are rasterized at 300 dpi.
  D2D1_PRINT_CONTROL_PROPERTIES props{D2D1_PRINT_FONT_SUBSET_MODE_DEFAULT, 300.f, D2D1_COLOR_SPACE_SRGB};
  hr = device->CreatePrintControl(wic.Get(), impl->target.Get(), &props, &impl->control);
  if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl->dc);
  if (FAILED(hr)) return hr;
  impl_ = std::move(impl);
  return S_OK;
}

HRESULT PrintJob::AddPage(float widthPt, float heightPt, const std::function<void(Painter&)>& draw) {
  if (!impl_ || impl_->finished) return E_UNEXPECTED;
  ComPtr<ID2D1CommandList> list;
  HRESULT hr = impl_->dc->CreateCommandList(&list);
  if (FAILED(hr)) return hr;
  impl_->dc->SetTarget(list.Get());
  impl_->dc->BeginDraw();
  impl_->dc->SetTransform(D2D1::Matrix3x2F::Scale(kDipsPerPoint, kDipsPerPoint));
  {
    D2DPainter painter(impl_->dc.Get(), &impl_->bitmaps);
    draw(painter);
  }
  hr = impl_->dc->EndDraw();
  impl_->dc->SetTarget(nullptr);
  if (SUCCEEDED(hr)) hr = list->Close();
  if (SUCCEEDED(hr))
    hr = impl_->control->AddPage(list.Get(), D2D1::SizeF(widthPt * kDipsPerPoint, heightPt * kDipsPerPoint), nullptr);
  if (SUCCEEDED(hr)) ++impl_->pages;
  return hr;
}

HRESULT PrintJob::Finish() {
  if (!impl_ || impl_->finished) return E_UNEXPECTED;
  impl_->finished = true;
  return impl_->control->Close();
}

PrintJob::Outcome PrintJob::Status() const {
  return impl_ && impl_->sink ? impl_->sink->Outcome() : Outcome::InProgress;
}

namespace {

// Waits for `event` until `end`, handling window messages meanwhile: status updates and the
// printer driver's writes to the output stream can arrive through them. False on timeout.
bool PumpUntil(HANDLE event, ULONGLONG end) {
  for (;;) {
    ULONGLONG now = GetTickCount64();
    if (now >= end) return false;
    DWORD r = MsgWaitForMultipleObjects(1, &event, FALSE, static_cast<DWORD>(end - now), QS_ALLINPUT);
    if (r == WAIT_OBJECT_0) return true;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

}  // namespace

PrintJob::Outcome PrintJob::Wait(DWORD timeoutMs) {
  if (!impl_ || !impl_->sink) return Outcome::Failed;
  const ULONGLONG end = GetTickCount64() + timeoutMs;
  if (!PumpUntil(impl_->sink->Done(), end)) return Status();
  // With an output stream, the job is done only when the driver has written all of it.
  if (impl_->output && Status() == Outcome::Completed) {
    impl_->ReleaseTarget();
    // The event only says the count dropped to one at some point, so check again each time.
    while (!impl_->output->Free()) {
      ResetEvent(impl_->output->Released());
      if (impl_->output->Free()) break;
      if (!PumpUntil(impl_->output->Released(), end)) return Outcome::InProgress;
    }
  }
  return Status();
}

int PrintJob::Pages() const { return impl_ ? impl_->pages : 0; }

}  // namespace draw
