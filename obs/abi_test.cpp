// ABI test bridge (obs/abi_test.cpp). Compiled only by the aclink test
// target, never into the plugin module, so no test-only symbols leak into
// obs-onvif.dll's export table.
#include "abi.h"
#include "abi_internal.h"

#include <string>
#include <vector>

#include "registry/store.h"

extern "C" void obs_onvif_abi_test_configure(
	const obs_onvif_test_config_t *cfg)
{
	using namespace obs_onvif;
	using namespace obs_onvif::abi;
	if (!cfg) {
		Shutdown();
		return;
	}
	const std::string dir = cfg->config_dir ? cfg->config_dir : "";
	const std::string user = cfg->default_user ? cfg->default_user : "";
	const std::string pass = cfg->default_pass ? cfg->default_pass : "";
	registry::Store store(dir);

	Initialize(
		dir,
		[store]() -> std::vector<registry::Camera> {
			std::vector<registry::Camera> cams;
			store.LoadCameras(cams);
			return cams;
		},
		[store, user, pass](const std::string &id)
			-> registry::CameraCreds {
			std::string secret;
			bool found = false;
			if (store.ReadCredential(
				    registry::Store::CameraCredTarget(id),
				    secret, found) &&
			    found && !secret.empty()) {
				const size_t at = secret.find(':');
				if (at != std::string::npos)
					return {secret.substr(0, at),
						secret.substr(at + 1)};
				return {secret, ""};
			}
			return {user, pass};
		});
	if (cfg->collection_uuid)
		SetCollection(cfg->collection_uuid);
}

extern "C" void obs_onvif_abi_test_shutdown(void)
{
	obs_onvif::abi::Shutdown();
}