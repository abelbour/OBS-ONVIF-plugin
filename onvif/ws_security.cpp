#include "ws_security.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#include <windows.h>
#include <bcrypt.h>

#include "base64.h"
#include "sha1.h"

namespace obs_onvif {

namespace {

// RFC 3339 / W3C dateTime subset that all ONVIF devices accept.
std::string CreatedNow()
{
	SYSTEMTIME st;
	GetSystemTime(&st);
	char buf[32];
	std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ",
		      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
		      st.wSecond);
	return buf;
}

std::array<uint8_t, 16> RandomBytes()
{
	std::array<uint8_t, 16> out{};
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
		    &alg, BCRYPT_RNG_ALGORITHM, nullptr, 0))) {
		BCryptGenRandom(alg, out.data(), (ULONG)out.size(), 0);
		BCryptCloseAlgorithmProvider(alg, 0);
	} else {
		for (auto &b : out) {
			unsigned int r = 0;
			rand_s(&r); // fallback; RDRAND-backed on modern CPUs
			b = uint8_t(r);
		}
	}
	return out;
}

} // namespace

std::string PasswordDigest(const std::string &nonceBase64,
			   const std::string &created,
			   const std::string &password)
{
	const std::string nonce = base64_decode(nonceBase64);
	return base64_encode(sha1(nonce + created + password));
}

UsernameToken BuildUsernameToken(const std::string &username,
				 const std::string &password)
{
	const auto nonce = RandomBytes();
	return BuildUsernameTokenFixed(username, password,
				       base64_encode(nonce.data(), nonce.size()),
				       CreatedNow());
}

UsernameToken BuildUsernameTokenFixed(const std::string &username,
				      const std::string &password,
				      const std::string &nonceBase64,
				      const std::string &created)
{
	UsernameToken t;
	t.username = username;
	t.passwordText = password;
	t.nonceBase64 = nonceBase64;
	t.created = created;
	t.passwordDigest = PasswordDigest(nonceBase64, created, password);
	return t;
}

} // namespace obs_onvif