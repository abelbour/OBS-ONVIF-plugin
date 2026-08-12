#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "camera.h"
#include "onvif_client.h"
#include "soap_client.h"

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

	// M4 §6.8: apply the SOAP keep-alive and PTZ auth-cache knobs to every
	// per-camera transport pool (current and future).
	void SetPtzTransport(bool keepalive, bool authCache);

	bool Move(const Camera &cam, double pan, double tilt, double zoom,
		  std::string &err);
	bool Stop(const Camera &cam, std::string &err);
	// PTZ controller executor entry points (M4 §6.8): route through the
	// profile/service cache and pass an AbortHandle to the transport so an
	// immediate Stop can cancel the in-flight move.
	bool MoveAbortable(const Camera &cam, double pan, double tilt,
			   double zoom, unsigned timeoutSeconds,
			   obs_onvif::AbortHandle &abort, std::string &err);
	bool StopAbortable(const Camera &cam, obs_onvif::AbortHandle &abort,
			   std::string &err);
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

	// Camera configuration (config-panel backend). All resolve the camera's
	// first profile internally and build a fresh client per call.
	// FirstProfile resolves the camera's first MediaProfile (video-source +
	// encoder tokens); the config ops use it to target the right elements.
	bool FirstProfile(const Camera &cam, obs_onvif::MediaProfile &out,
			  std::string &err);
	bool EncoderConfig(const Camera &cam, obs_onvif::VideoEncoderConfig &out,
			   std::string &err);
	bool EncoderOptions(const Camera &cam, obs_onvif::VideoEncoderOptions &out,
			   std::string &err);
	bool SetEncoderConfig(const Camera &cam,
			     const obs_onvif::VideoEncoderConfig &cfg,
			     std::string &err);
	bool ImagingSettings(const Camera &cam, obs_onvif::ImagingSettings &out,
			    std::string &err);
	bool ImagingOptions(const Camera &cam, obs_onvif::ImagingOptions &out,
			    std::string &err);
	bool SetImagingSettings(const Camera &cam,
				const obs_onvif::ImagingSettings &s,
				std::string &err);
	bool NetworkInterfaces(const Camera &cam,
			      std::vector<obs_onvif::NetworkInterfaceInfo> &out,
			      std::string &err);
	bool SetNetworkInterface(const Camera &cam,
				const obs_onvif::NetworkInterfaceInfo &ni,
				std::string &err);
	bool OSDs(const Camera &cam,
		  std::vector<obs_onvif::OSDConfig> &out, std::string &err);
	bool SetOSD(const Camera &cam, const obs_onvif::OSDConfig &cfg,
		    std::string &err);
	bool DeleteOSD(const Camera &cam, const std::string &osdToken,
		       std::string &err);

private:
	// Creates the ONVIF client for `cam` using the resolved credentials and
	// (optionally) cached capabilities / a shared transport pool.
	obs_onvif::OnvifClient BuildClient(
		const Camera &cam, const obs_onvif::Capabilities *caps,
		const std::shared_ptr<obs_onvif::SoapPool> &pool) const;

	// Per-camera SoapPool shared by every client built for that camera
	// (connection reuse + auth-mode cache). Created on first use.
	std::shared_ptr<obs_onvif::SoapPool> PoolFor(
		const std::string &cameraId) const;

	// Builds a client for `cam`, reusing the cached capabilities when the
	// cache entry is fresh (skips the GetCapabilities round trip).
	bool ClientFor(const Camera &cam, obs_onvif::OnvifClient &out,
		      std::string &err) const;

	// Resolves the camera's first MediaProfile through the profile/service
	// cache: a fresh entry answers with zero network; a miss resolves and
	// refreshes the cache. `client` may already carry capabilities.
	bool FirstProfileCached(const Camera &cam,
			       obs_onvif::OnvifClient &client,
			       obs_onvif::MediaProfile &out, std::string &err);

	struct CacheEntry {
		obs_onvif::Capabilities caps;
		obs_onvif::MediaProfile profile;
		std::chrono::steady_clock::time_point at;
	};
	void StoreCache(const std::string &cameraId,
			const obs_onvif::Capabilities &caps,
			const obs_onvif::MediaProfile &profile) const;

	std::function<CameraCreds(const std::string &)> creds_;
	unsigned timeoutMs_ = 3000;
	bool allowBasicFallback_ = true;
	bool keepalive_ = true;
	bool authCache_ = true;
	mutable std::mutex mu_;
	mutable std::map<std::string, std::string> lastPreset_; // id -> token

	mutable std::mutex cacheMu_;
	mutable std::map<std::string, CacheEntry> profileCache_;
	mutable std::map<std::string, std::shared_ptr<obs_onvif::SoapPool>>
		pools_;
};

} // namespace obs_onvif::registry