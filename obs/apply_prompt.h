#pragma once

namespace obs_onvif::glue {

/* Shows the Apply/Defer/Ignore prompt when the apply policy has a pending
 * incident (a moved camera while streaming). Must run on the OBS main (Qt)
 * thread. No-op when no incident is pending. */
void ShowApplyPrompt();

} // namespace obs_onvif::glue