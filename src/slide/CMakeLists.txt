# fwslide (plan §8, P1): the presentation model and its .pptx reader: slides, layouts, masters,
# themes, notes, with inheritance resolved and cached. Headless: no window, no PDFium. Rendering
# (src/slide/render), the tab (src/slide/view) and the slide show (src/slide/show) build on it.
add_library(fwslide STATIC
  Reader.cpp
  Shapes.cpp
  Tables.cpp
  TextStyles.cpp
)
target_include_directories(fwslide PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslide PUBLIC fwpkg fwcore fwdraw)
target_compile_options(fwslide PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide.cpp")
  target_link_libraries(fw_tests PRIVATE fwslide)
endif()
