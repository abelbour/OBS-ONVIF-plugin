#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "camera.h"
#include "worker.h"

namespace obs_onvif::discovery {

/* Continuous WS-Discovery bridge (M2→M3). Runs a background thread that owns a
 * multicast socket bound to the discovery group: it listens for Hello /
 * ProbeMatches / Bye, sends a Probe at startup and then a heartbeat every
 * discovery_interval_s, resolves every contact through GetDeviceInformation
 * (fingerprint) + GetProfiles/GetStreamUri, and keeps a live camera table.
 *
 * OBS-free: everything here touches only the onvif core, the registry store,
 * and the callbacks supplied by the OBS glue (credentials + move reporting).
 * The OBS glue wires `onMoved` to a queued main-thread task so the apply
 * policy is only ever touched on the UI thread. */

// Resolves credentials for a camera id; an empty id means "unknown camera"
// and should fall back to the default credentials (Store::DefaultCredTarget).
using CredsFn = std::function<registry::CameraCreds(const std::string &cameraId)>;

// Reports a camera whose XAddr or mapped stream URI changed. `credentials` is
// a raw "user:pass" (may be empty) to splice into rewritten source URLs.
using MovedFn = std::function<void(const std::string &cameraId,
				   const std::string &newStreamUri,
				   const std::string &credentials)>;

// Optional diagnostic logger. The plugin wires this to obs_log; the OBS-free
// live tests pass a no-op (discovery.cpp must not link libobs).
using LogFn = std::function<void(const std::string &line)>;

// Configures the loop: store root, credentials resolver, move callback, and
// discovery/soap timeouts from the persisted AppConfig. Seeds the live table
// from the persisted cameras (offline until re-seen). Safe to call once.
void Configure(const std::string &configDir, CredsFn creds, MovedFn onMoved,
	       LogFn log = LogFn());

// Starts the background discovery loop. No-op if already running.
void Start();

// Stops the loop and joins its thread.
void Stop();

// Signals the background loop to send an immediate multicast Probe (the dock's
// Refresh/Scan button). Thread-safe; no-op if the loop isn't running.
void RequestScan();

// True while the loop is (or was) configured and not yet stopped.
bool Running();

// Why the discovery loop stopped at startup (e.g. UDP port 3702 bind failure),
// or empty when healthy. Diagnostics surfaces this so a dead loop is not
// mistaken for "no cameras on the network".
std::string LoopFault();

// Thread-safe snapshot of the live camera table.
std::vector<registry::Camera> Snapshot();

// Live state of the most recent discovery probe batch, for the dock's status
// line ("scanning…", "last scan … ago · N replies").
struct ScanStatus {
	bool scanning = false;         // last probe batch's reply window is open
	uint64_t probesSent = 0;       // lifetime probes (multicast + directed)
	unsigned repliesSinceScan = 0; // parsed datagrams since the last batch
	unsigned camerasOnline = 0;
	unsigned camerasTotal = 0;
	uint64_t lastScanMs = 0;       // monotonic ms of last batch (0 = never)
};

// Thread-safe snapshot of the scan status.
ScanStatus ScanStatusSnapshot();

// True while the last probe batch's reply window is still open (the dock
// renders "scanning…"). False before any probe has been sent.
bool Scanning();

// Whole seconds since the last probe batch (heartbeat/retry/manual scan), or 0
// when no probe batch has gone out yet.
unsigned SecondsSinceLastScan();

// One unicast Probe to `host:port`, then processes every ProbeMatch through
// the shared contact-resolution path. Test/utility entry point that shares all
// detection logic with the loop.
void ProbeOnce(const std::string &host, uint16_t port,
	       const std::string &messageId);

// Parses one WS-Discovery datagram and applies it to the live table the way
// the background loop does: a Hello refreshes presence for a known camera
// (or resolves when new/on a new address), a Bye marks the device offline
// immediately, and a ProbeMatch is fully resolved. Shared by the loop and the
// live tests (no multicast needed).
void HandleDiscoveryDatagram(const std::string &xml);

// Manual add-by-IP fallback (M5d): resolves `xaddr` (a device-service URL) with
// the given credentials through the shared contact path, registers the camera
// in the live table, persists it, and stores the credentials in the Credential
// Vault. Returns false + `err` when the camera cannot be reached or identified.
bool AddManual(const std::string &xaddr, const std::string &username,
	       const std::string &password, std::string &err);

// Removes a camera (by fingerprint id) from the live table, the persisted
// store, and the Credential Vault. Returns false + `err` when no camera with
// that id exists.
bool RemoveManual(const std::string &cameraId, std::string &err);

// Diagnostic counters (OBS-free; surfaced by the dock's Diagnostics tab).
uint64_t ProbesSent();
uint64_t DatagramsParsed();
unsigned LastParsedDevices();

// One advertised device that could not be resolved (auth required, no stable
// identity, or unreachable endpoint). Surfaced so discovery failures are
// visible instead of silently dropping the camera.
struct PendingContact {
	std::string xaddr;
	std::string reason; // e.g. "GetDeviceInformation: SOAP fault ..."
	uint64_t lastAttempt = 0; // monotonic ms
	unsigned attempts = 0;
};

// Thread-safe snapshot of the unresolved-contacts table.
std::vector<PendingContact> PendingContacts();

// Compact, human-readable summary of the discovery loop state.
std::string Diagnostics();

} // namespace obs_onvif::discovery
