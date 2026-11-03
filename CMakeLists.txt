cmake_minimum_required(VERSION 3.21)

project(Featherweight VERSION 0.9.4 LANGUAGES CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Featherweight builds with MSVC for 64-bit Windows only.")
endif()

option(FW_WITH_V8 "Use the PDFium build with form JavaScript (V8) and XFA" ON)
option(FW_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static C runtime: no Visual C++ Redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/FetchPdfium.cmake)

configure_file(src/Version.h.in "${CMAKE_BINARY_DIR}/generated/Version.h" @ONLY)

set(FW_COMPILE_OPTIONS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
set(FW_DEFINITIONS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# --- Pure logic (no UI, no PDFium): shared by the app and the unit tests ---
add_library(fwcore STATIC
  src/core/Strings.cpp
  src/core/FileKinds.cpp
  src/core/Ini.cpp
  src/core/Json.cpp
  src/core/Layout.cpp
  src/core/PageRange.cpp
  src/core/SemVer.cpp
)
target_include_directories(fwcore PUBLIC src)
target_compile_options(fwcore PRIVATE ${FW_COMPILE_OPTIONS})
target_compile_definitions(fwcore PUBLIC ${FW_DEFINITIONS})

# --- The app ---
file(GLOB FW_APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/ui/*.cpp src/pdf/*.cpp)
add_executable(Featherweight WIN32
  ${FW_APP_SOURCES}
  src/resource.rc
  src/Featherweight.manifest
)
target_include_directories(Featherweight PRIVATE src "${CMAKE_BINARY_DIR}/generated")
target_compile_options(Featherweight PRIVATE ${FW_COMPILE_OPTIONS})
target_compile_definitions(Featherweight PRIVATE "$<$<BOOL:${FW_WITH_V8}>:FW_WITH_V8=1;PDF_ENABLE_V8;PDF_ENABLE_XFA>")
target_link_libraries(Featherweight PRIVATE
  fwcore pdfium
  d2d1 d3d11 dxgi dwrite windowscodecs dxguid dwmapi uxtheme shlwapi shell32 ole32 oleaut32
  comctl32 comdlg32 winhttp bcrypt version advapi32 user32 gdi32 winspool delayimp
)
# pdfium.dll is loaded on first use, so the window appears before the engine is loaded.
target_link_options(Featherweight PRIVATE /DELAYLOAD:pdfium.dll
  $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF /LTCG>)
target_compile_options(Featherweight PRIVATE $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
target_compile_options(fwcore PRIVATE $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
set_target_properties(Featherweight PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Featherweight>")

add_custom_command(TARGET Featherweight POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FW_PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:Featherweight>"
  COMMENT "Copying pdfium.dll")

# --- Unit tests ---
if(FW_BUILD_TESTS)
  enable_testing()
  add_executable(fw_tests tests/unit/main.cpp tests/unit/test_core.cpp)
  target_link_libraries(fw_tests PRIVATE fwcore)
  target_compile_options(fw_tests PRIVATE ${FW_COMPILE_OPTIONS})
  add_test(NAME unit COMMAND fw_tests)
  # File kinds, settings and file associations: temporary settings files, and registry dry runs only.
  target_sources(fw_tests PRIVATE tests/unit/test_filekinds.cpp tests/unit/test_settings.cpp
    tests/unit/test_fileassoc.cpp src/Settings.cpp src/Util.cpp src/FileAssoc.cpp)
  target_link_libraries(fw_tests PRIVATE bcrypt shell32 ole32 advapi32 user32)
endif()

# --- Office foundations (plan milestone D0): fwpkg, fwtool, corpus tests and fuzz targets ---
# Headless: no UI, no PDFium. miniz and pugixml are downloaded by cmake/FetchDeps.cmake.
option(FW_FUZZ "Build the fuzz targets in tests/fuzz (see tests/fuzz/README.md)" OFF)
include(cmake/FetchDeps.cmake)
add_subdirectory(src/pkg)
add_subdirectory(src/draw)
add_subdirectory(tools/fwtool)
target_link_libraries(Featherweight PRIVATE fwdraw)
if(FW_FUZZ)
  enable_testing()
  add_subdirectory(tests/fuzz)
endif()
if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE
    tests/unit/test_units.cpp
    tests/unit/test_zip.cpp
    tests/unit/test_xml.cpp
    tests/unit/test_package.cpp
    tests/unit/test_roundtrip.cpp
    tests/unit/test_draw.cpp)
  target_link_libraries(fw_tests PRIVATE fwpkg fwdraw windowscodecs ole32)
  # Never-damage tests on the public corpus (plan §10).
  set(_fw_office "${CMAKE_SOURCE_DIR}/tests/office")
  add_test(NAME office_roundtrip COMMAND fwtool roundtrip "${_fw_office}/generated" "${_fw_office}/edge")
  add_test(NAME office_reserialize COMMAND fwtool roundtrip --reserialize-all "${_fw_office}/generated" "${_fw_office}/edge")
  add_test(NAME office_check COMMAND fwtool check "${_fw_office}/generated" "${_fw_office}/edge")
  add_test(NAME office_malicious COMMAND fwtool roundtrip --expect-reject "${_fw_office}/malicious")
endif()

# Packaging (installer, portable zip, checksums) is done by scripts/release.ps1.
