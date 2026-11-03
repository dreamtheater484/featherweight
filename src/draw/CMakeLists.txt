# fwdraw: the Painter interface, its Direct2D backend and vector printing (plan §4.1, §4.5, §4.6).
# Headless: no UI and no window.
add_library(fwdraw STATIC
  D2DDevice.cpp
  D2DPainter.cpp
  Painter.cpp
  PrintJob.cpp
)
target_include_directories(fwdraw PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwdraw PUBLIC fwcore d2d1 d3d11 dxgi dwrite PRIVATE windowscodecs prntvpt ole32)
target_compile_options(fwdraw PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
