// Fuzz target: number formats and value parsing (src/sheet/numfmt, S1). The input's first line is a
// format code (UTF-8); every 8 bytes after it are also read as a double, and each later line as a
// text value and as typed text. Seeds: tests/fuzz/seeds/numfmt.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "core/Strings.h"
#include "sheet/numfmt/NumFormat.h"
#include "sheet/numfmt/Parse.h"

using namespace fw::sheet;

namespace {

void Check(const Formatted& f) {
  if (f.overflow) {
    if (!f.text.empty() || f.fill != 0 || f.fillChar != L'#' || !f.pads.empty()) std::abort();
    return;
  }
  if (f.fill < -1 || f.fill > static_cast<int>(f.text.size())) std::abort();
  if (static_cast<size_t>(f.generalAt) + f.generalLength > f.text.size()) std::abort();
  uint32_t last = 0;
  for (size_t k = 0; k < f.pads.size(); ++k) {
    const auto& p = f.pads[k];
    if (p.at >= f.text.size() || f.text[p.at] != L' ' || (k && p.at <= last)) std::abort();
    last = p.at;
  }
  if (f.color > 56 || f.text.size() > 200000) std::abort();
}

struct Limit {
  size_t chars;
};
bool Fits(void* context, const Formatted& f) { return f.text.size() <= static_cast<Limit*>(context)->chars; }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view in(reinterpret_cast<const char*>(data), size);
  const size_t nl = in.find('\n');
  const std::wstring code = fw::Utf8ToWide(in.substr(0, nl));
  const std::string_view rest = nl == std::string_view::npos ? std::string_view() : in.substr(nl + 1);
  const NumberFormat f(code);
  TypedValue typed;  // the line as typed into a cell
  ParseTyped(code, EnNlLocale(), false, 2026, typed);
  ParseTyped(code, EnUsLocale(), true, 2026, typed);
  const NumLocale* locales[] = {&EnUsLocale(), &EnNlLocale()};
  Formatted out;
  for (const NumLocale* loc : locales) {
    for (int width = 0; width < 2; ++width) {
      Limit limit{size % 13};
      FormatContext ctx;
      ctx.locale = loc;
      ctx.date1904 = (size & 1) != 0;
      if (width) ctx.fits = Fits, ctx.fitsContext = &limit;
      for (double v : {0.0, -0.0, 1.0, -1.0, 0.5, -1234.5678, 45000.75, 1e15, 1e-15, 1e300, -1e300, 2958465.999999})
        f.Format(v, ctx, out), Check(out);
      for (size_t k = 0; k + 8 <= rest.size() && k < 128; k += 8) {
        double v;
        std::memcpy(&v, rest.data() + k, 8);
        f.Format(v, ctx, out);
        Check(out);
      }
    }
  }
  // The code as en-NL types it (TEXT()): read or refused, and the same reading twice.
  {
    std::wstring inv, again;
    NumberFormat local;
    const bool read = NumberFormat::LocalToInvariant(code, EnNlLocale(), inv);
    if (read != NumberFormat::FromLocal(code, EnNlLocale(), local)) std::abort();
    if (!read && !inv.empty()) std::abort();
    if (NumberFormat::LocalToInvariant(code, EnNlLocale(), again) != read || again != inv) std::abort();
    if (!NumberFormat::LocalToInvariant(code, EnUsLocale(), again) || again != code) std::abort();
    FormatContext ctx;
    ctx.locale = &EnNlLocale();
    for (double v : {0.0, -1.0, 1234.5678, 45000.75, 1e300}) local.Format(v, ctx, out), Check(out);
  }
  // Each later line: a text value, and typed text.
  size_t start = 0;
  for (int lines = 0; start <= rest.size() && lines < 16; ++lines) {
    size_t end = rest.find('\n', start);
    if (end == std::string_view::npos) end = rest.size();
    const std::wstring line = fw::Utf8ToWide(rest.substr(start, end - start));
    FormatContext ctx;
    f.Format(line, ctx, out);
    Check(out);
    for (const NumLocale* loc : locales) {
      ParsedValue p;
      if (ParseValue(line, *loc, false, p) && !std::isfinite(p.number)) std::abort();
      if (ParseExact(line, *loc, false, p)) {
        if (p.kind == ParsedValue::Bool) {
          if (line != L"TRUE" && line != L"FALSE") std::abort();
          continue;
        }
        // It shows exactly as written, in its own format.
        FormatContext c;
        c.locale = loc;
        (p.code.empty() ? NumberFormat() : NumberFormat(p.code)).Format(p.number, c, out);
        if (out.text != line) std::abort();
      }
    }
    start = end + 1;
  }
  return 0;
}
