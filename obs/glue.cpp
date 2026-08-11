#include "glue.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include "abi.h"
#include "dock.h"
#include "hotkeys.h"
#include "obs-onvif.h"
#include "obs_apply.h"
#include "scene_presets.h"
#include "store.h"

namespace obs_onvif::glue {

namespace {

// ABI providers: the camera table is a snapshot of the persisted registry
// (the continuous discovery listener that refreshes live state is the M2→M3
// bridge); credentials come from the Windows Credential Vault.
std::vector<obs_onvif::registry::Camera> LoadCameraTable()
{
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
#ifdef ENABLE_QT
	LoadDock();
#endif
}

void Unload()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	hotkeys::UnregisterAll();
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