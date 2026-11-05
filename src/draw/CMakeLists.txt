# fwdraw: the Painter interface, its Direct2D backend and vector printing (plan §4.1, §4.5, §4.6),
# and the drawing formats the three apps share: DrawingML and a VML subset, themes, pictures.
# Headless: no UI and no window.
add_library(fwdraw STATIC
  D2DDevice.cpp
  D2DPainter.cpp
  Drawing.cpp
  DrawingML.cpp
  Geometry.cpp
  Images.cpp
  Painter.cpp
  PrintJob.cpp
  Shapes.cpp
  Theme.cpp
  Vml.cpp
)
target_include_directories(fwdraw PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwdraw PUBLIC fwcore fwpkg d2d1 d3d11 dxgi dwrite
  PRIVATE windowscodecs prntvpt ole32 shlwapi gdi32 dxguid)
target_compile_options(fwdraw PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)

# The tests live with the others in tests/; they're added here so the root CMakeLists.txt stays as
# it is while the D1 engines are built side by side.
if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_drawing.cpp")
  target_link_libraries(fw_tests PRIVATE gdi32)
endif()
