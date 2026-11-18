# The grid view (plan §7.3 "Grid view", §5.3). Two libraries:
#  - fwsheetrender: headless drawing of cells and pages through draw::Painter (column widths and row
#    heights as Excel has them, cell text, fills, borders, pagination). Tools and tests can use it.
#  - fwsheetview: the workbook tab (a TabContent), on the app's shell; linked into the exe only.
add_library(fwsheetrender STATIC Grid.cpp CellText.cpp CellPaint.cpp PrintLayout.cpp CellEditor.cpp)
target_link_libraries(fwsheetrender PUBLIC fwsheet fwsheetobjects fwtext fwdraw)
target_compile_options(fwsheetrender PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

# The tab: built against the app's shell headers (src/), and linked into the exe, which has them.
add_library(fwsheetview STATIC WorkbookView.cpp WorkbookInput.cpp WorkbookCommands.cpp WorkbookPrint.cpp
            WorkbookEdit.cpp WorkbookFunctions.cpp
            WorkbookSheetOps.cpp WorkbookPanels.cpp WorkbookFill.cpp
            WorkbookClipboard.cpp WorkbookFile.cpp ClipHtml.cpp WorkbookFormat.cpp WorkbookFilter.cpp
            WorkbookData.cpp WorkbookNames.cpp)
target_include_directories(fwsheetview PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(fwsheetview PUBLIC fwsheetrender)
target_compile_options(fwsheetview PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
# The grid's editing logic, and the workbook tab's file (S2 "Saving", built into the tests with
# FileSession, which src/doc adds).
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_grid_edit.cpp"
                                  WorkbookFile.cpp "${PROJECT_SOURCE_DIR}/tests/unit/test_workbookfile.cpp")
  # Pasting HTML from Excel, Word and the web (the reader alone).
  target_sources(fw_tests PRIVATE ClipHtml.cpp "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_clip_html.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheetrender)
endif()
