# The workbook model (plan §7.3): sheets of cells in copy-on-write row blocks, shared strings,
# styles, defined names and calculation settings.
target_sources(fwsheet PRIVATE Sheet.cpp Styles.cpp TableStyles.cpp Types.cpp Workbook.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_model.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
