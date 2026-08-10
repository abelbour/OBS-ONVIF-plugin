#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace obs_onvif {

using SHA1Digest = std::array<uint8_t, 20>;

SHA1Digest sha1(const void *data, size_t len);
SHA1Digest sha1(const std::string &in);

std::string sha1_hex(const void *data, size_t len);
std::string sha1_hex(const std::string &in);

} // namespace obs_onvif