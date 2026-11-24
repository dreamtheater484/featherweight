# The start page's screen test (P3, p3-start), run by ctest:
#   cmake -DEXE=<Featherweight.exe> -DOUT=<folder> -P start.cmake
#
# Off-screen snapshots of the start page at 1366, 1920, 2560 and 5120 px wide, light and dark, with
# recent files of every kind (one pinned) and two templates of the user's; then its states: the
# keyboard on a card and on the list, the filter, the recent files hidden, no recent files, a narrow
# window and 200 %. Snapshots never touch the user's settings, clipboard or a running Featherweight.

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

function(run name size theme script)
  execute_process(COMMAND "${EXE}" --snapshot "${OUT}/${name}.png" --size ${size} --theme ${theme} --script "${script}" ${ARGN}
                  RESULT_VARIABLE code ERROR_VARIABLE err TIMEOUT 120)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: the run failed (${code})\n${err}")
  endif()
  if(NOT EXISTS "${OUT}/${name}.png")
    message(FATAL_ERROR "${name}: no snapshot")
  endif()
endfunction()

# Folders in the profile show without it, so the pictures look the same for everyone.
string(REPLACE "/" "\\" home "$ENV{USERPROFILE}")
set(recent "recent:${home}\\Downloads\\Lease agreement 2026.pdf|604800")
string(APPEND recent ";recent:${home}\\Documents\\Finance\\Budget 2026.xlsx|259200")
string(APPEND recent ";recent:${home}\\Documents\\Work\\Quarterly report Q3.docx|90000")
string(APPEND recent ";recent:D:\\Projects\\2026\\Clients\\Acme Corporation\\Decks\\Kickoff meeting.pptx|7200")
string(APPEND recent ";recent:${home}\\Documents\\Letters\\Letter to the landlord.docx|5400")
string(APPEND recent ";recent:${home}\\Documents\\Finance\\Expenses October.xlsx|3600")
string(APPEND recent ";recent:${home}\\Desktop\\Team offsite plan.pptx|1500")
string(APPEND recent ";recent:${home}\\Downloads\\Invoice 10482.pdf|600")
string(APPEND recent ";recent:${home}\\Documents\\Notes\\Meeting notes.docx|20")
string(APPEND recent ";pin:Budget 2026.xlsx")
string(APPEND recent ";usertemplates:${home}\\Documents\\Custom Office Templates\\Monthly budget.xltx,${home}\\Documents\\Custom Office Templates\\Company deck.potx")

foreach(theme light dark)
  foreach(size 1366x768 1920x1080 2560x1440 5120x1440)
    run(start-${size}-${theme} ${size} ${theme} "${recent}")
  endforeach()
endforeach()
run(start-keys-card 1366x768 light "${recent};key:tab;key:tab;key:right")
# Tab: Open…, three cards, six templates, the filter, the eye, the list.
run(start-keys-list 1920x1080 dark "${recent};key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:tab;key:down")
run(start-filter 1920x1080 light "${recent};startfilter:report")
run(start-filter-none 1366x768 dark "${recent};startfilter:zebra")
run(start-hidden 2560x1440 dark "${recent};cmd:3960")
run(start-empty 1920x1080 light "wait:1")
run(start-narrow 640x720 light "${recent}")
run(start-200 2732x1536 dark "${recent}" --scale 2)
message(STATUS "start page screens: ${OUT}")
