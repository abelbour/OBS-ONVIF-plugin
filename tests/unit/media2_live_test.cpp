// Live Media2 test (tests/unit/media2_live_test.cpp). Drives the Media2-first
// profile/stream/encoder path and its classic fallback against the mock:
//   mode media2   -- camera advertises a Media2 endpoint that answers; asserts
//                    GetProfiles returns Media2 profiles, stream URIs and
//                    encoder config/options/set go through the Media2 service.
//   mode fallback -- the Media2 endpoint faults; asserts GetProfiles falls
//                    back to classic Media (a Media2 fault is never a hard
//                    failure).
//
// argv: media2_live_test <mock_http_port> <media2|fallback>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "onvif_client.h"

using obs_onvif::OnvifClient;
using obs_onvif::VideoEncoderConfig;

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr,
			     "usage: media2_live_test <mock_http_port> "
			     "<media2|fallback>\n");
		return 2;
	}
	const int httpPort = std::atoi(argv[1]);
	const std::string mode = argv[2];
	const std::string url = "http://127.0.0.1:" +
				std::to_string(httpPort) +
				"/onvif/device_service";

	OnvifClient client(url, "admin", "pass",
			   /*allowBasicFallback=*/true,
			   /*validateCert=*/false, /*timeoutMs=*/3000);

	const auto caps = client.GetCapabilities();
	CHECK(!caps.media2XAddr.empty()); // both flavors advertise Media2

	const auto profiles = client.GetProfiles();
	if (mode == "media2") {
		CHECK_EQ(profiles.size(), size_t(2));
		CHECK(profiles[0].media2);
		CHECK_EQ(profiles[0].token, std::string("mp1"));
		CHECK_EQ(profiles[0].name, std::string("main2"));
		CHECK(!profiles[0].videoSourceToken.empty());
		CHECK(!profiles[0].videoEncoderToken.empty());

		// Stream URI comes from the Media2 service (profile ownership).
		const auto stream = client.GetStreamUri(profiles[0]);
		CHECK(stream.uri.find("rtsp://") != std::string::npos);

		// Encoder config / options / set through Media2.
		const auto cfgs = client.GetVideoEncoderConfigurations2();
		VideoEncoderConfig enc;
		bool found = false;
		for (const auto &c : cfgs) {
			if (c.token == profiles[0].videoEncoderToken) {
				enc = c;
				found = true;
			}
		}
		CHECK(found);
		CHECK_EQ(enc.encoding, std::string("video/H264"));
		CHECK_EQ(enc.resolution.width, 1920);
		CHECK_EQ(enc.resolution.height, 1080);

		const auto opts = client.GetVideoEncoderConfigurationOptions2(
			profiles[0].videoEncoderToken);
		// Media2 options carry bitrate + resolutions but no frame-rate range.
		CHECK(opts.max_bitrate > 0);
		CHECK_EQ(opts.max_frame_rate, 0.0);
		CHECK(opts.resolutions.size() >= 3);

		enc.resolution.width = 1280;
		enc.resolution.height = 720;
		enc.frameRate = 15.0;
		enc.bitrate = 2048;
		client.SetVideoEncoderConfiguration2(enc);

		const auto cfgs2 = client.GetVideoEncoderConfigurations2();
		bool updated = false;
		for (const auto &c : cfgs2) {
			if (c.token == profiles[0].videoEncoderToken) {
				updated = c.resolution.width == 1280 &&
					  c.resolution.height == 720 &&
					  c.bitrate == 2048;
			}
		}
		CHECK(updated);
	} else {
		// GetProfiles2 faults on the mock -> classic Media fallback.
		CHECK_EQ(profiles.size(), size_t(2));
		CHECK(!profiles[0].media2);
		CHECK_EQ(profiles[0].token, std::string("profile1"));
		CHECK_EQ(profiles[0].name, std::string("main"));
		const auto stream = client.GetStreamUri(profiles[0]);
		CHECK(stream.uri.find("rtsp://") != std::string::npos);
	}

	RUN_TESTS(("media2_live/" + mode).c_str());
}
