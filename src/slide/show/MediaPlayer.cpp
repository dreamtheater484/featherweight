// Videos and audio in the slide show (P3, p3-present): Media Foundation's media engine. See MediaPlayer.h.
#include "slide/show/MediaPlayer.h"

#include <d2d1_1helper.h>
#include <d3d10.h>  // ID3D10Multithread
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <iterator>

#include "core/Strings.h"
#include "ui/Gfx.h"
#include "ui/SwapChain.h"

namespace fw::slide::show {

namespace {

// mfplat.dll, loaded at the first play; the Lite start-up leaves the sockets out.
using StartupFn = HRESULT(WINAPI*)(ULONG, DWORD);
using AttributesFn = HRESULT(WINAPI*)(IMFAttributes**, UINT32);
using DxgiManagerFn = HRESULT(WINAPI*)(UINT*, IMFDXGIDeviceManager**);
using OnStreamFn = HRESULT(WINAPI*)(IStream*, IMFByteStream**);
using MediaTypeFn = HRESULT(WINAPI*)(IMFMediaType**);
using ReaderFn = HRESULT(WINAPI*)(IMFByteStream*, IMFAttributes*, IMFSourceReader**);

struct Mf {
  bool tried = false, ok = false;
  AttributesFn attributes = nullptr;
  DxgiManagerFn dxgiManager = nullptr;
  OnStreamFn onStream = nullptr;
  MediaTypeFn mediaType = nullptr;
  ComPtr<IMFMediaEngineClassFactory> factory;
  // Videos decode on a device of their own, on the shared device's adapter, made at the first video.
  ComPtr<ID3D11Device> device;
  ComPtr<IMFDXGIDeviceManager> manager;
};

Mf& M() {
  static Mf m;
  return m;
}

template <typename F>
F Proc(HMODULE dll, const char* name) {
  return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

bool LoadMf() {
  Mf& m = M();
  if (m.tried) return m.ok;
  m.tried = true;
  HMODULE dll = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return false;
  auto startup = Proc<StartupFn>(dll, "MFStartup");
  m.attributes = Proc<AttributesFn>(dll, "MFCreateAttributes");
  m.dxgiManager = Proc<DxgiManagerFn>(dll, "MFCreateDXGIDeviceManager");
  m.onStream = Proc<OnStreamFn>(dll, "MFCreateMFByteStreamOnStream");
  m.mediaType = Proc<MediaTypeFn>(dll, "MFCreateMediaType");
  if (!startup || !m.attributes || !m.dxgiManager || !m.onStream || !m.mediaType ||
      FAILED(startup(MF_VERSION, MFSTARTUP_LITE)))
    return false;
  m.ok = SUCCEEDED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&m.factory)));
  return m.ok;
}

bool VideoDevice() {
  Mf& m = M();
  if (m.manager) return true;
  ID3D11Device* shared = gpu::D3DDevice();
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  if (!shared || FAILED(shared->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) return false;
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                              D3D_FEATURE_LEVEL_10_0};
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                               static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &m.device, nullptr, nullptr)))
    return false;
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(m.device.As(&mt))) mt->SetMultithreadProtected(TRUE);  // Media Foundation's threads use it too
  UINT token = 0;
  if (FAILED(m.dxgiManager(&token, &m.manager)) || FAILED(m.manager->ResetDevice(m.device.Get(), token))) {
    m.manager.Reset();
    m.device.Reset();
    return false;
  }
  return true;
}

// The engine's events: only an error matters (the state is asked for when needed).
class Notify final : public IMFMediaEngineNotify {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
      *out = static_cast<IMFMediaEngineNotify*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }
  STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override {
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) failed_ = true;
    return S_OK;
  }
  bool Failed() const { return failed_; }

 private:
  volatile LONG refs_ = 1;
  std::atomic<bool> failed_{false};
};

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// A picture as PNG bytes.
bool EncodePng(IWICBitmapSource* source, std::vector<uint8_t>& out) {
  IWICImagingFactory* wic = gfx::WIC();
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(nullptr, 0));
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (!wic || !stream || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    return false;
  STATSTG st{};
  if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart > 0x7FFFFFFF) return false;
  out.resize(static_cast<size_t>(st.cbSize.QuadPart));
  ULONG read = 0;
  const LARGE_INTEGER zero{};
  return SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) &&
         SUCCEEDED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) && read == out.size();
}

}  // namespace

