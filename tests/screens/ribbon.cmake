# The ribbon's screen and coverage tests (R1, r1-control), run by ctest:
#   cmake -DEXE=<Featherweight.exe> -DFILES=<tests/office/generated> -DBASELINES=<tests/ribbon>
#         -DOUT=<folder> -DMODE=<screens|word|sheet|slide> -P ribbon.cmake
#
# screens: off-screen snapshots of the widget gallery's demo ribbon and of the three editors at
#   1366, 1024 and 800 px, light and dark, with `ribboncheck` (every item runs a named command,
#   every group fits the window, KeyTips are unique) on the gallery; KeyTips, the File page, a
#   collapsed ribbon, a gallery's grid and a contextual tab; and `ribbonbench` (switching tabs and
#   laying out again within 16 ms).
# word, sheet, slide: an editor's coverage check, `ribboncheck:<baseline>`: the same checks on its
#   ribbon, and every command its toolbar and ⋯ menu reached before R1 (tests/ribbon/*.txt, made
#   with `dumpreach`) reachable from the ribbon, the File page, a right-click menu or Alt+Q. The
#   module sessions add their states (a table, a picture, a chart selected) as more runs below.
# Snapshots never touch the user's settings, clipboard or a running Featherweight.

file(MAKE_DIRECTORY "${OUT}")

function(run name size theme script)
  set(args ${ARGN})
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/${name}.png" --size ${size} --theme ${theme} --script "${script}" ${args}
                  RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 120)
  string(REPLACE ";" "," report "${err}")  # a CMake list would split the lines at their semicolons
  string(REGEX MATCHALL "(ribboncheck|ribbonbench)[^\n]*" lines "${report}")
  foreach(line ${lines})
    message(STATUS "${name}: ${line}")
  endforeach()
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: the run failed (${code})\n${err}")
  endif()
  if(NOT EXISTS "${OUT}/${name}.png")
    message(FATAL_ERROR "${name}: no snapshot")
  endif()
endfunction()

set(sizes 1366x768 1024x700 800x600)

if(MODE STREQUAL "screens")
  file(REMOVE_RECURSE "${OUT}")
  file(MAKE_DIRECTORY "${OUT}")
  foreach(theme light dark)
    foreach(size ${sizes})
      run(gallery-${size}-${theme} ${size} ${theme} "galleryribbon:on;ribboncheck" --widget-gallery)
      foreach(kind docx xlsx pptx)
        run(${kind}-${size}-${theme} ${size} ${theme} "wait:100" "${FILES}/simple.${kind}")
      endforeach()
    endforeach()
  endforeach()
  run(gallery-keytips 1366x768 light "galleryribbon:on;keytips" --widget-gallery)
  run(gallery-keytips-home 1366x768 dark "galleryribbon:on;keytips:H" --widget-gallery)
  run(gallery-file-options 1366x768 light "galleryribbon:on;filepage:options" --widget-gallery)
  run(gallery-file-new 1024x700 dark "galleryribbon:on;filepage:new" --widget-gallery)
  run(gallery-collapsed 1366x768 light "galleryribbon:on;ribbon:collapse;ribbon:open" --widget-gallery)
  run(gallery-grid 1366x768 light "galleryribbon:on;keytips:HL" --widget-gallery)
  run(gallery-contextual 1024x700 dark "galleryribbon:table;ribbon:Table Design;ribboncheck" --widget-gallery)
  run(gallery-folded 560x600 light "galleryribbon:on;ribboncheck" --widget-gallery)
  run(gallery-bench 1366x768 light "galleryribbon:table;ribbonbench" --widget-gallery)
  run(gallery-icons-light 1366x900 light "galleryicons:on" --widget-gallery)
  run(gallery-icons-dark 1366x900 dark "galleryicons:on" --widget-gallery)
  message(STATUS "ribbon screens: ${OUT}")
