#pragma once

// The interface's languages (F1, f1-lang; docs/f1-plan.md "Languages" and its decisions).
//
// Every string a person reads goes through T(): T(L"Save") is the chosen language's text for "Save",
// or "Save" itself in English, where no catalog is loaded at all. The English text is the key
// (gettext's model), so strings need no ids. A context separates words that translate two ways:
// T(L"Open", L"verb"). A table built before main (constexpr, or at namespace scope) marks its strings
// with N_(), which only tells the catalog about them, and the code that shows one calls T() on it.
//
// Catalogs are gettext PO files, strings/<code>.po, with strings/featherweight.pot as their template
// (tools/strings). The build compiles them into one compressed resource per language, and only the
// chosen language's is read, once, at start. Test builds also have the pseudo-language "qps":
// accented, bracketed and 40 % longer, to find unmarked strings and clipped labels before any
// translation exists.
//
// T() is cheap (one test in English; a hash and a binary search otherwise) and safe from any thread
// once Init has run. Its result lives as long as the process.

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Never inlined: called from some 5,500 places, each call is a few bytes (an inlined copy would be
// a hundred).
__declspec(noinline) const wchar_t* T(const wchar_t* en);
__declspec(noinline) const wchar_t* T(const wchar_t* en, const wchar_t* context);
__declspec(noinline) const char16_t* T(const char16_t* en);
__declspec(noinline) const char* T(const char* en);  // UTF-8 in, UTF-8 out

// Marks a string for the catalog where T() can't run yet (constexpr tables, namespace scope).
#define N_(s) s

// A sentence with numbers or names in it, whole, so each language orders its words: {0}, {1} ...
// replaced by `args`. Fmt(T(L"Page {0} of {1}"), {std::to_wstring(page), std::to_wstring(count)}).
// A sentence is never glued from pieces: "Exported " + n + " pages" can't be put in German order.
std::wstring Fmt(const wchar_t* pattern, std::initializer_list<std::wstring_view> args);
std::string Fmt(const char* pattern, std::initializer_list<std::string_view> args);  // UTF-8
std::u16string Fmt(const char16_t* pattern, std::initializer_list<std::u16string_view> args);

// A table's string, translated each time it's read: a table built before main keeps its N_()
// strings in this type (UiText label; constexpr UiText kNames[] = {N_(L"Left"), ...}), and every
// reader gets T() without asking for it. `en` is the English, for the rare reader that wants it.
template <class C>
struct UiStr {
  const C* en;
  constexpr UiStr(const C* s) : en(s) {}  // implicit, so tables keep their {L"..."} form
  operator const C*() const { return T(en); }
  operator std::basic_string_view<C>() const { return T(en); }
};
template <class C>
std::basic_string<C> operator+(std::basic_string<C> a, const UiStr<C>& b) {
  return a += static_cast<const C*>(b);
}
template <class C>
std::basic_string<C> operator+(const UiStr<C>& a, const std::basic_string<C>& b) {
  return static_cast<const C*>(a) + b;
}
using UiText = UiStr<wchar_t>;
using UiText8 = UiStr<char>;
using UiText16 = UiStr<char16_t>;

namespace fw::lang {

struct Language {
  const wchar_t* code;  // "en", "nl", "de", "fr", "es" (and "qps" in test builds)
  const wchar_t* name;  // in its own language: "Nederlands"
};

// The languages the interface comes in, English first.
const std::vector<Language>& Languages();

// The language Windows shows its own interface in, if Featherweight has it ("de"); else "en".
std::wstring WindowsLanguage();

// Chooses the interface's language once, at start, before any window or table shows text: `chosen`
// (File > Options, or a policy) when it's one of Languages(), else Windows' display language. Loads
// its catalog; English loads nothing. A catalog that's missing or damaged leaves English.
void Init(const std::wstring& chosen);

// The language in use ("en" until Init chose another).
const wchar_t* Current();

}  // namespace fw::lang
