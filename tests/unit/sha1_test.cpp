#include <string>

#include "check.h"
#include "sha1.h"

using obs_onvif::sha1;
using obs_onvif::sha1_hex;

static void TestVectors()
{
	CHECK_EQ(sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK_EQ(sha1_hex("abc"),
		 "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK_EQ(sha1_hex("The quick brown fox jumps over the lazy dog"),
		 "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
	CHECK_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	CHECK_EQ(sha1_hex(std::string(1000000, 'a')),
		 "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

static void TestBinarySafe()
{
	const std::string bytes("abc\x00\xff"
				"def",
				8);
	const auto digest = sha1(bytes);
	CHECK_EQ(digest.size(), size_t(20));
	// Contains an interior NUL; sha1_hex length must still be 40.
	CHECK_EQ(sha1_hex(bytes).size(), size_t(40));
}

int main()
{
	TestVectors();
	TestBinarySafe();
	RUN_TESTS("sha1");
}