# fwtext: rich text on DirectWrite's text analyzer: fonts, line layout, hyphenation and spelling
# (plan §4.1, §4.4). Headless: no UI and no window. The hyphenation patterns (cmake/FetchDeps.cmake) are packed
# into a generated source file.
file(GLOB _fw_hyph_inputs "${FW_HYPH_UTF8_DIR}/*" "${FW_OPENTAAL_HYPH_DIR}/*")
set(_fw_hyph_cpp "${CMAKE_CURRENT_BINARY_DIR}/HyphenationData.cpp")
add_custom_command(OUTPUT "${_fw_hyph_cpp}"
  COMMAND "${CMAKE_COMMAND}" "-DHYPH_UTF8=${FW_HYPH_UTF8_DIR}" "-DOPENTAAL=${FW_OPENTAAL_HYPH_DIR}"
    "-DOUT=${_fw_hyph_cpp}" -P "${CMAKE_CURRENT_SOURCE_DIR}/PackPatterns.cmake"
  DEPENDS PackPatterns.cmake ${_fw_hyph_inputs}
  COMMENT "Packing hyphenation patterns"
  VERBATIM)

add_library(fwtext STATIC
  Analysis.cpp
  Fonts.cpp
  Hyphenation.cpp
  LineLayout.cpp
  SpellCheck.cpp
  "${_fw_hyph_cpp}"
)
target_include_directories(fwtext PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwtext PUBLIC fwcore dwrite ole32 PRIVATE miniz)
target_compile_options(fwtext PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_text.cpp")
  target_link_libraries(fw_tests PRIVATE fwtext)
endif()
