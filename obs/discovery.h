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

// Configures the loop: store root, credentials resolver, move callback, and
// discovery/soap timeouts from the persisted AppConfig. Seeds the live table
// from the persisted cameras (offline until re-seen). Safe to call once.
void Configure(const std::string &configDir, CredsFn creds, MovedFn onMoved);

// Starts the background discovery loop. No-op if already running.
void Start();

// Stops the loop and joins its thread.
void Stop();

// True while the loop is (or was) configured and not yet stopped.
bool Running();

// Thread-safe snapshot of the live camera table.
std::vector<registry::Camera> Snapshot();

// One unicast Probe to `host:port`, then processes every ProbeMatch through
// the shared contact-resolution path. Test/utility entry point that shares all
// detection logic with the loop.
void ProbeOnce(const std::string &host, uint16_t port,
	       const std::string &messageId);

} // namespace obs_onvif::discovery