struct MediaPlayer::Impl {
  ComPtr<IMFMediaEngine> engine;
  ComPtr<Notify> notify;
  bool video = true;
  bool started = false;
  bool wanted = false;  // told to play (the engine says so only once it has handled it)
  bool hasFrame = false;
  // A frame: decoded into `decoded` (the video device), which the shared device opens as `opened`
  // and copies into `copy`, which `bitmap` draws. The keyed mutex hands it from one device to the other.
  ComPtr<ID3D11Texture2D> decoded, opened, copy;
  ComPtr<IDXGIKeyedMutex> decodedLock, openedLock;
  ComPtr<ID2D1Bitmap1> bitmap;
  UINT w = 0, h = 0;
  unsigned generation = 0;
  // Stats: the video's times (100 ns) between the frames taken.
  uint64_t frames = 0;
  LONGLONG lastPts = -1;
  std::vector<LONGLONG> steps;

  bool Textures(ID2D1DeviceContext* dc, UINT width, UINT height);
  void DropTextures() {
    bitmap.Reset();
    copy.Reset();
    openedLock.Reset();
    opened.Reset();
    decodedLock.Reset();
    decoded.Reset();
    hasFrame = false;
    w = h = 0;
  }
};

bool MediaPlayer::Impl::Textures(ID2D1DeviceContext* dc, UINT width, UINT height) {
  if (bitmap && w == width && h == height && generation == gpu::Generation()) return true;
  DropTextures();
  ID3D11Device* shared = gpu::D3DDevice();
  Mf& m = M();
  if (!shared || !m.device) return false;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = width;
  d.Height = height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  ComPtr<IDXGIResource> resource;
  HANDLE handle = nullptr;
  ComPtr<IDXGISurface> surface;
  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
  bool ok = SUCCEEDED(m.device->CreateTexture2D(&d, nullptr, &decoded)) && SUCCEEDED(decoded.As(&decodedLock)) &&
            SUCCEEDED(decoded.As(&resource)) && SUCCEEDED(resource->GetSharedHandle(&handle)) &&
            SUCCEEDED(shared->OpenSharedResource(handle, IID_PPV_ARGS(&opened))) && SUCCEEDED(opened.As(&openedLock));
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = 0;
  ok = ok && SUCCEEDED(shared->CreateTexture2D(&d, nullptr, &copy)) && SUCCEEDED(copy.As(&surface)) &&
       SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap));
  if (!ok) {
    DropTextures();
    return false;
  }
  w = width;
  h = height;
  generation = gpu::Generation();
  return true;
}

MediaPlayer::MediaPlayer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MediaPlayer::~MediaPlayer() {
  if (impl_->engine) impl_->engine->Shutdown();  // its threads end; the callback is let go
}

