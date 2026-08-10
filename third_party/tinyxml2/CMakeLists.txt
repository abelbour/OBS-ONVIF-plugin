# Vendored TinyXML2 10.0.0 (zlib). Compiled isolated from the project's
# warnings-as-errors policy: we do not own this code and don't want upstream
# warnings to gate our builds.
add_library(tinyxml2_lib STATIC tinyxml2.cpp)

target_include_directories(tinyxml2_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
	target_compile_options(tinyxml2_lib PRIVATE /w)
else()
	target_compile_options(tinyxml2_lib PRIVATE -w)
endif()