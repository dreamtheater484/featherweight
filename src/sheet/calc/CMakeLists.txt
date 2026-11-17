# The formula engine (plan §7.3 "Formula engine"): parser, bytecode, evaluator, functions and the
# dependency graph.
target_sources(fwsheet PRIVATE
  Bind.cpp
  Engine.cpp
  Entry.cpp
  Eval.cpp
  FnArray.cpp
  FnDate.cpp
  FnLookup.cpp
  FnMath.cpp
  FnFinance.cpp
  FnShape.cpp
  FnText.cpp
  Functions.cpp
  Parser.cpp
  Print.cpp
  Typing.cpp
  FunctionHelp.cpp
  Value.cpp
)
if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_calc.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_entry.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/test_sheet_functions.cpp")
  target_link_libraries(fw_tests PRIVATE fwsheet)
  # Every formula of the Excel-saved test workbooks recalculated and compared with Excel's values
  # (this PC's Excel is en-NL: a decimal comma and day-first dates).
  add_test(NAME office_formulas COMMAND fwtool formulas --locale en-NL --min-match 100 "${PROJECT_SOURCE_DIR}/tests/office/excel")
endif()
