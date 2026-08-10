#pragma once

#include <cstddef>
#include <string>

namespace obs_onvif {

std::string base64_encode(const void *data, size_t len);
std::string base64_encode(const std::string &in);

std::string base64_decode(const std::string &in);

} // namespace obs_onvif