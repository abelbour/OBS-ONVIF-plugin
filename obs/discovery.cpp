// Continuous WS-Discovery bridge (obs/discovery.cpp).
//
// Background loop (M2→M3): a multicast socket on the discovery group listens
// for Hello/ProbeMatches/Bye while periodic heartbeat Probes re-discover the
// network. Every contact is resolved through GetDeviceInformation (stable
// fingerprint), GetProfiles and GetStreamUri, folded into a live camera table,
// and — when the device's XAddr or mapped stream URI changed — reported via
// the `onMoved` callback. The OBS glue queues that onto the main thread, where
// the apply policy runs.
#include "discovery.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "identity.h"
#include "onvif_client.h"
#include "store.h"
#include "ws_discovery.h"

namespace obs_onvif::discovery {

namespace {

using namespace obs_onvif::registry;

std::mutex &StateMu()
{
	static std::mutex m;
	return m;
}

std::map<std::string, registry::Camera> &Live()
{
	static std::map<std::string, registry::Camera> c;
	return c;
}

std::atomic<bool> &Seeded()
{
	static std::atomic<bool> f(false);
	return f;
}

std::atomic<bool> &Configured()
{
	static std::atomic<bool> f(false);
	return f;
}

std::atomic<bool> &StartedFlag()
{
	static std::atomic<bool> f(false);
	return f;
}

std::atomic<bool> &StopFlag()
{
	static std::atomic<bool> f(false);
	return f;
}

std::thread &LoopThread()
{
	static std::thread t;
	return t;
}

std::string &ConfigDir()
{
	static std::string d;
	return d;
}

CredsFn &Creds()
{
	static CredsFn f;
	return f;
}

MovedFn &OnMoved()
{
	static MovedFn f;
	return f;
}

unsigned &HeartbeatS()
{
	static unsigned s = 60;
	return s;
}

unsigned &SoapTimeoutMs()
{
	static unsigned t = 5000;
	return t;
}

uint64_t NowMs()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// "user:pass" used to splice credentials into rewritten URLs (raw, matching
// the registry tests and the wincred secret format).
std::string CredentialString(const registry::CameraCreds &c)
{
	if (c.username.empty() && c.password.empty())
		return std::string();
	return c.username + ":" + c.password;
}

// Folds the persisted camera table into the live table (offline until seen).
// Runs once; ProbeOnce and the loop both seed so callers are deterministic.
void SeedFromStore()
{
	if (Seeded().exchange(true))
		return;
	Store store(ConfigDir());
	std::vector<registry::Camera> cams;
	if (!store.LoadCameras(cams))
		return;
	std::lock_guard<std::mutex> lock(StateMu());
	for (auto &c : cams) {
		c.online = false;
		Live()[c.id] = std::move(c);
	}
}

void PersistAll()
{
	std::vector<registry::Camera> cams;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		cams.reserve(Live().size());
		for (const auto &kv : Live())
			cams.push_back(kv.second);
	}
	Store store(ConfigDir());
	store.SaveCameras(cams);
}

// Everything the contact resolution learned about one discovered device.
struct ContactInfo {
	std::string fingerprint;
	std::string display_name;
	std::string xaddr;
	std::string scope_mac;
	std::string profile_token;
	std::string stream_uri;
	std::string credentials;
};

// Full resolution of a ProbeMatch (or an unknown/new-address Hello): identity
// fingerprint + stream URI, folded into the live table. Forward-declared so
// the cheap Hello/Bye paths can fall back to it.
void ProcessContact(const DiscoveredDevice &dev);

// Resolves a discovery contact: identity (fingerprint) + stream URI. Returns
// false when the device cannot be reached or has no identifiable fingerprint.
// `initialCreds` (manual add-by-IP) supplies the first GetDeviceInformation
// attempt; otherwise the default credentials resolver is used.
bool ResolveContact(const DiscoveredDevice &dev, ContactInfo &out,
		    const registry::CameraCreds *initialCreds = nullptr)
{
	if (dev.xaddrs.empty())
		return false;
	const std::string xaddr = dev.xaddrs[0];

	const registry::CameraCreds defaultCreds =
		initialCreds ? *initialCreds
			     : (Creds() ? Creds()("") : registry::CameraCreds{});
	auto client = std::make_unique<OnvifClient>(
		xaddr, defaultCreds.username, defaultCreds.password,
		/*allowBasicFallback=*/true, /*validateCert=*/false,
		SoapTimeoutMs());

	DeviceInfo info;
	try {
		info = client->GetDeviceInformation();
	} catch (const std::exception &) {
		return false;
	}

	DeviceIdentity id;
	id.serialNumber = info.serialNumber;
	id.hardwareId = info.hardwareId;
	id.scopes = dev.scopes;
	id.uuid = dev.uuid;
	const std::string fingerprint = BuildFingerprint(id);
	if (fingerprint.empty())
		return false;

	out.fingerprint = fingerprint;
	out.display_name = info.model.empty() ? info.manufacturer : info.model;
	out.xaddr = xaddr;
	out.scope_mac = ParseScopeMac(dev.scopes);

	/* Known camera: switch to its per-camera credentials (the first
	 * GetDeviceInformation used the default creds before the fingerprint
	 * was known). */
	bool known = false;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		known = Live().count(fingerprint) != 0;
	}
	if (known) {
		const registry::CameraCreds c =
			Creds() ? Creds()(fingerprint) : registry::CameraCreds{};
		if (!(c.username == defaultCreds.username &&
		      c.password == defaultCreds.password)) {
			client = std::make_unique<OnvifClient>(
				xaddr, c.username, c.password,
				/*allowBasicFallback=*/true,
				/*validateCert=*/false, SoapTimeoutMs());
		}
		out.credentials = CredentialString(c);
	} else {
		out.credentials = CredentialString(defaultCreds);
	}

