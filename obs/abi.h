#pragma once

#include <functional>
#include <string>
#include <vector>

#include "camera.h"
#include "worker.h"

namespace obs_onvif::abi {

// Supplies the live camera table snapshot (discovery loop / store in the
// plugin; store in tests).
using CameraProvider = std::function<std::vector<registry::Camera>()>;

// Resolves credentials for a camera id (wincred in the plugin; fallback for
// tests).
using CredsProvider =
	std::function<registry::CameraCreds(const std::string &)>;

// Configures the ABI singleton. OBS-free: everything the ABI touches (camera
// table, worker, per-collection binding store) has no libobs dependency.
void Initialize(const std::string &configDir, CameraProvider cams,
		CredsProvider creds);

// Drops the providers; subsequent calls return empty/not-found results.
void Shutdown();

// Current scene collection UUID used by the scene-binding operations. The
// plugin resolves it from OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED; tests
// set it directly.
void SetCollection(const std::string &collectionUuid);

} // namespace obs_onvif::abi