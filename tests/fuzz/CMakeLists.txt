# Fuzz targets (plan §10): one per parser, seeded from the corpus. See README.md.
#   libfuzzer: libFuzzer + AddressSanitizer, with the code under test instrumented too.
#   replay:    plain executables that run each file given once, for toolchains without libFuzzer.
set(FW_FUZZ_ENGINE "libfuzzer" CACHE STRING "Fuzz engine: libfuzzer or replay")
set_property(CACHE FW_FUZZ_ENGINE PROPERTY STRINGS libfuzzer replay)

set(_fw_fuzz_targets fuzz_zip fuzz_xml)
if(FW_FUZZ_ENGINE STREQUAL "libfuzzer")
  # VS 2022 understands /fsanitize=fuzzer. VS 2019 16.9+ ignores it but has the pieces:
  # the coverage flags, and the libFuzzer and sancov runtimes, linked by hand.
  set(_fw_instrument /fsanitize=address /fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
    /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
  foreach(lib fwcore fwpkg miniz pugixml)
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

# Replay the corpus and the regression files once (libFuzzer: -runs=0 runs the inputs, no fuzzing).
set(_fw_runs $<$<STREQUAL:${FW_FUZZ_ENGINE},libfuzzer>:-runs=0>)
add_test(NAME fuzz_zip_replay COMMAND fuzz_zip ${_fw_runs} "${CMAKE_SOURCE_DIR}/tests/office"
  "${CMAKE_CURRENT_SOURCE_DIR}/regressions/zip")
add_test(NAME fuzz_xml_replay COMMAND fuzz_xml ${_fw_runs} "${CMAKE_CURRENT_SOURCE_DIR}/seeds/xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/regressions/xml")
