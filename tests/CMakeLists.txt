include(CTest)

if(BUILD_TESTING)
	add_executable(onvif_smoke unit/smoke.cpp)

	target_link_libraries(onvif_smoke PRIVATE onvif_core)

	add_test(NAME onvif_smoke COMMAND onvif_smoke)
endif()