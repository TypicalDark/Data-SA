cmake_minimum_required(VERSION 3.24)
project(grid_of_sa VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GSA_BUILD_BACKEND "Build the back-end service" ON)
option(GSA_BUILD_APP "Build the Vulkan desktop app (needs Vulkan SDK and GLFW)" ON)
option(GSA_BUILD_TESTS "Build tests" ON)
option(GSA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GsaDeps)
include(GsaWarnings)

if(GSA_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)
if(GSA_BUILD_BACKEND)
  add_subdirectory(backend)
endif()
if(GSA_BUILD_APP)
  add_subdirectory(app)
endif()