elseif(MODE STREQUAL "word")
  # r1-word: the coverage check as the document opens, and with every contextual tab built
  # (docribbon:all: Table Design and Layout, Picture and Shape Format, Header & Footer); Home and
  # Insert in both themes; each contextual tab once.
  file(REMOVE_RECURSE "${OUT}")
  file(MAKE_DIRECTORY "${OUT}")
  foreach(size ${sizes})
    run(word-${size} ${size} light "ribboncheck:${BASELINES}/word.txt" "${FILES}/simple.docx")
    run(word-all-${size} ${size} light "docribbon:all;ribboncheck:${BASELINES}/word.txt" "${FILES}/simple.docx")
    foreach(theme light dark)
      run(word-home-${size}-${theme} ${size} ${theme} "wait:100" "${FILES}/simple.docx")
      run(word-insert-${size}-${theme} ${size} ${theme} "ribbon:Insert" "${FILES}/simple.docx")
    endforeach()
  endforeach()
  foreach(tab "Table Design" "Table Layout" "Picture Format" "Shape Format" "Header & Footer")
    string(REGEX REPLACE "[ &]+" "-" name "${tab}")
    run(word-${name} 1366x768 light "docribbon:all;ribbon:${tab}" "${FILES}/simple.docx")
  endforeach()
  # The ribbon's own boxes edit the document: Layout › Paragraph's left indent and spacing after,
  # set through their items, then read back from the paragraph.
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/word-boxes.png" --size 1366x768 --theme light
                          --script "ribbon:Layout;docribbon:set=3650,2.5;docribbon:set=3653,12;docribbon:print=3650;docribbon:print=3653"
                          "${FILES}/simple.docx"
                  RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 120)
  if(NOT code EQUAL 0 OR NOT err MATCHES "docribbon: 3650 = 2\\.(49|50)" OR NOT err MATCHES "docribbon: 3653 = 12\\.0")
    message(FATAL_ERROR "Layout › Paragraph's boxes didn't change the paragraph (${code}):\n${err}")
  endif()
elseif(MODE STREQUAL "sheet")
  # r1-sheet: Excel's ribbon on a workbook. The coverage check at each width; then the contextual tabs
  # with what they work on selected (a table, a chart, a shape, a picture, print preview); then the
  # Home and Formulas tabs at each width, light and dark.
  file(REMOVE_RECURSE "${OUT}")
  file(MAKE_DIRECTORY "${OUT}")
  foreach(size ${sizes})
    run(sheet-${size} ${size} light "ribboncheck:${BASELINES}/sheet.txt" "${FILES}/simple.xlsx")
  endforeach()
  run(sheet-table 1024x700 light "table:create;ribboncheck" "${FILES}/simple.xlsx")
  run(sheet-chart 1024x700 dark "cmd:2801;ribboncheck" "${FILES}/simple.xlsx")
  run(sheet-shape 1024x700 light "object:rect;ribboncheck" "${FILES}/simple.xlsx")
  run(sheet-picture 1024x700 light "object:picture,${FILES}/../fixtures/picture.png;ribboncheck" "${FILES}/simple.xlsx")
  run(sheet-preview 800x600 dark "cmd:1500;ribboncheck" "${FILES}/simple.xlsx")
  foreach(theme light dark)
    foreach(size ${sizes})
      foreach(tab Home Formulas)
        run(sheet-${tab}-${size}-${theme} ${size} ${theme} "ribbon:${tab}" "${FILES}/simple.xlsx")
      endforeach()
    endforeach()
  endforeach()
  message(STATUS "workbook ribbon screens: ${OUT}")
elseif(MODE STREQUAL "slide")
  foreach(size ${sizes})
    run(slide-${size} ${size} light "ribboncheck:${BASELINES}/slide.txt" "${FILES}/simple.pptx")
  endforeach()
  # r1-slide: Home in dark and Insert in both themes at each width; the contextual tabs with what
  # selects them (a shape, a picture, a table cell, a title being typed in), each checked.
  foreach(size ${sizes})
    run(slide-home-${size}-dark ${size} dark "ribbon:Home" "${FILES}/simple.pptx")
    foreach(theme light dark)
      run(slide-insert-${size}-${theme} ${size} ${theme} "ribbon:Insert;ribboncheck" "${FILES}/simple.pptx")
    endforeach()
  endforeach()
  run(slide-shape 1366x768 light "insert:shape|rect|100,100,200,120;ribboncheck" "${FILES}/simple.pptx")
  run(slide-picture 1024x700 dark "insert:picture|${FILES}/../fixtures/picture.png;ribboncheck" "${FILES}/simple.pptx")
  run(slide-table 1366x768 light "insert:table|3x3;ribbon:Table Layout;ribboncheck" "${FILES}/simple.pptx")
  run(slide-text 800x600 light "edit:title;ribboncheck" "${FILES}/simple.pptx")
  run(slide-design 1366x768 dark "ribbon:Design;ribboncheck" "${FILES}/layouts.pptx")
else()
  message(FATAL_ERROR "MODE: screens, word, sheet or slide")
endif()
