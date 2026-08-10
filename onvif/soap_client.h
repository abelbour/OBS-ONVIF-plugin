#pragma once

#include <string>

namespace obs_onvif {

// SOAP over HTTP(S) via WinHTTP (Schannel for TLS 1.2/1.3). Proxy is bypassed
// deliberately: cameras are reached on the local LAN only (see PLAN.md §Scope
// decisions) and a system proxy would only misroute link-local traffic.
struct SoapRequest {
	std::string url;      // full service XAddr: scheme://host[:port]/path
	std::string body;     // complete SOAP envelope (wsse Security included
			      // when the caller uses WS-Security auth)
	std::string soapAction; // SOAPAction value (empty => `SOAPAction: ""`)
	std::string basicUser;  // HTTP Basic auth, supplied to WinHTTP via
	std::string basicPass;  // WinHttpSetCredentials (not raw headers)
	bool validateCert = false; // https: reject unknown/self-signed/expired
				   // certs when true
	unsigned timeoutMs = 5000;
};

struct SoapFault {
	bool present = false;
	std::string code;   // e.g. "soap:Sender", "s:Server", "env:Receiver"
	std::string reason; // faultstring / Reason/Text
};

struct SoapResult {
	bool transportOk = false; // the round trip completed at the HTTP layer
	unsigned httpStatus = 0;  // meaningful only when transportOk
	std::string body;         // raw response body
	std::string error;        // human-readable transport/parse error
	SoapFault fault;          // parsed from the body when present
};

// Extracts a SOAP fault from an envelope body (SOAP 1.1 and 1.2 tolerant).
SoapFault ParseFault(const std::string &body);

class SoapClient {
public:
	// POSTs one request. Returns true when the transport round trip
	// completed (transportOk == true and the request was not rejected before
	// reaching the server). Apply-level failures surface in out.fault /
	// out.httpStatus.
	bool Send(const SoapRequest &req, SoapResult &out);

	// Tries `a` (WS-Security digest style) and, only when the server rejects
	// it (HTTP 401, a SOAP fault, or a transport failure), tries `b` (HTTP
	// Basic style). `out` reflects the final attempt; returns out.transportOk.
	bool SendWithAuthFallback(const SoapRequest &a, const SoapRequest &b,
				  SoapResult &out);
};

} // namespace obs_onvif