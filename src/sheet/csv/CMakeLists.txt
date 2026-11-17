# CSV and TSV (plan §7.1): read without changing any value, the source's details kept for saving.
target_sources(fwsheet PRIVATE Csv.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_csv.cpp")
endif()
