#include "store.h"

#include <windows.h>
#include <wincred.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#pragma comment(lib, "advapi32.lib")

namespace obs_onvif::registry {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::wstring Utf8ToWide(const std::string &s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
					  nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring w;
	w.resize((size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

// Reads a json file that may not exist yet. Returns true when found.
bool LoadJson(const fs::path &p, json &out)
{
	std::ifstream f(p);
	if (!f.is_open())
		return false;
	try {
		f >> out;
		return true;
	} catch (const json::exception &) {
		return false;
	}
}

bool SaveJson(const fs::path &p, const json &j)
{
	std::ofstream f(p, std::ios::trunc);
	if (!f.is_open())
		return false;
	f << j.dump(2);
	return true;
}

json CameraToJson(const Camera &c)
{
	json rtsp = json::object();
	for (const auto &kv : c.lastKnownRTSP)
		rtsp[kv.first] = kv.second;
	return json{{"id", c.id},
		    {"name", c.name},
		    {"xaddr", c.xaddr},
		    {"scope_mac", c.scopeMac},
		    {"online", c.online},
		    {"last_seen", c.lastSeen},
		    {"last_known_rtsp", rtsp}};
}

Camera CameraFromJson(const json &j)
{
	Camera c;
	c.id = j.value("id", std::string());
	c.name = j.value("name", std::string());
	c.xaddr = j.value("xaddr", std::string());
	c.scopeMac = j.value("scope_mac", std::string());
	c.online = j.value("online", false);
	c.lastSeen = j.value("last_seen", uint64_t(0));
	if (j.contains("last_known_rtsp") && j["last_known_rtsp"].is_object()) {
		for (auto it = j["last_known_rtsp"].begin();
		     it != j["last_known_rtsp"].end(); ++it)
			c.lastKnownRTSP[it.key()] = it.value().get<std::string>();
	}
	return c;
}

fs::path DirOf(const std::string &config_dir)
{
	return fs::path(config_dir);
}

} // namespace

Store::Store(const std::string &config_dir) : config_dir_(config_dir) {}

std::string Store::DefaultConfigDir()
{
	const char *appdata = std::getenv("APPDATA");
	if (!appdata || !*appdata)
		return ".";
	return std::string(appdata) +
	       "\\obs-studio\\plugin_config\\obs-onvif\\";
}

const std::string &Store::ConfigDir() const
{
	return config_dir_;
}

bool Store::LoadCameras(std::vector<Camera> &out) const
{
	out.clear();
	json j;
	// A missing file is a valid empty registry, not an error.
	if (!LoadJson(DirOf(config_dir_) / "cameras.json", j))
		return true;
	if (!j.contains("cameras") || !j["cameras"].is_array())
		return false;
	for (const auto &e : j["cameras"])
		out.push_back(CameraFromJson(e));
	return true;
}

bool Store::SaveCameras(const std::vector<Camera> &cameras) const
{
	fs::create_directories(DirOf(config_dir_));
	json arr = json::array();
	for (const auto &c : cameras)
		arr.push_back(CameraToJson(c));
	return SaveJson(DirOf(config_dir_) / "cameras.json",
			json{{"cameras", arr}});
}

bool Store::LoadCollection(const std::string &uuid, CollectionState &out) const
{
	json j;
	if (!LoadJson(DirOf(config_dir_) / ("collection_" + uuid + ".json"), j))
		return false;
	out.uuid = uuid;
	out.display_name = j.value("display_name", uuid);
	out.mappings.clear();
	if (j.contains("camera_mappings") && j["camera_mappings"].is_array()) {
		for (const auto &m : j["camera_mappings"]) {
			SourceMapping sm;
			sm.collection_uuid = uuid;
			sm.source_name = m.value("source", std::string());
			sm.camera_id = m.value("camera_id", std::string());
			sm.profileToken = m.value("profile", std::string());
			sm.auto_apply = m.value("auto_apply", true);
			out.mappings.push_back(std::move(sm));
		}
	}
	return true;
}

bool Store::SaveCollection(const CollectionState &cs) const
{
	fs::create_directories(DirOf(config_dir_));
	json arr = json::array();
	for (const auto &m : cs.mappings)
		arr.push_back({{"source", m.source_name},
			       {"camera_id", m.camera_id},
			       {"profile", m.profileToken},
			       {"auto_apply", m.auto_apply}});
	return SaveJson(DirOf(config_dir_) / ("collection_" + cs.uuid + ".json"),
			json{{"uuid", cs.uuid},
			     {"display_name", cs.display_name},
			     {"camera_mappings", arr}});
}

bool Store::RemoveCollection(const std::string &uuid) const
{
	std::error_code ec;
	fs::remove(DirOf(config_dir_) / ("collection_" + uuid + ".json"), ec);
	return !ec;
}

bool Store::LoadAppConfig(AppConfig &cfg) const
{
	json j;
	if (!LoadJson(DirOf(config_dir_) / "config.json", j))
		return false;
	cfg.discovery_interval_s = j.value("discovery_interval_s",
					   cfg.discovery_interval_s);
	cfg.hello_listener_enabled = j.value("hello_listener_enabled",
					     cfg.hello_listener_enabled);
	cfg.soap_timeout_s = j.value("soap_timeout_s", cfg.soap_timeout_s);
	cfg.soap_retry_media = j.value("soap_retry_media", cfg.soap_retry_media);
	cfg.discovery_probe_timeout_s =
		j.value("discovery_probe_timeout_s", cfg.discovery_probe_timeout_s);
	cfg.prompt_timeout_s = j.value("prompt_timeout_s", cfg.prompt_timeout_s);
	ApplyPolicyChoice policy;
	if (ParsePolicy(j.value("apply_policy", std::string()), policy))
		cfg.apply_policy = policy;
	if (j.value("default_stream", std::string()) == "low")
		cfg.default_stream = StreamChoice::Low;
	cfg.preset_hotkeys_prebound =
		j.value("preset_hotkeys_prebound", cfg.preset_hotkeys_prebound);
	cfg.restore_settings_on_reconnect =
		j.value("restore_settings_on_reconnect",
			cfg.restore_settings_on_reconnect);
	return true;
}

bool Store::SaveAppConfig(const AppConfig &cfg) const
{
	fs::create_directories(DirOf(config_dir_));
	json j{{"discovery_interval_s", cfg.discovery_interval_s},
	       {"hello_listener_enabled", cfg.hello_listener_enabled},
	       {"soap_timeout_s", cfg.soap_timeout_s},
	       {"soap_retry_media", cfg.soap_retry_media},
	       {"discovery_probe_timeout_s", cfg.discovery_probe_timeout_s},
	       {"prompt_timeout_s", cfg.prompt_timeout_s},
	       {"apply_policy", PolicyToString(cfg.apply_policy)},
	       {"default_stream",
		cfg.default_stream == StreamChoice::High ? "high" : "low"},
	       {"preset_hotkeys_prebound", cfg.preset_hotkeys_prebound},
	       {"restore_settings_on_reconnect",
		cfg.restore_settings_on_reconnect}};
	return SaveJson(DirOf(config_dir_) / "config.json", j);
}

bool Store::LoadCameraPolicies(
	std::map<std::string, ApplyPolicyChoice> &out) const
{
	json j;
	if (!LoadJson(DirOf(config_dir_) / "policies.json", j) ||
	    !j.contains("policies") || !j["policies"].is_object())
		return false;
	out.clear();
	for (auto it = j["policies"].begin(); it != j["policies"].end(); ++it) {
		ApplyPolicyChoice c;
		if (ParsePolicy(it.value().get<std::string>(), c))
			out[it.key()] = c;
	}
	return true;
}

bool Store::SaveCameraPolicies(
	const std::map<std::string, ApplyPolicyChoice> &policies) const
{
	fs::create_directories(DirOf(config_dir_));
	json obj = json::object();
	for (const auto &kv : policies)
		obj[kv.first] = PolicyToString(kv.second);
	return SaveJson(DirOf(config_dir_) / "policies.json",
			json{{"policies", obj}});
}

bool Store::WriteCredential(const std::string &target, const std::string &secret)
{
	CREDENTIAL cred{};
	cred.Type = CRED_TYPE_GENERIC;
	std::wstring targetW = Utf8ToWide(target);
	cred.TargetName = const_cast<wchar_t *>(targetW.c_str());
	cred.CredentialBlobSize = (DWORD)secret.size();
	cred.CredentialBlob = (LPBYTE)secret.data();
	// Skip the prompt/consent entirely (target name is not a mapped
	// resource), and persist across sessions.
	cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	return !!CredWrite(&cred, 0);
}

bool Store::ReadCredential(const std::string &target, std::string &secret,
			   bool &found)
{
	PCREDENTIAL cred = nullptr;
	std::wstring targetW = Utf8ToWide(target);
	if (!CredRead(targetW.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
		// ERROR_NOT_FOUND is the expected "absent" case.
		found = false;
		return true;
	}
	secret.assign((const char *)cred->CredentialBlob,
		      cred->CredentialBlobSize);
	CredFree(cred);
	found = true;
	return true;
}

bool Store::DeleteCredential(const std::string &target)
{
	std::wstring targetW = Utf8ToWide(target);
	return !!CredDelete(targetW.c_str(), CRED_TYPE_GENERIC, 0);
}

std::string Store::CameraCredTarget(const std::string &camera_id)
{
	return "obs-onvif/" + camera_id;
}

std::string Store::DefaultCredTarget()
{
	return "obs-onvif/default";
}

std::string Store::PolicyToString(ApplyPolicyChoice c)
{
	switch (c) {
	case ApplyPolicyChoice::Ask:
		return "ask";
	case ApplyPolicyChoice::Always:
		return "always";
	case ApplyPolicyChoice::Ignore:
		return "ignore";
	}
	return "ask";
}

bool Store::ParsePolicy(const std::string &s, ApplyPolicyChoice &out)
{
	if (s == "ask") {
		out = ApplyPolicyChoice::Ask;
		return true;
	}
	if (s == "always") {
		out = ApplyPolicyChoice::Always;
		return true;
	}
	if (s == "ignore") {
		out = ApplyPolicyChoice::Ignore;
		return true;
	}
	return false;
}

} // namespace obs_onvif::registry