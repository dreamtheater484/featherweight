#include "text/SpellCheck.h"

#include <windows.h>

#include <objbase.h>
#include <spellcheck.h>
#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <optional>

using Microsoft::WRL::ComPtr;

namespace fw::text {

namespace {

std::wstring Lower(std::wstring_view s) {
  std::wstring out(s);
  for (wchar_t& c : out) c = static_cast<wchar_t>(towlower(c));
  return out;
}

std::wstring_view Primary(std::wstring_view tag) { return tag.substr(0, tag.find(L'-')); }

std::vector<std::wstring> Strings(IEnumString* e, size_t max = SIZE_MAX) {
  std::vector<std::wstring> out;
  if (!e) return out;
  LPOLESTR s = nullptr;
  while (out.size() < max && e->Next(1, &s, nullptr) == S_OK) {
    out.emplace_back(s);
    CoTaskMemFree(s);
  }
  return out;
}

}  // namespace

struct SpellChecker::Impl {
  bool tried = false;
  ComPtr<ISpellCheckerFactory> factory;
  std::vector<std::wstring> languages;
  std::map<std::wstring, std::wstring> resolved;              // lower(tag) -> installed tag, or ""
  std::map<std::wstring, ComPtr<ISpellChecker>> checkers;     // installed tag -> checker

  ISpellCheckerFactory* Factory() {
    if (!tried) {
      tried = true;
      if (FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
        factory.Reset();
      if (factory) {
        ComPtr<IEnumString> e;
        if (SUCCEEDED(factory->get_SupportedLanguages(&e))) languages = Strings(e.Get());
      }
    }
    return factory.Get();
  }

  ISpellChecker* Checker(std::wstring_view lang) {
    const std::wstring tag = Resolve(lang);
    if (tag.empty()) return nullptr;
    auto it = checkers.find(tag);
    if (it == checkers.end()) {
      ComPtr<ISpellChecker> c;
      if (FAILED(factory->CreateSpellChecker(tag.c_str(), &c))) c.Reset();
      it = checkers.emplace(tag, c).first;
    }
    return it->second.Get();
  }

  std::wstring Resolve(std::wstring_view lang) {
    if (lang.empty() || !Factory()) return {};
    const std::wstring key = Lower(lang);
    if (auto it = resolved.find(key); it != resolved.end()) return it->second;
    std::wstring found;
    for (const std::wstring& l : languages)
      if (Lower(l) == key) found = l;
    if (found.empty()) {
      const std::wstring primary = Lower(Primary(lang));
      for (const std::wstring& l : languages)
        if (Lower(Primary(l)) == primary) {
          found = l;
          break;
        }
    }
    resolved.emplace(key, found);
    return found;
  }
};

SpellChecker::SpellChecker() : impl_(std::make_unique<Impl>()) {}
SpellChecker::~SpellChecker() = default;

bool SpellChecker::Available() { return impl_->Factory() != nullptr; }

std::wstring SpellChecker::Resolve(std::wstring_view lang) { return impl_->Resolve(lang); }

std::vector<std::wstring> SpellChecker::Languages() {
  impl_->Factory();
  return impl_->languages;
}

std::vector<SpellChecker::Error> SpellChecker::Check(std::wstring_view lang, std::wstring_view text) {
  std::vector<Error> out;
  ISpellChecker* checker = impl_->Checker(lang);
  if (!checker || text.empty()) return out;
  const std::wstring copy(text);  // the API reads up to a NUL
  ComPtr<IEnumSpellingError> errors;
  if (FAILED(checker->Check(copy.c_str(), &errors)) || !errors) return out;
  ComPtr<ISpellingError> e;
  while (errors->Next(&e) == S_OK && e) {
    ULONG start = 0, length = 0;
    CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
    e->get_StartIndex(&start);
    e->get_Length(&length);
    e->get_CorrectiveAction(&action);
    if (action != CORRECTIVE_ACTION_NONE && length > 0 && start + length <= copy.size()) {
      Error err;
      err.start = start;
      err.length = length;
      err.action = action == CORRECTIVE_ACTION_REPLACE  ? Action::Replace
                   : action == CORRECTIVE_ACTION_DELETE ? Action::Delete
                                                        : Action::Suggest;
      if (err.action == Action::Replace) {
        LPWSTR r = nullptr;
        if (SUCCEEDED(e->get_Replacement(&r)) && r) {
          err.replacement = r;
          CoTaskMemFree(r);
        }
      }
      out.push_back(std::move(err));
    }
    e.Reset();
  }
  return out;
}

std::vector<std::wstring> SpellChecker::Suggest(std::wstring_view lang, std::wstring_view word, size_t max) {
  ISpellChecker* checker = impl_->Checker(lang);
  if (!checker || word.empty()) return {};
  ComPtr<IEnumString> e;
  if (checker->Suggest(std::wstring(word).c_str(), &e) != S_OK) return {};  // S_FALSE: spelled right
  return Strings(e.Get(), max);
}

void SpellChecker::Ignore(std::wstring_view lang, std::wstring_view word) {
  if (ISpellChecker* checker = impl_->Checker(lang); checker && !word.empty())
    checker->Ignore(std::wstring(word).c_str());
}

bool SpellChecker::Add(std::wstring_view lang, std::wstring_view word) {
  ISpellChecker* checker = impl_->Checker(lang);
  return checker && !word.empty() && SUCCEEDED(checker->Add(std::wstring(word).c_str()));
}

}  // namespace fw::text
