if(BUILD_TESTING)
	set(ONVIF_TESTS
		unit/smoke.cpp
		unit/sha1_test.cpp
		unit/ws_security_test.cpp
		unit/xml_test.cpp
		unit/ws_discovery_test.cpp
		unit/identity_test.cpp
	)

	foreach(_test IN LISTS ONVIF_TESTS)
		get_filename_component(_name ${_test} NAME_WE)
		add_executable(${_name} ${_test})
		target_link_libraries(${_name} PRIVATE onvif_core)
		add_test(NAME ${_name} COMMAND ${_name})
	endforeach()

	# Registry unit tests (persistence + wincred + apply policy + detection).
	set(REGISTRY_TESTS
		unit/store_test.cpp
		unit/apply_test.cpp
		unit/registry_test.cpp
	)

	foreach(_test IN LISTS REGISTRY_TESTS)
		get_filename_component(_name ${_test} NAME_WE)
		add_executable(${_name} ${_test})
		target_link_libraries(${_name} PRIVATE onvif_core registry_lib)
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

	# Live UDP discovery round-trip against the mock's echo responder.
	if(Python3_FOUND)
		add_executable(ws_disc_live_test unit/ws_discovery_test.cpp)
		target_link_libraries(ws_disc_live_test PRIVATE onvif_core)
		add_test(
			NAME discovery_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_discovery_test.py
				$<TARGET_FILE:ws_disc_live_test>
		)
	endif()

	# Live typed-client round-trips against the mock (digest + basic/auth
	# fallback).
	if(Python3_FOUND)
		add_executable(onvif_client_test unit/onvif_client_test.cpp)
		target_link_libraries(onvif_client_test PRIVATE onvif_core)
		add_test(
			NAME client_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_client_test.py
				$<TARGET_FILE:onvif_client_test>
		)
	endif()

	# Live registry flow against the mock: discovery round-trip seeds a
	# camera, a re-bound mock host is detected as an IP move, and the mapped
	# source URL rewrite is observed.
	if(Python3_FOUND)
		add_executable(registry_live_test unit/registry_test.cpp)
		target_link_libraries(registry_live_test PRIVATE onvif_core registry_lib)
		add_test(
			NAME registry_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_registry_test.py
				$<TARGET_FILE:registry_live_test>
		)
	endif()

	# Public ABI (M3): a C consumer links the obs-free ABI core and drives
	# get_camera_list / presets / moves / scene-bindings against the mock.
	# The driver also asserts the built plugin module exports
	# obs_onvif_get_abi via its .exp file. Needs a build of the module.
	if(Python3_FOUND)
		add_executable(aclink aclink/aclink_main.c ../obs/abi_test.cpp)
		set_target_properties(aclink PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
		target_link_libraries(aclink PRIVATE obs_abi)
		add_test(
			NAME aclink
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_aclink_test.py
				$<TARGET_FILE:aclink>
				${CMAKE_BINARY_DIR}
		)
	endif()
endif()