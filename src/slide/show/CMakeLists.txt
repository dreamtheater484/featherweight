# The slide show (plan §8.2, §8.3; docs/p1-plan.md, p1-show). Two parts:
#  - fwslideshow: headless, the transitions and timing read from a slide's DOM and the animation
#    interpreter. Unit tests and fuzz_timing use it.
#  - the show's window (full screen, transitions on Direct2D, input), built into the exe.
add_library(fwslideshow STATIC Timing.cpp)
target_include_directories(fwslideshow PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslideshow PUBLIC fwpkg fwcore)
target_compile_options(fwslideshow PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slideshow.cpp")
  target_link_libraries(fw_tests PRIVATE fwslideshow)
endif()
