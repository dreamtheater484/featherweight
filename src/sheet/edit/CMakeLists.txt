# The edit layer (plan §4.3 rules 4 and 5, S2): operations with inverses on the copy-on-write row
# blocks, transactions, undo and redo. The only code that changes a loaded workbook model.
target_sources(fwsheet PRIVATE Edit.cpp Journal.cpp Refs.cpp)
# Run once per edit or save, never per cell: compiled for size (the S2 size budget).
set_source_files_properties(Edit.cpp Journal.cpp Refs.cpp TARGET_DIRECTORY fwsheet PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_edit.cpp"
                                  "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_save.cpp"
                                  "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_shift.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
