#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "apply.h"
#include "camera.h"
#include "identity.h"
#include "store.h"

namespace obs_onvif::registry {

// Result of one discovery contact with a device.
struct DeviceUpdate {
	bool first_seen = false;
	bool address_changed = false;
	ApplyDecision action = ApplyDecision::Ignored; // what Apply did
	// Stream URI used for this contact (empty when the caller did not
	// refresh it, e.g. the camera has no mapping yet).
	std::string stream_uri;
};

// In-memory camera state + IP-change detection. Deterministic and OBS-free:
// the caller (tests, or the M3 worker) feeds discovery results and the current
// output activity; this class owns no sockets and touches no UI thread.
//
// Rewrites produced by applied incidents are handed back by value; the caller
// (M3 obs_apply) dispatches them to the OBS main thread.
class Registry {
public:
	Registry();

	// Camera table (keyed by fingerprint).
	Camera *FindCamera(const std::string &id);
	const Camera *FindCamera(const std::string &id) const;
	const std::map<std::string, Camera> &Cameras() const;
	size_t CameraCount() const;
	void RemoveCamera(const std::string &id);

	// Source-mapping mirror (per scene collection). Drives rewrite
	// generation; kept in sync with the policy object.
	void SetMappings(const std::vector<SourceMapping> &mappings);
	const std::vector<SourceMapping> &Mappings() const;
	std::vector<SourceMapping> MappingsForCamera(const std::string &camera_id) const;
	void AddSourceMapping(const SourceMapping &m);
	void RemoveSourceMapping(const std::string &source_name);

	// Restore/persist the camera table through `store`.
	bool Restore(Store &store);
	bool Persist(Store &store) const;

	// Handles one discovery contact (ProbeMatch/Hello) resolved through
	// GetDeviceInformation. Detects first-seen (add) vs. a moved camera
	// (xaddr changed) and, on a move, drives the apply policy.
	//
	//   identity       fingerprint inputs (serial/hardwareId/scopes/uuid)
	//   display_name   camera name for a first-seen device (e.g. model)
	//   new_xaddr      device-service XAddr from the discovery message
	//   profile_token  the mapped profile whose stream URI changed
	//   new_stream_uri GetStreamUri result for that profile (may be empty)
	//   now_ms         wall-clock ms for lastSeen
	//   output_active  whether stream/record outputs are currently running
	//   credentials    URL-encoded "user:pass" to splice (may be empty)
	//   rewrites       filled when the incident auto-applied
	DeviceUpdate SeenDevice(const DeviceIdentity &identity,
				const std::string &display_name,
				const std::string &new_xaddr,
				const std::string &profile_token,
				const std::string &new_stream_uri,
				uint64_t now_ms, bool output_active,
				const std::string &credentials,
				std::vector<SourceRewrite> &rewrites);

	ApplyPolicy &Apply();
	const ApplyPolicy &Apply() const;

private:
	Camera *UpsertCamera(const Camera &cam);

	std::map<std::string, Camera> cameras_;
	std::vector<SourceMapping> mappings_;
	ApplyPolicy apply_;
};

} // namespace obs_onvif::registry