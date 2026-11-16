# The .xlsx reader, and the writer the round-trip tests use (saving comes in S2).
target_sources(fwsheet PRIVATE Reader.cpp SheetReader.cpp Writer.cpp)
# Never-damage tests through the workbook model (plan §10): every workbook in the public corpus is
# read into the model, which writes back the parts it owns; they must equal the originals after
# canonicalization, and the saved copy must pass check. Every folder of tests/office but the
# malicious, protected and reference ones (broken files are read, but fail check on purpose).
if(FW_BUILD_TESTS)
  set(_fw_office "${PROJECT_SOURCE_DIR}/tests/office")
  file(GLOB _fw_office_dirs LIST_DIRECTORIES true "${_fw_office}/*")
  set(_fw_workbook_dirs)
  set(_fw_workbook_check_dirs)
  foreach(dir ${_fw_office_dirs})
    get_filename_component(name "${dir}" NAME)
    if(IS_DIRECTORY "${dir}" AND NOT name MATCHES "^(malicious|protected|reference)$")
      list(APPEND _fw_workbook_dirs "${dir}")
      if(NOT name STREQUAL "broken")
        list(APPEND _fw_workbook_check_dirs "${dir}")
      endif()
    endif()
  endforeach()
  add_test(NAME office_workbook_roundtrip COMMAND fwtool roundtrip --model --workbooks ${_fw_workbook_dirs})
  add_test(NAME office_workbook_reserialize COMMAND fwtool roundtrip --model --reserialize-all --workbooks ${_fw_workbook_dirs})
  add_test(NAME office_workbook_check COMMAND fwtool check --model --workbooks ${_fw_workbook_check_dirs})
  add_test(NAME office_workbook_malicious COMMAND fwtool roundtrip --model --expect-reject --workbooks "${_fw_office}/malicious")
endif()
