# Downloads the pinned source releases fwpkg builds on (miniz, pugixml) and the hyphenation
# patterns fwtext ships, and verifies them.
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

# Hyphenation patterns for fwtext (plan §4.4, §4.9): hyph-utf8's plain-text patterns at the commit
# of its CTAN-2024.12.31 tag, and OpenTaal's Dutch patterns (version 2.20.21) from LibreOffice's
# dictionaries. Licences: THIRD_PARTY_NOTICES.md. src/text packs them, compressed, into the program.
set(FW_HYPH_UTF8_VERSION "2024.12.31")
set(_fw_hyph_utf8_commit "29e003ea905f6740a0d66e114db9f60b26d07516")
set(FW_OPENTAAL_HYPH_VERSION "2.20.21")
set(_fw_opentaal_commit "4f1df64b2e8e14751aa7c2205d170ca016071315")

# fw_fetch_file(<dir> <url> <sha256>): one file into dir, under its own name. A file already there
# is checked again, since nothing else would notice it changing.
function(fw_fetch_file dir url sha256)
  get_filename_component(name "${url}" NAME)
  set(path "${dir}/${name}")
  if(EXISTS "${path}")
    file(SHA256 "${path}" have)
    if(have STREQUAL sha256)
      return()
    endif()
    file(REMOVE "${path}")
  endif()
  file(DOWNLOAD "${url}" "${path}.part" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${path}.part")
    message(FATAL_ERROR "${name} download failed: ${status}")
  endif()
  file(RENAME "${path}.part" "${path}")
endfunction()

set(FW_HYPH_UTF8_DIR "${CMAKE_SOURCE_DIR}/third_party/hyphenation/hyph-utf8-${FW_HYPH_UTF8_VERSION}")
set(FW_OPENTAAL_HYPH_DIR "${CMAKE_SOURCE_DIR}/third_party/hyphenation/opentaal-${FW_OPENTAAL_HYPH_VERSION}")
set(_fw_hyph_utf8_url "https://raw.githubusercontent.com/hyphenation/tex-hyphen/${_fw_hyph_utf8_commit}/hyph-utf8/tex/generic/hyph-utf8/patterns/txt")
foreach(entry
    hyph-en-us.pat.txt=0f57318b878b132547ae92db39a6e1d1cf2a05d9008874955d6ecb910007a463
    hyph-en-us.hyp.txt=5e785c27e151f878c312991d937b5582615b77737ab30bac413d5967d6aa21da
    hyph-en-gb.pat.txt=651b89b4a0d4051995db8586e81ab109a4c7395d7706c06483893ebab5af013b
    hyph-en-gb.hyp.txt=947b7b8e76f6a0d8ebe296178ff92ad624e4912ffbaaf59b26f81a395124e366
    hyph-de-1996.pat.txt=3534fe7edb6cf20e085865b069f22013dc532e93959a49fc1f97567a8be6b3ef
    hyph-fr.pat.txt=fd40a89e9e8eb0784a0919e50378814843e44afa41c5beaf0a85f6562820f6c2
    hyph-es.pat.txt=7ae8fa8d6a61ea4111c4a0412d019b62e7c07a62673121d0b711c5bc08d8e00c
    hyph-it.pat.txt=ce650cffad4e81ecaf04591f3274a8ed909e82c587698c55e33b00e2aa7746de
    hyph-pt.pat.txt=6b50256e72d5448cec1801866e9027491f66552c8957f72b6e0ec315159e7931
    hyph-pt.hyp.txt=5c093a9e48dc8f87624693f31b8da7d090ed1e6eeb07fce7bcfd5689d7f941a8
    hyph-da.pat.txt=a093ef99375717fe34d261b05d2fdc8e64f8a9f741578c020204374f4c39a20c
    hyph-sv.pat.txt=1fbf98f5d316712fd4e86f564643ad7341184f9ce8b30e0e18d06eceb4f52a79
    hyph-nb.pat.txt=e080c77da3d0f8de7e464f69082f5e2485e43ed349a2a7018b84e0adef785806
    hyph-nb.hyp.txt=6484bbd793e20a206a2f0d7abfc499a5f8688c54ea7e91ee7af87fba53959d00
    hyph-fi.pat.txt=ccabb0b3f403eca9999c0cd73b75d16b0220f2052d8f6aad2f98b5f1854dc027)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 name)
  list(GET pair 1 sha256)
  fw_fetch_file("${FW_HYPH_UTF8_DIR}" "${_fw_hyph_utf8_url}/${name}" ${sha256})
endforeach()
fw_fetch_file("${FW_OPENTAAL_HYPH_DIR}"
  "https://raw.githubusercontent.com/LibreOffice/dictionaries/${_fw_opentaal_commit}/nl_NL/hyph_nl_NL.dic"
  0a97b76b259ed4165d094dedf9b6fb98c57734a437df0b5b263ee13e262d5f8f)

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
