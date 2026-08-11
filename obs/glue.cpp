#include "glue.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include "hotkeys.h"
#include "obs-onvif.h"
#include "scene_presets.h"

namespace obs_onvif::glue {

namespace {

void OnFrontendEvent(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		ScenePresets::OnSceneChanged();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		/* Output-activity reporting to the apply policy (M3 follow-up). */
		break;
	default:
		break;
	}
}

void InitAbi()
{
	char *conf = obs_frontend_get_current_profile_path();
	char *collection = obs_frontend_get_current_scene_collection();
	obs_onvif_abi_init(conf ? conf : "", collection ? collection : "");
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
}

void Unload()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	hotkeys::UnregisterAll();
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