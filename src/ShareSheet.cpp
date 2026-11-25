// Windows' share sheet (F1, f1-pdf): the Windows Runtime part of Share.h. The few objects it
// hands Windows (the request handler, the file's lookup, the one-item collection) are plain COM
// objects written out here: WRL's RuntimeClass and Callback cost about 30 KB for the same.
#include "Share.h"

#include <roapi.h>
#include <shobjidl_core.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.foundation.collections.h>
#include <windows.storage.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <new>
#include <type_traits>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;
namespace dt = ABI::Windows::ApplicationModel::DataTransfer;
namespace ws = ABI::Windows::Storage;

namespace share {

namespace {

// A COM object answering for one interface: a reference count, and agile through the
// free-threaded marshaler, as WRL's classes are (the file is found on a thread-pool thread).
// Made with a reference count of one, which the maker owns.
template <class I>
class Object : public I {
 public:
  Object() { CoCreateFreeThreadedMarshaler(static_cast<I*>(this), &ftm_); }
  virtual ~Object() {
    if (ftm_) ftm_->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(I) || iid == __uuidof(IAgileObject) ||
        (std::is_base_of_v<IInspectable, I> && iid == __uuidof(IInspectable))) {
      *out = static_cast<I*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == __uuidof(IMarshal) && ftm_) return ftm_->QueryInterface(iid, out);
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }

 private:
  LONG refs_ = 1;
  IUnknown* ftm_ = nullptr;
};

// The same, for a Windows Runtime interface (one with IInspectable's three methods).
template <class I>
class Inspectable : public Object<I> {
 public:
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
    *iids = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
    if (!*iids) return E_OUTOFMEMORY;
    **iids = __uuidof(I);
    *count = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
    return WindowsCreateString(L"Featherweight.Share", 19, name);
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override {
    *level = BaseTrust;
    return S_OK;
  }
};

// The one file being shared, as DataPackage::SetStorageItems takes it: a collection of one.
class OneIterator : public Inspectable<wfc::IIterator<ws::IStorageItem*>> {
 public:
  explicit OneIterator(ws::IStorageItem* item) : item_(item) {}
  HRESULT STDMETHODCALLTYPE get_Current(ws::IStorageItem** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (done_) return E_BOUNDS;
    return item_.CopyTo(out);
  }
  HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean* out) override {
    *out = !done_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MoveNext(boolean* out) override {
    done_ = true;
    *out = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMany(unsigned capacity, ws::IStorageItem** items, unsigned* actual) override {
    *actual = 0;
    if (done_ || capacity == 0) return S_OK;
    item_.CopyTo(items);
    *actual = 1;
    done_ = true;
    return S_OK;
  }

 private:
  ComPtr<ws::IStorageItem> item_;
  bool done_ = false;
};

class OneItem : public Inspectable<wfc::IIterable<ws::IStorageItem*>> {
 public:
  explicit OneItem(ws::IStorageItem* item) : item_(item) {}
  HRESULT STDMETHODCALLTYPE First(wfc::IIterator<ws::IStorageItem*>** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) OneIterator(item_.Get());
    return *out ? S_OK : E_OUTOFMEMORY;
  }

 private:
  ComPtr<ws::IStorageItem> item_;
};

// Puts the file into the package once Windows has found it, then lets the share sheet go on
// (its deferral) or the test (its event).
class FileFound : public Object<wf::IAsyncOperationCompletedHandler<ws::StorageFile*>> {
 public:
  FileFound(dt::IDataPackage* package, dt::IDataRequestDeferral* deferral, HANDLE done)
      : package_(package), deferral_(deferral), done_(done) {}
  HRESULT STDMETHODCALLTYPE Invoke(wf::IAsyncOperation<ws::StorageFile*>* async, wf::AsyncStatus status) override {
    ComPtr<ws::IStorageFile> file;
    ComPtr<ws::IStorageItem> item;
    if (status == wf::AsyncStatus::Completed && SUCCEEDED(async->GetResults(&file)) && file &&
        SUCCEEDED(file.As(&item))) {
      ComPtr<OneItem> items;
      items.Attach(new (std::nothrow) OneItem(item.Get()));
      if (items) package_->SetStorageItemsReadOnly(items.Get());
    }
    Finish(deferral_.Get(), done_);
    return S_OK;
  }
  static void Finish(dt::IDataRequestDeferral* deferral, HANDLE done) {
    if (deferral) deferral->Complete();
    if (done) SetEvent(done);
  }

