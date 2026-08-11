#include "scene_presets.h"

#include <cstring>
#include <string>
#include <thread>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include "obs-onvif.h"

namespace obs_onvif::glue {

namespace {

struct Binding {
	std::string camera_id;
	std::string preset_token;
};

bool FindBindingForScene(const char *scene_name, Binding &out)
{
	obs_cast_abi_t *abi = obs_onvif_get_abi();
	if (!abi || !scene_name || !*scene_name)
		return false;

	const char **scenes = nullptr;
	const char **cams = nullptr;
	const char **tokens = nullptr;
	int n = 0;
	if (abi->get_bindings(&scenes, &cams, &tokens, &n) != 0) {
		if (abi->release_bindings)
			abi->release_bindings(scenes, cams, tokens, n);
		return false;
	}

	bool found = false;
	for (int i = 0; i < n; ++i) {
		if (std::strcmp(scenes[i], scene_name) == 0) {
			out.camera_id = cams[i];
			out.preset_token = tokens[i];
			found = true;
			break;
		}
	}
	if (abi->release_bindings)
		abi->release_bindings(scenes, cams, tokens, n);
	return found;
}

void FireGoto(const Binding &b)
{
	/* GotoPreset is a SOAP round-trip with a 3 s budget; never block the
	 * OBS main thread. */
	std::thread([b]() {
		obs_cast_abi_t *abi = obs_onvif_get_abi();
		if (!abi)
			return;
		try {
			abi->goto_preset(b.camera_id.c_str(), b.preset_token.c_str());
		} catch (...) {
		}
	}).detach();
}

} // namespace

void ScenePresets::OnSceneChanged()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;

	Binding b;
	if (FindBindingForScene(obs_source_get_name(scene), b) &&
	    !b.camera_id.empty())
		FireGoto(b);
	obs_source_release(scene);
}

} // namespace obs_onvif::glue