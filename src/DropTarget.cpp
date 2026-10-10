#include "DropTarget.h"

#include <shellapi.h>

#include "core/Strings.h"

HRESULT DropTarget::QueryInterface(REFIID riid, void** ppv) {
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *ppv = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

ULONG DropTarget::Release() {
  ULONG r = InterlockedDecrement(&refs_);
  if (r == 0) delete this;
  return r;
}

std::vector<std::wstring> DropTarget::Files(IDataObject* data) {
  std::vector<std::wstring> files;
  FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&fmt, &medium))) return files;
  if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
      UINT len = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(len + 1, L'\0');
      DragQueryFileW(drop, i, path.data(), len + 1);
      path.resize(len);
      files.push_back(path);
    }
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  return files;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) {
  acceptable_ = false;
  for (const auto& f : Files(data))
    if (fw::EndsWithNoCase(f, L".pdf")) acceptable_ = true;
  *effect = acceptable_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  host_->OnDragEnter(acceptable_);
  return S_OK;
}

HRESULT DropTarget::DragOver(DWORD, POINTL, DWORD* effect) {
  *effect = acceptable_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}

HRESULT DropTarget::DragLeave() {
  host_->OnDragLeave();
  return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) {
  std::vector<std::wstring> files = Files(data);
  *effect = acceptable_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  host_->OnDragLeave();
  host_->OnDropFiles(files);
  return S_OK;
}
