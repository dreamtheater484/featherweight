#include "FormFill.h"

#include <windows.h>

#include <fpdf_annot.h>

#include <algorithm>
#include <map>
#include <string>

#include "Document.h"
#include "core/Strings.h"

namespace {

std::map<UINT_PTR, TimerCallback>& Timers() {
  static std::map<UINT_PTR, TimerCallback> timers;
  return timers;
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
  auto it = Timers().find(id);
  if (it != Timers().end() && it->second) it->second(static_cast<int>(id));
}

FormEnv* Env(FPDF_FORMFILLINFO* p) { return static_cast<FormEnv*>(p); }
DocumentHost* Host(FPDF_FORMFILLINFO* p) { return Env(p)->doc->HostPtr(); }

std::wstring Wide(FPDF_WIDESTRING s) { return s ? std::wstring(reinterpret_cast<const wchar_t*>(s)) : std::wstring(); }

int WriteWide(const std::wstring& s, void* buffer, int length) {
  int bytes = static_cast<int>((s.size() + 1) * sizeof(wchar_t));
  if (buffer && length >= bytes) memcpy(buffer, s.c_str(), bytes);
  return bytes;
}

// ---- FPDF_FORMFILLINFO callbacks ----

void FFI_Invalidate(FPDF_FORMFILLINFO* p, FPDF_PAGE page, double l, double t, double r, double b) {
  if (auto* h = Host(p)) {
    int index = Env(p)->doc->IndexOfPage(page);
    if (index >= 0) h->OnFormInvalidate(index, l, t, r, b);
  }
}

void FFI_SetCursor(FPDF_FORMFILLINFO* p, int type) {
  if (auto* h = Host(p)) h->OnFormCursor(type);
}

int FFI_SetTimer(FPDF_FORMFILLINFO* p, int elapse, TimerCallback fn) {
  UINT_PTR id = SetTimer(nullptr, 0, static_cast<UINT>(std::max(elapse, 10)), TimerProc);
  if (!id) return 0;
  Timers()[id] = fn;
  Env(p)->timers.push_back(static_cast<int>(id));
  return static_cast<int>(id);
}

void FFI_KillTimer(FPDF_FORMFILLINFO* p, int id) {
  KillTimer(nullptr, static_cast<UINT_PTR>(id));
  Timers().erase(static_cast<UINT_PTR>(id));
  auto& t = Env(p)->timers;
  t.erase(std::remove(t.begin(), t.end(), id), t.end());
}

FPDF_SYSTEMTIME FFI_GetLocalTime(FPDF_FORMFILLINFO*) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  FPDF_SYSTEMTIME r{};
  r.wYear = st.wYear;
  r.wMonth = st.wMonth;
  r.wDayOfWeek = st.wDayOfWeek;
  r.wDay = st.wDay;
  r.wHour = st.wHour;
  r.wMinute = st.wMinute;
  r.wSecond = st.wSecond;
  r.wMilliseconds = st.wMilliseconds;
  return r;
}

void FFI_OnChange(FPDF_FORMFILLINFO* p) {
  Env(p)->doc->SetModified(true);
  if (auto* h = Host(p)) h->OnFormChanged();
}

FPDF_PAGE FFI_GetPage(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT, int index) {
  Document* d = Env(p)->doc;
  if (index < 0 || index >= d->PageCount()) return nullptr;
  return d->Page(index);
}

FPDF_PAGE FFI_GetCurrentPage(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT) {
  Document* d = Env(p)->doc;
  int index = d->CurrentPageForForm();
  if (index < 0 || index >= d->PageCount()) return nullptr;
  return d->Page(index);
}

int FFI_GetRotation(FPDF_FORMFILLINFO*, FPDF_PAGE) { return 0; }

void FFI_ExecuteNamedAction(FPDF_FORMFILLINFO* p, FPDF_BYTESTRING name) {
  if (auto* h = Host(p)) h->OnFormNamedAction(name ? name : "");
}

void FFI_SetTextFieldFocus(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_DWORD, FPDF_BOOL) {}

void FFI_DoURIAction(FPDF_FORMFILLINFO* p, FPDF_BYTESTRING uri) {
  if (auto* h = Host(p)) h->OnFormOpenUri(fw::Utf8ToWide(uri ? uri : ""));
}

