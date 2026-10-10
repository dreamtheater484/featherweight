cmake_minimum_required(VERSION 3.21)

project(Featherweight VERSION 0.9.3 LANGUAGES CXX RC)

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
file(GLOB FW_APP_SOURCES CONFIGURE_DEPENDS src/*.cpp)
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
  d2d1 dwrite windowscodecs dxguid dwmapi uxtheme shlwapi shell32 ole32 oleaut32
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
endif()

# Packaging (installer, portable zip, checksums) is done by scripts/release.ps1.
