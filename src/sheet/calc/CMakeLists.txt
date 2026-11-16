# The formula engine (plan §7.3 "Formula engine"): parser, bytecode, evaluator, functions and the
# dependency graph.
target_sources(fwsheet PRIVATE
  Functions.cpp
  Parser.cpp
  Print.cpp
  Value.cpp
)
if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_calc.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
endif()
