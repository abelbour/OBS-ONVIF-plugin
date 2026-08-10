#include "soap_client.h"

#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include "xml.h"

namespace obs_onvif {

namespace {

std::wstring ToWide(const std::string &s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
					  nullptr, 0);
	std::wstring out;
	out.resize(n > 0 ? (size_t)n : 0);
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
				    &out[0], n);
	return out;
}

struct Url {
	bool https = false;
	std::wstring host;
	unsigned port = 0;
	std::wstring path;
	std::string error;
};

Url ParseUrl(const std::string &raw)
{
	Url url;
	const std::string schemeSep = "://";
	const size_t schemeEnd = raw.find(schemeSep);
	if (schemeEnd == std::string::npos) {
		url.error = "URL missing scheme";
		return url;
	}
	const std::string scheme = raw.substr(0, schemeEnd);
	if (scheme != "http" && scheme != "https") {
		url.error = "unsupported scheme '" + scheme + "'";
		return url;
	}
	url.https = scheme == "https";

	const std::string rest = raw.substr(schemeEnd + schemeSep.size());
	const size_t pathStart = rest.find('/');
	const std::string authority =
		pathStart == std::string::npos ? rest : rest.substr(0, pathStart);

	std::string host = authority;
	unsigned defaultPort = url.https ? 443u : 80u;
	std::string portText;

	if (!host.empty() && host[0] == '[') { // IPv6 literal
		const size_t close = host.find(']');
		if (close == std::string::npos) {
			url.error = "malformed IPv6 literal";
			return url;
		}
			const std::string inside = host.substr(1, close - 1);
		const std::string after = host.substr(close + 1);
		host = inside;
		if (!after.empty()) {
			if (after[0] != ':') {
				url.error = "malformed authority";
				return url;
			}
			portText = after.substr(1);
		}
	} else {
		const size_t colon = host.find_last_of(':');
		if (colon != std::string::npos) {
			portText = host.substr(colon + 1);
			host = host.substr(0, colon);
		}
	}

	if (!portText.empty()) {
		try {
			url.port = (unsigned)std::stoi(portText);
		} catch (...) {
			url.error = "bad port '" + portText + "'";
			return url;
		}
	} else {
		url.port = defaultPort;
	}

	if (host.empty()) {
		url.error = "empty host";
		return url;
	}

	url.host = ToWide(host);
	url.path = ToWide(pathStart == std::string::npos
				  ? "/"
				  : rest.substr(pathStart));
	return url;
}

std::string LastWinhttpError()
{
	return "WinHTTP error " + std::to_string(GetLastError());
}

} // namespace

SoapFault ParseFault(const std::string &body)
{
	SoapFault fault;
	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		return fault;

	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		return fault;
	const std::vector<const tinyxml2::XMLElement *> bodies =
		xml::Children(env, "Body");
	if (bodies.empty())
		return fault;

	const tinyxml2::XMLElement *faultEl =
		xml::Child(bodies[0], "Fault");
	if (!faultEl)
		return fault;
	fault.present = true;

	const tinyxml2::XMLElement *code =
		xml::Child(faultEl, "faultcode"); // SOAP 1.1
	if (!code)
		code = xml::Descendant(faultEl, {"Code", "Value"}); // 1.2
	if (code)
		fault.code = xml::TextOf(code);

	const tinyxml2::XMLElement *reason =
		xml::Child(faultEl, "faultstring"); // SOAP 1.1
	if (!reason)
		reason = xml::Descendant(faultEl, {"Reason", "Text"}); // 1.2
	if (reason)
		fault.reason = xml::TextOf(reason);
	return fault;
}

bool SoapClient::Send(const SoapRequest &req, SoapResult &out)
{
	out = SoapResult{};

	const Url url = ParseUrl(req.url);
	if (!url.error.empty()) {
		out.error = url.error;
		return false;
	}

	HINTERNET hSession = WinHttpOpen(L"obs-onvif/0.1 (WinHTTP; LAN)",
					 WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr,
					 nullptr, 0);
	if (!hSession) {
		out.error = LastWinhttpError();
		return false;
	}

	HINTERNET hConnect =
		WinHttpConnect(hSession, url.host.c_str(), url.port, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		out.error = LastWinhttpError();
		return false;
	}

	const DWORD secure = url.https ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET hRequest =
		WinHttpOpenRequest(hConnect, L"POST", url.path.c_str(), nullptr,
				   WINHTTP_NO_REFERER,
				   WINHTTP_DEFAULT_ACCEPT_TYPES, secure);
	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		out.error = LastWinhttpError();
		return false;
	}

	const DWORD timeout = req.timeoutMs ? req.timeoutMs : 5000;
	WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

	if (url.https && !req.validateCert) {
		DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
				 SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
				 SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
		WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
				 &secFlags, sizeof secFlags);
	}

	const std::wstring contentType =
		L"Content-Type: text/xml; charset=\"utf-8\"";
	WinHttpAddRequestHeaders(hRequest, contentType.c_str(), -1L,
				 WINHTTP_ADDREQ_FLAG_REPLACE);

	const std::wstring soapAction =
		L"SOAPAction: \"" + ToWide(req.soapAction) + L"\"";
	WinHttpAddRequestHeaders(hRequest, soapAction.c_str(), -1L,
				 WINHTTP_ADDREQ_FLAG_REPLACE);

	if (!req.basicUser.empty()) {
		WinHttpSetCredentials(
			hRequest, WINHTTP_AUTH_TARGET_SERVER,
			WINHTTP_AUTH_SCHEME_BASIC,
			ToWide(req.basicUser).c_str(), ToWide(req.basicPass).c_str(),
			nullptr);
	}

	const DWORD bodyLen = (DWORD)req.body.size();
	const BOOL sent = WinHttpSendRequest(
		hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		bodyLen ? (LPVOID)req.body.data() : WINHTTP_NO_REQUEST_DATA,
		bodyLen, bodyLen, 0);
	if (!sent) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		out.error = LastWinhttpError();
		return false;
	}

	if (!WinHttpReceiveResponse(hRequest, nullptr)) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		out.error = LastWinhttpError();
		return false;
	}

	DWORD status = 0;
	DWORD statusLen = sizeof status;
	if (WinHttpQueryHeaders(hRequest,
				WINHTTP_QUERY_STATUS_CODE |
					WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status,
				&statusLen,
				WINHTTP_NO_HEADER_INDEX))
		out.httpStatus = status;

	std::string body;
	char buf[4096];
	DWORD read = 0;
	for (;;) {
		if (!WinHttpReadData(hRequest, buf, sizeof buf, &read)) {
			out.error = LastWinhttpError();
			break;
		}
		if (read == 0)
			break;
		body.append(buf, read);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	out.transportOk = true;
	out.body = body;
	out.fault = ParseFault(body);
	return true;
}

bool SoapClient::SendWithAuthFallback(const SoapRequest &a,
				      const SoapRequest &b, SoapResult &out)
{
	if (!Send(a, out))
		return false;
	if (out.transportOk && out.httpStatus != 401 && !out.fault.present)
		return true; // first (digest) attempt was accepted

	SoapResult second;
	if (!Send(b, second))
		return false;
	out = second;
	return out.transportOk;
}

} // namespace obs_onvif