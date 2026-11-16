# fwsheetobjects: what sits on a sheet above its cells (plan §7.2): pictures, shapes, charts and
# chart sheets through fwdraw and fwchart, notes and threaded comments with their text through
# fwtext. Headless: the grid view calls it from its painting and printing. Apart from fwsheet so
# the model, calc and their tools and fuzzers don't link fwchart and fwtext.
add_library(fwsheetobjects STATIC ChartData.cpp RichText.cpp SheetObjects.cpp)
target_include_directories(fwsheetobjects PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwsheetobjects PUBLIC fwsheet fwdraw fwchart fwtext)
target_compile_options(fwsheetobjects PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_objects.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheetobjects)
endif()
