#include <regex>
#include <string>

#include "check.h"
#include "base64.h"
#include "sha1.h"
#include "ws_security.h"

using obs_onvif::PasswordDigest;
using obs_onvif::UsernameToken;

static void TestDigestMath()
{
	// Reference values computed with .NET SHA1:
	//   pre = "1234567890123456" (nonce) || "2026-08-10T00:00:00Z" || password
	const std::string nonce = "MTIzNDU2Nzg5MDEyMzQ1Ng=="; // base64("1234567890123456")
	CHECK_EQ(PasswordDigest(nonce, "2026-08-10T00:00:00Z", "pass"),
		 "Jbq71vZh2EhQ6eQCjQa1dzi/aEo=");
	CHECK_EQ(PasswordDigest(nonce, "2026-08-10T00:00:00Z", "secret"),
		 "ClmkrKX/DAQei8RFrLhpzAvKYXs=");
	CHECK_EQ(PasswordDigest(nonce, "2026-08-10T00:00:00Z", "MyPass!za"),
		 "vWarHZVNAuadNG7kSZYMj1Fs0eo=");
}

static void TestFixedToken()
{
	const UsernameToken t = obs_onvif::BuildUsernameTokenFixed(
		"admin", "pass", "MTIzNDU2Nzg5MDEyMzQ1Ng==",
		"2026-08-10T00:00:00Z");

	CHECK_EQ(t.username, "admin");
	CHECK_EQ(t.passwordText, "pass");
	CHECK_EQ(t.nonceBase64, "MTIzNDU2Nzg5MDEyMzQ1Ng==");
	CHECK_EQ(t.created, "2026-08-10T00:00:00Z");
	CHECK_EQ(t.passwordDigest, "Jbq71vZh2EhQ6eQCjQa1dzi/aEo=");
}

static void TestFreshToken()
{
	const UsernameToken t =
		obs_onvif::BuildUsernameToken("user1", "pw");

	CHECK_EQ(t.username, "user1");
	CHECK_EQ(t.passwordText, "pw");

	// Nonce must be exactly 16 random bytes.
	const std::string rawNonce = obs_onvif::base64_decode(t.nonceBase64);
	CHECK_EQ(rawNonce.size(), size_t(16));

	// ISO8601 UTC timestamp.
	CHECK(std::regex_match(
		t.created,
		std::regex(R"([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z)")));

	// Digest must reconstruct from the same nonce/created.
	CHECK_EQ(t.passwordDigest,
		  PasswordDigest(t.nonceBase64, t.created, "pw"));
	CHECK_NE(t.nonceBase64, "");
}

int main()
{
	TestDigestMath();
	TestFixedToken();
	TestFreshToken();
	RUN_TESTS("ws_security");
}