	/* Media path is optional: a camera that answers identity but not media
	 * still gets recorded (online), it just has no stream URI to compare. */
	try {
		client->GetCapabilities();
		const std::vector<MediaProfile> profiles = client->GetProfiles();
		if (!profiles.empty()) {
			out.profile_token = profiles.front().token;
			const StreamUriResult r =
				client->GetStreamUri(profiles.front());
			out.stream_uri = r.uri;
		}
	} catch (const std::exception &) {
	}

	return true;
}

// "mac:AA:BB:CC:DD:EE:FF" fingerprint from a discovery scopes string, or an
// empty string when no MAC scope is present. Lets a Hello/Bye be matched to a
// known camera without a SOAP round trip.
std::string ScopeMacFingerprint(const std::string &scopes)
{
	const std::string mac = ParseScopeMac(scopes);
	return mac.empty() ? std::string() : "mac:" + mac;
}

// A WS-Discovery Bye: mark the matching known camera offline immediately.
// No SOAP; matches by advertised XAddr (the fingerprint is not derivable from
// a Bye without a MAC scope).
void ProcessBye(const DiscoveredDevice &dev)
{
	if (dev.xaddrs.empty())
		return;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		for (auto &kv : Live()) {
			for (const std::string &xa : dev.xaddrs) {
				if (kv.second.xaddr == xa &&
				    kv.second.online) {
					kv.second.online = false;
					changed = true;
					break;
				}
			}
		}
	}
	if (changed)
		PersistAll();
}

// A WS-Discovery Hello. For a camera already known at the same address this is
// a pure presence refresh (no SOAP — the DHCP-sack win). An unknown device or
// one announcing a *new* address goes through full resolution so an IP change
// is still detected and reported.
void ProcessHello(const DiscoveredDevice &dev)
{
	const std::string fp = ScopeMacFingerprint(dev.scopes);
	if (fp.empty()) {
		ProcessContact(dev);
		return;
	}
	const uint64_t now = NowMs();
	bool known = false;
	bool sameAddress = false;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		auto it = Live().find(fp);
		if (it != Live().end()) {
			known = true;
			for (const std::string &xa : dev.xaddrs) {
				if (xa == it->second.xaddr) {
					sameAddress = true;
					break;
				}
			}
			if (sameAddress) {
				it->second.online = true;
				it->second.lastSeen = now;
			}
		}
	}
	if (!known || !sameAddress)
		ProcessContact(dev);
}

