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

	# Live SOAP round-trips against tests/mock_onvif_server.py. Needs a
	# Python3 interpreter; skipped (with a warning) when absent.
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_FOUND)
		add_executable(soap_live_test unit/soap_live_test.cpp)
		target_link_libraries(soap_live_test PRIVATE onvif_core)
		add_test(
			NAME soap_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_soap_live_test.py
				$<TARGET_FILE:soap_live_test>
		)
	else()
		message(WARNING "Python3 interpreter not found; skipping soap_live network tests")
	endif()
endif()