# Downloads the pinned source releases fwpkg builds on (miniz, pugixml) and verifies them.
# Like PDFium, updating one is a deliberate change: bump its version and hash together.
# The hashes are the SHA-256 digests GitHub publishes for each release asset.
# Archives are untrusted until their hash matches, and only the files we build are extracted.

enable_language(C)

set(FW_MINIZ_VERSION "3.1.2")
set(_fw_miniz_sha256 "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a")
set(FW_PUGIXML_VERSION "1.16")
set(_fw_pugixml_sha256 "4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b")

# fw_fetch_source(<name> <version> <url> <sha256> <probe file> <files to extract...>)
# Sets FW_<NAME>_DIR to third_party/<name>/<version>.
function(fw_fetch_source name version url sha256 probe)
  set(dir "${CMAKE_SOURCE_DIR}/third_party/${name}/${version}")
  if(NOT EXISTS "${dir}/${probe}")
    get_filename_component(asset "${url}" NAME)
    set(archive "${CMAKE_SOURCE_DIR}/third_party/${name}/${asset}")
    message(STATUS "Downloading ${name} ${version}")
    file(DOWNLOAD "${url}" "${archive}"
      EXPECTED_HASH SHA256=${sha256}
      TLS_VERIFY ON
      STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "${name} download failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}" PATTERNS ${ARGN})
    file(REMOVE "${archive}")
    if(NOT EXISTS "${dir}/${probe}")
      message(FATAL_ERROR "${name} ${version}: ${probe} is missing from the archive")
    endif()
  endif()
  string(TOUPPER "${name}" upper)
  set(FW_${upper}_DIR "${dir}" PARENT_SCOPE)
endfunction()

fw_fetch_source(miniz ${FW_MINIZ_VERSION}
  "https://github.com/richgel999/miniz/releases/download/${FW_MINIZ_VERSION}/miniz-${FW_MINIZ_VERSION}.zip"
  ${_fw_miniz_sha256} miniz.c
  miniz.c miniz.h LICENSE)

fw_fetch_source(pugixml ${FW_PUGIXML_VERSION}
  "https://github.com/zeux/pugixml/releases/download/v${FW_PUGIXML_VERSION}/pugixml-${FW_PUGIXML_VERSION}.tar.gz"
  ${_fw_pugixml_sha256} pugixml-${FW_PUGIXML_VERSION}/src/pugixml.cpp
  pugixml-${FW_PUGIXML_VERSION}/src pugixml-${FW_PUGIXML_VERSION}/LICENSE.md)

# miniz: only raw deflate/inflate and CRC-32. fwpkg reads and writes the zip container itself,
# so its limits apply before anything is allocated.
add_library(miniz STATIC "${FW_MINIZ_DIR}/miniz.c")
target_include_directories(miniz SYSTEM PUBLIC "${FW_MINIZ_DIR}")
target_compile_definitions(miniz PUBLIC
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# pugixml: the XML DOM. No XPath: nothing needs it, and it is less code to fuzz.
set(_fw_pugixml_src "${FW_PUGIXML_DIR}/pugixml-${FW_PUGIXML_VERSION}/src")
add_library(pugixml STATIC "${_fw_pugixml_src}/pugixml.cpp")
target_include_directories(pugixml SYSTEM PUBLIC "${_fw_pugixml_src}")
target_compile_definitions(pugixml PUBLIC PUGIXML_NO_XPATH)
