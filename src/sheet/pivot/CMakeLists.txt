# Pivot tables (S4, s4-pivot): caches and tables read, refreshed from their source, laid out and
# written as Excel does (Pivot.h). Run on a refresh, never per cell: compiled for size.
target_sources(fwsheet PRIVATE PivotRead.cpp PivotLayout.cpp PivotWrite.cpp)
set_source_files_properties(PivotRead.cpp PivotLayout.cpp PivotWrite.cpp TARGET_DIRECTORY fwsheet PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_pivot.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
