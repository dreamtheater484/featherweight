target_sources(fwsheet PRIVATE
  Ods.cpp
  OdsFormula.cpp
  OdsStyles.cpp
)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_ods.cpp")
endif()
