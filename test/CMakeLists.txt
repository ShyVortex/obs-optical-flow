# Standalone libobs test harnesses (configure with -DBUILD_TESTS=ON).
#
#   ctest --test-dir build -C Release --output-on-failure
#
# Every test runs once per graphics backend available on the platform:
# Windows tests Direct3D 11 (the default OBS renderer there) and OpenGL.

set(OBS_TESTS effect-test flow-test perf-test debug-test)

if(WIN32)
	set(OBS_TEST_BACKENDS d3d11 opengl)
else()
	set(OBS_TEST_BACKENDS opengl)
endif()

foreach(_t IN LISTS OBS_TESTS)
	add_executable(${_t} ${_t}.c test-common.h)
	target_link_libraries(${_t} PRIVATE obs-optical-flow-options OBS::libobs)
	if(UNIX AND NOT APPLE)
		target_link_libraries(${_t} PRIVATE m ${CMAKE_DL_LIBS})
	endif()
	if(APPLE)
		set_target_properties(${_t} PROPERTIES
			BUILD_RPATH "${OBS_APP}/Contents/Frameworks"
			INSTALL_RPATH "${OBS_APP}/Contents/Frameworks")
	endif()

	foreach(_b IN LISTS OBS_TEST_BACKENDS)
		add_test(NAME ${_t}-${_b}
			COMMAND ${_t} --backend ${_b} --effect "${PROJECT_SOURCE_DIR}/data/effects/optical-flow.effect"
			WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
		if(WIN32)
			# obs.dll and the graphics modules live in the OBS install
			set_tests_properties(${_t}-${_b} PROPERTIES
				ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${OBS_DIR}/bin/64bit")
		endif()
	endforeach()

	# packaged separately (CI "tests" artifact):  cmake --install build --component tests
	install(TARGETS ${_t} RUNTIME DESTINATION . COMPONENT tests EXCLUDE_FROM_ALL)
endforeach()

install(FILES "${PROJECT_SOURCE_DIR}/data/effects/optical-flow.effect"
	DESTINATION data/effects COMPONENT tests EXCLUDE_FROM_ALL)
