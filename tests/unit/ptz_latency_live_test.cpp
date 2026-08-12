// Live PTZ latency/keep-alive test (tests/unit/ptz_latency_live_test.cpp).
//
// Talks a tiny line protocol over stdin/stdout with the Python driver
// (tests/run_ptz_latency_test.py), which hosts the mock and asserts the
// request-level invariants:
//   WARM  → one move to populate the profile/service + auth cache
//   MOVES → two more moves; the driver asserts each costs exactly 1 HTTP
//           request and that both reuse the warm keep-alive connection
//   LATENCY → with the mock delaying moves in-flight, stop must land well
//           before the delay elapses (immediate abort path)
//
// argv: ptz_latency_live_test <config_dir> <mock_http_port>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "abi_internal.h"
#include "obs-onvif.h"
#include "store.h"

using namespace obs_onvif::registry;

namespace {

bool AwaitLine(const char *expected)
{
	std::string line;
	if (!std::getline(std::cin, line))
		return false;
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	return line == expected;
}

void SendLine(const char *s)
{
	std::cout << s << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr,
			     "usage: ptz_latency_live_test <config_dir> "
			     "<mock_http_port>\n");
		return 2;
	}
	const std::string configDir = argv[1];
	const int httpPort = std::atoi(argv[2]);
	const std::string xaddr = "http://127.0.0.1:" +
				  std::to_string(httpPort) +
				  "/onvif/device_service";

	{
		Store store(configDir);
		Camera cam;
		cam.id = "sn:DS2CD2032I20170801AACH12345678";
		cam.name = "PTZ-CAM";
		cam.xaddr = xaddr;
		cam.online = true;
		if (!store.SaveCameras({cam}))
			return 2;
	}

	obs_onvif_test_config_t cfg;
	cfg.config_dir = configDir.c_str();
	cfg.default_user = "admin";
	cfg.default_pass = "pass";
	cfg.collection_uuid = "col-ptz";
	obs_onvif_abi_test_configure(&cfg);

	obs_cast_abi_t *abi = obs_onvif_get_abi();
	if (!abi) {
		obs_onvif_abi_test_shutdown();
		return 3;
	}
	const char *cam = "PTZ-CAM";

	int rc = 0;
	SendLine("READY");
	if (!AwaitLine("WARM")) {
		rc = 1;
	} else {
		if (abi->move(cam, 0.1, -0.2, 0.05) != 0)
			rc = 1;
		obs_onvif_abi_test_flush();
		SendLine("WARM_DONE");
	}

	if (!rc && AwaitLine("MOVES")) {
		for (int i = 0; i < 2; ++i) {
			if (abi->move(cam, 0.1, -0.2, 0.05) != 0)
				rc = 1;
			obs_onvif_abi_test_flush();
		}
		SendLine("MOVES_DONE");
	} else if (!rc) {
		rc = 1;
	}

	if (!rc && AwaitLine("LATENCY")) {
		// The driver has armed the mock's in-flight move delay now.
		if (abi->move(cam, 0.2, 0.0, 0.0) != 0)
			rc = 1;
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto t0 = std::chrono::steady_clock::now();
		if (abi->stop(cam) != 0)
			rc = 1;
		obs_onvif_abi_test_flush();
		const long long elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0)
				.count();
		// Far below the mock's 2s move delay: proves the in-flight move was
		// aborted (immediate stop) rather than waited out.
		const bool fast = elapsed < 1500;
		SendLine(fast ? "LATENCY_OK" : "LATENCY_FAIL");
		if (!fast) {
			std::fprintf(stderr, "stop took %lld ms\n", elapsed);
			rc = 1;
		}
	} else if (!rc) {
		rc = 1;
	}

	if (!rc && !AwaitLine("DONE"))
		rc = 1;
	if (rc)
		SendLine("FAILED");

	obs_onvif_abi_test_shutdown();
	return rc;
}