std::unique_ptr<MediaPlayer> MediaPlayer::Open(Source source, const Settings& settings) {
  if (!PlayableMediaExtension(source.extension) || (source.bytes.empty() && source.path.empty())) return nullptr;
  if (source.bytes.size() > 0x7FFFFFFF) return nullptr;
  if (!LoadMf() || (source.video && !VideoDevice())) return nullptr;
  Mf& m = M();
  auto impl = std::make_unique<Impl>();
  impl->video = source.video;
  impl->notify.Attach(new Notify);
  ComPtr<IMFAttributes> attrs;
  if (FAILED(m.attributes(&attrs, 3))) return nullptr;
  attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, impl->notify.Get());
  if (source.video) {
    attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, m.manager.Get());
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);  // frame-server mode
  }
  ComPtr<IMFMediaEngineEx> ex;
  if (FAILED(m.factory->CreateInstance(source.video ? 0 : MF_MEDIA_ENGINE_AUDIOONLY, attrs.Get(), &impl->engine)) ||
      FAILED(impl->engine.As(&ex)))
    return nullptr;
  // The source is a stream of bytes, never a URL the engine would resolve.
  ComPtr<IStream> stream;
  if (!source.bytes.empty())
    stream.Attach(SHCreateMemStream(source.bytes.data(), static_cast<UINT>(source.bytes.size())));
  else
    SHCreateStreamOnFileEx(source.path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
                           nullptr, &stream);
  ComPtr<IMFByteStream> bytes;
  if (!stream || FAILED(m.onStream(stream.Get(), &bytes))) {
    impl->engine->Shutdown();
    return nullptr;
  }
  BSTR name = SysAllocString((L"media" + Lower(source.extension)).c_str());  // its type, by extension
  HRESULT hr = name ? ex->SetSourceFromByteStream(bytes.Get(), name) : E_OUTOFMEMORY;
  SysFreeString(name);
  if (FAILED(hr)) {
    impl->engine->Shutdown();
    return nullptr;
  }
  impl->engine->SetAutoPlay(FALSE);
  impl->engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
  impl->engine->SetVolume(std::clamp(static_cast<double>(settings.volume), 0.0, 1.0));
  impl->engine->SetMuted(settings.mute ? TRUE : FALSE);
  impl->engine->SetLoop(settings.loop ? TRUE : FALSE);
  return std::unique_ptr<MediaPlayer>(new MediaPlayer(std::move(impl)));
}

void MediaPlayer::Play(double fromMs) {
  Impl& v = *impl_;
  if (v.started || fromMs > 0) v.engine->SetCurrentTime(std::max(0.0, fromMs) / 1000);
  v.engine->Play();
  v.started = v.wanted = true;
}

void MediaPlayer::TogglePause() {
  Impl& v = *impl_;
  if (!v.started) Play(0);  // a click on a video that hasn't played yet plays it
  else if (!v.wanted || v.engine->IsEnded()) Resume();
  else Pause();
}

void MediaPlayer::Pause() {
  impl_->engine->Pause();
  impl_->wanted = false;
}

void MediaPlayer::Resume() {
  Impl& v = *impl_;
  if (v.engine->IsEnded()) v.engine->SetCurrentTime(0);
  v.engine->Play();
  v.started = v.wanted = true;
}

void MediaPlayer::Stop() {
  Impl& v = *impl_;
  v.engine->Pause();
  v.engine->SetCurrentTime(0);
  v.started = v.wanted = false;  // its poster again
  v.hasFrame = false;
}

bool MediaPlayer::Playing() const {
  const Impl& v = *impl_;
  return v.wanted && !v.notify->Failed() && !v.engine->IsEnded();
}

bool MediaPlayer::Shown() const { return impl_->started && !impl_->engine->IsEnded(); }

bool MediaPlayer::IsVideo() const { return impl_->video; }

