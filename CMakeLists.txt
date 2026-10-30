cmake_minimum_required(VERSION 3.24)
project(wavy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WAVY_BUILD_TESTS "Build wavy unit tests" ON)

set(JITNODES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JITNODES_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JITNODES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(JITNODES_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/jitnodes)

if(WIN32)
  if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "wavy supports single-config generators only (use the Ninja presets)")
  endif()

  # newui is built in its own project so its C++17 settings, Blend2D-embedded asmjit, tests and examples stay out of this one.
  # Building it bumps VERSION.state in the newui checkout, as any newui build does.
  include(ExternalProject)
  set(newuiDir "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/newui")
  set(newuiBinary "${CMAKE_BINARY_DIR}/newui")
  set(newuiLib "${newuiBinary}/newui.lib")
  set(newuiAutomationLib "${newuiBinary}/newui_automation.lib")
  set(blend2dLib "${newuiBinary}/3rdparty/blend2d/blend2d.lib")

  set(newuiArgs -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  if(CMAKE_C_COMPILER)
    list(APPEND newuiArgs -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
  endif()
  if(CMAKE_MAKE_PROGRAM)
    list(APPEND newuiArgs -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
  endif()

  ExternalProject_Add(newuiExternal
    SOURCE_DIR "${newuiDir}"
    BINARY_DIR "${newuiBinary}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS ${newuiArgs}
    BUILD_COMMAND ${CMAKE_COMMAND} --build "${newuiBinary}" --target newui newui_automation
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${newuiLib}" "${newuiAutomationLib}" "${blend2dLib}"
    USES_TERMINAL_BUILD ON)

  file(MAKE_DIRECTORY "${newuiBinary}/include")
  add_library(newui::newui STATIC IMPORTED GLOBAL)
  set_target_properties(newui::newui PROPERTIES
    IMPORTED_LOCATION "${newuiLib}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${newuiDir}/include;${newuiBinary}/include;${newuiDir}/3rdparty/json5/include;${newuiDir}/3rdparty/wil/include;${newuiDir}/3rdparty/blend2d"
    INTERFACE_COMPILE_DEFINITIONS "BL_STATIC"
    INTERFACE_LINK_LIBRARIES
      "${blend2dLib};Comctl32.lib;Uxtheme.lib;Ole32.lib;Uuid.lib;Shell32.lib;Advapi32.lib;Dwmapi.lib;Shlwapi.lib;d3d11.lib;dxgi.lib;dxguid.lib")
  add_dependencies(newui::newui newuiExternal)

  add_library(newui::automation STATIC IMPORTED GLOBAL)
  set_target_properties(newui::automation PROPERTIES
    IMPORTED_LOCATION "${newuiAutomationLib}"
    INTERFACE_LINK_LIBRARIES newui::newui)
endif()

set(wavyVersionHeader "${CMAKE_BINARY_DIR}/include/wavy/version.h")
add_custom_target(wavy_version
  COMMAND ${CMAKE_COMMAND}
    -DINITIAL_MAJOR=${wavy_VERSION_MAJOR}
    -DINITIAL_MINOR=${wavy_VERSION_MINOR}
    -DINITIAL_RELEASE=${wavy_VERSION_PATCH}
    -DSTATE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/VERSION.state
    -DIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/include/wavy/version.h.in
    -DOUT_FILE=${wavyVersionHeader}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generateVersion.cmake
  BYPRODUCTS ${wavyVersionHeader}
  COMMENT "Generating wavy/version.h (incrementing build number)"
  VERBATIM)

add_library(wavyCore STATIC src/wavy.cpp src/audioEngine.cpp src/asioDevice.cpp src/midiInput.cpp src/clipPlayer.cpp src/wavLoader.cpp)
if(WIN32)
  target_link_libraries(wavyCore PRIVATE Winmm.lib)
endif()
add_library(wavy::core ALIAS wavyCore)

# The Steinberg ASIO SDK is never vendored (its license forbids redistribution): point at a local copy to build the ASIO backend.
set(ASIO_SDK_DIR "$ENV{ASIO_SDK_DIR}" CACHE PATH "Steinberg ASIO SDK root (contains common/ and host/)")
if(WIN32 AND ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/asio.h")
  set(asioSources
    "${ASIO_SDK_DIR}/common/asio.cpp"
    "${ASIO_SDK_DIR}/host/asiodrivers.cpp"
    "${ASIO_SDK_DIR}/host/pc/asiolist.cpp")
  target_sources(wavyCore PRIVATE ${asioSources})
  set_source_files_properties(${asioSources} PROPERTIES COMPILE_OPTIONS "/W0")
  target_include_directories(wavyCore PRIVATE
    "${ASIO_SDK_DIR}/common" "${ASIO_SDK_DIR}/host" "${ASIO_SDK_DIR}/host/pc")
  target_compile_definitions(wavyCore PUBLIC WAVY_HAS_ASIO)
  target_link_libraries(wavyCore PRIVATE Winmm.lib)
  add_executable(asioSine src/asioSine.cpp)
  target_link_libraries(asioSine PRIVATE wavy::core)
  message(STATUS "ASIO backend enabled (${ASIO_SDK_DIR})")
else()
  message(STATUS "ASIO backend disabled (set ASIO_SDK_DIR to build it)")
endif()
add_dependencies(wavyCore wavy_version)
target_include_directories(wavyCore PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>)
target_link_libraries(wavyCore PUBLIC jitnodes::jitnodes)
if(MSVC)
  target_compile_options(wavyCore PRIVATE /W4 /wd4324 /permissive- /utf-8)
else()
  target_compile_options(wavyCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(wavy src/main.cpp)
target_link_libraries(wavy PRIVATE wavy::core)
if(WIN32)
  target_link_libraries(wavy PRIVATE newui::newui)
endif()

if(WIN32 AND TARGET asioSine)
  option(WAVY_GUI_AUTOMATION "Build the editor with newui's automation server (--automation <pipe>)" ON)
  add_executable(wavyEditor WIN32 editor/main.cpp editor/session.cpp editor/canvases.cpp)
  target_include_directories(wavyEditor PRIVATE editor)
  target_link_libraries(wavyEditor PRIVATE wavy::core newui::newui comdlg32.lib)
  if(WAVY_GUI_AUTOMATION)
    target_link_libraries(wavyEditor PRIVATE newui::automation)
    target_compile_definitions(wavyEditor PRIVATE WAVY_GUI_AUTOMATION)
  endif()
  if(MSVC)
    target_compile_definitions(wavyEditor PRIVATE _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
    target_compile_options(wavyEditor PRIVATE /W3 /permissive /utf-8 /Zc:__cplusplus)
    target_link_options(wavyEditor PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  endif()
  add_custom_command(TARGET wavyEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wavyEditor>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/editor/ui/mainWindow.newui"
      "$<TARGET_FILE_DIR:wavyEditor>/Resources/mainWindow.newui")
endif()

if(WAVY_BUILD_TESTS)
  # Reuses the googletest vendored in newui (v1.17.0) instead of keeping a second copy.
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  add_subdirectory(3rdparty/newui/3rdparty/googletest googletest EXCLUDE_FROM_ALL)
  enable_testing()
  add_subdirectory(tests)
endif()