void FFI_DoGoToAction(FPDF_FORMFILLINFO* p, int page, int zoomMode, float* pos, int count) {
  auto* h = Host(p);
  if (!h) return;
  // zoomMode 1 (XYZ) carries [x, y, zoom].
  bool hasPos = zoomMode == 1 && pos && count >= 2;
  h->OnFormGoToPage(page, hasPos, hasPos ? pos[0] : 0, hasPos ? pos[1] : 0);
}

void FFI_DisplayCaret(FPDF_FORMFILLINFO* p, FPDF_PAGE page, FPDF_BOOL visible, double l, double t, double r,
                      double b) {
  if (auto* h = Host(p)) h->OnFormCaret(Env(p)->doc->IndexOfPage(page), visible != 0, l, t, r, b);
}

int FFI_GetCurrentPageIndex(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT) { return Env(p)->doc->CurrentPageForForm(); }

void FFI_SetCurrentPage(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT, int page) {
  if (auto* h = Host(p)) h->OnFormGoToPage(page, false, 0, 0);
}

void FFI_GotoURL(FPDF_FORMFILLINFO* p, FPDF_DOCUMENT, FPDF_WIDESTRING url) {
  if (auto* h = Host(p)) h->OnFormOpenUri(Wide(url));
}

void FFI_GetPageViewRect(FPDF_FORMFILLINFO*, FPDF_PAGE page, double* l, double* t, double* r, double* b) {
  // Report the whole page as visible; PDFium uses this to place XFA popups.
  *l = 0;
  *b = 0;
  *r = FPDF_GetPageWidthF(page);
  *t = FPDF_GetPageHeightF(page);
}

void FFI_PageEvent(FPDF_FORMFILLINFO* p, int, FPDF_DWORD) {
  if (auto* h = Host(p)) h->OnFormPageCountChanged();
}

FPDF_BOOL FFI_PopupMenu(FPDF_FORMFILLINFO*, FPDF_PAGE, FPDF_WIDGET, int, float, float) { return FALSE; }

int FFI_GetPlatform(FPDF_FORMFILLINFO*, void* buffer, int length) { return WriteWide(L"WIN", buffer, length); }

int FFI_GetLanguage(FPDF_FORMFILLINFO*, void* buffer, int length) {
  wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"en-US";
  GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
  return WriteWide(name, buffer, length);
}

FPDF_BOOL FFI_PostRequestURL(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING,
                             FPDF_WIDESTRING, FPDF_BSTR*) {
  return FALSE;  // forms never talk to the network
}

FPDF_BOOL FFI_PutRequestURL(FPDF_FORMFILLINFO*, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING) { return FALSE; }

void FFI_OnFocusChange(FPDF_FORMFILLINFO* p, FPDF_ANNOTATION annot, int page) {
  auto* h = Host(p);
  if (!h) return;
  FS_RECTF rect{};
  bool hasRect = annot && FPDFAnnot_GetRect(annot, &rect);
  h->OnFormFocusChanged(page, hasRect, rect);
}

void FFI_DoURIActionWithKeyboardModifier(FPDF_FORMFILLINFO* p, FPDF_BYTESTRING uri, int) { FFI_DoURIAction(p, uri); }

// ---- IPDF_JSPLATFORM callbacks (form JavaScript) ----

DocumentHost* JsHost(IPDF_JSPLATFORM* p) { return static_cast<JsPlatform*>(p)->doc->HostPtr(); }

int JS_Alert(IPDF_JSPLATFORM* p, FPDF_WIDESTRING msg, FPDF_WIDESTRING title, int type, int iconType) {
  auto* h = JsHost(p);
  return h ? h->OnFormAlert(Wide(msg), Wide(title), type, iconType) : 1;
}

void JS_Beep(IPDF_JSPLATFORM*, int) { MessageBeep(MB_OK); }

int JS_Response(IPDF_JSPLATFORM*, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_BOOL, void*,
                int) {
  return 0;
}

int JS_GetFilePath(IPDF_JSPLATFORM* p, void* buffer, int length) {
  std::string path = fw::WideToUtf8(static_cast<JsPlatform*>(p)->doc->Path());
  int bytes = static_cast<int>(path.size() + 1);
  if (buffer && length >= bytes) memcpy(buffer, path.c_str(), bytes);
  return bytes;
}

void JS_Mail(IPDF_JSPLATFORM*, void*, int, FPDF_BOOL, FPDF_WIDESTRING, FPDF_WIDESTRING, FPDF_WIDESTRING,
             FPDF_WIDESTRING, FPDF_WIDESTRING) {}

