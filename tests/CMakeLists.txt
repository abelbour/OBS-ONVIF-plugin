if(BUILD_TESTING)
	set(ONVIF_TESTS
		unit/smoke.cpp
		unit/sha1_test.cpp
		unit/ws_security_test.cpp
		unit/xml_test.cpp
	)

	foreach(_test IN LISTS ONVIF_TESTS)
		get_filename_component(_name ${_test} NAME_WE)
		add_executable(${_name} ${_test})
		target_link_libraries(${_name} PRIVATE onvif_core)
		add_test(NAME ${_name} COMMAND ${_name})
	endforeach()
endif()