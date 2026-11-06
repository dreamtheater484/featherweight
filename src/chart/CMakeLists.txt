# fwchart (plan D5): charts read from c:chartSpace's cached values and drawn as fwdraw art. fwdraw
# calls it through its chart hook (fw::chart::Install), so fwdraw doesn't depend on it.
add_library(fwchart STATIC Draw.cpp Read.cpp)
target_link_libraries(fwchart PUBLIC fwdraw)
target_compile_options(fwchart PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_chart.cpp")
  target_link_libraries(fw_tests PRIVATE fwchart)
endif()
