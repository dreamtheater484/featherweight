#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "core/FileKinds.h"

// Registers Featherweight for the current user only (no admin rights needed): one ProgID per kind
// of file, each with its own file icon, for every kind whose viewer has shipped.
namespace fileassoc {

// A string value under HKEY_CURRENT_USER. An empty `name` is the key's default value.
struct Value {
  std::wstring key, name, data;
  bool operator==(const Value&) const = default;
};
// What unregistering removes: the whole key, or one value of it.
struct Removal {
  std::wstring key, value;
  bool tree = false;
  bool operator==(const Removal&) const = default;
};

// What Register writes for `kinds`, in order, without touching the registry.
std::vector<Value> Plan(const std::wstring& exePath, const std::vector<fw::FileKind>& kinds);
// What Unregister removes, in order: every kind in the table, shipped or not.
std::vector<Removal> RemovalPlan();

bool Register(const std::wstring& exePath);
void Unregister();
// True if registered, and every shipped kind's registration points at `exePath`.
bool IsRegisteredAt(const std::wstring& exePath);
bool IsRegistered();
// True if registered at `exePath`, but a kind that shipped since isn't registered yet.
bool MissesShippedKinds(const std::wstring& exePath);
// The exe that the current registration points at (empty if not registered).
std::wstring RegisteredExe();
// True if Windows currently opens the kind's main extension (.pdf, .docx…) with Featherweight.
bool IsDefault(fw::FileKind kind = fw::FileKind::Pdf);
// Opens Windows Settings on the page where the user confirms Featherweight as the default.
void OpenDefaultAppsSettings();

}  // namespace fileassoc
