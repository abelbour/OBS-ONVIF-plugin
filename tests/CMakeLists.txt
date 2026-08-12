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
		unit/ptz_controller_test.cpp
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

	# Live Media2 path + classic fallback against the mock (M5a).
	if(Python3_FOUND)
		add_executable(media2_live_test unit/media2_live_test.cpp)
		target_link_libraries(media2_live_test PRIVATE onvif_core)
		add_test(
			NAME media2_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_media2_test.py
				$<TARGET_FILE:media2_live_test>
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

	# Live discovery-bridge flow against the mock: the M2→M3 discovery loop's
	# contact resolution seeds a camera, then a re-bound mock host is detected
	# as a move and reported through the onMoved callback. Shares obs_abi's
	# pattern of compiling an obs-layer source into the test.
	if(Python3_FOUND)
		add_executable(discovery_live_test unit/discovery_live_test.cpp
			       ../obs/discovery.cpp)
		target_include_directories(discovery_live_test PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}/../obs")
		target_link_libraries(discovery_live_test PRIVATE onvif_core registry_lib)
		add_test(
			NAME discovery_bridge_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_discovery_bridge_test.py
				$<TARGET_FILE:discovery_live_test>
		)
	endif()

	# Live camera-config flow against the mock: the config-panel backend
	# (encoder / imaging / network / OSD) round-trips through mock state via
	# the public ABI.
	if(Python3_FOUND)
		add_executable(config_live_test unit/config_live_test.cpp
			       ../obs/abi_test.cpp)
		target_link_libraries(config_live_test PRIVATE obs_abi)
		add_test(
			NAME config_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_config_test.py
				$<TARGET_FILE:config_live_test>
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

	# Live PTZ latency/keep-alive flow against the mock (M4 §6.8): the driver
	# asserts one-request cached moves, connection reuse, a prompt immediate
	# stop, and queue depth <= 1.
	if(Python3_FOUND)
		add_executable(ptz_latency_live_test unit/ptz_latency_live_test.cpp
			       ../obs/abi_test.cpp)
		target_link_libraries(ptz_latency_live_test PRIVATE obs_abi)
		add_test(
			NAME ptz_latency_live
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/run_ptz_latency_test.py
				$<TARGET_FILE:ptz_latency_live_test>
		)
	endif()
endif()