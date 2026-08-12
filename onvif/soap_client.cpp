#include "soap_client.h"

#include <string>
#include <utility>
#include <vector>

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

std::string WideToUtf8(const std::wstring &w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
					  nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return {};
	std::string out;
	out.resize((size_t)n);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n,
			    nullptr, nullptr);
	return out;
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

// -- AbortHandle -------------------------------------------------------------

AbortHandle::AbortHandle() = default;
AbortHandle::~AbortHandle() = default;

void AbortHandle::Signal()
{
	HINTERNET toClose = nullptr;
	{
		std::lock_guard<std::mutex> lock(mu_);
		signaled_ = true;
		toClose = request_;
		request_ = nullptr;
	}
	if (toClose)
		WinHttpCloseHandle(toClose);
}

bool AbortHandle::Signaled() const
{
	return signaled_.load();
}

void AbortHandle::Attach(HINTERNET request)
{
	std::lock_guard<std::mutex> lock(mu_);
	if (signaled_.load()) // already aborted before this request attached
		return;
	request_ = request;
}

void AbortHandle::Detach()
{
	std::lock_guard<std::mutex> lock(mu_);
	request_ = nullptr;
}

// -- SoapPool ----------------------------------------------------------------

SoapPool::SoapPool() = default;
SoapPool::~SoapPool()
{
	Shutdown();
}

void SoapPool::SetKeepalive(bool on)
{
	keepalive_.store(on);
}

bool SoapPool::keepalive() const
{
	return keepalive_.load();
}

void SoapPool::SetAuthCache(bool on)
{
	authCache_.store(on);
}

SoapPool::AuthMode SoapPool::AuthFor(const std::string &serviceUrl) const
{
	if (!authCache_.load())
		return AuthMode::Unknown;
	std::lock_guard<std::mutex> lock(authMu_);
	const auto it = auth_.find(serviceUrl);
	return it == auth_.end() ? AuthMode::Unknown : it->second;
}

void SoapPool::RememberAuth(const std::string &serviceUrl, AuthMode mode)
{
	if (!authCache_.load())
		return;
	std::lock_guard<std::mutex> lock(authMu_);
	auth_[serviceUrl] = mode;
}

std::shared_ptr<SoapPool::Conn> SoapPool::Acquire(const std::string &key)
{
	std::shared_ptr<Conn> conn;
	{
		std::lock_guard<std::mutex> lock(mapMu_);
		const auto it = pool_.find(key);
		if (it == pool_.end()) {
			conn = std::make_shared<Conn>();
			conn->key = key;
			pool_[key] = conn;
		} else {
			conn = it->second;
		}
	}
	std::unique_lock<std::mutex> lk(conn->mu);
	conn->cv.wait(lk, [&] { return !conn->inUse; });
	conn->inUse = true;
	return conn;
}

void SoapPool::Release(const std::shared_ptr<Conn> &conn)
{
	{
		std::lock_guard<std::mutex> lock(conn->mu);
		conn->inUse = false;
	}
	conn->cv.notify_one();
}

void SoapPool::Close(Conn *conn)
{
	std::lock_guard<std::mutex> lock(conn->mu);
	if (conn->connect) {
		WinHttpCloseHandle(conn->connect);
		conn->connect = nullptr;
	}
	if (conn->session) {
		WinHttpCloseHandle(conn->session);
		conn->session = nullptr;
	}
}

void SoapPool::Shutdown()
{
	std::vector<std::shared_ptr<Conn>> conns;
	{
		std::lock_guard<std::mutex> lock(mapMu_);
		for (const auto &kv : pool_)
			conns.push_back(kv.second);
	}
	for (const auto &c : conns) {
		std::lock_guard<std::mutex> lock(c->mu);
		if (c->connect)
			WinHttpCloseHandle(c->connect);
		if (c->session)
			WinHttpCloseHandle(c->session);
		c->connect = nullptr;
		c->session = nullptr;
	}
}

// -- SoapClient --------------------------------------------------------------

SoapClient::SoapClient() = default;

