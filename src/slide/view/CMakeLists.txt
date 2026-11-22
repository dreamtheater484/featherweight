# The presentation tab (P1, p1-view; plan §5.4, §8). Two libraries:
#  - fwslideviewcore: headless (no window): the tab's geometry, the printed and exported pages
#    (slides, handouts, notes pages) drawn through draw::Painter, and the tab's Deck over p1-model's
#    presentation (fwslide). Tools and tests use it.
#  - fwslideview: the tab (a TabContent) on the app's shell; linked into the exe only.
add_library(fwslideviewcore STATIC SlideLayout.cpp SlidePages.cpp ModelDeck.cpp
  # P2, p2-slides: the thumbnails' selection and moves, design data, slide numbers, footer and date.
  SlideOrder.cpp SlideDesign.cpp SlideHeaderFooter.cpp NewDeck.cpp)
# p2-text: text editing on slides, headless (the working copy, caret and selection, formatting,
# clipboard, find, spelling, links).
target_sources(fwslideviewcore PRIVATE TextDoc.cpp TextSession.cpp TextFormat.cpp TextClip.cpp TextFind.cpp TextSpell.cpp TextModelEdit.cpp)
target_link_libraries(fwslideviewcore PUBLIC fwdoc fwproof)
# Formatting, the clipboard, find and spelling run once per command: compiled for size (typing's
# path, TextDoc and TextSession, stays compiled for speed).
set_source_files_properties(TextFormat.cpp TextClip.cpp TextFind.cpp TextSpell.cpp TextModelEdit.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
target_include_directories(fwslideviewcore PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslideviewcore PUBLIC fwslide_render fwslide fwpkg fwdraw fwtext fwcore)
target_compile_options(fwslideviewcore PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

add_library(fwslideview STATIC PresentationView.cpp PresentationCommands.cpp PresentationPrint.cpp
  ObjectTool.cpp PresentationObjects.cpp  # p2-objects
  PresentationSlides.cpp PresentationDesign.cpp SlideGallery.cpp NotesEdit.cpp  # P2, p2-slides
  PresentationFile.cpp PresentationSave.cpp  # p2-model: saving and recovery
  PresentationText.cpp)  # P2, p2-text
target_include_directories(fwslideview PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(fwslideview PUBLIC fwslideviewcore fwslideobjects)
target_compile_options(fwslideview PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
# Commands, menus and printing run once per click: compiled for size.
set_source_files_properties(PresentationCommands.cpp PresentationPrint.cpp PresentationSlides.cpp PresentationDesign.cpp SlideGallery.cpp NotesEdit.cpp
  PresentationText.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_view.cpp")
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_text.cpp")
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_slides.cpp")  # p2-slides
  # p2-model: the tab's file (an app source, tested with temporary folders; FileSession.cpp comes with src/doc's).
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_file.cpp" PresentationFile.cpp)
  target_link_libraries(fw_tests PRIVATE fwslideviewcore)
  # The tab off-screen: find, the notes, handouts and notes pages, Export PDF, a .ppsx.
  add_test(NAME presentation_screens COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:Featherweight>"
           "-DDECKS=${PROJECT_SOURCE_DIR}/tests/office/generated" "-DOUT=${CMAKE_BINARY_DIR}/screens/presentation"
           -P "${PROJECT_SOURCE_DIR}/tests/screens/presentation.cmake")
endif()
