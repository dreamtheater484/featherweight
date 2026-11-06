# fwtool: the headless command line for the never-damage tests and the fidelity measurement
# (plan §10). Not shipped.
# fwfidelity compares layout dumps with Word's lines; the unit tests use it too.
add_library(fwfidelity STATIC Fidelity.cpp)
target_include_directories(fwfidelity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fwfidelity PUBLIC fwcore)
target_compile_options(fwfidelity PRIVATE ${FW_COMPILE_OPTIONS})

add_executable(fwtool main.cpp Roundtrip.cpp Save.cpp Inspect.cpp Model.cpp Print.cpp Render.cpp Reference.cpp Compare.cpp Bench.cpp
  BenchApp.cpp Crypt.cpp Export.cpp LayoutHook.cpp Paste.cpp PdfCheck.cpp Spelling.cpp Torture.cpp Wrap.cpp)
target_link_libraries(fwtool PRIVATE fwpkg fwdoc fwdoclayout fwdraw fwproof fwtext fwfidelity pdfium delayimp ole32 windowscodecs)
# Picture editing (D3), for fwtool torture.
if(TARGET fwpictures)
  target_link_libraries(fwtool PRIVATE fwpictures)
endif()
target_compile_options(fwtool PRIVATE ${FW_COMPILE_OPTIONS})
# PDFium reads Word's reference PDFs. As in the app, it's delay-loaded: only `lines` and `compare`
# load it, and only the copy next to fwtool.exe.
target_link_options(fwtool PRIVATE /DELAYLOAD:pdfium.dll)
add_custom_command(TARGET fwtool POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FW_PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:fwtool>"
  COMMENT "Copying pdfium.dll for fwtool")

if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_fidelity.cpp")
  target_link_libraries(fw_tests PRIVATE fwfidelity)
  # The fidelity measurement checks itself on Word's reference renders (tests/office/README.md):
  # Word's lines against themselves and against known changes, then through dump files and the
  # thresholds, which must hold at 100 %.
  set(_fw_reference "${PROJECT_SOURCE_DIR}/tests/office/reference")
  set(_fw_fidelity "${CMAKE_BINARY_DIR}/fidelity")
  add_test(NAME fidelity_selftest COMMAND fwtool compare --self-test "${_fw_reference}")
  add_test(NAME fidelity_word_lines COMMAND fwtool lines "${_fw_reference}" --out "${_fw_fidelity}/word")
  add_test(NAME fidelity_word_vs_word COMMAND fwtool compare "${_fw_reference}" --dumps "${_fw_fidelity}/word"
    --min-pages 100 --min-lines 100 --json "${_fw_fidelity}/word.json" --report "${_fw_fidelity}/word.md")
  set_tests_properties(fidelity_word_lines PROPERTIES FIXTURES_SETUP fidelity_word)
  set_tests_properties(fidelity_word_vs_word PROPERTIES FIXTURES_REQUIRED fidelity_word)
  # Export PDF (D4) on the public corpus: each document's PDF strictly well formed, read back by
  # PDFium with the text where it's drawn, and its pages as the screen draws them.
  add_test(NAME pdf_check COMMAND fwtool pdf-check "${PROJECT_SOURCE_DIR}/tests/office/generated"
    "${PROJECT_SOURCE_DIR}/tests/office/edge" "${PROJECT_SOURCE_DIR}/tests/office/layout" "${PROJECT_SOURCE_DIR}/tests/office/torture")
endif()
