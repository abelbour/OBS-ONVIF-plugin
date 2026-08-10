#include "base64.h"

#include <algorithm>
#include <cstdint>

namespace obs_onvif {

namespace {

const char *const kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline int DecodeChar(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

} // namespace

std::string base64_encode(const std::string &in)
{
	return base64_encode(in.data(), in.size());
}

std::string base64_encode(const void *data, size_t len)
{
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	std::string out;
	out.reserve(((len + 2) / 3) * 4);

	size_t i = 0;
	while (i + 2 < len) {
		const uint32_t triple = (uint32_t(bytes[i]) << 16) |
					(uint32_t(bytes[i + 1]) << 8) |
					uint32_t(bytes[i + 2]);
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
		out.push_back(kAlphabet[triple & 0x3F]);
		i += 3;
	}

	const size_t rem = len - i;
	if (rem == 1) {
		const uint32_t triple = uint32_t(bytes[i]) << 16;
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	} else if (rem == 2) {
		const uint32_t triple = (uint32_t(bytes[i]) << 16) |
					(uint32_t(bytes[i + 1]) << 8);
		out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
		out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

std::string base64_decode(const std::string &in)
{
	std::string head;
	head.reserve(in.size());
	for (char c : in) {
		if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
			continue;
		if (c == '=')
			break;
		if (DecodeChar(c) < 0)
			return {};
		head.push_back(c);
	}

	if (head.empty())
		return {};

	std::string out;
	out.reserve((head.size() / 4) * 3);

	for (size_t i = 0; i < head.size(); i += 4) {
		const size_t group = std::min<size_t>(4, head.size() - i);
		auto sextet = [&head, &i, group](size_t off) {
			return off < group ? uint32_t(DecodeChar(head[i + off])) : 0u;
		};
		const uint32_t triple = (sextet(0) << 18) | (sextet(1) << 12) |
					(sextet(2) << 6) | sextet(3);
		out.push_back(char((triple >> 16) & 0xFF));
		if (group >= 3)
			out.push_back(char((triple >> 8) & 0xFF));
		if (group == 4)
			out.push_back(char(triple & 0xFF));
	}
	return out;
}

} // namespace obs_onvif