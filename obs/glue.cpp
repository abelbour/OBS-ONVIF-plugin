#include "glue.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <tuple>

#include "abi.h"
#include "discovery.h"
#include "dock.h"
#include "hotkeys.h"
#include "obs-onvif.h"
#include "obs_apply.h"
#include "scene_presets.h"
#include "store.h"

namespace obs_onvif::glue {

namespace {

// ABI providers: the camera table comes from the live discovery loop once it
// is running (seeded from the persisted registry); otherwise it falls back to
// the persisted store snapshot. Credentials come from the Windows Credential
// Vault.
std::vector<obs_onvif::registry::Camera> LoadCameraTable()
{
	if (obs_onvif::discovery::Running())
		return obs_onvif::discovery::Snapshot();
	obs_onvif::registry::Store store(ConfigDir());
	std::vector<obs_onvif::registry::Camera> cams;
	store.LoadCameras(cams);
	return cams;
}

obs_onvif::registry::CameraCreds LoadCredentials(const std::string &id)
{
	auto credsFor = [](const std::string &target)
		-> obs_onvif::registry::CameraCreds {
		std::string secret;
		bool found = false;
		if (!obs_onvif::registry::Store::ReadCredential(target, secret,
								 found) ||
		    !found || secret.empty())
			return obs_onvif::registry::CameraCreds{};
		const size_t at = secret.find(':');
		if (at == std::string::npos)
			return obs_onvif::registry::CameraCreds{secret, ""};
		return obs_onvif::registry::CameraCreds{secret.substr(0, at),
						      secret.substr(at + 1)};
	};
	obs_onvif::registry::CameraCreds c = credsFor(
		obs_onvif::registry::Store::CameraCredTarget(id));
	if (!c.username.empty() || !c.password.empty())
		return c;
	return credsFor(obs_onvif::registry::Store::DefaultCredTarget());
}

void OnFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		ScenePresets::OnSceneChanged();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		SetOutputActive(true);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		SetOutputActive(false);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED: {
		char *collection = obs_frontend_get_current_scene_collection();
		const std::string cname = collection ? collection : "";
		abi::SetCollection(cname);
		SetStoreContext(ConfigDir(), cname);
		if (collection)
			bfree(collection);
		break;
	}
	default:
		break;
	}
}

void InitAbi()
{
	char *conf = obs_frontend_get_current_profile_path();
	char *collection = obs_frontend_get_current_scene_collection();
	const std::string configDir = conf ? conf : "";
	const std::string cname = collection ? collection : "";
	obs_onvif_abi_init(configDir.c_str(), cname.c_str());
	abi::Initialize(configDir, LoadCameraTable, LoadCredentials);
	SetStoreContext(configDir, cname);
	if (collection)
		bfree(collection);
	if (conf)
		bfree(conf);
}

/* The discovery loop reports moves from its own thread; the apply policy must
 * only run on the OBS main thread, so marshal through obs_queue_task. */
void OnDiscoveryMoved(const std::string &camera_id,
		      const std::string &new_stream_uri,
		      const std::string &credentials)
{
	auto *payload = new std::tuple<std::string, std::string, std::string>(
		camera_id, new_stream_uri, credentials);
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			auto *t = static_cast<
				std::tuple<std::string, std::string,
					   std::string> *>(param);
			OnCameraMoved(std::get<0>(*t), std::get<1>(*t),
				      std::get<2>(*t));
			delete t;
		},
		payload, false);
}

void StartDiscovery()
{
	obs_onvif::discovery::Configure(
		ConfigDir(), LoadCredentials, OnDiscoveryMoved,
		[](const std::string &line) {
			obs_log(LOG_INFO, "[obs-onvif] discovery: %s",
				line.c_str());
		});
	obs_onvif::discovery::Start();
}

} // namespace

void Load()
{
	InitAbi();
	/* Force the obs-free ABI object into the module's export table so
	 * obs_onvif_get_abi is resolvable by external consumers. */
	(void)obs_onvif_get_abi();
	hotkeys::RegisterPresetHotkeys();
	hotkeys::RegisterMoveHotkeys();
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	StartDiscovery();
#ifdef ENABLE_QT
	LoadDock();
#endif
}

void Unload()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	hotkeys::UnregisterAll();
	obs_onvif::discovery::Stop();
	abi::Shutdown();
#ifdef ENABLE_QT
	UnloadDock();
#endif
}

} // namespace obs_onvif::glue

extern "C" void obs_onvif_glue_load(void)
{
	obs_onvif::glue::Load();
}

extern "C" void obs_onvif_glue_unload(void)
{
	obs_onvif::glue::Unload();
}