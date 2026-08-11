// Public plugin ABI implementation (obs/abi.cpp).
//
// OBS-free by design: the ABI touches only the registry (camera table,
// per-collection scene-preset store) and the ONVIF worker. It never calls a
// libobs function, so the same object can be linked into the plugin module
// (exported from obs-onvif.dll) and into tests/aclink (a standalone C
// consumer driven against the mock camera).
#include "abi.h"
#include "abi_internal.h"
#include "obs-onvif.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "store.h"

namespace obs_onvif::abi {
namespace {

using namespace obs_onvif::registry;

struct Backend {
	Store store{""};
	CameraProvider cams;
	CredsProvider creds;
	std::string collection;
	std::shared_ptr<registry::Worker> worker;
};

Backend &GetBackend()
{
	static Backend b;
	return b;
}

std::mutex &Guard()
{
	static std::mutex m;
	return m;
}

// Finds a camera by fingerprint id or display name. The caller must hold
// Guard() while `fn` runs (the snapshot live in this scope).
template <typename Fn> int RunCamera(const char *cam, Fn &&fn)
{
	Backend &b = GetBackend();
	if (!cam || !*cam || !b.cams)
		return -1;
	const std::vector<Camera> table = b.cams();
	const Camera *found = nullptr;
	for (const auto &c : table) {
		if (c.id == cam || c.name == cam) {
			found = &c;
			break;
		}
	}
	if (!found)
		return -1;
	if (!found->online)
		return -2;
	return fn(*found);
}

registry::Worker &MakeWorker(Backend &b)
{
	if (!b.worker)
		b.worker = std::make_shared<registry::Worker>(b.creds);
	return *b.worker;
}

void CopyToken(char *out, size_t cap, const std::string &token)
{
	if (!out || !cap)
		return;
	const size_t n = token.size() >= cap ? cap - 1 : token.size();
	std::memcpy(out, token.data(), n);
	out[n] = '\0';
}

} // namespace

void Initialize(const std::string &configDir, CameraProvider cams,
		CredsProvider creds)
{
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	b.store = Store(configDir);
	b.cams = std::move(cams);
	b.creds = std::move(creds);
}

void Shutdown()
{
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	b.cams = CameraProvider();
	b.creds = CredsProvider();
	b.collection.clear();
}

void SetCollection(const std::string &collectionUuid)
{
	std::lock_guard<std::mutex> lock(Guard());
	GetBackend().collection = collectionUuid;
}

} // namespace obs_onvif::abi

namespace {

using namespace obs_onvif;
using namespace obs_onvif::abi;
using namespace obs_onvif::registry;

// camera list --------------------------------------------------------------
// The array is preceded by a size_t count so release_camera_list can free it
// without a count parameter.
int AGetCameraList(obs_cast_camera_info_t **out, int *count)
{
	*out = nullptr;
	*count = 0;
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	if (!b.cams)
		return 0;
	const std::vector<Camera> table = b.cams();

	char *base = (char *)calloc(1, sizeof(size_t) +
					    table.size() *
						    sizeof(obs_cast_camera_info_t));
	if (!base)
		return -3;
	*(size_t *)base = table.size();
	obs_cast_camera_info_t *arr =
		(obs_cast_camera_info_t *)(base + sizeof(size_t));
	for (size_t i = 0; i < table.size(); ++i) {
		arr[i].camera_id = strdup(table[i].id.c_str());
		arr[i].name = strdup(table[i].name.c_str());
		arr[i].xaddr = strdup(table[i].xaddr.c_str());
		arr[i].online = table[i].online ? 1 : 0;
	}
	*out = arr;
	*count = (int)table.size();
	return 0;
}

void AReleaseCameraList(obs_cast_camera_info_t *out)
{
	if (!out)
		return;
	char *base = (char *)out - sizeof(size_t);
	const size_t n = *(size_t *)base;
	for (size_t i = 0; i < n; ++i) {
		free((void *)out[i].camera_id);
		free((void *)out[i].name);
		free((void *)out[i].xaddr);
	}
	free(base);
}

// PTZ ----------------------------------------------------------------------
int AMove(const char *cam, double pan, double tilt, double zoom)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.Move(c, pan, tilt, zoom, err))
			return -3;
		return 0;
	});
}

int AStop(const char *cam)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.Stop(c, err))
			return -3;
		return 0;
	});
}

// presets ------------------------------------------------------------------
int AGotoPreset(const char *cam, const char *preset_token)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.GotoPreset(c, preset_token ? preset_token : "", err))
			return -3;
		return 0;
	});
}

int ASavePreset(const char *cam, const char *name, char *token_out,
		size_t token_cap)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string token, err;
		if (!w.SavePreset(c, name ? name : "", token, err))
			return -3;
		CopyToken(token_out, token_cap, token);
		return 0;
	});
}

