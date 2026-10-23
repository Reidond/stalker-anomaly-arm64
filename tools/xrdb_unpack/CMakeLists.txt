# Standalone .db archive reader and extractor. Independent of xrCore.
add_library(xrdb STATIC xrdb.cpp lzhuf.cpp lzo1x.cpp)
target_include_directories(xrdb PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(xrdb_unpack main.cpp)
target_link_libraries(xrdb_unpack PRIVATE xrdb)

add_executable(ogf_scan ogf_scan.cpp)
target_link_libraries(ogf_scan PRIVATE xrdb)
