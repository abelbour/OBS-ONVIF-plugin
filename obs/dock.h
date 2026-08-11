#pragma once

namespace obs_onvif::glue {

/* ONVIF Control settings dock (Qt6, built when ENABLE_QT is set). Adds a
 * dock to the OBS main window plus a Tools-menu toggle. All UI lives on the
 * OBS main thread; slow ABI (SOAP) work is dispatched to worker threads. */
void LoadDock();
void UnloadDock();

} // namespace obs_onvif::glue