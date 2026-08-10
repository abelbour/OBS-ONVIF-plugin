add_library(onvif_core STATIC
	base64.cpp
	sha1.cpp
	ws_security.cpp
	xml.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../third_party/tinyxml2/tinyxml2.cpp
)

target_include_directories(onvif_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
)

target_link_libraries(onvif_core PUBLIC bcrypt ws2_32 winhttp)