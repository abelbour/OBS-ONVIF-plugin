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

#include "ptz_controller.h"
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
	std::shared_ptr<registry::PtzController> ptz;
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

void CopyStr(char *out, size_t cap, const std::string &s)
{
	CopyToken(out, cap, s);
}

// Runs on the PTZ controller thread. Resolves the camera (by id or name),
// then dispatches through the shared Worker (profile/service cache + shared
// transport pool) with the controller's AbortHandle wired to the transport so
// an immediate Stop cancels the in-flight move.
bool PtzExecutor(const std::string &cameraId, const registry::PtzCommand &cmd,
		 obs_onvif::AbortHandle &abort, std::string &err)
{
	std::shared_ptr<registry::Worker> w;
	registry::Camera cam;
	bool found = false;
	{
		std::lock_guard<std::mutex> lock(Guard());
		Backend &b = GetBackend();
		if (!b.cams || !b.worker) {
			err = "ABI not initialized";
			return false;
		}
		w = b.worker;
		const std::vector<Camera> table = b.cams();
		for (const auto &c : table) {
			if (c.id == cameraId || c.name == cameraId) {
				cam = c;
				found = true;
				break;
			}
		}
	}
	if (!found) {
		err = "camera not found";
		return false;
	}
	if (!cam.online) {
		err = "camera offline";
		return false;
	}
	if (cmd.stop)
		return w->StopAbortable(cam, abort, err);
	return w->MoveAbortable(cam, cmd.pan, cmd.tilt, cmd.zoom,
			       cmd.timeoutSeconds, abort, err);
}

void LoadConfigIntoBackend(Backend &b)
{
	AppConfig cfg;
	b.store.LoadAppConfig(cfg);
	if (b.worker) {
		b.worker->SetTimeout((unsigned)(cfg.soap_timeout_s * 1000));
		b.worker->SetPtzTransport(cfg.soap_keepalive,
					  cfg.ptz_auth_cache);
	}
	if (b.ptz) {
		PtzSettings s;
		s.keepalive = cfg.soap_keepalive;
		s.authCache = cfg.ptz_auth_cache;
		s.moveTimeoutSeconds =
			cfg.ptz_move_timeout_s > 0
				? (unsigned)cfg.ptz_move_timeout_s
				: 0;
		s.stopImmediate = cfg.ptz_stop_mode != "queued";
		s.minIntervalMs = cfg.ptz_min_interval_ms > 0
					  ? (unsigned)cfg.ptz_min_interval_ms
					  : 75;
		b.ptz->SetSettings(s);
	}
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
	b.worker = std::make_shared<registry::Worker>(b.creds);
	b.ptz = std::make_shared<registry::PtzController>(PtzExecutor);
	LoadConfigIntoBackend(b);
}

void Shutdown()
{
	std::shared_ptr<registry::PtzController> ptz;
	{
		std::lock_guard<std::mutex> lock(Guard());
		ptz = std::move(GetBackend().ptz);
	}
	// Join the controller thread outside Guard() (PtzExecutor takes it).
	if (ptz)
		ptz->Shutdown();

	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	b.worker.reset();
	b.cams = CameraProvider();
	b.creds = CredsProvider();
	b.collection.clear();
}

void SetCollection(const std::string &collectionUuid)
{
	std::lock_guard<std::mutex> lock(Guard());
	GetBackend().collection = collectionUuid;
}

void ApplyAppConfig(const registry::AppConfig &cfg)
{
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	if (b.worker) {
		b.worker->SetTimeout((unsigned)(cfg.soap_timeout_s * 1000));
		b.worker->SetPtzTransport(cfg.soap_keepalive,
					  cfg.ptz_auth_cache);
	}
	if (b.ptz) {
		PtzSettings s;
		s.keepalive = cfg.soap_keepalive;
		s.authCache = cfg.ptz_auth_cache;
		s.moveTimeoutSeconds =
			cfg.ptz_move_timeout_s > 0
				? (unsigned)cfg.ptz_move_timeout_s
				: 0;
		s.stopImmediate = cfg.ptz_stop_mode != "queued";
		s.minIntervalMs = cfg.ptz_min_interval_ms > 0
					  ? (unsigned)cfg.ptz_min_interval_ms
					  : 75;
		b.ptz->SetSettings(s);
	}
}

