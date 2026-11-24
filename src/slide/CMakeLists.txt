# fwslide (plan §8, P1): the presentation model and its .pptx reader: slides, layouts, masters,
# themes, notes, with inheritance resolved and cached. Headless: no window, no PDFium. Rendering
# (src/slide/render), the tab (src/slide/view) and the slide show (src/slide/show) build on it.
add_library(fwslide STATIC
  Reader.cpp
  Shapes.cpp
  Tables.cpp
  TextStyles.cpp
  # P3 (p3-ppt): PowerPoint 97-2003 presentations, converted to .pptx.
  ppt/Ppt.cpp ppt/Shapes.cpp ppt/Text.cpp
  # P2 (p2-model): the edit layer, the .pptx writer and crash recovery.
  edit/DesignOps.cpp edit/TableGrid.cpp edit/TableOps.cpp edit/Transform.cpp
  edit/HeaderFooter.cpp  # p2-slides
  edit/SlideDesignOps.cpp  # p2-slides
  edit/Edit.cpp
  edit/Journal.cpp
  edit/Parts.cpp
  edit/Save.cpp
  edit/ShapeClip.cpp
  edit/ShapeOps.cpp
  edit/SlideCopy.cpp
  edit/SlideOps.cpp
  edit/TextOps.cpp
  edit/CommentOps.cpp  # p3-review
  edit/TimingOps.cpp  # p3-anim
)
target_include_directories(fwslide PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslide PUBLIC fwpkg fwcore fwdraw)
target_compile_options(fwslide PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
# p2-slides' design operations run once per command: compiled for size.
set_source_files_properties(edit/HeaderFooter.cpp edit/SlideDesignOps.cpp edit/CommentOps.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
# p3-ppt: a .ppt is converted once, when it opens: compiled for size.
set_source_files_properties(ppt/Ppt.cpp ppt/Shapes.cpp ppt/Text.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")
# p2-model: the edit layer runs once per edit, undo or save, never per frame: compiled for size.
set_source_files_properties(edit/DesignOps.cpp edit/TableGrid.cpp edit/TableOps.cpp edit/Transform.cpp edit/Edit.cpp edit/Journal.cpp
  edit/Parts.cpp edit/Save.cpp edit/ShapeClip.cpp edit/ShapeOps.cpp edit/SlideCopy.cpp edit/SlideOps.cpp edit/TextOps.cpp
  edit/TimingOps.cpp
  PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_eot.cpp")
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_edit.cpp")  # p2-model
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_ppt.cpp")  # p3-ppt
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_review.cpp")  # p3-review
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_anim.cpp")  # p3-anim
  # t2embed makes the Embedded OpenType the decoder is checked against (test only).
  target_link_libraries(fw_tests PRIVATE fwslide t2embed)
endif()
