# The presentation tab's screen test (P1, p1-view), run by ctest as presentation_screens:
#   cmake -DEXE=<Featherweight.exe> -DDECKS=<tests/office/generated> -DOUT=<folder> -P presentation.cmake
# Off-screen snapshots (--snapshot) of the tab with find and the notes, the printed and exported
# pages as handouts and notes pages, Export PDF, and a .ppsx that asks for the show at once.
# Snapshots never touch the user's settings, clipboard or a running Featherweight.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(snapshot name script deck)
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/${name}.png" --theme light --size 1200x760 --script "${script}" "${deck}"
                  RESULT_VARIABLE code TIMEOUT 120)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: the snapshot run failed (${code})")
  endif()
  if(NOT EXISTS "${OUT}/${name}.png")
    message(FATAL_ERROR "${name}: no snapshot")
  endif()
  file(SIZE "${OUT}/${name}.png" size)
  if(size LESS 8000)
    message(FATAL_ERROR "${name}: the snapshot is nearly empty (${size} bytes)")
  endif()
endfunction()

function(expect_pages dir count)
  if(NOT EXISTS "${dir}/page-${count}.png")
    message(FATAL_ERROR "${dir}: page-${count}.png is missing")
  endif()
  math(EXPR over "${count} + 1")
  if(EXISTS "${dir}/page-${over}.png")
    message(FATAL_ERROR "${dir}: more than ${count} pages")
  endif()
endfunction()

# The tab: slide 2 of simple.pptx, a find with three matches, the notes below.
snapshot(tab "slide:2;cmd:find;type:point;wait:300" "${DECKS}/simple.pptx")

# Pages: 11 slides as handouts of 6 (2 pages) and of 9 (2), notes pages (11), and the slides as a PDF.
snapshot(pages "printlayout:handouts6;pagepngs:${OUT}/h6|48;printlayout:handouts9;pagepngs:${OUT}/h9|48;printlayout:notes;pagepngs:${OUT}/notes|48;printlayout:slides;exportpdf:${OUT}/slides.pdf;printlayout:handouts3;exportpdf:${OUT}/handouts3.pdf"
         "${DECKS}/layouts.pptx")
expect_pages("${OUT}/h6" 2)
expect_pages("${OUT}/h9" 2)
expect_pages("${OUT}/notes" 11)
foreach(pdf slides handouts3)
  if(NOT EXISTS "${OUT}/${pdf}.pdf")
    message(FATAL_ERROR "${pdf}.pdf wasn't exported")
  endif()
  file(READ "${OUT}/${pdf}.pdf" head LIMIT 4 HEX)
  if(NOT head STREQUAL "25504446")  # %PDF
    message(FATAL_ERROR "${pdf}.pdf isn't a PDF")
  endif()
endforeach()
# A slide show file opens in the tab and asks for the show after the first frame.
file(COPY_FILE "${DECKS}/simple.pptx" "${OUT}/show.ppsx")
snapshot(ppsx "wait:200" "${OUT}/show.ppsx")
message(STATUS "presentation screens: ${OUT}")
