#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace obs_onvif::registry {

// Live-output policy for a moved camera (PLAN.md §Live-output policy).
enum class ApplyPolicyChoice { Ask, Always, Ignore };

// Default stream quality when auto-pairing a mapped source.
enum class StreamChoice { High, Low };

// Persistent, stable camera record. `id` is the fingerprint from identity.cpp.
// Username/password are mirrored from wincred by the store at load time and
// are never written into the JSON files.
struct Camera {
	std::string id;                              // stable fingerprint
	std::string name;                            // display name (editable)
	std::string username;                        // from wincred at load
	std::string password;
	std::string xaddr;                           // device-service XAddr
	std::string scopeMac;                        // hardware MAC from discovery
	std::map<std::string, std::string> lastKnownRTSP; // profileToken -> rtsp url
	bool online = false;
	uint64_t lastSeen = 0;                       // wall-clock ms
	// User added this camera by IP. It stays "online" (selectable for config /
	// PTZ) regardless of multicast liveness, because its reachability was
	// verified with a direct unicast call at add time.
	bool manual = false;
};

// A camera attached to one OBS Media Source, namespaced per scene collection.
struct SourceMapping {
	std::string collection_uuid; // stable per-scene-collection key
	std::string source_name;     // OBS source name
	std::string camera_id;
	std::string profileToken;
	bool auto_apply = true;      // rewrite URL + restart on IP change
};

// Scene-activation -> camera preset binding (fired by scene_presets, M3).
struct ScenePreset {
	std::string collection_uuid;
	std::string scene_name;
	std::string camera_id;
	std::string preset_token;
};

// Global application settings (store/config.json).
struct AppConfig {
	int discovery_interval_s = 60;
	bool hello_listener_enabled = true;
	int soap_timeout_s = 5;
	bool soap_retry_media = true;
	int discovery_probe_timeout_s = 3;
	int prompt_timeout_s = 30;
	ApplyPolicyChoice apply_policy = ApplyPolicyChoice::Ask;
	StreamChoice default_stream = StreamChoice::High;
	bool restore_settings_on_reconnect = false;
	// M4 §6.8 — PTZ transport/motor-control knobs (Settings → PTZ tab).
	bool soap_keepalive = true;      // reuse WinHTTP connections across SOAP calls
	bool ptz_auth_cache = true;      // remember negotiated auth mode per camera
	int ptz_move_timeout_s = 0;      // 0 = ContinuousMove without Timeout (until Stop)
	std::string ptz_stop_mode = "immediate"; // "immediate" (abort) | "queued"
	int ptz_min_interval_ms = 75;    // hard floor between movement dispatches
};

// Per-scene-collection state (store/collection_<uuid>.json).
struct CollectionState {
	std::string uuid;
	std::string display_name;
	std::vector<SourceMapping> mappings;
	std::vector<ScenePreset> scene_presets;
};

} // namespace obs_onvif::registry