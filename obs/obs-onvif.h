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
} obs_cast_abi_t;

OBS_ONVIF_API obs_cast_abi_t *obs_onvif_get_abi(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OBS_ONVIF_H */