add_library(registry_lib STATIC
	apply.cpp
	registry.cpp
	store.cpp
)

target_include_directories(registry_lib PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(registry_lib PUBLIC onvif_core advapi32)
