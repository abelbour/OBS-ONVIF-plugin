// Live typed-client test against tests/mock_onvif_server.py.
//
// usage: onvif_client_test <url> <mode>
//   mode: digest | basic   (basic also exercises the client's auth fallback)
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "check.h"
#include "onvif_client.h"

using obs_onvif::OnvifClient;

namespace {

template <typename Fn> bool Throws(Fn fn)
{
	try {
		fn();
		return false;
	} catch (const std::exception &) {
		return true;
	}
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::cerr << "usage: onvif_client_test <url> <mode>" << std::endl;
		return 2;
	}
	const std::string url = argv[1];
	const std::string mode = argv[2];

	OnvifClient client(url, "admin", "pass",
			   /*allowBasicFallback=*/true,
			   /*validateCert=*/false, /*timeoutMs=*/3000);

	// Device service.
	const auto info = client.GetDeviceInformation();
	CHECK_EQ(info.manufacturer, std::string("HIKVISION"));
	CHECK_EQ(info.model, std::string("DS-2CD2032-I"));
	CHECK(info.serialNumber.find("DS2CD2032I") != std::string::npos);
	CHECK(!info.firmwareVersion.empty());

	const auto caps = client.GetCapabilities();
	CHECK(!caps.deviceXAddr.empty());
	CHECK(!caps.mediaXAddr.empty());
	CHECK(!caps.ptzXAddr.empty());

	// Media service (authority rewritten to the mock's endpoint).
	const auto profiles = client.GetProfiles();
	CHECK_EQ(profiles.size(), size_t(2));
	CHECK_EQ(profiles[0].token, std::string("profile1"));
	CHECK_EQ(profiles[0].name, std::string("main"));
	CHECK_EQ(profiles[1].token, std::string("profile2"));
	CHECK_EQ(profiles[1].name, std::string("sub"));
	CHECK(!profiles[0].videoSourceToken.empty());
	CHECK(!profiles[0].videoEncoderToken.empty());
	CHECK(!profiles[0].ptzConfigToken.empty());

	const auto stream = client.GetStreamUri(profiles[0].token);
	CHECK(stream.uri.find("rtsp://") != std::string::npos);
	CHECK(!stream.timeout.empty());

	// PTZ service.
	client.GotoPreset(profiles[0].token, "Home1");

	// Preset lifecycle (the mock keeps real preset state).
	auto presets0 = client.GetPresets(profiles[0].token);
	CHECK_EQ(presets0.size(), size_t(1));
	CHECK_EQ(presets0[0].token, std::string("preset1"));
	CHECK_EQ(presets0[0].name, std::string("Home"));

	const std::string newToken =
		client.SetPreset(profiles[0].token, "WideAngle");
	CHECK(!newToken.empty());
	auto presets1 = client.GetPresets(profiles[0].token);
	CHECK_EQ(presets1.size(), presets0.size() + 1);

	client.RenamePreset(profiles[0].token, newToken, "Narrow");
	client.GotoPreset(profiles[0].token, newToken);
	auto renamed = client.GetPresets(profiles[0].token);
	bool foundRenamed = false;
	for (const auto &p : renamed) {
		if (p.token == newToken)
			foundRenamed = (p.name == "Narrow");
	}
	CHECK(foundRenamed);

	client.DeletePreset(profiles[0].token, newToken);
	auto presets2 = client.GetPresets(profiles[0].token);
	CHECK_EQ(presets2.size(), presets0.size());

	// Velocity moves + stop round-trip.
	client.ContinuousMove(profiles[0].token, 0.1, -0.2, 0.05, 0.5);
	client.Stop(profiles[0].token);

	// Transport failure surfaces as an exception.
	OnvifClient broken("http://127.0.0.1:1/onvif/device_service",
			   "admin", "pass", true, false, 1000);
	CHECK(Throws([&]() { broken.GetDeviceInformation(); }));

	RUN_TESTS(("onvif_client/" + mode).c_str());
}