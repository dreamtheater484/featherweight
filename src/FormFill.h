#pragma once

#include <fpdf_formfill.h>

#include <vector>

class Document;

struct JsPlatform : IPDF_JSPLATFORM {
  Document* doc = nullptr;
};

// PDFium's form-fill environment for one document. PDFium calls back into it for repaints,
// timers (caret blinking), actions and JavaScript UI; it forwards everything to the DocumentHost.
struct FormEnv : FPDF_FORMFILLINFO {
  explicit FormEnv(Document* d);
  ~FormEnv();

  Document* doc;
  JsPlatform js;
  std::vector<int> timers;
};
