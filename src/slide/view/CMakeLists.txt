# The presentation tab (P1, p1-view; plan §5.4, §8). Two libraries:
#  - fwslideviewcore: headless (no window): the tab's geometry, the printed and exported pages
#    (slides, handouts, notes pages) drawn through draw::Painter, and the tab's Deck over p1-model's
#    presentation (fwslide). Tools and tests use it.
#  - fwslideview: the tab (a TabContent) on the app's shell; linked into the exe only.
add_library(fwslideviewcore STATIC SlideLayout.cpp SlidePages.cpp ModelDeck.cpp)
# p2-text: text editing on slides, headless (the working copy, caret and selection, formatting,
# clipboard, find, spelling, links).
target_sources(fwslideviewcore PRIVATE TextDoc.cpp TextSession.cpp TextFormat.cpp TextClip.cpp TextFind.cpp)
target_link_libraries(fwslideviewcore PUBLIC fwdoc)
target_include_directories(fwslideviewcore PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslideviewcore PUBLIC fwslide_render fwslide fwpkg fwdraw fwtext fwcore)
target_compile_options(fwslideviewcore PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

add_library(fwslideview STATIC PresentationView.cpp PresentationCommands.cpp PresentationPrint.cpp)
target_include_directories(fwslideview PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(fwslideview PUBLIC fwslideviewcore)
target_compile_options(fwslideview PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
# Commands, menus and printing run once per click: compiled for size.
set_source_files_properties(PresentationCommands.cpp PresentationPrint.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_view.cpp")
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_text.cpp")
  target_link_libraries(fw_tests PRIVATE fwslideviewcore)
  # The tab off-screen: find, the notes, handouts and notes pages, Export PDF, a .ppsx.
  add_test(NAME presentation_screens COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:Featherweight>"
           "-DDECKS=${PROJECT_SOURCE_DIR}/tests/office/generated" "-DOUT=${CMAKE_BINARY_DIR}/screens/presentation"
           -P "${PROJECT_SOURCE_DIR}/tests/screens/presentation.cmake")
endif()
