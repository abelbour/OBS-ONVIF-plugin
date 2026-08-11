#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "apply.h"
#include "camera.h"
#include "check.h"
#include "identity.h"
#include "onvif_client.h"
#include "registry.h"
#include "store.h"
#include "ws_discovery.h"

using namespace obs_onvif;
using namespace obs_onvif::registry;

namespace fs = std::filesystem;

static DeviceIdentity MakeIdentity(const std::string &serial)
{
	DeviceIdentity id;
	id.serialNumber = serial;
	id.scopes =
		"onvif://www.onvif.org/name/Cam01 "
		"onvif://www.onvif.org/mac/40:d8:2e:12:34:56 "
		"onvif://www.onvif.org/type/video_encoder";
	id.uuid = "urn:uuid:abc";
	return id;
}

static std::string UniqueDir()
{
	const auto now =
		std::chrono::steady_clock::now().time_since_epoch().count();
	return (fs::temp_directory_path() /
		("obs-onvif-regtest-" + std::to_string(now)))
		.string();
}

// -- unit -----------------------------------------------------------

static void TestUnidentifiable()
{
	Registry reg;
	DeviceIdentity id; // nothing to fingerprint from
	std::vector<SourceRewrite> rw;
	const auto up = reg.SeenDevice(id, "Cam", "http://10.0.0.1/x", "profile1",
				       "rtsp://10.0.0.1/1", 1, false, "", rw);
	CHECK(!up.first_seen);
	CHECK(!up.address_changed);
	CHECK(reg.CameraCount() == 0);
	CHECK_EQ(rw.size(), size_t(0));
}

static void TestSeenDeviceFirstSeen()
{
	Registry reg;
	const auto id = MakeIdentity("SN-1");
	std::vector<SourceRewrite> rw;
	const auto up = reg.SeenDevice(id, "DS-2CD2032", "http://10.0.0.1/onvif/device_service",
				       "profile1", "rtsp://10.0.0.1:554/1", 1000,
				       /*output_active=*/false, "", rw);
	CHECK(up.first_seen);
	CHECK(!up.address_changed);

	const Camera *cam = reg.FindCamera("sn:SN-1");
	CHECK(cam != nullptr);
	CHECK_EQ(cam->name, std::string("DS-2CD2032"));
	CHECK_EQ(cam->xaddr, std::string("http://10.0.0.1/onvif/device_service"));
	CHECK_EQ(cam->scopeMac, std::string("40:d8:2e:12:34:56"));
	CHECK(cam->online);
	CHECK_EQ(cam->lastKnownRTSP.at("profile1"),
		 std::string("rtsp://10.0.0.1:554/1"));
	CHECK_EQ(rw.size(), size_t(0)); // first contact: nothing to rewrite
}

static void TestSeenDeviceNoChange()
{
	Registry reg;
	const auto id = MakeIdentity("SN-2");
	std::vector<SourceRewrite> rw;
	reg.SeenDevice(id, "Cam", "http://10.0.0.1/x", "profile1",
		       "rtsp://10.0.0.1/1", 1000, false, "", rw);

	const auto up = reg.SeenDevice(id, "Cam", "http://10.0.0.1/x", "profile1",
				       "rtsp://10.0.0.1/1", 2000, false, "", rw);
	CHECK(!up.first_seen);
	CHECK(!up.address_changed);
	CHECK_EQ(rw.size(), size_t(0));
}

