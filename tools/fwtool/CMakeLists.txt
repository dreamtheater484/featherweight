# fwtool: the headless command line for the never-damage tests (plan §10). Not shipped.
add_executable(fwtool main.cpp Roundtrip.cpp Inspect.cpp Print.cpp Lines.cpp)
target_link_libraries(fwtool PRIVATE fwpkg fwdraw fwtext ole32)
target_compile_options(fwtool PRIVATE ${FW_COMPILE_OPTIONS})
