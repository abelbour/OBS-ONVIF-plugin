#pragma once

#include <string>

namespace obs_onvif {

// ONVIF WS-Security UsernameToken.
//
//   PasswordDigest = Base64( SHA1( base64decode(Nonce) || Created || Password ) )
//
// Created is a W3C dateTime formatted as UTC, e.g. "2026-08-10T12:34:56Z".
struct UsernameToken {
	std::string username;
	std::string passwordText;   // raw password (legacy plaintext fallback)
	std::string nonceBase64;    // 16 random bytes, base64
	std::string created;        // ISO8601 UTC
	std::string passwordDigest; // populated for digest mode
};

// Builds a token with a fresh random nonce and the current UTC time.
UsernameToken BuildUsernameToken(const std::string &username,
				 const std::string &password);

// Builds a `<wsse:Security>` header fragment embedding the token. `digest`
// selects the PasswordDigest type URI; false selects the legacy PasswordText.
std::string SecurityHeader(const UsernameToken &t, bool digest = true);

// Builds a token from explicit inputs so tests can pin the digest math.
UsernameToken BuildUsernameTokenFixed(const std::string &username,
				      const std::string &password,
				      const std::string &nonceBase64,
				      const std::string &created);

// Computes just the digest. Equivalent to what BuildUsernameToken* fill in.
std::string PasswordDigest(const std::string &nonceBase64,
			   const std::string &created,
			   const std::string &password);

} // namespace obs_onvif