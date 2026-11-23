# The document's ribbon (R1, r1-word), run by ctest as document_ribbon:
#   cmake -DEXE=<Featherweight.exe> -DDOCS=<tests/office/generated> -DOUT=<folder> -P document-ribbon.cmake
# ribbon:check builds every tab, the contextual ones too, and fails when an item runs a command the
# document doesn't know, or when a command the old toolbar and ⋯ menu reached can't be reached from
# the ribbon, the File page, the right-click menu or command search.
# Snapshots never touch the user's settings, clipboard or a running Featherweight.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

execute_process(COMMAND "${EXE}" --snapshot "${OUT}/check.png" --theme light --size 1366x768 --script "ribbon:check"
                        "${DOCS}/simple.docx"
                RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 120)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "the ribbon check run failed (${code})\n${err}")
endif()
string(REGEX MATCHALL "ribbon-check: [^\n]*" lines "${err}")
string(REPLACE ";" "\n" report "${lines}")
if(NOT err MATCHES "ribbon-check: ok:")
  message(FATAL_ERROR "the document's ribbon doesn't cover its commands:\n${report}")
endif()
message(STATUS "${report}")

# The ribbon's own boxes edit the document: Layout › Paragraph's left indent and spacing after, set
# through their items (ribbon:set), then read back from the selection.
execute_process(COMMAND "${EXE}" --snapshot "${OUT}/boxes.png" --theme light --size 1366x768
                        --script "ribbon:set=3650,2.5;ribbon:set=3653,12;ribbon:dump" "${DOCS}/simple.docx"
                RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 120)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "the ribbon boxes run failed (${code})\n${err}")
endif()
if(NOT err MATCHES "item 3650 [^\n]* = 2\\.(49|50)" OR NOT err MATCHES "item 3653 [^\n]* = 12\\.0")
  string(REGEX MATCHALL "ribbon: [^\n]*item 365[03][^\n]*" lines "${err}")
  message(FATAL_ERROR "the paragraph boxes didn't change the paragraph: ${lines}")
endif()
