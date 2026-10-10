# Downloads a pinned, prebuilt PDFium from bblanchon/pdfium-binaries and verifies it.
# Updating PDFium is a deliberate change: bump the build number and both hashes together.

set(FW_PDFIUM_BUILD "8086")

if(FW_WITH_V8)
  set(_fw_pdfium_variant "v8")
  set(_fw_pdfium_asset "pdfium-v8-win-x64.tgz")
  set(_fw_pdfium_sha256 "cb8f1e0e0c0dd74cbb17e8ec11d8969c4b95638fd3e66fd1c7e4a684d28a0d46")
else()
  set(_fw_pdfium_variant "standard")
  set(_fw_pdfium_asset "pdfium-win-x64.tgz")
  set(_fw_pdfium_sha256 "1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d")
endif()

set(_fw_pdfium_root "${CMAKE_SOURCE_DIR}/third_party/pdfium")
set(FW_PDFIUM_DIR "${_fw_pdfium_root}/${FW_PDFIUM_BUILD}-${_fw_pdfium_variant}")

if(NOT EXISTS "${FW_PDFIUM_DIR}/include/fpdfview.h")
  set(_fw_url "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F${FW_PDFIUM_BUILD}/${_fw_pdfium_asset}")
  set(_fw_archive "${_fw_pdfium_root}/${FW_PDFIUM_BUILD}-${_fw_pdfium_asset}")
  message(STATUS "Downloading PDFium ${FW_PDFIUM_BUILD} (${_fw_pdfium_variant})")
  file(DOWNLOAD "${_fw_url}" "${_fw_archive}"
    EXPECTED_HASH SHA256=${_fw_pdfium_sha256}
    TLS_VERIFY ON
    STATUS _fw_status)
  list(GET _fw_status 0 _fw_code)
  if(NOT _fw_code EQUAL 0)
    file(REMOVE "${_fw_archive}")
    message(FATAL_ERROR "PDFium download failed: ${_fw_status}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_fw_archive}" DESTINATION "${FW_PDFIUM_DIR}")
  file(REMOVE "${_fw_archive}")
endif()

add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
  IMPORTED_LOCATION "${FW_PDFIUM_DIR}/bin/pdfium.dll"
  IMPORTED_IMPLIB "${FW_PDFIUM_DIR}/lib/pdfium.dll.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${FW_PDFIUM_DIR}/include")