// Folds a resolved contact into the live table: registers a new camera,
// updates an existing one, persists on change, and reports IP/stream moves
// through the `onMoved` callback. Shared by discovery processing and the
// manual add-by-IP path.
bool ApplyContact(const ContactInfo &ci)
{
	const uint64_t now = NowMs();
	bool isNew = false;
	bool moved = false;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		auto it = Live().find(ci.fingerprint);
		if (it == Live().end()) {
			registry::Camera cam;
			cam.id = ci.fingerprint;
			cam.name = ci.display_name;
			cam.xaddr = ci.xaddr;
			cam.scopeMac = ci.scope_mac;
			cam.online = true;
			cam.lastSeen = now;
			if (!ci.stream_uri.empty())
				cam.lastKnownRTSP[ci.profile_token] =
					ci.stream_uri;
			Live()[ci.fingerprint] = std::move(cam);
			isNew = true;
		} else {
			registry::Camera &cam = it->second;
			/* An XAddr change only counts as a move when we also have
			 * a fresh stream URI to rewrite to (a device that moved
			 * but stalls on media just updates its address). */
			if (cam.xaddr != ci.xaddr && !ci.stream_uri.empty())
				moved = true;
			const auto prev = cam.lastKnownRTSP.find(ci.profile_token);
			if (prev == cam.lastKnownRTSP.end() ||
			    prev->second != ci.stream_uri) {
				if (!ci.stream_uri.empty())
					moved = true;
			}
			cam.xaddr = ci.xaddr;
			cam.online = true;
			cam.lastSeen = now;
			if (!ci.stream_uri.empty())
				cam.lastKnownRTSP[ci.profile_token] =
					ci.stream_uri;
		}
	}

	if (isNew || moved)
		PersistAll();
	if (moved && OnMoved())
		OnMoved()(ci.fingerprint, ci.stream_uri, ci.credentials);
	return isNew || moved;
}

void ProcessContact(const DiscoveredDevice &dev)
{
	ContactInfo ci;
	if (!ResolveContact(dev, ci))
		return;
	ApplyContact(ci);
}

void SweepStale()
{
	const uint64_t cutoff = NowMs() - (uint64_t)HeartbeatS() * 3000;
	std::lock_guard<std::mutex> lock(StateMu());
	for (auto &kv : Live()) {
		if (kv.second.lastSeen < cutoff)
			kv.second.online = false;
	}
}

void LoopBody()
{
	SeedFromStore();

	intptr_t sock = OpenUdpSocket(kDiscoveryPort, /*joinMulticast=*/true,
				     /*reuseAddr=*/true);
	if (sock == -1)
		return;

	SendUdp(sock, BuildProbe("urn:uuid:obs-onvif-start"), kDiscoveryGroup,
		kDiscoveryPort);

	uint64_t nextHeartbeat = NowMs() + (uint64_t)HeartbeatS() * 1000;
	while (!StopFlag().load()) {
		// Drain the pending datagram batch (DHCP-sack: a Hello/Bye burst
		// during a network event is cheap — no SOAP — but must not be
		// starved behind slow ProbeMatch resolution). The first read
		// blocks up to 500 ms; the rest are zero-timeout drains.
		int batch = 0;
		for (;;) {
			std::string msg;
			const long n =
				RecvUdp(sock, msg, batch == 0 ? 500 : 0);
			if (n <= 0)
				break;
			HandleDiscoveryDatagram(msg);
			if (++batch >= 128)
				break;
		}
		if (NowMs() >= nextHeartbeat) {
			SendUdp(sock, BuildProbe("urn:uuid:obs-onvif-heartbeat"),
				kDiscoveryGroup, kDiscoveryPort);
			SweepStale();
			nextHeartbeat =
				NowMs() + (uint64_t)HeartbeatS() * 1000;
		}
	}
	CloseUdpSocket(sock);
}

} // namespace

