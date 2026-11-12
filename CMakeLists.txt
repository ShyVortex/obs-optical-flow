# obs-optical-flow — cross-platform build (Linux, Windows, macOS).
#
# The plugin compiles against the libobs headers vendored in deps/ and links
# against the libobs of an *installed* OBS Studio — no OBS source build needed.
#
#   Linux:   links the system libobs.so (distro / PPA package)
#   Windows: generates an obs.lib import library from the installed obs.dll
#   macOS:   links libobs.framework inside OBS.app (build.sh does the same)
#
# Quick start:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   cmake --install build --config Release   # into the per-user plugin folder

cmake_minimum_required(VERSION 3.22)

if(APPLE)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
	set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures")
endif()

project(obs-optical-flow VERSION 1.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BUILD_TESTS "Build the standalone libobs test harnesses in test/" OFF)

set(OBS_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libobs-headers")

# ---------------------------------------------------------------------------
# Locate the installed libobs  ->  imported target OBS::libobs
# ---------------------------------------------------------------------------
if(WIN32)
	set(OBS_DIR "C:/Program Files/obs-studio" CACHE PATH
		"OBS Studio installation (the folder containing bin/64bit/obs.dll)")
	include(cmake/WindowsImportLib.cmake)
	obs_generate_import_lib("${OBS_DIR}" OBS_IMPORT_LIB)
	add_library(OBS::libobs UNKNOWN IMPORTED)
	set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_IMPORT_LIB}")
elseif(APPLE)
	set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS.app to link against")
	find_library(OBS_LIBOBS libobs PATHS "${OBS_APP}/Contents/Frameworks" NO_DEFAULT_PATH)
	if(NOT OBS_LIBOBS)
		message(FATAL_ERROR "libobs.framework not found in ${OBS_APP}/Contents/Frameworks (set -DOBS_APP=...)")
	endif()
	add_library(OBS::libobs INTERFACE IMPORTED)
	set_target_properties(OBS::libobs PROPERTIES INTERFACE_LINK_LIBRARIES "${OBS_LIBOBS}")
else()
	set(OBS_LIB_DIR "" CACHE PATH "Optional extra directory to search for libobs.so")
	# libobs.so is a dev symlink some packages (e.g. the official .deb) lack
	find_library(OBS_LIBOBS NAMES obs libobs.so.30 libobs.so.0 HINTS "${OBS_LIB_DIR}")
	if(NOT OBS_LIBOBS)
		message(FATAL_ERROR "libobs.so not found — install OBS Studio (or libobs-dev) or set -DOBS_LIB_DIR=...")
	endif()
	add_library(OBS::libobs UNKNOWN IMPORTED)
	set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_LIBOBS}")
endif()
message(STATUS "obs-optical-flow ${PROJECT_VERSION}: linking libobs from ${OBS_LIBOBS}${OBS_IMPORT_LIB}")

# Shared warning / define setup for the plugin and the test harnesses.
add_library(obs-optical-flow-options INTERFACE)
target_include_directories(obs-optical-flow-options INTERFACE "${OBS_HEADERS_DIR}")
if(MSVC)
	target_compile_options(obs-optical-flow-options INTERFACE /W3 /utf-8)
	target_compile_definitions(obs-optical-flow-options INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
	target_compile_options(obs-optical-flow-options INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# The plugin module
# ---------------------------------------------------------------------------
add_library(obs-optical-flow MODULE src/plugin-main.c src/optical-flow-filter.c)
target_link_libraries(obs-optical-flow PRIVATE obs-optical-flow-options OBS::libobs)
target_compile_definitions(obs-optical-flow PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")
set_target_properties(obs-optical-flow PROPERTIES
	PREFIX ""                      # obs-optical-flow.so, not libobs-optical-flow.so
	C_VISIBILITY_PRESET hidden)    # only MODULE_EXPORT symbols are exported
if(UNIX AND NOT APPLE)
	target_link_libraries(obs-optical-flow PRIVATE m)
endif()

if(APPLE)
	# Same .plugin bundle layout as build.sh; version taken from project().
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/bundle/Info.plist" _plist)
	string(REGEX REPLACE
		"(<key>CFBundle(ShortVersionString|Version)</key>[ \t\r\n]*<string>)[^<]*"
		"\\1${PROJECT_VERSION}" _plist "${_plist}")
	file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" "${_plist}")
	set_target_properties(obs-optical-flow PROPERTIES
		BUNDLE TRUE
		BUNDLE_EXTENSION plugin
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
	add_custom_command(TARGET obs-optical-flow POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/data"
			"$<TARGET_BUNDLE_CONTENT_DIR:obs-optical-flow>/Resources"
		COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:obs-optical-flow>"
		COMMENT "Copying data into the bundle and ad-hoc codesigning")
endif()

# ---------------------------------------------------------------------------
# Install: the per-user OBS plugin folder by default
#   Linux:   ~/.config/obs-studio/plugins/obs-optical-flow/{bin/64bit,data}
#   Windows: C:/ProgramData/obs-studio/plugins/obs-optical-flow/{bin/64bit,data}
#   macOS:   ~/Library/Application Support/obs-studio/plugins/obs-optical-flow.plugin
# Override with --prefix / CMAKE_INSTALL_PREFIX (the layout below the prefix
# stays the same, which is also what the release packages contain).
# ---------------------------------------------------------------------------
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	if(WIN32)
		set(_prefix "C:/ProgramData/obs-studio/plugins")
	elseif(APPLE)
		set(_prefix "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
	elseif(DEFINED ENV{XDG_CONFIG_HOME})
		set(_prefix "$ENV{XDG_CONFIG_HOME}/obs-studio/plugins")
	else()
		set(_prefix "$ENV{HOME}/.config/obs-studio/plugins")
	endif()
	set(CMAKE_INSTALL_PREFIX "${_prefix}" CACHE PATH "Install prefix" FORCE)
endif()

if(APPLE)
	install(TARGETS obs-optical-flow LIBRARY DESTINATION . BUNDLE DESTINATION .)
	# install may rewrite the binary's rpaths, which invalidates the signature
	install(CODE "execute_process(COMMAND codesign --force --sign - \"\${CMAKE_INSTALL_PREFIX}/obs-optical-flow.plugin\" COMMAND_ERROR_IS_FATAL ANY)")
else()
	install(TARGETS obs-optical-flow
		LIBRARY DESTINATION obs-optical-flow/bin/64bit   # .so
		RUNTIME DESTINATION obs-optical-flow/bin/64bit)  # .dll
	install(DIRECTORY data/ DESTINATION obs-optical-flow/data)
endif()

if(BUILD_TESTS)
	enable_testing()
	add_subdirectory(test)
endif()
