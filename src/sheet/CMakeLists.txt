# fwsheet: the spreadsheet engines (plan §4.8, §7.3, docs/s1-plan.md): the workbook model, the
# .xlsx and CSV readers, number formats and the formula engine. Headless: no window, no PDFium,
# no fwui. Each folder adds its files with target_sources.
add_library(fwsheet STATIC)
target_include_directories(fwsheet PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwsheet PUBLIC fwpkg fwcore)
target_compile_options(fwsheet PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
add_subdirectory(calc)
