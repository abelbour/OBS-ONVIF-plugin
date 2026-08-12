// Live camera-config test (tests/unit/config_live_test.cpp).
//
// Seeds a store with one camera pointing at the mock, configures the ABI
// singleton through the test bridge, then drives every config-panel operation
// through the public ABI and asserts the mock round-trips each change:
//   encoder  read config + options, set resolution/fps/bitrate, re-read
//   imaging  read settings + options, set the four image values, re-read
//   network  read interfaces, switch the first to static IP, re-read
//   osd      read overlays, change the text, re-read, delete, re-read
//
// argv: config_live_test <config_dir> <mock_http_port>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "abi_internal.h"
#include "check.h"
#include "obs-onvif.h"
#include "store.h"

using namespace obs_onvif::registry;

static bool Near(double a, double b, double tol)
{
	return a > b - tol && a < b + tol;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage: config_live_test <config_dir> <mock_http_port>\n");
		return 2;
	}
	const std::string configDir = argv[1];
	const int httpPort = std::atoi(argv[2]);
	const std::string xaddr = "http://127.0.0.1:" + std::to_string(httpPort) +
				  "/onvif/device_service";

	// Seed the store with one camera pointing at the mock.
	{
		Store store(configDir);
		Camera cam;
		cam.id = "sn:DS2CD2032I20170801AACH12345678";
		cam.name = "CONFIG-CAM";
		cam.xaddr = xaddr;
		cam.online = true;
		CHECK(store.SaveCameras({cam}));
	}

	obs_onvif_test_config_t cfg;
	cfg.config_dir = configDir.c_str();
	cfg.default_user = "admin";
	cfg.default_pass = "pass";
	cfg.collection_uuid = "col-config";
	obs_onvif_abi_test_configure(&cfg);

	obs_cast_abi_t *abi = obs_onvif_get_abi();
	CHECK(abi != nullptr);
	if (!abi) {
		obs_onvif_abi_test_shutdown();
		return 3;
	}
	const char *cam = "CONFIG-CAM";

	// Encoder -------------------------------------------------------------
	obs_cast_encoder_config_t enc{};
	CHECK(abi->get_encoder_config(cam, &enc) == 0);
	CHECK_EQ(std::string(enc.token), std::string("enc1"));
	CHECK_EQ(std::string(enc.name), std::string("main"));
	CHECK_EQ(enc.width, 1920);
	CHECK_EQ(enc.height, 1080);
	CHECK(Near(enc.frame_rate, 25.0, 0.01));
	CHECK_EQ(enc.bitrate, 4096);

	obs_cast_encoder_options_t opt{};
	CHECK(abi->get_encoder_options(cam, &opt) == 0);
	CHECK(opt.min_frame_rate > 0.0);
	CHECK_EQ(opt.max_frame_rate, 30.0);
	CHECK(opt.min_bitrate > 0);
	CHECK_EQ(opt.max_bitrate, 8192);
	CHECK(opt.resolution_count >= 3);

	enc.width = 1280;
	enc.height = 720;
	enc.frame_rate = 15.0;
	enc.bitrate = 2048;
	CHECK(abi->set_encoder_config(cam, &enc) == 0);
	obs_cast_encoder_config_t enc2{};
	CHECK(abi->get_encoder_config(cam, &enc2) == 0);
	CHECK_EQ(enc2.width, 1280);
	CHECK_EQ(enc2.height, 720);
	CHECK(Near(enc2.frame_rate, 15.0, 0.01));
	CHECK_EQ(enc2.bitrate, 2048);

	// Imaging ------------------------------------------------------------
	obs_cast_imaging_settings_t im{};
	CHECK(abi->get_imaging_settings(cam, &im) == 0);
	CHECK(im.present);
	CHECK(Near(im.brightness, 50.0, 0.01));

	obs_cast_imaging_options_t iopt{};
	CHECK(abi->get_imaging_options(cam, &iopt) == 0);
	CHECK(iopt.present);
	CHECK_EQ(iopt.max_brightness, 100.0);

	im.brightness = 30.0;
	im.color_saturation = 60.0;
	im.contrast = 70.0;
	im.sharpness = 80.0;
	CHECK(abi->set_imaging_settings(cam, &im) == 0);
	obs_cast_imaging_settings_t im2{};
	CHECK(abi->get_imaging_settings(cam, &im2) == 0);
	CHECK(Near(im2.brightness, 30.0, 0.01));
	CHECK(Near(im2.color_saturation, 60.0, 0.01));
	CHECK(Near(im2.contrast, 70.0, 0.01));
	CHECK(Near(im2.sharpness, 80.0, 0.01));

	// Network ------------------------------------------------------------
	obs_cast_network_interface_t *nifs = nullptr;
	int ncount = 0;
	CHECK(abi->get_network_interfaces(cam, &nifs, &ncount) == 0);
	CHECK(ncount >= 2);
	CHECK_EQ(std::string(nifs[0].token), std::string("eth0"));
	CHECK(nifs[0].dhcp == 1);
	obs_cast_network_interface_t ni = nifs[0];
	if (abi->release_network_interfaces)
		abi->release_network_interfaces(nifs, ncount);
	ni.dhcp = 0;
	std::snprintf(ni.address, sizeof ni.address, "%s", "10.0.0.5");
	ni.prefix_length = 24;
	CHECK(abi->set_network_interface(cam, &ni) == 0);

	obs_cast_network_interface_t *nifs2 = nullptr;
	int ncount2 = 0;
	CHECK(abi->get_network_interfaces(cam, &nifs2, &ncount2) == 0);
	CHECK_EQ(std::string(nifs2[0].address), std::string("10.0.0.5"));
	CHECK(nifs2[0].dhcp == 0);
	if (abi->release_network_interfaces)
		abi->release_network_interfaces(nifs2, ncount2);

	// OSD -----------------------------------------------------------------
	obs_cast_osd_config_t *osds = nullptr;
	int ocount = 0;
	CHECK(abi->get_osds(cam, &osds, &ocount) == 0);
	CHECK_EQ(ocount, 1);
	CHECK_EQ(std::string(osds[0].text), std::string("CAM-01"));
	obs_cast_osd_config_t osd = osds[0];
	if (abi->release_osds)
		abi->release_osds(osds, ocount);
	std::snprintf(osd.text, sizeof osd.text, "%s", "FRONT");
	osd.enabled = 1;
	CHECK(abi->set_osd(cam, &osd) == 0);

	obs_cast_osd_config_t *osds2 = nullptr;
	int ocount2 = 0;
	CHECK(abi->get_osds(cam, &osds2, &ocount2) == 0);
	CHECK_EQ(ocount2, 1);
	CHECK_EQ(std::string(osds2[0].text), std::string("FRONT"));
	if (abi->release_osds)
		abi->release_osds(osds2, ocount2);

	CHECK(abi->delete_osd(cam, "osd1") == 0);
	obs_cast_osd_config_t *osds3 = nullptr;
	int ocount3 = 0;
	CHECK(abi->get_osds(cam, &osds3, &ocount3) == 0);
	CHECK_EQ(ocount3, 0);
	if (abi->release_osds)
		abi->release_osds(osds3, ocount3);

	// Hostname + NTP (M5b) -----------------------------------------------
	char hn[128] = {};
	CHECK(abi->get_hostname(cam, hn, sizeof hn) == 0);
	CHECK_EQ(std::string(hn), std::string("mock-cam"));
	CHECK(abi->set_hostname(cam, "obs-cam") == 0);
	char hn2[128] = {};
	CHECK(abi->get_hostname(cam, hn2, sizeof hn2) == 0);
	CHECK_EQ(std::string(hn2), std::string("obs-cam"));
	CHECK(abi->set_ntp(cam, 0, "10.0.0.5") == 0);
	CHECK(abi->set_ntp(cam, 1, "") == 0);

	obs_onvif_abi_test_shutdown();
	RUN_TESTS("config_live");
}
