# Data (plan §7.2): sort order and the header guess, filter criteria (remove duplicates follows).
# Reads the model only; the edit layer applies the result.
target_sources(fwsheet PRIVATE Sort.cpp Filter.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_sort.cpp"
                                  "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_filter.cpp")
endif()
