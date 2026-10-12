cmake_minimum_required(VERSION 3.24)
project(ApexTelemetry VERSION 1.3.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(APEX_BUILD_HUD "Build the OpenGL dashboard" ON)
include(CTest)

function(apex_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    # A portable Windows executable, with no VC runtime DLL installation required.
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(WIN32)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(${target} PRIVATE ws2_32)
  endif()
endfunction()

if(APEX_BUILD_HUD)
  include(FetchContent)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  endif()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Pinned release commits: GLFW 3.4 and Dear ImGui 1.92.5.
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c)
  FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 6d910d5487d11ca567b61c7824b0c78c569d62f0)
  FetchContent_MakeAvailable(glfw imgui)
  find_package(OpenGL REQUIRED)

  add_library(imgui_backend STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui_backend PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_backend PUBLIC glfw OpenGL::GL ${CMAKE_DL_LIBS})

  add_executable(apex_hud WIN32 main.cpp)
  target_link_libraries(apex_hud PRIVATE imgui_backend)
  apex_options(apex_hud)
  install(TARGETS apex_hud RUNTIME DESTINATION .)
  install(FILES README.md DESTINATION .)
  install(DIRECTORY docs/ DESTINATION docs)
  install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME Dear-ImGui.txt)
  install(FILES ${glfw_SOURCE_DIR}/LICENSE.md DESTINATION licenses RENAME GLFW.md)
endif()

if(BUILD_TESTING)
  add_executable(telemetry_tests tests.cpp)
  apex_options(telemetry_tests)
  add_test(NAME telemetry_invariants_and_packets COMMAND telemetry_tests)
endif()
