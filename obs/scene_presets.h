#pragma once

namespace obs_onvif::glue {

/* Scene -> preset bindings. Fired from the frontend SCENE_CHANGED event; the
 * actual PTZ dispatch (SOAP, may block) runs off the UI thread. */
namespace ScenePresets {
void OnSceneChanged();
}

} // namespace obs_onvif::glue