add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/tinyxml2"
	"${CMAKE_CURRENT_BINARY_DIR}/third_party/tinyxml2")

add_library(onvif_core STATIC
	base64.cpp
	identity.cpp
	sha1.cpp
	soap_client.cpp
	ws_discovery.cpp
	ws_security.cpp
	xml.cpp
)

target_include_directories(onvif_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
)

target_link_libraries(onvif_core PUBLIC bcrypt tinyxml2_lib winhttp ws2_32)