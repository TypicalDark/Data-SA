add_library(gsa_backend_core STATIC
  src/api.cpp
  src/api_data.cpp
  src/graph.cpp
  src/packs.cpp
  src/pmtiles.cpp
  src/sqlite.cpp)
target_include_directories(gsa_backend_core PUBLIC include)
target_compile_definitions(gsa_backend_core PUBLIC GSA_VERSION="${PROJECT_VERSION}")
target_link_libraries(gsa_backend_core
  PUBLIC gsa_common nlohmann_json::nlohmann_json httplib::httplib Threads::Threads
  PRIVATE SQLite::SQLite3)
if(WIN32)
  target_link_libraries(gsa_backend_core PUBLIC ws2_32)
endif()
gsa_warnings(gsa_backend_core)

add_executable(gsa-backend src/main.cpp)
target_link_libraries(gsa-backend PRIVATE gsa_backend_core)
gsa_warnings(gsa-backend)
install(TARGETS gsa-backend RUNTIME DESTINATION bin)

if(GSA_BUILD_TESTS)
  add_executable(gsa_backend_tests tests/test_pmtiles.cpp tests/test_api.cpp tests/test_data_api.cpp tests/main.cpp)
  target_link_libraries(gsa_backend_tests PRIVATE gsa_backend_core doctest::doctest)
  target_compile_definitions(gsa_backend_tests PRIVATE GSA_DEVDATA="${PROJECT_SOURCE_DIR}/devdata")
  gsa_warnings(gsa_backend_tests)
  add_test(NAME backend COMMAND gsa_backend_tests)
endif()