ID2D1Bitmap1* MediaPlayer::Frame(ID2D1DeviceContext* dc, bool* changed) {
  Impl& v = *impl_;
  if (changed) *changed = false;
  if (!v.video || !v.started || !dc) return nullptr;
  LONGLONG pts = 0;
  DWORD w = 0, h = 0;
  const bool tick = v.engine->OnVideoStreamTick(&pts) == S_OK;
  if (tick) {
    ++v.frames;
    if (v.lastPts >= 0 && pts > v.lastPts && v.steps.size() < 1000000) v.steps.push_back(pts - v.lastPts);
    v.lastPts = pts;  // a loop starts again: earlier, not counted
  }
  if (tick && SUCCEEDED(v.engine->GetNativeVideoSize(&w, &h)) && w && h && w <= 16384 && h <= 16384 &&
      v.Textures(dc, w, h)) {
    if (SUCCEEDED(v.decodedLock->AcquireSync(0, 100))) {
      const RECT dst{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
      const MFARGB black{0, 0, 0, 255};
      const HRESULT hr = v.engine->TransferVideoFrame(v.decoded.Get(), nullptr, &dst, &black);
      v.decodedLock->ReleaseSync(1);
      if (SUCCEEDED(v.openedLock->AcquireSync(1, 100))) {
        ComPtr<ID3D11DeviceContext> ctx;
        gpu::D3DDevice()->GetImmediateContext(&ctx);
        if (SUCCEEDED(hr) && ctx) {
          ctx->CopyResource(v.copy.Get(), v.opened.Get());
          v.hasFrame = true;
          if (changed) *changed = true;
        }
        v.openedLock->ReleaseSync(0);
      } else {
        v.DropTextures();  // the hand-over is stuck: new textures next frame
      }
    }
  }
  return v.hasFrame ? v.bitmap.Get() : nullptr;
}

MediaPlayer::Stats MediaPlayer::VideoStats() const {
  const Impl& v = *impl_;
  Stats s;
  s.frames = v.frames;
  s.paused = v.engine->IsPaused() != FALSE;
  s.ended = v.engine->IsEnded() != FALSE;
  ComPtr<IMFMediaError> err;
  if (SUCCEEDED(v.engine->GetError(&err)) && err) {
    s.error = static_cast<int>(err->GetErrorCode());
    s.extended = static_cast<long>(err->GetExtendedErrorCode());
  }
  if (v.steps.empty()) return s;
  const LONGLONG step = *std::min_element(v.steps.begin(), v.steps.end());
  for (LONGLONG d : v.steps) {
    s.coveredMs += d / 10000.0;
    const long long n = std::llround(static_cast<double>(d) / static_cast<double>(step));
    if (n > 1) s.skipped += static_cast<uint64_t>(n - 1);
  }
  s.frameMs = step / 10000.0;
  return s;
}

bool PlayableMediaExtension(const std::wstring& extension) {
  static const wchar_t* const kPlayable[] = {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".asf", L".mpg",
                                             L".mpeg", L".3gp", L".mp3", L".m4a", L".aac", L".wav", L".wma"};
  const std::wstring e = Lower(extension);
  return std::any_of(std::begin(kPlayable), std::end(kPlayable), [&](const wchar_t* k) { return e == k; });
}

std::wstring LocalMediaPath(const std::string& target, const std::wstring& deckPath) {
  std::wstring t = Utf8ToWide(target);
  if (t.empty() || t.size() > 4096) return {};
  std::wstring path;
  if (Lower(t.substr(0, 5)) == L"file:") {
    wchar_t buf[4096];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (FAILED(PathCreateFromUrlW(t.c_str(), buf, &n, 0))) return {};
    path.assign(buf, n);
  } else {
    if (PathIsURLW(t.c_str()) || t.find(L"://") != std::wstring::npos) return {};  // http and the like: never
    std::replace(t.begin(), t.end(), L'/', L'\\');
    if (PathIsRelativeW(t.c_str())) {
      const size_t slash = deckPath.find_last_of(L"\\/");
      if (slash == std::wstring::npos || t.front() == L'\\') return {};
      path = deckPath.substr(0, slash + 1) + t;
    } else {
      path = t;
    }
  }
  wchar_t full[4096];
  const DWORD len = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
  if (len == 0 || len >= std::size(full)) return {};
  path.assign(full, len);
  // A local drive only: no share (\\server, \\?\UNC, devices), no network drive.
  if (path.size() < 3 || path[0] == L'\\' || path[1] != L':' || path[2] != L'\\') return {};
  const std::wstring root = path.substr(0, 3);
  const UINT kind = GetDriveTypeW(root.c_str());
  if (kind == DRIVE_REMOTE || kind == DRIVE_NO_ROOT_DIR || kind == DRIVE_UNKNOWN) return {};
  // A file that's there: not a cloud placeholder, which opening would download.
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)))
    return {};
  const wchar_t* dot = PathFindExtensionW(path.c_str());
  if (!PlayableMediaExtension(dot ? dot : L"")) return {};
  return path;
}

