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

// Builds a Hello or Bye announcement for a camera identified by scope MAC and
// address. Used to exercise the loop's cheap presence paths without a camera.
std::string PresenceEnvelope(const char *action, const std::string &mac,
			     const std::string &xaddr)
{
	return std::string(
		"<?xml version=\"1.0\"?>\n"
		"<e:Envelope "
		"xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
		"xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
		"xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">\n"
		" <e:Header>\n"
		"  <w:MessageID>urn:uuid:1-2-3-4-5</w:MessageID>\n"
		"  <w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>\n"
		"  <w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/" +
		std::string(action) + "</w:Action>\n"
		" </e:Header>\n"
		" <e:Body>\n"
		"  <d:" + std::string(action) + ">\n"
		"   <w:Address>urn:uuid:7a7b7c7d-7e7f-4a1b-9c2d-c0ffee123456"
		"</w:Address>\n"
		"   <d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
		"   <d:Scopes>onvif://www.onvif.org/name/MAC-CAM "
		"onvif://www.onvif.org/mac/" + mac + "</d:Scopes>\n"
		"   <d:XAddrs>" + xaddr + "</d:XAddrs>\n"
		"  </d:" + std::string(action) + ">\n"
		" </e:Body>\n"
		"</e:Envelope>");
}

void RunPhase(const std::string &phase, const std::string &host,
	      int httpPort, int udpPort, const std::string &configDir)
{
	(void)httpPort;

	// The presence phase seeds its own store BEFORE Configure so the loop
	// loads exactly the camera under test (Configure seeds once per process).
	if (phase == "presence") {
		const std::string deadXaddr =
			"http://127.0.0.1:1/onvif/device_service";
		registry::Store store(configDir);
		registry::Camera cam;
		cam.id = "mac:aa:bb:cc:dd:ee:ff";
		cam.name = "MAC-CAM";
		cam.xaddr = deadXaddr;
		cam.online = false;
		cam.lastSeen = 0;
		CHECK(store.SaveCameras({cam}));
	}

	obs_onvif::discovery::Configure(configDir, TestCreds, OnMoved);

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
	} else if (phase == "presence") {
		// DHCP-sack: Hello refreshes presence and Bye goes offline, both
		// with ZERO SOAP. The seeded camera's xaddr points at a dead port,
		// so any resolution attempt would fail — proving the cheap paths.
		const std::string deadXaddr =
			"http://127.0.0.1:1/onvif/device_service";
		CHECK_EQ(obs_onvif::discovery::Snapshot().size(), size_t(1));
		CHECK(!obs_onvif::discovery::Snapshot()[0].online);

		// Hello at the SAME address -> pure presence refresh (no SOAP).
		obs_onvif::discovery::HandleDiscoveryDatagram(
			PresenceEnvelope("Hello", "aa:bb:cc:dd:ee:ff",
					 deadXaddr));
		CHECK(obs_onvif::discovery::Snapshot()[0].online);

		// Bye -> immediate offline (again no SOAP).
		obs_onvif::discovery::HandleDiscoveryDatagram(
			PresenceEnvelope("Bye", "aa:bb:cc:dd:ee:ff", deadXaddr));
		CHECK(!obs_onvif::discovery::Snapshot()[0].online);
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
