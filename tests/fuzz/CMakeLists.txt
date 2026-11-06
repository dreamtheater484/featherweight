# Fuzz targets (plan §10): one per parser, seeded from the corpus. See README.md.
#   libfuzzer: libFuzzer + AddressSanitizer, with the code under test instrumented too.
#   replay:    plain executables that run each file given once, for toolchains without libFuzzer.
set(FW_FUZZ_ENGINE "libfuzzer" CACHE STRING "Fuzz engine: libfuzzer or replay")
set_property(CACHE FW_FUZZ_ENGINE PROPERTY STRINGS libfuzzer replay)

set(_fw_fuzz_targets fuzz_zip fuzz_xml fuzz_opc fuzz_check fuzz_formula fuzz_docx fuzz_drawing fuzz_layout fuzz_picture fuzz_rtf
  fuzz_html fuzz_clip fuzz_shapes fuzz_sfnt fuzz_cfb fuzz_chart fuzz_doc)
if(FW_FUZZ_ENGINE STREQUAL "libfuzzer")
  # VS 2022 understands /fsanitize=fuzzer. VS 2019 16.9+ ignores it but has the pieces:
  # the coverage flags, and the libFuzzer and sancov runtimes, linked by hand.
  set(_fw_instrument /fsanitize=address /fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
    /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
  foreach(lib fwcore fwpkg fwdoc fwdraw fwchart fwtext fwdoclayout fwpictures miniz pugixml)
    target_compile_options(${lib} PRIVATE ${_fw_instrument})
  endforeach()
endif()

foreach(target ${_fw_fuzz_targets})
  if(FW_FUZZ_ENGINE STREQUAL "libfuzzer")
    add_executable(${target} ${target}.cpp)
    if(MSVC_VERSION GREATER_EQUAL 1930)
      target_compile_options(${target} PRIVATE /fsanitize=address /fsanitize=fuzzer)
    else()
      target_compile_options(${target} PRIVATE ${_fw_instrument})
      target_link_libraries(${target} PRIVATE clang_rt.fuzzer_MT-x86_64 libsancov)
    endif()
    target_link_options(${target} PRIVATE /INCREMENTAL:NO)
  else()
    add_executable(${target} ${target}.cpp replay_main.cpp)
  endif()
  target_link_libraries(${target} PRIVATE fwpkg)
  target_compile_options(${target} PRIVATE ${FW_COMPILE_OPTIONS})
endforeach()
target_link_libraries(fuzz_docx PRIVATE fwdoc)
foreach(target fuzz_rtf fuzz_html fuzz_clip fuzz_doc)
  target_link_libraries(${target} PRIVATE fwdoc)
endforeach()
target_link_libraries(fuzz_drawing PRIVATE fwdraw)
target_link_libraries(fuzz_sfnt PRIVATE fwdraw)
target_link_libraries(fuzz_layout PRIVATE fwdoclayout)
target_link_libraries(fuzz_picture PRIVATE fwpictures)
target_link_libraries(fuzz_shapes PRIVATE fwpictures)
target_link_libraries(fuzz_chart PRIVATE fwchart)

# Replay the seeds and the regression files once (libFuzzer: -runs=0 runs the inputs, no fuzzing).
# run-fuzzers.ps1 lists the same seed folders for the timed runs.
set(_fw_runs $<$<STREQUAL:${FW_FUZZ_ENGINE},libfuzzer>:-runs=0>)
set(_fw_seeds "${CMAKE_CURRENT_SOURCE_DIR}/seeds")
set(_fw_regressions "${CMAKE_CURRENT_SOURCE_DIR}/regressions")
add_test(NAME fuzz_zip_replay COMMAND fuzz_zip ${_fw_runs} "${CMAKE_SOURCE_DIR}/tests/office" "${_fw_regressions}/zip")
add_test(NAME fuzz_xml_replay COMMAND fuzz_xml ${_fw_runs} "${_fw_seeds}/xml" "${_fw_regressions}/xml")
add_test(NAME fuzz_opc_replay COMMAND fuzz_opc ${_fw_runs} "${_fw_seeds}/opc" "${_fw_regressions}/opc")
add_test(NAME fuzz_check_replay COMMAND fuzz_check ${_fw_runs} "${_fw_seeds}/package" "${_fw_regressions}/check")
add_test(NAME fuzz_docx_replay COMMAND fuzz_docx ${_fw_runs} "${_fw_seeds}/docx" "${_fw_seeds}/package" "${_fw_regressions}/docx")
add_test(NAME fuzz_layout_replay COMMAND fuzz_layout ${_fw_runs} "${_fw_seeds}/docx" "${_fw_seeds}/package" "${_fw_regressions}/layout")
add_test(NAME fuzz_formula_replay COMMAND fuzz_formula ${_fw_runs} "${_fw_seeds}/formula" "${_fw_regressions}/formula")
add_test(NAME fuzz_drawing_replay COMMAND fuzz_drawing ${_fw_runs} "${_fw_seeds}/drawing" "${_fw_regressions}/drawing")
add_test(NAME fuzz_picture_replay COMMAND fuzz_picture ${_fw_runs} "${_fw_seeds}/picture" "${_fw_regressions}/picture")
add_test(NAME fuzz_rtf_replay COMMAND fuzz_rtf ${_fw_runs} "${CMAKE_SOURCE_DIR}/tests/clipboard" "${_fw_regressions}/rtf")
add_test(NAME fuzz_html_replay COMMAND fuzz_html ${_fw_runs} "${CMAKE_SOURCE_DIR}/tests/clipboard" "${_fw_regressions}/html")
add_test(NAME fuzz_clip_replay COMMAND fuzz_clip ${_fw_runs} "${_fw_seeds}/docx" "${_fw_seeds}/package" "${_fw_regressions}/clip")
add_test(NAME fuzz_shapes_replay COMMAND fuzz_shapes ${_fw_runs} "${_fw_seeds}/shapes" "${_fw_seeds}/docx" "${_fw_seeds}/package"
  "${_fw_regressions}/shapes")
add_test(NAME fuzz_cfb_replay COMMAND fuzz_cfb ${_fw_runs} "${_fw_seeds}/cfb" "${_fw_regressions}/cfb")
add_test(NAME fuzz_doc_replay COMMAND fuzz_doc ${_fw_runs} "${_fw_seeds}/doc" "${_fw_regressions}/doc")
add_test(NAME fuzz_sfnt_replay COMMAND fuzz_sfnt ${_fw_runs} "${_fw_seeds}/font" "${_fw_regressions}/sfnt")
add_test(NAME fuzz_chart_replay COMMAND fuzz_chart ${_fw_runs} "${_fw_seeds}/chart" "${_fw_regressions}/chart")

# Every fuzz target, so CI builds new ones without its own list.
add_custom_target(fuzz_all DEPENDS ${_fw_fuzz_targets})
