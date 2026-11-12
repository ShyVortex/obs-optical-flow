# OBS Studio for Windows ships obs.dll but no obs.lib import library, which
# MSVC needs to link a plugin. Generate one from the DLL's export table:
#   dumpbin /EXPORTS obs.dll  ->  obs.def  ->  lib /DEF:obs.def  ->  obs.lib
# Same idea as linking against the installed OBS.app on macOS.

function(obs_generate_import_lib obs_dir out_var)
	set(_dll "${obs_dir}/bin/64bit/obs.dll")
	if(NOT EXISTS "${_dll}")
		message(FATAL_ERROR "obs.dll not found at ${_dll} — set -DOBS_DIR=<OBS Studio folder>")
	endif()

	get_filename_component(_tooldir "${CMAKE_LINKER}" DIRECTORY)
	find_program(OBS_DUMPBIN dumpbin HINTS "${_tooldir}" REQUIRED)
	find_program(OBS_LIBEXE lib HINTS "${_tooldir}" REQUIRED)

	set(_def "${CMAKE_CURRENT_BINARY_DIR}/obs.def")
	set(_lib "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")

	# Regenerate only when the DLL changes (e.g. after an OBS update).
	file(SHA256 "${_dll}" _hash)
	if(EXISTS "${_lib}" AND "${_hash}" STREQUAL "${OBS_IMPORT_LIB_HASH}")
		set(${out_var} "${_lib}" PARENT_SCOPE)
		return()
	endif()

	execute_process(COMMAND "${OBS_DUMPBIN}" /NOLOGO /EXPORTS "${_dll}"
		OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR "dumpbin /EXPORTS failed on ${_dll}")
	endif()

	# Export rows look like:  "    123   7A 0001B2C0 obs_register_source_s"
	# (ordinal, hint, RVA, name). Forwarded exports have no RVA and are skipped.
	set(_names "")
	string(REPLACE "\n" ";" _lines "${_out}")
	foreach(_line IN LISTS _lines)
		if(_line MATCHES "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([A-Za-z_?@$][^ \t\r]*)")
			string(APPEND _names "    ${CMAKE_MATCH_1}\n")
		endif()
	endforeach()
	if(_names STREQUAL "")
		message(FATAL_ERROR "no exports parsed from ${_dll}")
	endif()

	file(WRITE "${_def}" "LIBRARY obs\nEXPORTS\n${_names}")
	execute_process(COMMAND "${OBS_LIBEXE}" /NOLOGO "/DEF:${_def}" "/OUT:${_lib}" /MACHINE:X64
		RESULT_VARIABLE _rc)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR "lib /DEF failed generating ${_lib}")
	endif()

	set(OBS_IMPORT_LIB_HASH "${_hash}" CACHE INTERNAL "")
	message(STATUS "Generated obs.lib import library from ${_dll}")
	set(${out_var} "${_lib}" PARENT_SCOPE)
endfunction()
