#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "camera.h"

namespace obs_onvif {
class OnvifClient;
}

namespace obs_onvif::registry {

// Credentials for a camera (raw "user:pass" split at first ':').
struct CameraCreds {
	std::string username;
	std::string password;
};

struct PresetInfo {
	std::string token;
	std::string name;
};

// Per-camera ONVIF session executor. OBS-free: builds a fresh OnvifClient per
// operation (capabilities are re-resolved lazily) so concurrent callers never
// share mutable state and failed sessions cannot poison later calls. PTZ
// operations address the camera's first profile (the mock and typical
// single-stream cameras expose one PTZ-enabled profile).
//
// Every operation returns false and sets `err` instead of throwing.
class Worker {
public:
	// `creds` resolves credentials for a camera id (e.g. Store wincred).
	explicit Worker(std::function<CameraCreds(const std::string &)> creds);

	void SetTimeout(unsigned timeoutMs);
	void SetAllowBasicFallback(bool allow);

	bool Move(const Camera &cam, double pan, double tilt, double zoom,
		  std::string &err);
	bool Stop(const Camera &cam, std::string &err);
	bool GotoPreset(const Camera &cam, const std::string &presetToken,
			std::string &err);
	bool SavePreset(const Camera &cam, const std::string &name,
			std::string &tokenOut, std::string &err);
	bool ListPresets(const Camera &cam, std::vector<PresetInfo> &out,
			std::string &err);
	bool RenamePreset(const Camera &cam, const std::string &presetToken,
			  const std::string &newName, std::string &err);
	bool DeletePreset(const Camera &cam, const std::string &presetToken,
			  std::string &err);

	// Last preset token saved/recalled per camera (ABI get_current_preset).
	bool CurrentPresetToken(const std::string &cameraId,
				std::string &tokenOut) const;

private:
	// Creates the ONVIF client for `cam` using the resolved credentials.
	obs_onvif::OnvifClient BuildClient(const Camera &cam) const;
	bool FirstProfileToken(obs_onvif::OnvifClient &client,
			       std::string &token, std::string &err);

	std::function<CameraCreds(const std::string &)> creds_;
	unsigned timeoutMs_ = 3000;
	bool allowBasicFallback_ = true;
	mutable std::mutex mu_;
	mutable std::map<std::string, std::string> lastPreset_; // id -> token
};

} // namespace obs_onvif::registry