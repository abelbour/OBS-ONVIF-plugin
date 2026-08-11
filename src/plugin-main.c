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
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <obs-onvif.h>

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	/* Populated by the OBS layer (Milestone 3): scene changes, source
	 * listing, and streaming/recording lifecycle for the apply policy. */
	UNUSED_PARAMETER(event);
	UNUSED_PARAMETER(private_data);
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	obs_frontend_add_event_callback(on_frontend_event, NULL);

	/* Force the obs-free ABI object into the module's export table so
	 * obs_onvif_get_abi is resolvable by external consumers (Advanced
	 * Scene Switcher & co). */
	(void)obs_onvif_get_abi();
	obs_log(LOG_INFO, "obs-onvif ABI (%d) available",
		obs_onvif_get_abi()->api_version);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}