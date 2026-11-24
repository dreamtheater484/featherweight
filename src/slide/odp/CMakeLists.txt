# OpenDocument presentations (P3, p3-files): converted to a .pptx package once, when one is opened,
# never per frame: compiled for size. Headless.
add_library(fwslideodp STATIC Odp.cpp OdpGeometry.cpp)
target_include_directories(fwslideodp PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslideodp PUBLIC fwpkg fwdraw fwcore)
target_compile_options(fwslideodp PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy /O1>)

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_odp.cpp")
  target_link_libraries(fw_tests PRIVATE fwslideodp)
endif()
