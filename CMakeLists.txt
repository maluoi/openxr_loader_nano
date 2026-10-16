cmake_minimum_required(VERSION 3.16)

# The vendored headers are the single record of which OpenXR SDK release this tracks
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/openxr/openxr.h" XRL_VERSION_LINE REGEX "define XR_CURRENT_API_VERSION ")
string(REGEX REPLACE ".*XR_MAKE_VERSION\\(([0-9]+), *([0-9]+), *([0-9]+)\\).*" "\\1.\\2.\\3" XRL_OPENXR_VERSION "${XRL_VERSION_LINE}")

# Before project(), or platform defaults like MSVC's Debug get there first. Only standalone, a parent project picks its own.
get_property(XRL_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT XRL_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

# Same project name as upstream's OpenXR-SDK, so OPENXR_SOURCE_DIR and friends match
project(OPENXR VERSION ${XRL_OPENXR_VERSION} LANGUAGES C)
include(GNUInstallDirs)

# Upstream's option names and defaults, so existing FetchContent/CPM setups carry over
option(BUILD_LOADER "Build the loader, OFF for just the headers" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  option(DYNAMIC_LOADER "Build the loader as a shared library" OFF)
  set(OPENXR_DEBUG_POSTFIX "d" CACHE STRING "OpenXR loader debug postfix.")
else()
  option(DYNAMIC_LOADER "Build the loader as a shared library" ON)
  set(OPENXR_DEBUG_POSTFIX "" CACHE STRING "OpenXR loader debug postfix.")
endif()

# The headers target is named like upstream's, renamed only if the parent project already took the name
set(XRL_HEADERS_TARGET headers)
if(TARGET headers)
  set(XRL_HEADERS_TARGET openxr_loader_headers)
endif()
add_library(${XRL_HEADERS_TARGET} INTERFACE)
set_target_properties(${XRL_HEADERS_TARGET} PROPERTIES EXPORT_NAME headers)
target_include_directories(${XRL_HEADERS_TARGET} INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(NOT TARGET OpenXR::headers)
  add_library(OpenXR::headers ALIAS ${XRL_HEADERS_TARGET})
endif()
install(DIRECTORY include/openxr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Headers)
install(FILES LICENSE DESTINATION share/doc/openxr COMPONENT License)

if(NOT BUILD_LOADER)
  install(TARGETS ${XRL_HEADERS_TARGET} EXPORT openxr_loader_export)
  install(EXPORT openxr_loader_export FILE OpenXRTargets.cmake NAMESPACE OpenXR:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/openxr COMPONENT CMakeConfigs)
  return()
endif()

# The exported trampolines compile to the same few instructions either way,
# so only cold setup code is affected. These flags only apply in this directory.
option(XRL_OPTIMIZE_SIZE "Optimize the loader for size instead of speed in optimized builds" ON)
if(XRL_OPTIMIZE_SIZE)
  foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REGEX REPLACE "[-/]O[0-9sz]" "" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    if(MSVC)
      string(APPEND CMAKE_C_FLAGS_${config} " /O1")
    else()
      string(APPEND CMAKE_C_FLAGS_${config} " -Os")
    endif()
  endforeach()
endif()

if(DYNAMIC_LOADER)
  add_library(openxr_loader SHARED)
else()
  add_library(openxr_loader STATIC)
endif()
add_library(OpenXR::openxr_loader ALIAS openxr_loader)

# Everything else matches upstream's names, so this is how to tell the loaders apart
set(XRL_NAME openxr_loader_nano)
target_compile_definitions(openxr_loader PRIVATE XRL_NAME=${XRL_NAME})

target_sources(openxr_loader PRIVATE
  src/loader.c
  src/manifest.c
  src/json.c
  src/fmt.c
  src/log.c
  src/platform.c
  src/exports.gen.c)
if(ANDROID)
  target_sources(openxr_loader PRIVATE src/android.c)
endif()

# PIC even when static, since it's usually linked into an engine's shared library
set_target_properties(openxr_loader PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)

# The loader always builds with its own headers, since the generated trampolines must match them exactly
target_link_libraries(openxr_loader PUBLIC OpenXR::headers)
target_include_directories(openxr_loader BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(openxr_loader PROPERTIES DEBUG_POSTFIX "${OPENXR_DEBUG_POSTFIX}")

if(DYNAMIC_LOADER)
  target_compile_definitions(openxr_loader PRIVATE XRL_SHARED)
endif()

# Manifest search paths, with upstream's variable names and its derivation from the install prefix
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(FALLBACK_CONFIG_DIRS "/etc/xdg" CACHE STRING "Search path to use when XDG_CONFIG_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")
  set(FALLBACK_DATA_DIRS "/usr/local/share:/usr/share" CACHE STRING "Search path to use when XDG_DATA_DIRS is unset or empty or the current process is SUID/SGID. Default is freedesktop compliant.")
  target_compile_definitions(openxr_loader PRIVATE
    XRL_FALLBACK_CONFIG_DIRS="${FALLBACK_CONFIG_DIRS}"
    XRL_FALLBACK_DATA_DIRS="${FALLBACK_DATA_DIRS}"
    XRL_SYSCONFDIR="${CMAKE_INSTALL_FULL_SYSCONFDIR}")
  if(NOT CMAKE_INSTALL_FULL_SYSCONFDIR STREQUAL "/etc")
    target_compile_definitions(openxr_loader PRIVATE XRL_EXTRASYSCONFDIR="/etc")
  endif()
endif()

if(MSVC)
  target_compile_definitions(openxr_loader PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(openxr_loader PRIVATE -ffunction-sections -fdata-sections)
endif()

# Warnings are only for working on the loader itself. In a parent project they
# would fight its own warning level, which MSVC reports as D9025 per compile.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(MSVC)
    target_compile_options(openxr_loader PRIVATE /W4 /wd4204)
  else()
    target_compile_options(openxr_loader PRIVATE -Wall -Wextra)
  endif()
endif()

# Feature macros go here instead of in a source, so a unity build still sees
# them before its first system header, whatever order the sources merge in.
if(WIN32)
  target_compile_definitions(openxr_loader PRIVATE WIN32_LEAN_AND_MEAN)
else()
  target_compile_definitions(openxr_loader PRIVATE _GNU_SOURCE) # secure_getenv, realpath
endif()

# Platform dependencies
find_package(Threads REQUIRED)
target_link_libraries(openxr_loader PRIVATE ${CMAKE_DL_LIBS} PUBLIC Threads::Threads)
if(ANDROID)
  target_link_libraries(openxr_loader PRIVATE log android)
  target_link_options(openxr_loader PRIVATE "-Wl,-z,max-page-size=16384")
elseif(WIN32)
  target_link_libraries(openxr_loader PUBLIC advapi32)
endif()

# Shared library exports and naming, matching upstream so it's a drop-in
if(DYNAMIC_LOADER)
  # Like upstream, the DLL carries its own CRT so apps don't need the VC++ redistributable
  if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  if(WIN32)
    target_sources(openxr_loader PRIVATE src/openxr_loader.def src/loader.rc)
    target_compile_definitions(openxr_loader PRIVATE
      XRL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} XRL_VERSION_MINOR=${PROJECT_VERSION_MINOR} XRL_VERSION_PATCH=${PROJECT_VERSION_PATCH})
  elseif(APPLE)
    target_link_options(openxr_loader PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/openxr_loader.expsym" "-Wl,-dead_strip")
  else()
    target_link_options(openxr_loader PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/openxr_loader.map" "-Wl,--gc-sections" "-Wl,--no-undefined")
    set_property(TARGET openxr_loader APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/openxr_loader.map")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(openxr_loader PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR} VERSION ${PROJECT_VERSION})
    # Upstream also provides a MAJOR.MINOR name in the build tree
    add_custom_target(libopenxr_loader.so.${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ALL
      COMMAND "${CMAKE_COMMAND}" -E create_symlink libopenxr_loader.so.${PROJECT_VERSION}
              libopenxr_loader.so.${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
  endif()
endif()

# Install and export like upstream: OpenXR::openxr_loader and OpenXR::headers through find_package(OpenXR),
# plus openxr.pc. Being in an export set also lets parents install targets that link the loader publicly.
include(CMakePackageConfigHelpers)
if(WIN32)
  set(XRL_CMAKE_DIR cmake)
else()
  set(XRL_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/openxr)
endif()
install(TARGETS openxr_loader ${XRL_HEADERS_TARGET} EXPORT openxr_loader_export
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Loader
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Loader
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Loader)
export(EXPORT openxr_loader_export FILE ${CMAKE_CURRENT_BINARY_DIR}/OpenXRTargets.cmake NAMESPACE OpenXR::)
install(EXPORT openxr_loader_export FILE OpenXRTargets.cmake NAMESPACE OpenXR:: DESTINATION ${XRL_CMAKE_DIR} COMPONENT CMakeConfigs)
set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_INCLUDEDIR}/)
configure_package_config_file(cmake/OpenXRConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/OpenXRConfig.cmake
  INSTALL_DESTINATION ${XRL_CMAKE_DIR} PATH_VARS INCLUDE_INSTALL_DIR)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/OpenXRConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/OpenXRConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/OpenXRConfigVersion.cmake
  DESTINATION ${XRL_CMAKE_DIR} COMPONENT CMakeConfigs)
if(NOT MSVC)
  set(XRL_API_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  set(XRL_EXTRA_LIBS ${CMAKE_THREAD_LIBS_INIT})
  configure_file(cmake/openxr.pc.in openxr.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/openxr.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

option(XRL_BUILD_TESTS "Build the loader's tests" OFF)
if(XRL_BUILD_TESTS)
  foreach(test xr_smoke probe)
    add_executable(${test} tests/${test}.c)
    set_target_properties(${test} PROPERTIES C_STANDARD 11)
    target_link_libraries(${test} PRIVATE openxr_loader)
  endforeach()
endif()
