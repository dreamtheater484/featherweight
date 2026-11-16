# fwsheet: the spreadsheet engine (plan §4.8, §7.3): the workbook model, the .xlsx and CSV readers,
# number formats and formulas. Headless: no window, no PDFium, no fwui. The grid view (fwsheetview)
# is apart, linked into the app only.
add_library(fwsheet STATIC)
target_include_directories(fwsheet PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwsheet PUBLIC fwpkg fwcore fwdraw)
target_compile_options(fwsheet PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
add_subdirectory(model)
add_subdirectory(xlsx)
add_subdirectory(numfmt)
add_subdirectory(calc)
add_subdirectory(view)
