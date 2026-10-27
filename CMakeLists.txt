cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/core/shortcut-filter.cpp
    src/core/zoom-logic.cpp
    src/input/input-hook.cpp
    src/zoom/auto-zoom-filter.cpp
    src/keystrokes/keystroke-source.cpp
)

if(OS_WINDOWS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/input/platform-win.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE user32)
elseif(OS_MACOS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/input/platform-mac.cpp)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      "$<LINK_LIBRARY:FRAMEWORK,ApplicationServices.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,Carbon.framework>"
      "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>"
  )
elseif(OS_LINUX)
  find_package(X11 REQUIRED)
  if(NOT X11_Xtst_FOUND OR NOT X11_Xrandr_FOUND)
    message(FATAL_ERROR "libxtst-dev (XRecord) and libxrandr-dev are required")
  endif()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/input/platform-x11.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE X11::X11 X11::Xtst X11::Xrandr)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
