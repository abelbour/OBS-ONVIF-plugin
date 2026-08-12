/*
OBS ONVIF Plugin
Copyright (C) 2026 Abel Bourne <abelbour@users.noreply.github.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include <util/platform.h>

#include <obs-onvif.h>
#include <glue.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-onvif", "en-US")

bool obs_module_load(void)
{
	/* Log the data directory OBS resolved for this module and whether the
	 * default locale file is actually there. OBS derives the data dir from
	 * the DLL's install location, so a misplaced install (e.g. the DLL
	 * copied into obs-plugins/64bit without its data folder) shows up here
	 * as a MISSING locale — and every obs_module_text() falls back to its
	 * raw key. */
	const char *locale = obs_module_file("locale/en-US.ini");
	obs_log(LOG_INFO,
		"obs-onvif data dir: '%s'; locale file: '%s' (%s)",
		obs_get_module_data_path(obs_current_module()),
		locale ? locale : "(unresolvable)",
		locale && os_file_exists(locale) ? "OK" : "MISSING");
	bfree((void *)locale);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_onvif_glue_load();
	obs_log(LOG_INFO, "obs-onvif ABI (%d) available",
		obs_onvif_get_abi()->api_version);
	return true;
}

void obs_module_unload(void)
{
	obs_onvif_glue_unload();
	obs_log(LOG_INFO, "plugin unloaded");
}