#pragma once

// Office's cloud fonts (D5): the families Word downloads when a document asks for them (Aptos,
// Open Sans, Yu Mincho, ...) into %LOCALAPPDATA%\Microsoft\FontCache\4\CloudFonts\<family>\. DirectWrite's
// system collection doesn't list them, so without these Featherweight would substitute fonts that
// Word draws as they are. Only the families the PC doesn't have otherwise. The folder is read once,
// the first time a family is missing, so documents whose fonts are all installed don't pay for it.
// fwtext's layout and fwdraw's outlined text both use them; the PDF writer embeds them as any other.
#include <dwrite_3.h>

#include <string_view>

namespace draw {

// The cloud fonts as a font set and as a collection; null when there are none. Thread-safe.
IDWriteFontSet* CloudFontSet();
IDWriteFontCollection1* CloudFontCollection();
// Whether the cloud fonts have the family `name`, in any of its languages ("MS Mincho" or its
// Japanese name). Only the families Word downloaded (a folder each), not other names their fonts
// have ("Open Sans Light"), which Word substitutes.
bool CloudFontsHave(std::wstring_view name);

}  // namespace draw