static void TestSeenDeviceMoveAutoApplies()
{
	Registry reg;
	const auto id = MakeIdentity("SN-3");
	std::vector<SourceRewrite> rw;
	reg.SeenDevice(id, "Cam", "http://10.0.0.1/x", "profile1",
		       "rtsp://10.0.0.1:554/1", 1000, false, "", rw);

	SourceMapping m;
	m.source_name = "CAM-101";
	m.camera_id = "sn:SN-3";
	m.profileToken = "profile1";
	reg.SetMappings({m});
	reg.Apply().TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/1");

	const auto up = reg.SeenDevice(id, "Cam", "http://10.0.0.2/x", "profile1",
				       "rtsp://10.0.0.2:554/1", 2000, false,
				       "admin:pass", rw);
	CHECK(up.address_changed);
	CHECK(up.action == ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
	CHECK_EQ(rw[0].new_url,
		 std::string("rtsp://admin:pass@10.0.0.2:554/1"));

	const Camera *cam = reg.FindCamera("sn:SN-3");
	CHECK(cam != nullptr);
	CHECK_EQ(cam->xaddr, std::string("http://10.0.0.2/x"));
	CHECK_EQ(cam->lastKnownRTSP.at("profile1"),
		 std::string("rtsp://10.0.0.2:554/1"));
}

static void TestSeenDeviceMoveLivePrompts()
{
	Registry reg;
	const auto id = MakeIdentity("SN-4");
	std::vector<SourceRewrite> rw;
	reg.SeenDevice(id, "Cam", "http://10.0.0.1/x", "profile1",
		       "rtsp://10.0.0.1:554/1", 1000, false, "", rw);

	SourceMapping m;
	m.source_name = "CAM-101";
	m.camera_id = "sn:SN-4";
	m.profileToken = "profile1";
	reg.SetMappings({m});
	reg.Apply().TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/1");

	// Live output + default policy (ask) -> prompt.
	const auto up = reg.SeenDevice(id, "Cam", "http://10.0.0.2/x", "profile1",
				       "rtsp://10.0.0.2:554/1", 2000, true,
				       "admin:pass", rw);
	CHECK(up.address_changed);
	CHECK(up.action == ApplyDecision::Prompted);
	CHECK(reg.Apply().HasPending());
	CHECK_EQ(rw.size(), size_t(0));

	// User answers "apply".
	CHECK(reg.Apply().ApplyPending(rw) == ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
}

static void TestPersistRestore()
{
	const std::string dir = UniqueDir();
	Store store(dir);

	Registry reg;
	const auto id = MakeIdentity("SN-5");
	std::vector<SourceRewrite> rw;
	reg.SeenDevice(id, "Cam", "http://10.0.0.5/x", "profile1",
		       "rtsp://10.0.0.5:554/1", 1000, false, "", rw);
	CHECK(reg.Persist(store));

	Registry reg2;
	CHECK(reg2.Restore(store));
	CHECK(reg2.CameraCount() == 1);
	const Camera *cam = reg2.FindCamera("sn:SN-5");
	CHECK(cam != nullptr);
	CHECK_EQ(cam->xaddr, std::string("http://10.0.0.5/x"));
	CHECK(cam->online);

	fs::remove_all(dir);
}

// -- live (mock-driven) ---------------------------------------------

// argv: bin seed <httpHost> <httpPort> <udpPort> <configDir>
static void LiveSeed(int argc, char **argv)
{
	if (argc < 6) {
		CHECK(true); // malformed driver invocation
		return;
	}
	const std::string httpHost = argv[2];
	const int httpPort = std::atoi(argv[3]);
	const int udpPort = std::atoi(argv[4]);
	const std::string configDir = argv[5];

	// 1. Discovery round-trip against the mock UDP responder.
	intptr_t sock = OpenUdpSocket(0, false, false);
	CHECK(sock != -1);
	CHECK(SendUdp(sock, BuildProbe("urn:uuid:seed"), "127.0.0.1",
		      (uint16_t)udpPort) > 0);
	std::string reply;
	CHECK(RecvUdp(sock, reply, 5000) > 0);
	std::vector<DiscoveredDevice> devs;
	CHECK(ParseDiscoveryResponse(reply, devs));
	CHECK_EQ(devs.size(), size_t(1));
	CHECK(!devs[0].xaddrs.empty());
	const std::string base = devs[0].xaddrs[0];

	// 2. Resolve the camera and fingerprint it.
	OnvifClient cl(base, "admin", "pass");
	const DeviceInfo info = cl.GetDeviceInformation();
	DeviceIdentity id;
	id.serialNumber = info.serialNumber;
	id.hardwareId = info.hardwareId;
	id.scopes = devs[0].scopes;
	id.uuid = devs[0].uuid;

	// 3. Seed the registry (first contact) and persist.
	Store store(configDir);
	Registry reg;
	reg.Restore(store);
	const StreamUriResult stream = cl.GetStreamUri("profile1");
	std::vector<SourceRewrite> rw;
	auto up = reg.SeenDevice(id, info.model, base, "profile1", stream.uri,
				  1, false, "", rw);
	CHECK(up.first_seen);
	const Camera *cam = reg.FindCamera(BuildFingerprint(id));
	CHECK(cam != nullptr);
	CHECK_EQ(cam->xaddr, base);
	CHECK(cam->online);
	CHECK_EQ(cam->lastKnownRTSP.at("profile1"), stream.uri);

	// 4. Re-contact on the same address: no move.
	up = reg.SeenDevice(id, info.model, base, "profile1", stream.uri, 2,
			    false, "", rw);
	CHECK(!up.address_changed);

	CHECK(reg.Persist(store));
	CloseUdpSocket(sock);
}

// argv: bin rehome <httpHost> <httpPort> <udpPort> <configDir>
static void LiveRehome(int argc, char **argv)
{
	if (argc < 6) {
		CHECK(true);
		return;
	}
	const std::string httpHost = argv[2];
	const int httpPort = std::atoi(argv[3]);
	const int udpPort = std::atoi(argv[4]);
	const std::string configDir = argv[5];

	// 1. Restore the camera seeded by the previous phase.
	Store store(configDir);
	Registry reg;
	CHECK(reg.Restore(store));
	CHECK(reg.CameraCount() == 1);
	const Camera *seeded = reg.FindCamera("sn:DS2CD2032I20170801AACH12345678");
	CHECK(seeded != nullptr);
	CHECK(seeded->xaddr.find("127.0.0.1") != std::string::npos);

	// 2. Re-discover: the mock now answers from a new loopback host.
	intptr_t sock = OpenUdpSocket(0, false, false);
	CHECK(sock != -1);
	CHECK(SendUdp(sock, BuildProbe("urn:uuid:rehome"), "127.0.0.1",
		      (uint16_t)udpPort) > 0);
	std::string reply;
	CHECK(RecvUdp(sock, reply, 5000) > 0);
	std::vector<DiscoveredDevice> devs;
	CHECK(ParseDiscoveryResponse(reply, devs));
	CHECK_EQ(devs.size(), size_t(1));
	CHECK(!devs[0].xaddrs.empty());
	const std::string base = devs[0].xaddrs[0];
	CHECK(base.find(httpHost) != std::string::npos);

	// 3. Resolve identity + fresh stream URI on the new host.
	OnvifClient cl(base, "admin", "pass");
	const DeviceInfo info = cl.GetDeviceInformation();
	DeviceIdentity id;
	id.serialNumber = info.serialNumber;
	id.hardwareId = info.hardwareId;
	id.scopes = devs[0].scopes;
	id.uuid = devs[0].uuid;
	const StreamUriResult stream = cl.GetStreamUri("profile1");
	CHECK(stream.uri.find(httpHost) != std::string::npos);

	// 4. Map CAM-101 to this camera (as M3 would) and observe the rewrite.
	const std::string fp = BuildFingerprint(id);
	SourceMapping m;
	m.source_name = "CAM-101";
	m.camera_id = fp;
	m.profileToken = "profile1";
	reg.SetMappings({m});
	reg.Apply().TrackSourceUrl("CAM-101",
				   "rtsp://127.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	const auto up = reg.SeenDevice(id, info.model, base, "profile1",
				       stream.uri, 3, /*output_active=*/false,
				       "admin:pass", rw);
	CHECK(up.address_changed);
	CHECK(up.action == ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
	CHECK_EQ(rw[0].new_url,
		 std::string("rtsp://admin:pass@") + httpHost +
			 ":554/Streaming/Channels/101");

	// 5. Registry state reflects the move; persists for the next run.
	const Camera *moved = reg.FindCamera(fp);
	CHECK(moved != nullptr);
	CHECK_EQ(moved->xaddr, base);
	CHECK_EQ(moved->lastKnownRTSP.at("profile1"), stream.uri);
	CHECK(reg.Persist(store));
	CloseUdpSocket(sock);
}

int main(int argc, char **argv)
{
	TestUnidentifiable();
	TestSeenDeviceFirstSeen();
	TestSeenDeviceNoChange();
	TestSeenDeviceMoveAutoApplies();
	TestSeenDeviceMoveLivePrompts();
	TestPersistRestore();

	if (argc >= 2 && std::string(argv[1]) == "seed") {
		LiveSeed(argc, argv);
	} else if (argc >= 2 && std::string(argv[1]) == "rehome") {
		LiveRehome(argc, argv);
	} else {
		CHECK(true); // not launched as the live test
	}
	RUN_TESTS("registry");
}