void FlushPtz()
{
	std::shared_ptr<registry::PtzController> ptz;
	{
		std::lock_guard<std::mutex> lock(Guard());
		ptz = GetBackend().ptz;
	}
	if (ptz)
		ptz->WaitIdle();
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
// M4 §6.8: move/stop enqueue on the PtzController (single in-flight, latest-wins
// coalescing, Stop purge+priority, min interval). Unknown/offline cameras are
// rejected synchronously; SOAP-level failures surface asynchronously.
int AMove(const char *cam, double pan, double tilt, double zoom)
{
	return RunCamera(cam, [&](const Camera &c) {
		Backend &b = GetBackend();
		if (!b.ptz)
			return -3;
		b.ptz->Move(c.id, pan, tilt, zoom);
		return 0;
	});
}

int AStop(const char *cam)
{
	return RunCamera(cam, [&](const Camera &c) {
		Backend &b = GetBackend();
		if (!b.ptz)
			return -3;
		b.ptz->Stop(c.id);
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

// camera configuration -------------------------------------------------------

int AGetEncoderConfig(const char *cam, obs_cast_encoder_config_t *out)
{
	if (!out)
		return -1;
	std::memset(out, 0, sizeof *out);
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::VideoEncoderConfig cfg;
		std::string err;
		if (!w.EncoderConfig(c, cfg, err))
			return -3;
		CopyStr(out->token, sizeof out->token, cfg.token);
		CopyStr(out->name, sizeof out->name, cfg.name);
		CopyStr(out->encoding, sizeof out->encoding, cfg.encoding);
		out->width = cfg.resolution.width;
		out->height = cfg.resolution.height;
		out->frame_rate = cfg.frameRate;
		out->bitrate = cfg.bitrate;
		return 0;
	});
}

int AGetEncoderOptions(const char *cam, obs_cast_encoder_options_t *out)
{
	if (!out)
		return -1;
	std::memset(out, 0, sizeof *out);
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::VideoEncoderOptions o;
		std::string err;
		if (!w.EncoderOptions(c, o, err))
			return -3;
		out->min_frame_rate = o.minFrameRate;
		out->max_frame_rate = o.maxFrameRate;
		out->min_bitrate = o.minBitrate;
		out->max_bitrate = o.maxBitrate;
		out->resolution_count = (int)std::min<size_t>(
			o.resolutions.size(), 16);
		for (int i = 0; i < out->resolution_count; ++i) {
			out->resolutions[i].width = o.resolutions[i].width;
			out->resolutions[i].height = o.resolutions[i].height;
		}
		return 0;
	});
}

int ASetEncoderConfig(const char *cam, const obs_cast_encoder_config_t *cfg)
{
	if (!cfg)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::VideoEncoderConfig v;
		v.token = cfg->token;
		v.name = cfg->name;
		v.encoding = cfg->encoding;
		v.resolution.width = cfg->width;
		v.resolution.height = cfg->height;
		v.frameRate = cfg->frame_rate;
		v.bitrate = cfg->bitrate;
		std::string err;
		if (!w.SetEncoderConfig(c, v, err))
			return -3;
		return 0;
	});
}

int AGetImagingSettings(const char *cam, obs_cast_imaging_settings_t *out)
{
	if (!out)
		return -1;
	std::memset(out, 0, sizeof *out);
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::ImagingSettings s;
		std::string err;
		if (!w.ImagingSettings(c, s, err))
			return -3;
		out->present = s.present ? 1 : 0;
		out->brightness = s.brightness;
		out->color_saturation = s.colorSaturation;
		out->contrast = s.contrast;
		out->sharpness = s.sharpness;
		return 0;
	});
}

int AGetImagingOptions(const char *cam, obs_cast_imaging_options_t *out)
{
	if (!out)
		return -1;
	std::memset(out, 0, sizeof *out);
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::ImagingOptions o;
		std::string err;
		if (!w.ImagingOptions(c, o, err))
			return -3;
		out->present = o.present ? 1 : 0;
		out->min_brightness = o.minBrightness;
		out->max_brightness = o.maxBrightness;
		out->min_color_saturation = o.minColorSaturation;
		out->max_color_saturation = o.maxColorSaturation;
		out->min_contrast = o.minContrast;
		out->max_contrast = o.maxContrast;
		out->min_sharpness = o.minSharpness;
		out->max_sharpness = o.maxSharpness;
		return 0;
	});
}

int ASetImagingSettings(const char *cam,
			const obs_cast_imaging_settings_t *settings)
{
	if (!settings)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::ImagingSettings s;
		s.present = true;
		s.brightness = settings->brightness;
		s.colorSaturation = settings->color_saturation;
		s.contrast = settings->contrast;
		s.sharpness = settings->sharpness;
		std::string err;
		if (!w.SetImagingSettings(c, s, err))
			return -3;
		return 0;
	});
}

int AGetNetworkInterfaces(const char *cam,
			  obs_cast_network_interface_t **out, int *count)
{
	*out = nullptr;
	*count = 0;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::vector<obs_onvif::NetworkInterfaceInfo> nis;
		std::string err;
		if (!w.NetworkInterfaces(c, nis, err))
			return -3;
		const size_t n = nis.size();
		auto *arr = (obs_cast_network_interface_t *)calloc(
			n ? n : 1, sizeof(obs_cast_network_interface_t));
		if (!arr)
			return -3;
		for (size_t i = 0; i < n; ++i) {
			CopyStr(arr[i].token, sizeof arr[i].token, nis[i].token);
			CopyStr(arr[i].name, sizeof arr[i].name, nis[i].name);
			CopyStr(arr[i].address, sizeof arr[i].address,
				nis[i].address);
			arr[i].enabled = nis[i].enabled ? 1 : 0;
			arr[i].dhcp = nis[i].dhcp ? 1 : 0;
			arr[i].prefix_length = nis[i].prefixLength;
		}
		*out = arr;
		*count = (int)n;
		return 0;
	});
}

