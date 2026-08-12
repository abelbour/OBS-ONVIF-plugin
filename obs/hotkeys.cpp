#include "hotkeys.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <obs-module.h>

#include "obs-onvif.h"

namespace obs_onvif::glue {
namespace hotkeys {

namespace {

constexpr int kPresetCount = 9;

std::vector<obs_hotkey_id> g_ids;

bool FirstOnlineCameraId(std::string &out)
{
	obs_cast_abi_t *abi = obs_onvif_get_abi();
	if (!abi)
		return false;
	obs_cast_camera_info_t *cams = nullptr;
	int n = 0;
	if (abi->get_camera_list(&cams, &n) != 0)
		return false;
	bool ok = false;
	for (int i = 0; i < n; ++i) {
		if (cams[i].online) {
			out = cams[i].camera_id;
			ok = true;
			break;
		}
	}
	if (abi->release_camera_list)
		abi->release_camera_list(cams);
	return ok;
}

/* Runs `fn` on a worker thread (the ABI PTZ calls perform blocking SOAP).
 * Templated so lambdas with captures can be forwarded to the worker thread. */
template <typename Fn> void FireAsync(Fn &&fn)
{
	std::thread([fn = std::forward<Fn>(fn)]() {
		obs_cast_abi_t *abi = obs_onvif_get_abi();
		if (!abi)
			return;
		try {
			fn(abi);
		} catch (...) {
		}
	}).detach();
}

const char *PresetTokenFor(int n)
{
	static const char *kToken[] = {"preset1", "preset2", "preset3",
				       "preset4", "preset5", "preset6",
				       "preset7", "preset8", "preset9"};
	return kToken[n - 1];
}

void OnPresetHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	const int n = (int)(intptr_t)data;
	FireAsync([n](obs_cast_abi_t *abi) {
		std::string cam;
		if (FirstOnlineCameraId(cam))
			abi->goto_preset(cam.c_str(), PresetTokenFor(n));
	});
}

struct MoveKey {
	double pan;
	double tilt;
	double zoom;
};

void OnMoveHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	const auto *key = static_cast<const MoveKey *>(data);
	/* M4 §6.8: the ABI enqueues on the PtzController (non-blocking), so
	 * this can run directly on the OBS thread. */
	obs_cast_abi_t *abi = obs_onvif_get_abi();
	if (!abi)
		return;
	std::string cam;
	if (!FirstOnlineCameraId(cam))
		return;
	if (pressed)
		abi->move(cam.c_str(), key->pan, key->tilt, key->zoom);
	else
		abi->stop(cam.c_str());
}

void RegisterMoveKey(const char *name, const char *description, double pan,
		     double tilt, double zoom)
{
	auto *key = new MoveKey{pan, tilt, zoom};
	g_ids.push_back(obs_hotkey_register_frontend(
		(std::string("obs-onvif.move_") + name).c_str(), description,
		OnMoveHotkey, key));
}

} // namespace

void RegisterPresetHotkeys()
{
	for (int i = 1; i <= kPresetCount; ++i) {
		const std::string id =
			std::string("obs-onvif.preset_") + std::to_string(i);
		const std::string desc =
			std::string("ONVIF Preset ") + std::to_string(i);
		g_ids.push_back(obs_hotkey_register_frontend(
			id.c_str(), desc.c_str(), OnPresetHotkey,
			(void *)(intptr_t)i));
	}
}

void RegisterMoveHotkeys()
{
	RegisterMoveKey("pan_left", "ONVIF Pan Left", -0.25, 0.0, 0.0);
	RegisterMoveKey("pan_right", "ONVIF Pan Right", 0.25, 0.0, 0.0);
	RegisterMoveKey("tilt_up", "ONVIF Tilt Up", 0.0, 0.25, 0.0);
	RegisterMoveKey("tilt_down", "ONVIF Tilt Down", 0.0, -0.25, 0.0);
	RegisterMoveKey("zoom_in", "ONVIF Zoom In", 0.0, 0.0, 0.2);
	RegisterMoveKey("zoom_out", "ONVIF Zoom Out", 0.0, 0.0, -0.2);
	RegisterMoveKey("stop", "ONVIF Stop Move", 0.0, 0.0, 0.0);
}

void UnregisterAll()
{
	for (obs_hotkey_id id : g_ids)
		obs_hotkey_unregister(id);
	g_ids.clear();
}

} // namespace hotkeys
} // namespace obs_onvif::glue