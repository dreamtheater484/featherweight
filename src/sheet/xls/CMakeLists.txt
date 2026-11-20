# .xls (Excel 97-2003, BIFF8) import (S4): converted into an .xlsx package for the xlsx reader.
target_sources(fwsheet PRIVATE Xls.cpp Ptg.cpp Crypt.cpp Drawing.cpp)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_xls.cpp")
endif()
