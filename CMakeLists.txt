cmake_minimum_required(VERSION 3.21)

project(Featherweight VERSION 4.0.0 LANGUAGES CXX RC)

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

# Reproducible builds (f1-ship, docs/reproducible-builds.md): the same source, toolchain and folder give the same
# bytes. /Brepro replaces the timestamps in the objects and the PE header with hashes of the content, and the
# exe names its PDB without the build folder.
option(FW_REPRODUCIBLE "Byte-identical builds: no timestamps or build folders in the binaries" ON)
if(FW_REPRODUCIBLE)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/Brepro>)
  add_link_options(/Brepro /PDBALTPATH:%_PDB%)
endif()

# --- Pure logic (no UI, no PDFium): shared by the app and the unit tests ---
add_library(fwcore STATIC
  src/core/Strings.cpp
  src/core/FileKinds.cpp
  src/core/Ini.cpp
  src/core/Json.cpp
  src/core/Layout.cpp
  src/core/LayoutDump.cpp
  src/core/PageRange.cpp
  src/core/SemVer.cpp
)
target_include_directories(fwcore PUBLIC src)
target_compile_options(fwcore PRIVATE ${FW_COMPILE_OPTIONS})
target_compile_definitions(fwcore PUBLIC ${FW_DEFINITIONS})

