# The desktop app: GLFW window, Vulkan renderer, Dear ImGui interface.
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799)
FetchContent_MakeAvailable(imgui stb)

add_library(gsa_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(gsa_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(gsa_imgui PUBLIC glfw Vulkan::Vulkan)

add_library(gsa_stb INTERFACE)
target_include_directories(gsa_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# GLSL -> SPIR-V headers (uint32_t arrays named map_vert, map_frag, text_vert, text_frag)
set(GSA_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/gen/shaders)
file(MAKE_DIRECTORY ${GSA_SHADER_OUT})
set(GSA_SHADER_HEADERS)
foreach(shader map text raster)
  foreach(stage vert frag)
    set(out ${GSA_SHADER_OUT}/${shader}_${stage}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2 --vn ${shader}_${stage} -o ${out}
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.${stage}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.${stage}
      COMMENT "Compiling ${shader}.${stage}")
    list(APPEND GSA_SHADER_HEADERS ${out})
  endforeach()
endforeach()
add_custom_target(gsa_shaders DEPENDS ${GSA_SHADER_HEADERS})

# Everything but main and the window, so tests can link it without a GPU.
add_library(gsa_app_core STATIC src/mvt.cpp src/gsb.cpp src/mesh.cpp src/style.cpp src/api_client.cpp src/tiles.cpp src/backend_process.cpp src/layers.cpp src/commands.cpp src/settings.cpp src/find.cpp src/measure.cpp
  src/text/atlas.cpp src/text/labels.cpp src/map_labels.cpp)
target_include_directories(gsa_app_core PUBLIC src)
target_link_libraries(gsa_app_core PUBLIC gsa_common nlohmann_json::nlohmann_json httplib::httplib protozero::protozero
                      earcut_hpp::earcut_hpp gsa_stb Threads::Threads)
if(WIN32)
  target_link_libraries(gsa_app_core PUBLIC ws2_32)
endif()
gsa_warnings(gsa_app_core)

add_executable(gsa WIN32 src/main.cpp src/app.cpp src/app_ui.cpp src/app_cards.cpp
 src/app_extra.cpp src/icons.cpp src/vk_context.cpp src/map_renderer.cpp src/text_renderer.cpp)
if(NOT WIN32)
  set_target_properties(gsa PROPERTIES WIN32_EXECUTABLE OFF)
endif()
add_dependencies(gsa gsa_shaders)
target_include_directories(gsa PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(gsa PRIVATE gsa_app_core gsa_imgui gsa_stb glfw Vulkan::Vulkan)
target_compile_definitions(gsa PRIVATE GSA_DEV_DATA="${PROJECT_SOURCE_DIR}/devdata" GLFW_INCLUDE_NONE)
if(MSVC)
  # keep a console-less GUI app but use the normal main()
  target_link_options(gsa PRIVATE /ENTRY:mainCRTStartup)
endif()
gsa_warnings(gsa)

# fonts next to the binary
add_custom_command(TARGET gsa POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:gsa>/assets)
if(TARGET gsa-backend)
  add_dependencies(gsa gsa-backend)
  add_custom_command(TARGET gsa POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gsa-backend> $<TARGET_FILE_DIR:gsa>)
endif()
install(TARGETS gsa RUNTIME DESTINATION bin)
install(DIRECTORY assets DESTINATION bin)

if(GSA_BUILD_TESTS)
  add_executable(gsa_app_tests tests/main.cpp tests/test_mvt_mesh.cpp tests/test_geo.cpp tests/test_text.cpp tests/test_layers.cpp tests/test_find.cpp tests/test_measure.cpp tests/test_gsb.cpp)
  target_link_libraries(gsa_app_tests PRIVATE gsa_app_core doctest::doctest)
  target_compile_definitions(gsa_app_tests PRIVATE GSA_DEVDATA="${PROJECT_SOURCE_DIR}/devdata"
                             GSA_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
  if(TARGET gsa-backend)
    add_dependencies(gsa_app_tests gsa-backend)
    target_compile_definitions(gsa_app_tests PRIVATE GSA_BACKEND_EXE="$<TARGET_FILE:gsa-backend>")
  endif()
  gsa_warnings(gsa_app_tests)
  add_test(NAME app COMMAND gsa_app_tests)
endif()
