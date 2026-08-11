# OBS layer. Milestone 3.
#
# obs_abi is the obs-free public-ABI core. Its single translation unit is
# dllexport-ed so linking it into the plugin module exports obs_onvif_get_abi
# from obs-onvif.dll; the same object links into tests/aclink where the ABI is
# driven against the mock camera. The libobs glue (plugin entry, source
# mapping, apply dispatch, scene→preset events, hotkeys) is added to the module
# target by the root CMakeLists once built.
add_library(obs_abi STATIC abi.cpp)

target_compile_definitions(
	obs_abi
	PRIVATE OBS_ONVIF_ABI_BUILD OBS_ONVIF_DLL
)

target_include_directories(obs_abi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(obs_abi PUBLIC onvif_core registry_lib)