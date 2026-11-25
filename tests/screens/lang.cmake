# The languages' clipping check (F1, f1-lang; docs/f1-plan.md "Languages"), run by ctest:
#   cmake -DEXE=<Featherweight.exe> -DFILES=<tests/office/generated> -DOUT=<folder> [-DLANG=qps] -P lang.cmake
#
# Each editor at 1366 x 768 in a language (the pseudo-language by default: every text 40 % longer and
# in brackets), with `clipcheck`: the window drawn with each ribbon tab and each File page section, and
# every label that doesn't fit its box reported. A run fails on any clipped label.
# Snapshots never touch the user's settings, clipboard or a running Featherweight.

if(NOT LANG)
  set(LANG qps)
endif()
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(failed "")

function(run name script)
  set(args ${ARGN})
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/${name}.png" --size 1366x768 --theme light --lang ${LANG}
                          --script "${script}" ${args}
                  RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 180)
  string(REPLACE ";" "," report "${err}")
  string(REGEX MATCHALL "clipcheck[^\n]*" lines "${report}")
  foreach(line ${lines})
    message(STATUS "${name}: ${line}")
  endforeach()
  if(NOT code EQUAL 0)
    set(failed "${failed} ${name}" PARENT_SCOPE)
  endif()
endfunction()

foreach(kind docx xlsx pptx)
  run(${kind}-${LANG} "wait:100;clipcheck" "${FILES}/simple.${kind}")
endforeach()
if(failed)
  message(FATAL_ERROR "clipped labels in${failed} (${OUT})")
endif()
message(STATUS "language screens: ${OUT}")
