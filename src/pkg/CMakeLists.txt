# fwpkg: the package layer the office engines share (plan §4.1). Headless: no UI, no PDFium.
add_library(fwpkg STATIC
  Canonical.cpp
  Check.cpp
  CompoundFile.cpp
  Derived.cpp
  Formula.cpp
  Limits.cpp
  Mce.cpp
  Package.cpp
  ReferenceRules.cpp
  References.cpp
  RoundTrip.cpp
  Backups.cpp
  OwnerFile.cpp
  Recovery.cpp
  SafeFile.cpp
  Snapshot.cpp
  XmlDom.cpp
  XmlPull.cpp
  XmlWriter.cpp
  Zip.cpp
)
target_include_directories(fwpkg PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(fwpkg PUBLIC fwcore pugixml PRIVATE miniz bcrypt crypt32)
target_compile_options(fwpkg PRIVATE ${FW_COMPILE_OPTIONS} $<$<CONFIG:Release,MinSizeRel>:/GL /Gy>)
