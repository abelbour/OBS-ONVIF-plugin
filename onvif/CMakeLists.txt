add_library(onvif_core STATIC
	base64.cpp
)

target_include_directories(onvif_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")