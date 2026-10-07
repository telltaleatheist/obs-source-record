# Build file of this fork, using the current obs-plugintemplate layout (cmake/common, cmake/windows,
# CMakePresets.json, buildspec.json) so it builds out of tree against a modern OBS. Upstream's
# in-tree / ObsPluginHelpers build is what this replaces.
cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

set(_obs_build "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.1/build_x64")
list(APPEND CMAKE_PREFIX_PATH ${_obs_build}/libobs ${_obs_build}/deps/w32-pthreads ${_obs_build}/UI/obs-frontend-api)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
if(WIN32)
  find_package(w32-pthreads REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::w32-pthreads)
endif()
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE source-record.c source-record.h sync-start.c sync-start.h panel.cpp panel-api.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