void JS_Print(IPDF_JSPLATFORM* p, FPDF_BOOL, int, int, FPDF_BOOL, FPDF_BOOL, FPDF_BOOL, FPDF_BOOL, FPDF_BOOL) {
  if (auto* h = JsHost(p)) h->OnFormPrint();
}

void JS_SubmitForm(IPDF_JSPLATFORM*, void*, int, FPDF_WIDESTRING) {}

void JS_GotoPage(IPDF_JSPLATFORM* p, int page) {
  if (auto* h = JsHost(p)) h->OnFormGoToPage(page, false, 0, 0);
}

int JS_Browse(IPDF_JSPLATFORM*, void*, int) { return 0; }

}  // namespace

FormEnv::FormEnv(Document* d) : FPDF_FORMFILLINFO{}, doc(d) {
  version = 2;  // required when PDFium is built with XFA; also enables the newer callbacks
  FPDF_FORMFILLINFO::FFI_Invalidate = ::FFI_Invalidate;
  FPDF_FORMFILLINFO::FFI_SetCursor = ::FFI_SetCursor;
  FPDF_FORMFILLINFO::FFI_SetTimer = ::FFI_SetTimer;
  FPDF_FORMFILLINFO::FFI_KillTimer = ::FFI_KillTimer;
  FPDF_FORMFILLINFO::FFI_GetLocalTime = ::FFI_GetLocalTime;
  FPDF_FORMFILLINFO::FFI_OnChange = ::FFI_OnChange;
  FPDF_FORMFILLINFO::FFI_GetPage = ::FFI_GetPage;
  FPDF_FORMFILLINFO::FFI_GetCurrentPage = ::FFI_GetCurrentPage;
  FPDF_FORMFILLINFO::FFI_GetRotation = ::FFI_GetRotation;
  FPDF_FORMFILLINFO::FFI_ExecuteNamedAction = ::FFI_ExecuteNamedAction;
  FPDF_FORMFILLINFO::FFI_SetTextFieldFocus = ::FFI_SetTextFieldFocus;
  FPDF_FORMFILLINFO::FFI_DoURIAction = ::FFI_DoURIAction;
  FPDF_FORMFILLINFO::FFI_DoGoToAction = ::FFI_DoGoToAction;
  FPDF_FORMFILLINFO::FFI_DisplayCaret = ::FFI_DisplayCaret;
  FPDF_FORMFILLINFO::FFI_GetCurrentPageIndex = ::FFI_GetCurrentPageIndex;
  FPDF_FORMFILLINFO::FFI_SetCurrentPage = ::FFI_SetCurrentPage;
  FPDF_FORMFILLINFO::FFI_GotoURL = ::FFI_GotoURL;
  FPDF_FORMFILLINFO::FFI_GetPageViewRect = ::FFI_GetPageViewRect;
  FPDF_FORMFILLINFO::FFI_PageEvent = ::FFI_PageEvent;
  FPDF_FORMFILLINFO::FFI_PopupMenu = ::FFI_PopupMenu;
  FPDF_FORMFILLINFO::FFI_GetPlatform = ::FFI_GetPlatform;
  FPDF_FORMFILLINFO::FFI_GetLanguage = ::FFI_GetLanguage;
  FPDF_FORMFILLINFO::FFI_PostRequestURL = ::FFI_PostRequestURL;
  FPDF_FORMFILLINFO::FFI_PutRequestURL = ::FFI_PutRequestURL;
  FPDF_FORMFILLINFO::FFI_OnFocusChange = ::FFI_OnFocusChange;
  FPDF_FORMFILLINFO::FFI_DoURIActionWithKeyboardModifier = ::FFI_DoURIActionWithKeyboardModifier;

  js.version = 3;
  js.doc = d;
  js.app_alert = JS_Alert;
  js.app_beep = JS_Beep;
  js.app_response = JS_Response;
  js.Doc_getFilePath = JS_GetFilePath;
  js.Doc_mail = JS_Mail;
  js.Doc_print = JS_Print;
  js.Doc_submitForm = JS_SubmitForm;
  js.Doc_gotoPage = JS_GotoPage;
  js.Field_browse = JS_Browse;
  js.m_pFormfillinfo = static_cast<FPDF_FORMFILLINFO*>(this);
  m_pJsPlatform = &js;
  xfa_disabled = FALSE;
}

FormEnv::~FormEnv() {
  for (int id : timers) {
    KillTimer(nullptr, static_cast<UINT_PTR>(id));
    Timers().erase(static_cast<UINT_PTR>(id));
  }
}