void AReleaseNetworkInterfaces(obs_cast_network_interface_t *out, int count)
{
	(void)count;
	free(out);
}

int ASetNetworkInterface(const char *cam,
			 const obs_cast_network_interface_t *ni)
{
	if (!ni)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::NetworkInterfaceInfo n;
		n.token = ni->token;
		n.name = ni->name;
		n.address = ni->address;
		n.enabled = ni->enabled != 0;
		n.dhcp = ni->dhcp != 0;
		n.prefixLength = ni->prefix_length;
		std::string err;
		if (!w.SetNetworkInterface(c, n, err))
			return -3;
		return 0;
	});
}

int AGetOSDs(const char *cam, obs_cast_osd_config_t **out, int *count)
{
	*out = nullptr;
	*count = 0;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::vector<obs_onvif::OSDConfig> osds;
		std::string err;
		if (!w.OSDs(c, osds, err))
			return -3;
		const size_t n = osds.size();
		auto *arr = (obs_cast_osd_config_t *)calloc(
			n ? n : 1, sizeof(obs_cast_osd_config_t));
		if (!arr)
			return -3;
		for (size_t i = 0; i < n; ++i) {
			CopyStr(arr[i].token, sizeof arr[i].token, osds[i].token);
			CopyStr(arr[i].text, sizeof arr[i].text, osds[i].text);
			arr[i].enabled = osds[i].enabled ? 1 : 0;
		}
		*out = arr;
		*count = (int)n;
		return 0;
	});
}

void AReleaseOSDs(obs_cast_osd_config_t *out, int count)
{
	(void)count;
	free(out);
}

int ASetOSD(const char *cam, const obs_cast_osd_config_t *osd)
{
	if (!osd)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		obs_onvif::OSDConfig o;
		o.token = osd->token;
		o.text = osd->text;
		o.enabled = osd->enabled != 0;
		std::string err;
		if (!w.SetOSD(c, o, err))
			return -3;
		return 0;
	});
}

int ADeleteOSD(const char *cam, const char *osd_token)
{
	if (!osd_token || !*osd_token)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.DeleteOSD(c, osd_token, err))
			return -3;
		return 0;
	});
}

// device hostname + NTP -------------------------------------------------------
int AGetHostname(const char *cam, char *name_out, size_t cap)
{
	if (!name_out || !cap)
		return -1;
	name_out[0] = '\0';
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string name, err;
		if (!w.GetHostname(c, name, err))
			return -3;
		CopyToken(name_out, cap, name);
		return 0;
	});
}

int ASetHostname(const char *cam, const char *name)
{
	if (!name)
		return -1;
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::string err;
		if (!w.SetHostname(c, name, err))
			return -3;
		return 0;
	});
}

int ASetNTP(const char *cam, int ntp_enabled, const char *servers_csv)
{
	return RunCamera(cam, [&](const Camera &c) {
		registry::Worker &w = MakeWorker(GetBackend());
		std::vector<std::string> servers;
		if (servers_csv && *servers_csv) {
			std::string rest = servers_csv;
			for (;;) {
				const size_t comma = rest.find(',');
				std::string one =
					comma == std::string::npos
						? rest
						: rest.substr(0, comma);
				if (!one.empty())
					servers.push_back(one);
				if (comma == std::string::npos)
					break;
				rest = rest.substr(comma + 1);
			}
		}
		std::string err;
		if (!w.SetNTP(c, servers, ntp_enabled != 0, err))
			return -3;
		return 0;
	});
}

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
		AGetEncoderConfig,
		AGetEncoderOptions,
		ASetEncoderConfig,
		AGetImagingSettings,
		AGetImagingOptions,
		ASetImagingSettings,
		AGetNetworkInterfaces,
		AReleaseNetworkInterfaces,
		ASetNetworkInterface,
		AGetOSDs,
		AReleaseOSDs,
		ASetOSD,
		ADeleteOSD,
		AGetHostname,
		ASetHostname,
		ASetNTP,
	};
	return &abi;
}

extern "C" OBS_ONVIF_API void obs_onvif_abi_init(const char *config_dir,
						 const char *collection)
{
	std::lock_guard<std::mutex> lock(Guard());
	Backend &b = GetBackend();
	b.store = Store(config_dir ? config_dir : "");
	if (collection && *collection)
		b.collection = collection;
}
