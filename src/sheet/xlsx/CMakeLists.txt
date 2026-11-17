# The .xlsx reader, and the writer the round-trip tests use (saving comes in S2).
target_sources(fwsheet PRIVATE Numbers.cpp Reader.cpp Save.cpp SheetReader.cpp Writer.cpp)
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
  # Excel-made workbooks of the S1 pieces' own (tests/sheet) too.
  list(APPEND _fw_workbook_dirs "${PROJECT_SOURCE_DIR}/tests/sheet")
  list(APPEND _fw_workbook_check_dirs "${PROJECT_SOURCE_DIR}/tests/sheet")
  add_test(NAME office_workbook_roundtrip COMMAND fwtool roundtrip --model --workbooks ${_fw_workbook_dirs})
  add_test(NAME office_workbook_reserialize COMMAND fwtool roundtrip --model --reserialize-all --workbooks ${_fw_workbook_dirs})
  add_test(NAME office_workbook_check COMMAND fwtool check --model --workbooks ${_fw_workbook_check_dirs})
  # The Open XML SDK validator on what the model writes back, where .NET is installed.
  find_program(FW_DOTNET dotnet)
  if(FW_DOTNET)
    add_test(NAME office_workbook_validate COMMAND ${CMAKE_COMMAND} -DFWTOOL=$<TARGET_FILE:fwtool>
             -DSOURCE=${PROJECT_SOURCE_DIR} -DBINARY=${PROJECT_BINARY_DIR} "-DDIRS=${_fw_workbook_check_dirs}"
             -P "${CMAKE_CURRENT_SOURCE_DIR}/ValidateWorkbooks.cmake")
  endif()
  add_test(NAME office_workbook_malicious COMMAND fwtool roundtrip --model --expect-reject --workbooks "${_fw_office}/malicious")
endif()
