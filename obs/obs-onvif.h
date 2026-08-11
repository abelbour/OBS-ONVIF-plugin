/*
OBS ONVIF Plugin — public plugin ABI
Copyright (C) 2026 Abel Bourne <abelbour@users.noreply.github.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

ABI version 1. Exported as a C symbol so a consumer can resolve
obs_onvif_get_abi via obs_get_module_symbol / GetProcAddress — get_sym_addr
compatible. Addressable by camera ID (fingerprint) or display name. All
strings are NUL-terminated; array results are released via the matching
release_* function.
*/

#ifndef OBS_ONVIF_H
#define OBS_ONVIF_H

#if defined(_WIN32)
#if defined(OBS_ONVIF_ABI_BUILD) && defined(OBS_ONVIF_DLL)
#define OBS_ONVIF_API __declspec(dllexport)
#elif defined(OBS_ONVIF_DLL)
#define OBS_ONVIF_API __declspec(dllimport)
#else
#define OBS_ONVIF_API
#endif
#else
#define OBS_ONVIF_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obs_cast_camera_info_s {
	const char *camera_id;
	const char *name;
	const char *xaddr;
	int online;
} obs_cast_camera_info_t;

typedef struct obs_cast_resolution_s {
	int width;
	int height;
} obs_cast_resolution_t;

/* Camera configuration panel structures. Fixed-size character buffers (not
 * heap pointers) so single-value results need no release function. */
typedef struct obs_cast_encoder_config_s {
	char token[64];
	char name[128];
	char encoding[16];
	int width;
	int height;
	double frame_rate;
	int bitrate;
} obs_cast_encoder_config_t;

typedef struct obs_cast_encoder_options_s {
	double min_frame_rate;
	double max_frame_rate;
	int min_bitrate;
	int max_bitrate;
	obs_cast_resolution_t resolutions[16];
	int resolution_count;
} obs_cast_encoder_options_t;

typedef struct obs_cast_imaging_settings_s {
	int present;
	double brightness;
	double color_saturation;
	double contrast;
	double sharpness;
} obs_cast_imaging_settings_t;

typedef struct obs_cast_imaging_options_s {
	int present;
	double min_brightness;
	double max_brightness;
	double min_color_saturation;
	double max_color_saturation;
	double min_contrast;
	double max_contrast;
	double min_sharpness;
	double max_sharpness;
} obs_cast_imaging_options_t;

typedef struct obs_cast_network_interface_s {
	char token[64];
	char name[64];
	char address[64];
	int enabled;
	int dhcp;
	int prefix_length;
} obs_cast_network_interface_t;

typedef struct obs_cast_osd_config_s {
	char token[64];
	char text[256];
	int enabled;
} obs_cast_osd_config_t;

/* ABI return codes: 0 = ok, -1 = camera/scene not found,
 * -2 = camera offline, -3 = SOAP/transport error. */
typedef struct obs_cast_abi_s {
	int api_version; /* 1 */

	/* cameras */
	int (*get_camera_list)(obs_cast_camera_info_t **out, int *count);
	void (*release_camera_list)(obs_cast_camera_info_t *out);

	/* PTZ (name-or-id addressing) */
	int (*move)(const char *cam, double pan, double tilt, double zoom);
	int (*stop)(const char *cam);

	/* presets */
	int (*goto_preset)(const char *cam, const char *preset_token);
	int (*save_preset)(const char *cam, const char *name,
			   char *token_out, size_t token_cap);
	int (*list_presets)(const char *cam, const char **names[],
			    const char **tokens[], int *count);
	void (*release_presets)(const char **names, const char **tokens,
				int count);
	int (*rename_preset)(const char *cam, const char *preset_token,
			     const char *new_name);
	int (*delete_preset)(const char *cam, const char *preset_token);
	int (*get_current_preset)(const char *cam, char *token_out,
				  size_t cap);

	/* scene->preset bindings (per current scene collection), read+write */
	int (*get_bindings)(const char **scenes[], const char **cameras[],
			    const char **tokens[], int *count);
	void (*release_bindings)(const char **scenes, const char **cameras,
				 const char **tokens, int count);
	int (*set_binding)(const char *scene_name, const char *cam,
			   const char *preset_token);
	int (*clear_binding)(const char *scene_name);

	/* camera configuration (config panel). All ops target the camera's
	 * first profile. Single-value results use fixed buffers; list results
	 * are released via the matching release_* function. */
	int (*get_encoder_config)(const char *cam,
				  obs_cast_encoder_config_t *out);
	int (*get_encoder_options)(const char *cam,
				   obs_cast_encoder_options_t *out);
	int (*set_encoder_config)(const char *cam,
				  const obs_cast_encoder_config_t *cfg);
	int (*get_imaging_settings)(const char *cam,
				    obs_cast_imaging_settings_t *out);
	int (*get_imaging_options)(const char *cam,
				   obs_cast_imaging_options_t *out);
	int (*set_imaging_settings)(const char *cam,
				    const obs_cast_imaging_settings_t *settings);
	int (*get_network_interfaces)(const char *cam,
				      obs_cast_network_interface_t **out,
				      int *count);
	void (*release_network_interfaces)(obs_cast_network_interface_t *out,
					   int count);
	int (*set_network_interface)(const char *cam,
				     const obs_cast_network_interface_t *ni);
	int (*get_osds)(const char *cam, obs_cast_osd_config_t **out,
			int *count);
	void (*release_osds)(obs_cast_osd_config_t *out, int count);
	int (*set_osd)(const char *cam, const obs_cast_osd_config_t *osd);
	int (*delete_osd)(const char *cam, const char *osd_token);
} obs_cast_abi_t;

OBS_ONVIF_API obs_cast_abi_t *obs_onvif_get_abi(void);

/* Configures the ABI backend for an OBS install: `config_dir` is the module
 * config directory (camera + collection persistence root) and `collection` is
 * the active scene-collection UUID/name used for per-collection state.
 * Optional — the ABI works with defaults (""/empty) for out-of-process use. */
OBS_ONVIF_API void obs_onvif_abi_init(const char *config_dir,
				      const char *collection);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OBS_ONVIF_H */