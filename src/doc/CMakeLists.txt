# fwdoc: the document model and its .docx reader and writer (plan §4.8, §6.3). Headless: no UI,
# no window, no PDFium. Layout and the view come in later phases.
add_library(fwdoc STATIC
  model/Document.cpp
  model/Numbering.cpp
  model/Resolve.cpp
  model/Schema.cpp
  model/Tree.cpp
  docx/Blank.cpp
  docx/Reader.cpp
  docx/Save.cpp
  docx/Writer.cpp
  edit/Clip.cpp
  edit/Editor.cpp
  edit/Format.cpp
  edit/Kept.cpp
  edit/Lists.cpp
  edit/Styles.cpp
  edit/TreeEdit.cpp
)
target_include_directories(fwdoc PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwdoc PUBLIC fwpkg fwcore)
target_compile_options(fwdoc PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

# fwdoclayout: pagination (plan §6.3 "Layout"), on fwtext's lines. Kept apart from fwdoc so the
# model, its tools and its fuzz target stay free of DirectWrite.
add_library(fwdoclayout STATIC
  layout/Flow.cpp
  layout/Layout.cpp
  layout/Stack.cpp
  layout/Table.cpp
  layout/Text.cpp
)
target_link_libraries(fwdoclayout PUBLIC fwdoc fwtext fwdraw)
target_compile_options(fwdoclayout PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

# fwproof: proofing (plan §6.2, milestone D2): spelling with Windows' spell checker, the language of
# text, AutoCorrect and word count, on the model and its editor. Headless.
add_library(fwproof STATIC
  proof/AutoCorrect.cpp
  proof/Language.cpp
  proof/Spelling.cpp
  proof/WordCount.cpp
)
target_link_libraries(fwproof PUBLIC fwdoc fwtext)
target_compile_options(fwproof PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

# Its unit tests join fw_tests, and the never-damage tests run the corpus through the model
# (plan §6.4 D1): read every document into the model, write every part it owns back, and check.
if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_doc.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_docsave.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/test_edit.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_format.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/test_layout.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_proof.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_tables.cpp"
    # The document tab's file (D2 "Files"): an app source, tested here with temporary folders.
    "${PROJECT_SOURCE_DIR}/tests/unit/test_docfile.cpp" "${PROJECT_SOURCE_DIR}/src/word/DocFile.cpp")
  target_link_libraries(fw_tests PRIVATE fwdoc fwdoclayout fwproof)
  set(_fw_office "${PROJECT_SOURCE_DIR}/tests/office")
  add_test(NAME office_model_roundtrip COMMAND fwtool roundtrip --model "${_fw_office}/generated" "${_fw_office}/edge"
    "${_fw_office}/layout")
  add_test(NAME office_model_reserialize
    COMMAND fwtool roundtrip --model --reserialize-all "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  # Saving as the app does (D2): with no edits the file is identical; after edits in every part the
  # model owns, the saved file opens and has no new fwtool check errors.
  add_test(NAME office_save COMMAND fwtool save "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  add_test(NAME office_save_edited
    COMMAND fwtool save --edited "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  add_test(NAME office_model_malicious COMMAND fwtool roundtrip --model --expect-reject "${_fw_office}/malicious")
endif()
