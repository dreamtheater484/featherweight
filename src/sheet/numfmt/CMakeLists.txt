# Number formats (plan §7.3): Excel's format codes, the locale they follow, and reading typed or CSV
# values back. Needs nothing from the other pieces.
target_sources(fwsheet PRIVATE NumFormat.cpp NumLocale.cpp Parse.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_numfmt.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