bool ReadMediaInfo(const std::vector<uint8_t>& bytes, const std::wstring& extension, bool video, MediaInfo* out) {
  if (!out || bytes.empty() || bytes.size() > 0x7FFFFFFF || !PlayableMediaExtension(extension) || !LoadMf())
    return false;
  Mf& m = M();
  static const ReaderFn reader = [] {
    HMODULE dll = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll ? Proc<ReaderFn>(dll, "MFCreateSourceReaderFromByteStream") : nullptr;
  }();
  ComPtr<IStream> stream;
  stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
  ComPtr<IMFByteStream> bs;
  if (!reader || !stream || FAILED(m.onStream(stream.Get(), &bs))) return false;
  ComPtr<IMFAttributes> named;
  if (SUCCEEDED(bs.As(&named))) named->SetString(MF_BYTESTREAM_ORIGIN_NAME, (L"media" + Lower(extension)).c_str());
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFSourceReader> r;
  if (FAILED(m.attributes(&attrs, 1)) || FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
      FAILED(reader(bs.Get(), attrs.Get(), &r)))
    return false;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  if (SUCCEEDED(r->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &pv)) &&
      pv.vt == VT_UI8)
    out->durationMs = static_cast<double>(pv.uhVal.QuadPart) / 10000;
  PropVariantClear(&pv);
  if (!video) return true;
  // The first frame, as 32-bit RGB.
  const DWORD stream0 = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  r->SetStreamSelection(stream0, TRUE);
  ComPtr<IMFMediaType> type, actual;
  if (FAILED(m.mediaType(&type)) || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
      FAILED(r->SetCurrentMediaType(stream0, nullptr, type.Get())) || FAILED(r->GetCurrentMediaType(stream0, &actual)))
    return false;
  UINT32 w = 0, h = 0;
  if (FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h || w > 16384 || h > 16384)
    return false;
  LONG stride = static_cast<LONG>(w * 4);
  UINT32 given = 0;
  if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &given))) stride = static_cast<LONG>(given);  // < 0: bottom up
  const size_t row = static_cast<size_t>(w) * 4, across = static_cast<size_t>(std::abs(stride));
  for (int tries = 0; tries < 64; ++tries) {
    DWORD flags = 0;
    LONGLONG at = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(r->ReadSample(stream0, 0, nullptr, &flags, &at, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
      return false;
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buffer;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    std::vector<uint8_t> pixels;
    const bool fits = across >= row && across * h <= length;
    if (fits) {
      pixels.resize(row * h);
      for (UINT32 y = 0; y < h; ++y)
        memcpy(&pixels[y * row], data + (stride >= 0 ? y : h - 1 - y) * across, row);
    }
    buffer->Unlock();
    if (!fits) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(gfx::WIC()->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, static_cast<UINT>(row),
                                                  static_cast<UINT>(pixels.size()), pixels.data(), &bitmap)) ||
        !EncodePng(bitmap.Get(), out->posterPng))
      return false;
    out->width = w;
    out->height = h;
    return true;
  }
  return false;
}

std::vector<uint8_t> AudioIconPng(UINT size) {
  // The speaker glyph of the icon font, dark gray on transparent.
  std::vector<uint8_t> png;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> rt;
  ComPtr<IDWriteTextFormat> format;
  ComPtr<ID2D1SolidColorBrush> brush;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (!gfx::WIC() || FAILED(gfx::WIC()->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bitmap)) ||
      FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)) ||
      FAILED(gfx::DWrite()->CreateTextFormat(gfx::IconFont().c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * 0.8f, L"",
                                             &format)) ||
      FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0x404040), &brush)))
    return png;
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  static const wchar_t kSpeaker[] = L"\xE767";  // Volume
  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  rt->DrawText(kSpeaker, 1, format.Get(), D2D1::RectF(0, 0, static_cast<float>(size), static_cast<float>(size)),
               brush.Get());
  if (FAILED(rt->EndDraw()) || !EncodePng(bitmap.Get(), png)) png.clear();
  return png;
}

}  // namespace fw::slide::show