int AListPresets(const char *cam, const char **names[], const char **tokens[],
		 int *count)
{
	*names = nullptr;
	*tokens = nullptr;
	*count = 0;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::vector<registry::PresetInfo> presets;
		std::string err;
		if (!w.ListPresets(c, presets, err))
			return -3;
		const char **ns = (const char **)calloc(
			presets.size() ? presets.size() : 1, sizeof(char *));
		const char **ts = (const char **)calloc(
			presets.size() ? presets.size() : 1, sizeof(char *));
		if (!ns || !ts) {
			free(ns);
			free(ts);
			return -3;
		}
		for (size_t i = 0; i < presets.size(); ++i) {
			ns[i] = strdup(presets[i].name.c_str());
			ts[i] = strdup(presets[i].token.c_str());
		}
		*names = ns;
		*tokens = ts;
		*count = (int)presets.size();
		return 0;
	});
}

void AReleasePresets(const char **names, const char **tokens, int count)
{
	for (int i = 0; i < count; ++i) {
		free((void *)names[i]);
		free((void *)tokens[i]);
	}
	free(names);
	free(tokens);
}

int ARenamePreset(const char *cam, const char *preset_token,
		  const char *new_name)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.RenamePreset(c, preset_token ? preset_token : "",
				    new_name ? new_name : "", err))
			return -3;
		return 0;
	});
}

int ADeletePreset(const char *cam, const char *preset_token)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.DeletePreset(c, preset_token ? preset_token : "", err))
			return -3;
		return 0;
	});
}

int AGetCurrentPreset(const char *cam, char *token_out, size_t cap)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string token;
		w.CurrentPresetToken(c.id, token);
		CopyToken(token_out, cap, token);
		return 0;
	});
}

// scene bindings (per current collection) ------------------------------------
int AGetBindings(const char **scenes[], const char **cameras[],
		 const char **tokens[], int *count)
{
	*scenes = nullptr;
	*cameras = nullptr;
	*tokens = nullptr;
	*count = 0;
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	if (b.collection.empty())
		return 0;
	Store s(b.store.ConfigDir());
	registry::CollectionState cs;
	s.LoadCollection(b.collection, cs);

	const size_t n = cs.scene_presets.size();
	const char **sn = (const char **)calloc(n ? n : 1, sizeof(char *));
	const char **ca = (const char **)calloc(n ? n : 1, sizeof(char *));
	const char **tk = (const char **)calloc(n ? n : 1, sizeof(char *));
	if (!sn || !ca || !tk) {
		free(sn);
		free(ca);
		free(tk);
		return -3;
	}
	for (size_t i = 0; i < n; ++i) {
		sn[i] = strdup(cs.scene_presets[i].scene_name.c_str());
		ca[i] = strdup(cs.scene_presets[i].camera_id.c_str());
		tk[i] = strdup(cs.scene_presets[i].preset_token.c_str());
	}
	*scenes = sn;
	*cameras = ca;
	*tokens = tk;
	*count = (int)n;
	return 0;
}

void AReleaseBindings(const char **scenes, const char **cameras,
		      const char **tokens, int count)
{
	for (int i = 0; i < count; ++i) {
		free((void *)scenes[i]);
		free((void *)cameras[i]);
		free((void *)tokens[i]);
	}
	free(scenes);
	free(cameras);
	free(tokens);
}

int ASetBinding(const char *scene_name, const char *cam,
		const char *preset_token)
{
	if (!scene_name || !*scene_name || !cam || !*cam || !preset_token)
		return -1;
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	if (b.collection.empty())
		return -1;
	Store s(b.store.ConfigDir());
	registry::CollectionState cs;
	const bool had = s.LoadCollection(b.collection, cs);
	if (!had) {
		cs.uuid = b.collection;
		cs.display_name = b.collection;
	}
	bool replaced = false;
	for (auto &p : cs.scene_presets) {
		if (p.scene_name == scene_name) {
			p.camera_id = cam;
			p.preset_token = preset_token;
			replaced = true;
			break;
		}
	}
	if (!replaced)
		cs.scene_presets.push_back(
			{cs.uuid, scene_name, cam, preset_token});
	return s.SaveCollection(cs) ? 0 : -3;
}

int AClearBinding(const char *scene_name)
{
	if (!scene_name || !*scene_name)
		return -1;
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	if (b.collection.empty())
		return -1;
	Store s(b.store.ConfigDir());
	registry::CollectionState cs;
	if (!s.LoadCollection(b.collection, cs))
		return 0;
	auto &v = cs.scene_presets;
	v.erase(std::remove_if(v.begin(), v.end(),
			       [&](const registry::ScenePreset &p) {
				       return p.scene_name == scene_name;
			       }),
		v.end());
	return s.SaveCollection(cs) ? 0 : -3;
}

} // namespace

extern "C" OBS_ONVIF_API obs_cast_abi_t *obs_onvif_get_abi(void)
{
	static obs_cast_abi_t abi = {
		1, // api_version
		AGetCameraList,
		AReleaseCameraList,
		AMove,
		AStop,
		AGotoPreset,
		ASavePreset,
		AListPresets,
		AReleasePresets,
		ARenamePreset,
		ADeletePreset,
		AGetCurrentPreset,
		AGetBindings,
		AReleaseBindings,
		ASetBinding,
		AClearBinding,
	};
	return &abi;
}
