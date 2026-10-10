#pragma once

#include <ole2.h>
#include <windows.h>

#include <string>
#include <vector>

class DropHost {
 public:
  virtual ~DropHost() = default;
  virtual void OnDragEnter(bool acceptable) = 0;
  virtual void OnDragLeave() = 0;
  virtual void OnDropFiles(const std::vector<std::wstring>& files) = 0;
};

// OLE drop target, so dragging a PDF over the window can show a "Drop to open" overlay.
class DropTarget : public IDropTarget {
 public:
  explicit DropTarget(DropHost* host) : host_(host) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

  static std::vector<std::wstring> Files(IDataObject* data);

 private:
  LONG refs_ = 1;
  DropHost* host_;
  bool acceptable_ = false;
};