 private:
  ComPtr<dt::IDataPackage> package_;
  ComPtr<dt::IDataRequestDeferral> deferral_;
  HANDLE done_;
};

// Looks the file up (asynchronously, as Windows only offers) and puts it into the package.
bool PutFile(dt::IDataPackage* package, const std::wstring& path, dt::IDataRequestDeferral* deferral, HANDLE done) {
  ComPtr<FileFound> found;
  found.Attach(new (std::nothrow) FileFound(package, deferral, done));
  ComPtr<ws::IStorageFileStatics> files;
  ComPtr<wf::IAsyncOperation<ws::StorageFile*>> op;
  if (!found ||
      FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_StorageFile).Get(), IID_PPV_ARGS(&files))) ||
      FAILED(files->GetFileFromPathAsync(HStringReference(path.c_str()).Get(), &op)) || FAILED(op->put_Completed(found.Get()))) {
    FileFound::Finish(deferral, done);
    return false;
  }
  return true;
}

// What the window shares next, kept until the share sheet asks for it.
struct Pending {
  std::wstring path, title;
  EventRegistrationToken token{};
  ComPtr<dt::IDataTransferManager> manager;
};
Pending& Current() {
  static Pending p;
  return p;
}

class Requested : public Object<wf::ITypedEventHandler<dt::DataTransferManager*, dt::DataRequestedEventArgs*>> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(dt::IDataTransferManager*, dt::IDataRequestedEventArgs* args) override {
    const Pending& p = Current();
    ComPtr<dt::IDataRequest> request;
    ComPtr<dt::IDataPackage> package;
    ComPtr<dt::IDataPackagePropertySet> props;
    ComPtr<dt::IDataRequestDeferral> deferral;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Data(&package)) ||
        FAILED(package->get_Properties(&props)))
      return S_OK;
    props->put_Title(HStringReference(p.title.c_str()).Get());
    // The sheet waits for the file (a deferral) while Windows finds it.
    if (SUCCEEDED(request->GetDeferral(&deferral))) PutFile(package.Get(), p.path, deferral.Get(), nullptr);
    return S_OK;
  }
};

}  // namespace

bool ShareFile(HWND hwnd, const std::wstring& path, const std::wstring& title) {
  Pending& p = Current();
  ComPtr<IDataTransferManagerInterop> interop;
  if (FAILED(RoGetActivationFactory(
          HStringReference(RuntimeClass_Windows_ApplicationModel_DataTransfer_DataTransferManager).Get(),
          IID_PPV_ARGS(&interop))))
    return false;
  ComPtr<dt::IDataTransferManager> manager;
  if (FAILED(interop->GetForWindow(hwnd, IID_PPV_ARGS(&manager)))) return false;
  if (!p.manager) {
    ComPtr<Requested> handler;
    handler.Attach(new (std::nothrow) Requested());
    if (!handler || FAILED(manager->add_DataRequested(handler.Get(), &p.token))) return false;
    p.manager = manager;
  }
  p.path = path;
  p.title = title;
  return SUCCEEDED(interop->ShowShareUIForWindow(hwnd));
}

bool PutFileForTesting(IUnknown* package, const std::wstring& path, HANDLE done) {
  ComPtr<dt::IDataPackage> data;
  if (!package || FAILED(package->QueryInterface(IID_PPV_ARGS(&data)))) return false;
  return PutFile(data.Get(), path, nullptr, done);
}

}  // namespace share
