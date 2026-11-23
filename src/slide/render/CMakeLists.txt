# fwslide_render (P1): slides drawn as PowerPoint draws them, through draw::Painter, so the slide
# view, the slide show, printing and Export PDF show the same thing (plan §8.3). Shapes, pictures
# and charts through fwdraw and fwchart, text through fwtext. Headless: no UI and no window.
add_library(fwslide_render STATIC
  Slide.cpp
  Table.cpp
  TextBody.cpp
  Thumbnails.cpp
)
target_include_directories(fwslide_render PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslide_render PUBLIC fwslide fwdraw fwtext PRIVATE fwpkg fwdoclayout windowscodecs ole32)
target_compile_options(fwslide_render PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_render.cpp")
  target_link_libraries(fw_tests PRIVATE fwslide_render fwchart)
endif()
