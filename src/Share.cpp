#include "Share.h"

#include "TabContent.h"
#include "core/Strings.h"
#include "pdf/PdfText.h"
#include "pdf/PdfView.h"

#include <roapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.foundation.collections.h>
#include <windows.storage.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <filesystem>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::WinRt;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;
namespace dt = ABI::Windows::ApplicationModel::DataTransfer;
namespace ws = ABI::Windows::Storage;

namespace share {

namespace {

// The one file being shared, as the collection DataPackage::SetStorageItems takes.
class OneItemIterator : public RuntimeClass<RuntimeClassFlags<WinRt>, wfc::IIterator<ws::IStorageItem*>> {
  InspectableClass(L"Featherweight.Share.Iterator", BaseTrust)

 public:
  explicit OneItemIterator(ComPtr<ws::IStorageItem> item) : item_(std::move(item)) {}
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

class OneItem : public RuntimeClass<RuntimeClassFlags<WinRt>, wfc::IIterable<ws::IStorageItem*>> {
  InspectableClass(L"Featherweight.Share.Items", BaseTrust)

 public:
  explicit OneItem(ComPtr<ws::IStorageItem> item) : item_(std::move(item)) {}
  HRESULT STDMETHODCALLTYPE First(wfc::IIterator<ws::IStorageItem*>** out) override {
    ComPtr<OneItemIterator> it = Make<OneItemIterator>(item_);
    if (!it) return E_OUTOFMEMORY;
    return it.CopyTo(out);
  }

 private:
  ComPtr<ws::IStorageItem> item_;
};

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

HRESULT OnDataRequested(dt::IDataTransferManager*, dt::IDataRequestedEventArgs* args) {
  const Pending& p = Current();
  ComPtr<dt::IDataRequest> request;
  ComPtr<dt::IDataPackage> package;
  ComPtr<dt::IDataPackagePropertySet> props;
  if (FAILED(args->get_Request(&request)) || FAILED(request->get_Data(&package)) ||
      FAILED(package->get_Properties(&props)))
    return S_OK;
  props->put_Title(HStringReference(p.title.c_str()).Get());
  // The file is found asynchronously: the sheet waits for it (a deferral).
  ComPtr<dt::IDataRequestDeferral> deferral;
  ComPtr<ws::IStorageFileStatics> files;
  ComPtr<wf::IAsyncOperation<ws::StorageFile*>> op;
  if (FAILED(request->GetDeferral(&deferral)) ||
      FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_StorageFile).Get(), IID_PPV_ARGS(&files))) ||
      FAILED(files->GetFileFromPathAsync(HStringReference(p.path.c_str()).Get(), &op))) {
    if (deferral) deferral->Complete();
    return S_OK;
  }
  auto done = Callback<wf::IAsyncOperationCompletedHandler<ws::StorageFile*>>(
      [package, deferral](wf::IAsyncOperation<ws::StorageFile*>* async, wf::AsyncStatus status) -> HRESULT {
        ComPtr<ws::IStorageFile> file;
        ComPtr<ws::IStorageItem> item;
        if (status == wf::AsyncStatus::Completed && SUCCEEDED(async->GetResults(&file)) && file &&
            SUCCEEDED(file.As(&item))) {
          ComPtr<OneItem> items = Make<OneItem>(item);
          if (items) package->SetStorageItemsReadOnly(items.Get());
        }
        deferral->Complete();
        return S_OK;
      });
  if (!done || FAILED(op->put_Completed(done.Get()))) deferral->Complete();
  return S_OK;
}

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
    auto handler = Callback<wf::ITypedEventHandler<dt::DataTransferManager*, dt::DataRequestedEventArgs*>>(OnDataRequested);
    if (!handler || FAILED(manager->add_DataRequested(handler.Get(), &p.token))) return false;
    p.manager = manager;
  }
  p.path = path;
  p.title = title;
  return SUCCEEDED(interop->ShowShareUIForWindow(hwnd));
}

bool ShareTab(HWND hwnd, TabContent& tab, bool asPdf, std::wstring& message) {
  if (!tab.Ready()) return false;
  std::wstring base = fw::FileNameOf(tab.Path().empty() ? tab.Title() : tab.Path());
  if (const size_t dot = base.rfind(L'.'); dot != std::wstring::npos && dot > 0) base.resize(dot);
  if (base.empty()) base = T(L"Document");
  std::wstring path;
  if (asPdf && tab.Kind() != TabKind::Pdf) {
    if (!tab.CanExportPdf()) {
      message = T(L"This can't be made into a PDF.");
      return false;
    }
    path = TempFolder() + L"\\" + base + L".pdf";
    if (!tab.ExportPdf(path, message)) return false;
  } else if (PdfView* pdf = tab.AsPdf(); pdf && (tab.Modified() || tab.Path().empty())) {
    path = TempFolder() + L"\\" + base + L".pdf";
    if (!pdf->WriteCopy(path, message)) return false;
  } else if (tab.Path().empty() || tab.Modified()) {
    message = T(L"Save the file first: Share sends it as it's saved.");
    return false;
  } else {
    path = tab.Path();
  }
  if (!ShareFile(hwnd, path, fw::FileNameOf(path))) {
    message = T(L"Windows couldn't open its share sheet here.");
    return false;
  }
  message.clear();
  return true;
}

std::wstring TempFolder() {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::filesystem::path dir = std::filesystem::path(temp) / L"Featherweight share";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // Copies from earlier shares: a day is plenty for any app to have read them.
  const auto now = std::filesystem::file_time_type::clock::now();
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && now - e.last_write_time(ec) > std::chrono::hours(24)) std::filesystem::remove(e.path(), ec);
  }
  return dir.wstring();
}

}  // namespace share
