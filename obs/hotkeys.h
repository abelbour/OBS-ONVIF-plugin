#pragma once

namespace obs_onvif::glue {

/* Preset (Ctrl+Alt+1..9) and move (pan/tilt/zoom) hotkeys. Registered as
 * regular OBS hotkeys; the user binds keys in OBS -> Settings -> Hotkeys.
 * Pressed/released callbacks dispatch PTZ work off the UI thread. */
namespace hotkeys {
void RegisterPresetHotkeys();
void RegisterMoveHotkeys();
void UnregisterAll();
}

} // namespace obs_onvif::glue