SoapClient::SoapClient(std::shared_ptr<SoapPool> pool) : pool_(std::move(pool))
{
}

bool SoapClient::ParseTarget(const std::string &raw, Target &url)
{
	url = Target{};
	const std::string schemeSep = "://";
	const size_t schemeEnd = raw.find(schemeSep);
	if (schemeEnd == std::string::npos) {
		url.error = "URL missing scheme";
		return false;
	}
	const std::string scheme = raw.substr(0, schemeEnd);
	if (scheme != "http" && scheme != "https") {
		url.error = "unsupported scheme '" + scheme + "'";
		return false;
	}
	url.https = scheme == "https";

	const std::string rest = raw.substr(schemeEnd + schemeSep.size());
	const size_t pathStart = rest.find('/');
	const std::string authority =
		pathStart == std::string::npos ? rest : rest.substr(0, pathStart);

	std::string host = authority;
	const unsigned defaultPort = url.https ? 443u : 80u;
	std::string portText;

	if (!host.empty() && host[0] == '[') { // IPv6 literal
		const size_t close = host.find(']');
		if (close == std::string::npos) {
			url.error = "malformed IPv6 literal";
			return false;
		}
		const std::string inside = host.substr(1, close - 1);
		const std::string after = host.substr(close + 1);
		host = inside;
		if (!after.empty()) {
			if (after[0] != ':') {
				url.error = "malformed authority";
				return false;
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
			return false;
		}
	} else {
		url.port = defaultPort;
	}

	if (host.empty()) {
		url.error = "empty host";
		return false;
	}

	url.host = ToWide(host);
	url.path = ToWide(pathStart == std::string::npos
				  ? "/"
				  : rest.substr(pathStart));
	return true;
}

bool SoapClient::SendPerCall(const SoapRequest &req, SoapResult &out,
			     AbortHandle *abort)
{
	Target t;
	if (!ParseTarget(req.url, t)) {
		out = SoapResult{};
		out.error = t.error;
		return false;
	}

	HINTERNET hSession = WinHttpOpen(L"obs-onvif/0.1 (WinHTTP; LAN)",
					 WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr,
					 nullptr, 0);
	if (!hSession) {
		out.error = LastWinhttpError();
		return false;
	}

	HINTERNET hConnect = WinHttpConnect(hSession, t.host.c_str(), t.port, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		out.error = LastWinhttpError();
		return false;
	}

	const bool ok = DoRequest(hConnect, t, req, out, abort);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return ok;
}

bool SoapClient::OpenConnection(SoapPool::Conn *conn, const Target &t)
{
	if (conn->connect)
		return true;
	HINTERNET session = WinHttpOpen(L"obs-onvif/0.1 (WinHTTP; LAN)",
					WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr,
					nullptr, 0);
	if (!session)
		return false;
	HINTERNET connect = WinHttpConnect(session, t.host.c_str(), t.port, 0);
	if (!connect) {
		WinHttpCloseHandle(session);
		return false;
	}
	conn->session = session;
	conn->connect = connect;
	return true;
}

bool SoapClient::SendPooled(const SoapRequest &req, SoapResult &out,
			    AbortHandle *abort)
{
	Target t;
	if (!ParseTarget(req.url, t)) {
		out = SoapResult{};
		out.error = t.error;
		return false;
	}
	const std::string key = (t.https ? "s:" : "h:") +
			       WideToUtf8(t.host) + ":" +
			       std::to_string(t.port);

	for (int attempt = 0; attempt < 2; ++attempt) {
		std::shared_ptr<SoapPool::Conn> conn = pool_->Acquire(key);
		if (!conn->connect && !OpenConnection(conn.get(), t)) {
			const std::string err = LastWinhttpError();
			pool_->Release(conn);
			out = SoapResult{};
			out.error = err;
			return false;
		}
		SoapResult attemptOut;
		const bool ok = DoRequest(conn->connect, t, req, attemptOut,
					  abort);
		if (ok) {
			// Keep the connection open for the next request.
			pool_->Release(conn);
			out = attemptOut;
			return true;
		}
		// Transport failure: drop the (stale/aborted) connection so a
		// retry starts from a fresh handshake. An aborted request is
		// never retried.
		const bool aborted = abort && abort->Signaled();
		pool_->Close(conn.get());
		pool_->Release(conn);
		if (aborted) {
			out = attemptOut;
			return false;
		}
		if (attempt == 0)
			continue; // stale keep-alive: retry once on a fresh conn
		out = attemptOut;
		return false;
	}
	return false;
}

bool SoapClient::DoRequest(HINTERNET hConnect, const Target &t,
			   const SoapRequest &req, SoapResult &out,
			   AbortHandle *abort)
{
	out = SoapResult{};
	HINTERNET hRequest =
		WinHttpOpenRequest(hConnect, L"POST", t.path.c_str(), nullptr,
				   WINHTTP_NO_REFERER,
				   WINHTTP_DEFAULT_ACCEPT_TYPES,
				   t.https ? WINHTTP_FLAG_SECURE : 0);
	if (!hRequest) {
		out.error = LastWinhttpError();
		return false;
	}
	if (abort)
		abort->Attach(hRequest);

	const DWORD timeout = req.timeoutMs ? req.timeoutMs : 5000;
	WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

	if (t.https && !req.validateCert) {
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
			ToWide(req.basicUser).c_str(),
			ToWide(req.basicPass).c_str(), nullptr);
	}

	const DWORD bodyLen = (DWORD)req.body.size();
	bool ok = false;
	if (abort && abort->Signaled()) {
		out.error = "request aborted";
	} else if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS,
					0,
					bodyLen ? (LPVOID)req.body.data()
						: WINHTTP_NO_REQUEST_DATA,
					bodyLen, bodyLen, 0)) {
		out.error = LastWinhttpError();
	} else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
		out.error = LastWinhttpError();
	} else {
		DWORD status = 0;
		DWORD statusLen = sizeof status;
		if (WinHttpQueryHeaders(hRequest,
					WINHTTP_QUERY_STATUS_CODE |
						WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status,
					&statusLen, WINHTTP_NO_HEADER_INDEX))
			out.httpStatus = status;

		std::string body;
		char buf[4096];
		DWORD read = 0;
		bool readFailed = false;
		for (;;) {
			if (!WinHttpReadData(hRequest, buf, sizeof buf, &read)) {
				if (abort && abort->Signaled())
					out.error = "request aborted";
				else
					out.error = LastWinhttpError();
				readFailed = true;
				break;
			}
			if (read == 0)
				break;
			body.append(buf, read);
		}
		if (!readFailed) {
			ok = true;
			out.transportOk = true;
			out.body = body;
			out.fault = ParseFault(body);
		}
	}

	if (abort)
		abort->Detach();
	WinHttpCloseHandle(hRequest);
	return ok;
}

bool SoapClient::Send(const SoapRequest &req, SoapResult &out)
{
	return Send(req, out, nullptr);
}

bool SoapClient::Send(const SoapRequest &req, SoapResult &out,
		      AbortHandle *abort)
{
	if (pool_ && pool_->keepalive())
		return SendPooled(req, out, abort);
	return SendPerCall(req, out, abort);
}

bool SoapClient::SendWithAuthFallback(const SoapRequest &a,
				      const SoapRequest &b, SoapResult &out)
{
	return SendWithAuthFallback(a, b, out, nullptr);
}

bool SoapClient::SendWithAuthFallback(const SoapRequest &a,
				      const SoapRequest &b, SoapResult &out,
				      AbortHandle *abort)
{
	if (!Send(a, out, abort))
		return false;
	if (out.transportOk && out.httpStatus != 401 && !out.fault.present)
		return true; // first (digest) attempt was accepted
	if (abort && abort->Signaled())
		return false;

	SoapResult second;
	if (!Send(b, second, abort))
		return false;
	out = second;
	return out.transportOk;
}

} // namespace obs_onvif
