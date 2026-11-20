# OpenDocument spreadsheets (S4, s4-ods): converted to an .xlsx package once, when one is opened,
# never per cell on screen: compiled for size.
target_sources(fwsheet PRIVATE
  Ods.cpp
  OdsFormula.cpp
  OdsStyles.cpp
)
set_source_files_properties(Ods.cpp OdsFormula.cpp OdsStyles.cpp TARGET_DIRECTORY fwsheet PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_ods.cpp")
endif()
