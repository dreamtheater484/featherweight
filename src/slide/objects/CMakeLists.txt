# Objects on slides (P2, p2-objects; plan §8.2 "Objects"): headless. Frames and handles, smart
# guides, align/distribute/order, selection over a slide's object tree, and the PresentationML of
# objects (transforms, fill, outline, shadow, crop, new objects, groups, table rows and columns,
# chart parts with their embedded workbook). The tab (src/slide/view) drives them; the edit layer
# (src/slide/edit) records their changes.
add_library(fwslideobjects STATIC Frame.cpp Guides.cpp Arrange.cpp Selection.cpp ObjectXml.cpp ChartPart.cpp)
target_include_directories(fwslideobjects PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwslideobjects PUBLIC fwslide fwpkg fwdraw fwcore)
target_compile_options(fwslideobjects PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
# Run once per edit or click, never per frame, apart from Frame and Guides: compiled for size.
set_source_files_properties(ObjectXml.cpp ChartPart.cpp Selection.cpp Arrange.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CONFIG:Release,MinSizeRel>:/O1>")

if(TARGET fw_tests)
  target_sources(fw_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_objects.cpp"
                                  "${PROJECT_SOURCE_DIR}/tests/unit/test_slide_objects_xml.cpp")
  target_link_libraries(fw_tests PRIVATE fwslideobjects)
endif()
