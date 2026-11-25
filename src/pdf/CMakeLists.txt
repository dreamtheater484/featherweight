# The PDF tab's tools (F1, f1-pdf; docs/f1-plan.md): page tools, OCR's text layer and signing,
# headless (no window), so the tab, fwtool and the unit tests share them. The tab itself
# (PdfView and the panels) is in the app's sources. PDFium stays delay-loaded wherever it's used.
add_library(fwpdftools STATIC tools/PageOps.cpp
  ocr/Ocr.cpp ocr/TextLayer.cpp)  # Windows.Media.Ocr through the Windows Runtime's ABI: loaded when used
target_include_directories(fwpdftools PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwpdftools PUBLIC pdfium fwpkg fwcore runtimeobject)
# They run once per click: compiled for size.
target_compile_options(fwpdftools PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy /O1>)

if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_f1_pdf.cpp")
  target_link_libraries(fw_tests PRIVATE fwpdftools delayimp)
  target_link_options(fw_tests PRIVATE /DELAYLOAD:pdfium.dll)
  # The tests load the build's pdfium.dll from where FetchPdfium put it (a POST_BUILD copy can only
  # be set up where fw_tests is made).
  target_compile_definitions(fw_tests PRIVATE "FW_PDFIUM_DLL=\"${FW_PDFIUM_DIR}/bin/pdfium.dll\"")
endif()
