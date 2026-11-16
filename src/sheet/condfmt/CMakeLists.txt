# Conditional formats (s1-plan phase 3): a sheet's rules evaluated into what each cell shows.
target_sources(fwsheet PRIVATE CondFormat.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_condfmt.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