# --- The app ---
file(GLOB FW_APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/ui/*.cpp src/pdf/*.cpp src/word/*.cpp)
# Test-only screens (the widget gallery) never ship (plan §11): scripts/release.ps1 turns this off.
option(FW_TEST_SCREENS "Build the widget gallery and other test-only screens into the app" ON)
if(NOT FW_TEST_SCREENS)
  list(FILTER FW_APP_SOURCES EXCLUDE REGEX "/src/ui/WidgetGallery\.cpp$")
endif()
add_executable(Featherweight WIN32
  ${FW_APP_SOURCES}
  src/resource.rc
  src/Featherweight.manifest
)
# The ribbon (R1) and the start page (P3) are UI code that runs once a frame at most: built for size.
set_source_files_properties(src/AppRibbon.cpp src/ui/Ribbon.cpp src/ui/FilePage.cpp src/ui/Icons.cpp src/ui/EmptyState.cpp
                            PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
target_include_directories(Featherweight PRIVATE src "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(Featherweight PRIVATE "$<$<BOOL:${FW_TEST_SCREENS}>:FW_TEST_SCREENS=1>")
target_compile_options(Featherweight PRIVATE ${FW_COMPILE_OPTIONS})
# PDFium's full API (with V8 and XFA) in every build: the program runs with either PDFium build, and looks up the
# few functions only the script engine has (f1-ship). FW_WITH_V8 picks the build linked and copied beside it.
target_compile_definitions(Featherweight PRIVATE PDF_ENABLE_V8 PDF_ENABLE_XFA "$<$<BOOL:${FW_WITH_V8}>:FW_WITH_V8=1>")
target_link_libraries(Featherweight PRIVATE
  fwcore pdfium
  d2d1 d3d11 dxgi dwrite windowscodecs dxguid dwmapi uxtheme shlwapi shell32 ole32 oleaut32
  comctl32 comdlg32 winhttp bcrypt version advapi32 user32 gdi32 winspool delayimp secur32
  wintrust  # f1-ship: the updater's signature check, delay-loaded below
)
# pdfium.dll is loaded on first use, so the window appears before the engine is loaded, and
# UI Automation's DLL only once a client such as a screen reader asks for the window. crypt32.dll
# (DPAPI, for recovery autosaves) waits for the first unsaved change.
target_link_options(Featherweight PRIVATE /DELAYLOAD:pdfium.dll /DELAYLOAD:uiautomationcore.dll /DELAYLOAD:crypt32.dll
  /DELAYLOAD:wintrust.dll
  $<$<CONFIG:Release,MinSizeRel>:/OPT:REF /OPT:ICF /LTCG>)
target_compile_options(Featherweight PRIVATE $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
target_compile_options(fwcore PRIVATE $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
set_target_properties(Featherweight PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Featherweight>")

add_custom_command(TARGET Featherweight POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FW_PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:Featherweight>"
  COMMENT "Copying pdfium.dll")
if(FW_PDFIUM_V8_DLL)  # f1-ship: release builds stage the script engine component beside it
  add_custom_command(TARGET Featherweight POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FW_PDFIUM_V8_DLL}" "$<TARGET_FILE_DIR:Featherweight>/pdfium-v8.dll"
    COMMENT "Copying pdfium-v8.dll (the form script engine component)")
endif()

# --- Unit tests ---
if(FW_BUILD_TESTS)
  enable_testing()
  add_executable(fw_tests tests/unit/main.cpp tests/unit/test_core.cpp)
  target_link_libraries(fw_tests PRIVATE fwcore)
  target_compile_options(fw_tests PRIVATE ${FW_COMPILE_OPTIONS})
  add_test(NAME unit COMMAND fw_tests)
  # File kinds, settings and file associations: temporary settings files, and registry dry runs only.
  target_sources(fw_tests PRIVATE tests/unit/test_filekinds.cpp tests/unit/test_settings.cpp
    tests/unit/test_fileassoc.cpp src/Settings.cpp src/Util.cpp src/FileAssoc.cpp
    tests/unit/test_usertemplates.cpp src/UserTemplates.cpp)
  target_link_libraries(fw_tests PRIVATE bcrypt shell32 ole32 advapi32 user32)
  # f1-ship: the updater's trust rule, with test-only certificates from tests/data/updater.
  target_sources(fw_tests PRIVATE tests/unit/test_f1_ship.cpp src/Updater.cpp)
  target_include_directories(fw_tests PRIVATE "${CMAKE_BINARY_DIR}/generated")
  target_sources(fw_tests PRIVATE src/Policy.cpp tests/unit/fw_tests.rc)
  target_compile_definitions(fw_tests PRIVATE "FW_TEST_DATA_DIR=L\"${CMAKE_SOURCE_DIR}/tests/data\""
    "FW_SOURCE_DIR=L\"${CMAKE_SOURCE_DIR}\"" "FW_PDFIUM_DLL=L\"${FW_PDFIUM_DIR}/bin/pdfium.dll\""
    "FW_PDFIUM_IS_V8=$<BOOL:${FW_WITH_V8}>")
  target_link_libraries(fw_tests PRIVATE winhttp wintrust crypt32 version)
  # The gate reads these names (FeatherweightNotes\f1\gate-contract.md): each runs its own part of fw_tests.
  foreach(_fw_case IN ITEMS updater_refuses_unsigned=UpdaterRefusesUnsigned updater_refuses_other_publisher=UpdaterRefusesOtherPublisher
                            updater_channels=UpdaterChannels updater_rollback=UpdaterRollback policy_updates_off=PolicyUpdatesOff)
    string(REPLACE "=" ";" _fw_case "${_fw_case}")
    list(GET _fw_case 0 _fw_name)
    list(GET _fw_case 1 _fw_filter)
    add_test(NAME ${_fw_name} COMMAND fw_tests ${_fw_filter})
    # fw_tests passes when no name matches, so each must show its own test passing.
    set_tests_properties(${_fw_name} PROPERTIES PASS_REGULAR_EXPRESSION "\\[ ok \\] ${_fw_filter}" FAIL_REGULAR_EXPRESSION "FAILED")
  endforeach()
endif()

# --- Office foundations (plan milestone D0): fwpkg, fwtool, corpus tests and fuzz targets ---
# Headless: no UI, no PDFium. miniz and pugixml are downloaded by cmake/FetchDeps.cmake.
option(FW_FUZZ "Build the fuzz targets in tests/fuzz (see tests/fuzz/README.md)" OFF)
include(cmake/FetchDeps.cmake)
add_subdirectory(src/pkg)
add_subdirectory(src/draw)
add_subdirectory(src/chart)
add_subdirectory(src/doc)
add_subdirectory(src/sheet)
add_subdirectory(src/text)
add_subdirectory(src/slide)
add_subdirectory(src/slide/render)
add_subdirectory(src/slide/show)
add_subdirectory(src/slide/objects)  # p2-objects: objects on slides
add_subdirectory(src/slide/odp)  # p3-files: OpenDocument presentations
add_subdirectory(src/slide/view)  # p1-view: the presentation tab
add_subdirectory(tools/fwtool)
target_link_libraries(Featherweight PRIVATE fwdraw fwchart fwdoclayout fwproof fwpictures fwsheetview fwslideview)
if(FW_FUZZ)
  enable_testing()
  add_subdirectory(tests/fuzz)
endif()
if(FW_BUILD_TESTS)
  target_sources(fw_tests PRIVATE
    tests/unit/test_units.cpp
    tests/unit/test_zip.cpp
    tests/unit/test_cfb.cpp
    tests/unit/test_crypto.cpp
    tests/unit/test_xml.cpp
    tests/unit/test_package.cpp
    tests/unit/test_safefile.cpp
    tests/unit/test_filesafety.cpp
    tests/unit/test_roundtrip.cpp
    tests/unit/test_references.cpp
    tests/unit/test_draw.cpp)
  target_link_libraries(fw_tests PRIVATE fwpkg fwdraw windowscodecs ole32)
  # Never-damage tests on the public corpus (plan §10).
  set(_fw_office "${CMAKE_SOURCE_DIR}/tests/office")
  add_test(NAME office_roundtrip COMMAND fwtool roundtrip "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  add_test(NAME office_reserialize COMMAND fwtool roundtrip --reserialize-all "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  add_test(NAME office_check COMMAND fwtool check "${_fw_office}/generated" "${_fw_office}/edge" "${_fw_office}/layout")
  add_test(NAME office_check_broken COMMAND fwtool check --expect-reject "${_fw_office}/broken")
  add_test(NAME office_malicious COMMAND fwtool roundtrip --expect-reject "${_fw_office}/malicious")
  # Password-protected documents (D5): each opens with its password and encrypts again.
  set(_fw_protected "${_fw_office}/protected")
  add_test(NAME office_protected COMMAND fwtool decrypt --password featherweight --out "${CMAKE_BINARY_DIR}/protected"
    "${_fw_protected}/report-featherweight.docx" "${_fw_protected}/simple-protected.docx"
    "${_fw_protected}/tables-images-links-protected.docx" "${_fw_protected}/simple-protected.doc"
    "${_fw_protected}/tables-images-links-protected.doc")
  add_test(NAME office_protected_standard COMMAND fwtool decrypt --password Password1234_
    --out "${CMAKE_BINARY_DIR}/protected-standard" "${_fw_protected}/ecma376-standard.docx")
  # The ribbon (R1): tests/screens/ribbon.cmake. fw_ribbon_test(<name> <mode>) runs a mode of it:
  # screens (the widget gallery's demo ribbon and the editors at 1366, 1024 and 800 px, light and
  # dark, KeyTips, the File page, tab switches within 16 ms), or an editor's coverage check (word,
  # sheet, slide), which r1-word, r1-sheet and r1-slide turn on once their ribbon shows.
  function(fw_ribbon_test name mode)
    if(NOT FW_TEST_SCREENS)  # its script steps (ribboncheck, dumpreach, ribbonbench) are test-only
      return()
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:Featherweight>"
             "-DFILES=${_fw_office}/generated" "-DBASELINES=${CMAKE_SOURCE_DIR}/tests/ribbon"
             "-DOUT=${CMAKE_BINARY_DIR}/screens/${name}" -DMODE=${mode} -P "${CMAKE_SOURCE_DIR}/tests/screens/ribbon.cmake")
  endfunction()
  if(FW_TEST_SCREENS)
    fw_ribbon_test(ribbon_screens screens)
    fw_ribbon_test(ribbon_slide slide)  # r1-slide: the presentation ribbon's coverage and states
    fw_ribbon_test(ribbon_sheet sheet)  # r1-sheet: the workbook's coverage and screens
  endif()
  fw_ribbon_test(ribbon_word word)  # r1-word: the document's coverage, its contextual tabs, Insert, the boxes
  # The start page (P3, p3-start): tests/screens/start.cmake, at 1366, 1920, 2560 and 5120 px, light
  # and dark, and its states (keyboard, filter, hidden, empty, narrow, 200 %).
  add_test(NAME start_screens COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:Featherweight>"
           "-DOUT=${CMAKE_BINARY_DIR}/screens/start" -P "${CMAKE_SOURCE_DIR}/tests/screens/start.cmake")
endif()

# Packaging (installer, portable zip, checksums) is done by scripts/release.ps1.
