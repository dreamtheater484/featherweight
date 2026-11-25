# The PDF tab's tools (F1, f1-pdf; docs/f1-plan.md): page tools, OCR's text layer and signing,
# headless (no window), so the tab, fwtool and the unit tests share them. The tab itself
# (PdfView and the panels) is in the app's sources. PDFium stays delay-loaded wherever it's used.
add_library(fwpdftools STATIC tools/PageOps.cpp tools/Editor.cpp  # Editor: the tools without a window (fwtool, f1-gate)
  ocr/Ocr.cpp ocr/TextLayer.cpp  # Windows.Media.Ocr through the Windows Runtime's ABI: loaded when used
  sign/PdfObjects.cpp sign/Sign.cpp sign/Stamp.cpp)  # certificate signatures: CMS through crypt32 (delay-loaded) and CNG
target_include_directories(fwpdftools PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwpdftools PUBLIC pdfium fwpkg fwcore runtimeobject crypt32 bcrypt PRIVATE miniz)
# They run once per click: compiled for size.
target_compile_options(fwpdftools PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy /O1>)

if(FW_BUILD_TESTS)
  # ShareSheet.cpp, the share sheet's Windows Runtime part, has no app code: its test is here too.
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_f1_pdf.cpp" "${PROJECT_SOURCE_DIR}/src/ShareSheet.cpp")
  target_link_libraries(fw_tests PRIVATE fwpdftools delayimp)
  target_link_options(fw_tests PRIVATE /DELAYLOAD:pdfium.dll)
  # The tests load the build's pdfium.dll from where FetchPdfium put it (a POST_BUILD copy can only
  # be set up where fw_tests is made).
  target_compile_definitions(fw_tests PRIVATE "FW_PDFIUM_DLL=\"${FW_PDFIUM_DIR}/bin/pdfium.dll\""
    "FW_F1PDF_TESTDATA=\"${PROJECT_SOURCE_DIR}/tests/pdfs\"")
  # The tools in the app, end to end (tests/screens/pdftools.cmake): the files they write.
  add_test(NAME pdf_tools COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:Featherweight>"
           "-DPDFS=${PROJECT_SOURCE_DIR}/tests/pdfs" "-DOUT=${CMAKE_BINARY_DIR}/screens/pdftools"
           -P "${PROJECT_SOURCE_DIR}/tests/screens/pdftools.cmake")
endif()
