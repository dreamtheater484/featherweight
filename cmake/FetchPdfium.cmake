# Downloads pinned, prebuilt PDFium from bblanchon/pdfium-binaries and verifies it.
# Updating PDFium is a deliberate change: scripts/update-pdfium.ps1 bumps the build number and every hash together.
#
# Two builds of each release (f1-ship): "standard" (no JavaScript, no XFA; about 7.5 MB, what the download ships)
# and "v8" (form scripts and XFA; about 32 MB, the optional script engine). FW_WITH_V8 picks the one linked and
# copied beside the exe; FW_PDFIUM_COMPONENT also fetches the other and copies it as pdfium-v8.dll (release builds).
# x64 and ARM64; the archives' SHA-256 are GitHub's asset digests, checked again on download.

set(FW_PDFIUM_BUILD "8086")
set(_fw_pdfium_sha256_standard_x64 "1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d")
set(_fw_pdfium_sha256_v8_x64 "cb8f1e0e0c0dd74cbb17e8ec11d8969c4b95638fd3e66fd1c7e4a684d28a0d46")
set(_fw_pdfium_sha256_standard_arm64 "1799b8034e6d64946fec0ae79f3edfc8ba58ccd80c70a46194803b1eba408344")
set(_fw_pdfium_sha256_v8_arm64 "c4943e7d3b0052983d5a783879a27d7dd0ccfc9e3c9d247be5ebc06ece77f0b5")

option(FW_PDFIUM_COMPONENT "Also fetch the PDFium build with form scripts, as the optional component pdfium-v8.dll" OFF)

if(MSVC_CXX_ARCHITECTURE_ID STREQUAL "ARM64")
  set(FW_ARCH "arm64")
else()
  set(FW_ARCH "x64")
endif()

# fw_fetch_pdfium(<standard|v8> <out var>): the folder holding that build for this architecture, downloaded and
# extracted on first use into third_party/pdfium/<build>-<variant>[-arm64].
function(fw_fetch_pdfium variant out)
  set(root "${CMAKE_SOURCE_DIR}/third_party/pdfium")
  set(suffix "")
  if(FW_ARCH STREQUAL "arm64")
    set(suffix "-arm64")
  endif()
  set(dir "${root}/${FW_PDFIUM_BUILD}-${variant}${suffix}")
  if(variant STREQUAL "v8")
    set(asset "pdfium-v8-win-${FW_ARCH}.tgz")
  else()
    set(asset "pdfium-win-${FW_ARCH}.tgz")
  endif()
  set(sha256 "${_fw_pdfium_sha256_${variant}_${FW_ARCH}}")
  if(NOT EXISTS "${dir}/include/fpdfview.h")
    set(url "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${FW_PDFIUM_BUILD}/${asset}")
    set(archive "${root}/${FW_PDFIUM_BUILD}-${asset}")
    message(STATUS "Downloading PDFium ${FW_PDFIUM_BUILD} (${variant}, ${FW_ARCH})")
    file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "PDFium download failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
    file(REMOVE "${archive}")
  endif()
  set(${out} "${dir}" PARENT_SCOPE)
endfunction()

if(FW_WITH_V8)
  fw_fetch_pdfium(v8 FW_PDFIUM_DIR)
else()
  fw_fetch_pdfium(standard FW_PDFIUM_DIR)
endif()
set(FW_PDFIUM_V8_DLL "")
if(FW_PDFIUM_COMPONENT)
  fw_fetch_pdfium(v8 _fw_pdfium_v8_dir)
  set(FW_PDFIUM_V8_DLL "${_fw_pdfium_v8_dir}/bin/pdfium.dll")
endif()

add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
  IMPORTED_LOCATION "${FW_PDFIUM_DIR}/bin/pdfium.dll"
  IMPORTED_IMPLIB "${FW_PDFIUM_DIR}/lib/pdfium.dll.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${FW_PDFIUM_DIR}/include")
