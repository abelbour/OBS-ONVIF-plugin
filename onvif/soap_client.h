#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>
#include <winhttp.h>

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

// Thread-safe abort handle (M4 §6.8 immediate stop). Signal() — called from any
// thread — closes the active WinHTTP request handle so a blocked Send returns
// promptly instead of waiting out the request timeout. One handle is created
// per dispatched PTZ command; it is never reused across commands.
class AbortHandle {
public:
	AbortHandle();
	~AbortHandle();
	AbortHandle(const AbortHandle &) = delete;
	AbortHandle &operator=(const AbortHandle &) = delete;

	void Signal();
	bool Signaled() const;

private:
	friend class SoapClient;
	void Attach(HINTERNET request);
	void Detach();

	std::atomic<bool> signaled_{false};
	std::mutex mu_;
	HINTERNET request_ = nullptr;
};

// Shared, thread-safe transport state (M4 §6.8). One instance is shared by
// every OnvifClient the Worker builds for a camera, so PTZ moves reuse a single
// keep-alive connection and the negotiated auth mode is remembered. The pool is
// keyed by (https, host, port); a connection is handed to exactly one caller at
// a time and closed on transport failure (stale keep-alive dropped by a camera)
// so the caller can retry once on a fresh connection.
class SoapPool {
public:
	SoapPool();
	~SoapPool();
	SoapPool(const SoapPool &) = delete;
	SoapPool &operator=(const SoapPool &) = delete;

	void SetKeepalive(bool on);
	bool keepalive() const;

	// ptz_auth_cache knob: when off, AuthFor reports Unknown and
	// RememberAuth is a no-op (every request renegotiates auth).
	void SetAuthCache(bool on);

	enum class AuthMode { Unknown, Wsse, Basic };
	AuthMode AuthFor(const std::string &serviceUrl) const;
	void RememberAuth(const std::string &serviceUrl, AuthMode mode);

	// Closes every pooled connection (module unload / test teardown).
	void Shutdown();

private:
	friend class SoapClient;
	struct Conn {
		std::string key;
		HINTERNET session = nullptr;
		HINTERNET connect = nullptr;
		bool inUse = false;
		std::mutex mu;
		std::condition_variable cv;
	};
	std::shared_ptr<Conn> Acquire(const std::string &key);
	void Release(const std::shared_ptr<Conn> &conn);
	void Close(Conn *conn);

	mutable std::mutex mapMu_;
	std::map<std::string, std::shared_ptr<Conn>> pool_;
	std::atomic<bool> keepalive_{true};
	std::atomic<bool> authCache_{true};

	mutable std::mutex authMu_;
	std::map<std::string, AuthMode> auth_;
};

class SoapClient {
public:
	// Per-call connection (legacy behavior) unless a pool is supplied.
	SoapClient();
	explicit SoapClient(std::shared_ptr<SoapPool> pool);

	// POSTs one request. Returns true when the transport round trip
	// completed (transportOk == true and the request was not rejected before
	// reaching the server). Apply-level failures surface in out.fault /
	// out.httpStatus. When `abort` is supplied and Signaled, the request is
	// cancelled mid-flight and this returns false without a retry.
	bool Send(const SoapRequest &req, SoapResult &out);
	bool Send(const SoapRequest &req, SoapResult &out, AbortHandle *abort);

	// Tries `a` (WS-Security digest style) and, only when the server rejects
	// it (HTTP 401, a SOAP fault, or a transport failure), tries `b` (HTTP
	// Basic style). `out` reflects the final attempt; returns out.transportOk.
	bool SendWithAuthFallback(const SoapRequest &a, const SoapRequest &b,
				  SoapResult &out);
	bool SendWithAuthFallback(const SoapRequest &a, const SoapRequest &b,
				  SoapResult &out, AbortHandle *abort);

private:
	// Parses the request target; fills `error` on malformed URLs.
	struct Target {
		bool https = false;
		std::wstring host;
		unsigned port = 0;
		std::wstring path;
		std::string error;
	};
	static bool ParseTarget(const std::string &url, Target &t);

	bool SendPerCall(const SoapRequest &req, SoapResult &out,
			 AbortHandle *abort);
	bool SendPooled(const SoapRequest &req, SoapResult &out,
			AbortHandle *abort);
	bool OpenConnection(SoapPool::Conn *conn, const Target &t);
	bool DoRequest(HINTERNET hConnect, const Target &t, const SoapRequest &req,
		       SoapResult &out, AbortHandle *abort);

	std::shared_ptr<SoapPool> pool_;
};

} // namespace obs_onvif
