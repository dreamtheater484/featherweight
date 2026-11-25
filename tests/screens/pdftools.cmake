# The PDF tab's tools (F1, f1-pdf), run by ctest as pdf_tools:
#   cmake -DEXE=<Featherweight.exe> -DPDFS=<tests/pdfs> -DOUT=<folder> -P pdftools.cmake
# Off-screen snapshots (--snapshot) with the tools driven by --script pdftool:… (their dialogs
# left out): page tools with Undo and Redo, insert (merge), extract and split; an unedited Save as
# that must be byte for byte the file; Recognize text on a scan; a certificate signature with the
# test PFX (loaded in memory, never into a certificate store) that the tab then finds intact; a
# drawn signature. Every PDF written must start as a PDF and end with %%EOF. Snapshots never
# touch the user's settings, clipboard or a running Featherweight.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(snapshot name script file)
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/${name}.png" --theme light --size 1200x760 --script "${script}" "${file}"
                  RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 180)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: the snapshot run failed (${code}): ${err}")
  endif()
  if(NOT EXISTS "${OUT}/${name}.png")
    message(FATAL_ERROR "${name}: no snapshot")
  endif()
endfunction()

function(expect_pdf path)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${path} wasn't written")
  endif()
  file(READ "${path}" head LIMIT 5 HEX)
  file(SIZE "${path}" size)
  math(EXPR tailAt "${size} - 8")
  file(READ "${path}" tail OFFSET ${tailAt} HEX)
  if(NOT head STREQUAL "255044462d" OR NOT tail MATCHES "2525454f46")  # %PDF- and %%EOF
    message(FATAL_ERROR "${path} isn't a whole PDF")
  endif()
endfunction()

# Page tools: rotate two pages, move the last to the front, delete one, merge a 12-page PDF at the
# end, Undo and Redo the merge, then Save as; extract two pages and split every 4 pages.
snapshot(pages "pdftool:select:2-3;pdftool:rotate:1;pdftool:select:5;pdftool:move:1;pdftool:select:2;pdftool:delete;pdftool:insert:${PDFS}/text.pdf|0;pdftool:undo;pdftool:redo;saveas:${OUT}/merged.pdf;pdftool:select:1-2;pdftool:extract:${OUT}/two.pdf;pdftool:split:4|${OUT}/part.pdf;wait:300"
         "${PDFS}/mixed-sizes.pdf")
expect_pdf("${OUT}/merged.pdf")
expect_pdf("${OUT}/two.pdf")
foreach(part "1-4" "5-8" "9-12" "13-16")
  expect_pdf("${OUT}/part (pages ${part}).pdf")
endforeach()
if(EXISTS "${OUT}/part (pages 17-20).pdf" OR EXISTS "${OUT}/part (page 17).pdf")
  message(FATAL_ERROR "the split made more parts than 16 pages have")
endif()

# Saving a PDF without edits: the file as it was, byte for byte.
snapshot(unedited "saveas:${OUT}/unedited.pdf;wait:100" "${PDFS}/text.pdf")
file(SHA256 "${PDFS}/text.pdf" before)
file(SHA256 "${OUT}/unedited.pdf" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "an unedited Save as changed the PDF")
endif()

# Recognize text on the two scanned pages (one stored sideways), then find a word.
snapshot(ocr "pdftool:ocr;saveas:${OUT}/ocr.pdf;cmd:find;type:fox;wait:400" "${PDFS}/scanned.pdf")
expect_pdf("${OUT}/ocr.pdf")

# A certificate signature, then the signed PDF opened again: checksig fails the run unless the
# tab finds the signature.
snapshot(sign "pdftool:sign:${PDFS}/signing/test-signer.pfx|featherweight|${OUT}/signed.pdf;wait:200" "${PDFS}/text.pdf")
expect_pdf("${OUT}/signed.pdf")
snapshot(signed "pdftool:checksig;wait:200" "${OUT}/signed.pdf")

# A drawn signature placed on the page, saved.
snapshot(stamp "pdftool:stamp;saveas:${OUT}/stamped.pdf;wait:200" "${PDFS}/text.pdf")
expect_pdf("${OUT}/stamped.pdf")
message(STATUS "pdf tools: ${OUT}")
