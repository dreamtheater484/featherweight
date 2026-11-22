# fwslide (plan §8, P1): the presentation model and its .pptx reader: slides, layouts, masters,
# themes, notes, with inheritance resolved and cached. Headless: no window, no PDFium. Rendering
# (src/slide/render), the tab (src/slide/view) and the slide show (src/slide/show) build on it.
add_library(fwslide STATIC
  Reader.cpp
  Shapes.cpp
  Tables.cpp
  TextStyles.cpp
  # P2 (p2-model): the edit layer, the .pptx writer and crash recovery.
  edit/DesignOps.cpp
  edit/Edit.cpp
  edit/Journal.cpp
  edit/Parts.cpp
  edit/Save.cpp
  edit/ShapeOps.cpp
  edit/SlideCopy.cpp
  edit/SlideOps.cpp
  edit/TextOps.cpp
)
target_include_directories(fwslide PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslide PUBLIC fwpkg fwcore fwdraw)
target_compile_options(fwslide PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide.cpp" "${PROJECT_SOURCE_DIR}/tests/unit/test_eot.cpp")
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_edit.cpp")  # p2-model
  # t2embed makes the Embedded OpenType the decoder is checked against (test only).
  target_link_libraries(fw_tests PRIVATE fwslide t2embed)
endif()
