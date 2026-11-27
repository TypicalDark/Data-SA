# Code shared by the back-end and the app. Nothing here knows about HTTP or Vulkan.
add_library(gsa_common STATIC src/gzip.cpp)
target_include_directories(gsa_common PUBLIC include)
target_link_libraries(gsa_common PRIVATE ZLIB::ZLIB)
gsa_warnings(gsa_common)
