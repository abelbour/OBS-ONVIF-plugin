#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "base64.h"

static int g_failed = 0;
static int g_checked = 0;

#define CHECK(cond)                                                         \
	do {                                                                 \
		++g_checked;                                                 \
		if (!(cond)) {                                               \
			++g_failed;                                          \
			std::cerr << "FAIL " __FILE__ ":" << __LINE__      \
				  << ": " #cond << std::endl;                 \
		}                                                            \
	} while (0)

static void RequireRoundTrip(const std::string &bytes)
{
	const std::string encoded = obs_onvif::base64_encode(bytes);
	const std::string decoded = obs_onvif::base64_decode(encoded);
	CHECK(decoded == bytes);
}

static void TestVectors()
{
	CHECK(obs_onvif::base64_encode("") == "");
	CHECK(obs_onvif::base64_encode("f") == "Zg==");
	CHECK(obs_onvif::base64_encode("fo") == "Zm8=");
	CHECK(obs_onvif::base64_encode("foo") == "Zm9v");
	CHECK(obs_onvif::base64_encode("foob") == "Zm9vYg==");
	CHECK(obs_onvif::base64_encode("fooba") == "Zm9vYmE=");
	CHECK(obs_onvif::base64_encode("foobar") == "Zm9vYmFy");
}

static void TestBinary()
{
	const std::vector<uint8_t> bytes = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x80};
	CHECK(obs_onvif::base64_encode(bytes.data(), bytes.size()) ==
	      "AAEC//6A");
	CHECK(obs_onvif::base64_decode("AAEC//6A") ==
	      std::string(bytes.begin(), bytes.end()));
}

static void TestDecodeEdgeCases()
{
	CHECK(obs_onvif::base64_decode("Zg==") == "f");
	CHECK(obs_onvif::base64_decode("Zm8=") == "fo");
	CHECK(obs_onvif::base64_decode("Zm9v") == "foo");
	CHECK(obs_onvif::base64_decode("Zm9v\nYmFy") == "foobar");
	CHECK(obs_onvif::base64_decode("Zm9v\r\nYmFy") == "foobar");
	CHECK(obs_onvif::base64_decode("Zm9vYmFy=\n")
	      == "foobar");
	CHECK(obs_onvif::base64_decode("!!!") == "");
	CHECK(obs_onvif::base64_decode("Zg") == "f");
	CHECK(obs_onvif::base64_decode("Zg=") == "f");
	CHECK(obs_onvif::base64_decode("") == "");
}

static void TestLongRoundTrips()
{
	uint32_t state = 0x1234ABCDu;
	auto next = [&state]() {
		state = state * 1664525u + 1013904223u;
		return uint8_t((state >> 16) & 0xFF);
	};

	for (size_t len : {1u, 2u, 3u, 7u, 64u, 257u, 1024u, 4096u}) {
		std::string bytes;
		bytes.reserve(len);
		for (size_t i = 0; i < len; ++i)
			bytes.push_back(char(next()));
		RequireRoundTrip(bytes);
	}

	RequireRoundTrip("hello");
	RequireRoundTrip(std::string("h\xC3\xA9llo \xE2\x86\x92", 10));
	RequireRoundTrip(std::string("\x00\x00\x00", 3));
}

int main()
{
	TestVectors();
	TestBinary();
	TestDecodeEdgeCases();
	TestLongRoundTrips();

	std::cout << (g_failed == 0 ? "PASS" : "FAIL") << ": " << g_checked
		  << " checks, " << g_failed << " failures" << std::endl;
	return g_failed == 0 ? 0 : 1;
}