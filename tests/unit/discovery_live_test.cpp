// Live discovery-loop test (tests/unit/discovery_live_test.cpp).
//
// Drives the M2→M3 discovery bridge's contact resolution against the mock
// server without multicast: Configure() sets the credentials resolver + move
// callback, then ProbeOnce() sends a unicast Probe and processes the reply
// through the same path the background loop uses.
//
// argv: seed|rehome <httpHost> <httpPort> <udpPort> <configDir>
//   seed    -- fresh config dir; first contact registers the camera, persists
//              it, and must NOT report a move.
//   rehome  -- config dir seeded by the previous phase; a re-discovery on a
//              new loopback host must report the move with the new stream URI.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "camera.h"
#include "check.h"
#include "discovery.h"
#include "store.h"
#include "worker.h"

using namespace obs_onvif;

namespace {

std::mutex g_mu;
bool g_moved = false;
std::string g_movedId;
std::string g_movedUri;
std::string g_movedCreds;

registry::CameraCreds TestCreds(const std::string &)
{
	return {"admin", "pass"};
}

void OnMoved(const std::string &cameraId, const std::string &streamUri,
	     const std::string &credentials)
{
	std::lock_guard<std::mutex> lock(g_mu);
	g_moved = true;
	g_movedId = cameraId;
	g_movedUri = streamUri;
	g_movedCreds = credentials;
}

void RunPhase(const std::string &phase, const std::string &host,
	      int httpPort, int udpPort, const std::string &configDir)
{
	obs_onvif::discovery::Configure(configDir, TestCreds, OnMoved);
	(void)httpPort;

	if (phase == "seed") {
		// The UDP responder always listens on 127.0.0.1; the ProbeMatch
		// it returns advertises the mock's real XAddr (host:httpPort).
		obs_onvif::discovery::ProbeOnce("127.0.0.1", (uint16_t)udpPort,
						 "urn:uuid:seed");
		const auto cams = obs_onvif::discovery::Snapshot();
		CHECK_EQ(cams.size(), size_t(1));
		CHECK(cams[0].xaddr.find(host) != std::string::npos);
		CHECK(cams[0].online);
		CHECK_EQ(cams[0].lastKnownRTSP.count("profile1"), size_t(1));

		{
			std::lock_guard<std::mutex> lock(g_mu);
			CHECK(!g_moved); // first contact: nothing to rewrite
		}

		// Persisted for the next phase.
		registry::Store store(configDir);
		std::vector<registry::Camera> persisted;
		CHECK(store.LoadCameras(persisted));
		CHECK_EQ(persisted.size(), size_t(1));
	} else if (phase == "rehome") {
		// The previous phase persisted the camera; Configure seeds it.
		const auto before = obs_onvif::discovery::Snapshot();
		CHECK_EQ(before.size(), size_t(1));
		CHECK(before[0].xaddr.find("127.0.0.1") != std::string::npos);

		obs_onvif::discovery::ProbeOnce("127.0.0.1", (uint16_t)udpPort,
						 "urn:uuid:rehome");
		{
			std::lock_guard<std::mutex> lock(g_mu);
			CHECK(g_moved);
			CHECK(!g_movedId.empty());
			CHECK(g_movedUri.find(host) != std::string::npos);
			CHECK_EQ(g_movedCreds, std::string("admin:pass"));
		}

		const auto after = obs_onvif::discovery::Snapshot();
		CHECK_EQ(after.size(), size_t(1));
		CHECK(after[0].xaddr.find(host) != std::string::npos);
	}
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 6) {
		CHECK(true); // not launched as the live test
		RUN_TESTS("discovery_live");
	}

	const std::string phase = argv[1];
	const std::string host = argv[2];
	const int httpPort = std::atoi(argv[3]);
	const int udpPort = std::atoi(argv[4]);
	const std::string configDir = argv[5];

	try {
		RunPhase(phase, host, httpPort, udpPort, configDir);
	} catch (const std::exception &e) {
		std::cerr << phase << ": uncaught exception: " << e.what()
			  << std::endl;
		CHECK(false);
	}

	RUN_TESTS("discovery_live");
}