void Configure(const std::string &configDir, CredsFn creds, MovedFn onMoved)
{
	ConfigDir() = configDir;
	Creds() = std::move(creds);
	OnMoved() = std::move(onMoved);

	Store store(configDir);
	AppConfig cfg;
	if (store.LoadAppConfig(cfg)) {
		if (cfg.discovery_interval_s > 0)
			HeartbeatS() = (unsigned)cfg.discovery_interval_s;
		if (cfg.soap_timeout_s > 0)
			SoapTimeoutMs() = (unsigned)cfg.soap_timeout_s * 1000;
	}

	Configured().store(true);
	SeedFromStore();
}

void Start()
{
	if (!Configured().load() || StartedFlag().exchange(true))
		return;
	StopFlag().store(false);
	LoopThread() = std::thread(LoopBody);
}

void Stop()
{
	if (!StartedFlag().exchange(false))
		return;
	StopFlag().store(true);
	if (LoopThread().joinable())
		LoopThread().join();
}

bool Running()
{
	return Configured().load();
}

std::vector<registry::Camera> Snapshot()
{
	std::lock_guard<std::mutex> lock(StateMu());
	std::vector<registry::Camera> out;
	out.reserve(Live().size());
	for (const auto &kv : Live())
		out.push_back(kv.second);
	return out;
}

void ProbeOnce(const std::string &host, uint16_t port,
	       const std::string &messageId)
{
	SeedFromStore();
	intptr_t sock = OpenUdpSocket(0, /*joinMulticast=*/false,
				     /*reuseAddr=*/false);
	if (sock == -1)
		return;
	if (SendUdp(sock, BuildProbe(messageId), host, port) > 0) {
		std::string reply;
		if (RecvUdp(sock, reply, /*timeoutMs=*/5000) > 0)
			HandleDiscoveryDatagram(reply);
	}
	CloseUdpSocket(sock);
}

void HandleDiscoveryDatagram(const std::string &xml)
{
	std::vector<DiscoveredDevice> devs;
	if (!ParseDiscoveryResponse(xml, devs))
		return;
	for (const auto &dev : devs) {
		switch (dev.type) {
		case DiscoveryMsgType::Bye:
			ProcessBye(dev);
			break;
		case DiscoveryMsgType::Hello:
			ProcessHello(dev);
			break;
		default:
			ProcessContact(dev);
			break;
		}
	}
}

bool AddManual(const std::string &xaddr, const std::string &username,
	       const std::string &password, std::string &err)
{
	SeedFromStore();
	DiscoveredDevice dev;
	dev.xaddrs.push_back(xaddr);

	registry::CameraCreds creds{username, password};
	ContactInfo ci;
	if (!ResolveContact(dev, ci, &creds)) {
		err = "cannot reach an ONVIF camera at " + xaddr;
		return false;
	}

	// Persist the camera's credentials in the Credential Vault so later
	// operations resolve them by fingerprint.
	Store store(ConfigDir());
	store.WriteCredential(Store::CameraCredTarget(ci.fingerprint),
			      username + ":" + password);

	ApplyContact(ci);
	return true;
}

bool RemoveManual(const std::string &cameraId, std::string &err)
{
	bool erased = false;
	{
		std::lock_guard<std::mutex> lock(StateMu());
		erased = Live().erase(cameraId) != 0;
	}
	if (erased) {
		PersistAll();
		Store store(ConfigDir());
		store.DeleteCredential(Store::CameraCredTarget(cameraId));
	}
	(void)err;
	return true;
}

} // namespace obs_onvif::discovery
