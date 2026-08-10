#pragma once

#include <string>

namespace obs_onvif {

struct DeviceIdentity {
	std::string serialNumber;
	std::string hardwareId;
	std::string scopes; // raw scopes string from discovery
	std::string uuid;   // wsa endpoint uuid
};

// Builds a stable fingerprint for a camera. Priority:
//   serialNumber > scope MAC > hardwareId > uuid.
// Returns an empty string when no source is available.
std::string BuildFingerprint(const DeviceIdentity &id);

// Returns the hardware MAC from the first onvif://www.onvif.org/mac/...
// scope token, or an empty string when absent.
std::string ParseScopeMac(const std::string &scopes);

} // namespace obs_onvif