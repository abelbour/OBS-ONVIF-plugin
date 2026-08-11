#pragma once

namespace obs_onvif::glue {

/* Opens the module settings dialog (config defaults / discovery / log /
 * about). Creates the dialog on demand; safe to call from the OBS main
 * thread. No-op while one is already open. */
void ShowSettingsDialog();

} // namespace obs_onvif::glue
