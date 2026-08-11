#pragma once

#include <map>
#include <string>
#include <vector>

#include "camera.h"

namespace obs_onvif::registry {

// JSON persistence under %APPDATA%\obs-studio\plugin_config\obs-onvif\ plus
// per-camera secrets in the Windows Credential Vault (wincred). The config
// directory is injectable so tests can run against a throwaway temp folder
// without touching the real registry.
class Store {
public:
	explicit Store(const std::string &config_dir);

	static std::string DefaultConfigDir();

	const std::string &ConfigDir() const;

	// cameras.json ----------------------------------------------------------
	bool LoadCameras(std::vector<Camera> &out) const;
	bool SaveCameras(const std::vector<Camera> &cameras) const;

	// collection_<uuid>.json (per scene collection) --------------------------
	bool LoadCollection(const std::string &uuid, CollectionState &out) const;
	bool SaveCollection(const CollectionState &cs) const;
	bool RemoveCollection(const std::string &uuid) const;

	// config.json + policies.json ---------------------------------------------
	bool LoadAppConfig(AppConfig &cfg) const;
	bool SaveAppConfig(const AppConfig &cfg) const;
	// Persisted per-camera apply-policy overrides (id -> choice). Only the
	// choices the user explicitly "remembered" are stored here.
	bool LoadCameraPolicies(std::map<std::string, ApplyPolicyChoice> &out) const;
	bool SaveCameraPolicies(const std::map<std::string, ApplyPolicyChoice> &policies) const;

	// Windows Credential Vault ------------------------------------------------
	// target names: "obs-onvif/<camera_id>" and "obs-onvif/default".
	static bool WriteCredential(const std::string &target, const std::string &secret);
	static bool ReadCredential(const std::string &target, std::string &secret,
				   bool &found);
	static bool DeleteCredential(const std::string &target);

	static std::string CameraCredTarget(const std::string &camera_id);
	static std::string DefaultCredTarget();

	// Serialization helpers reused by tests ----------------------------------
	static std::string PolicyToString(ApplyPolicyChoice c);
	static bool ParsePolicy(const std::string &s, ApplyPolicyChoice &out);

private:
	std::string config_dir_;
};

} // namespace obs_onvif::registry