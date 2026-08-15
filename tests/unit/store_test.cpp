#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "camera.h"
#include "check.h"
#include "store.h"

using namespace obs_onvif::registry;

namespace fs = std::filesystem;

static std::string UniqueDir()
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	return (fs::temp_directory_path() /
		("obs-onvif-store-test-" + std::to_string(now.count())))
		.string();
}

static void TestCamerasRoundTrip()
{
	const std::string dir = UniqueDir();
	Store store(dir);

	std::vector<Camera> cams;
	CHECK(store.LoadCameras(cams)); // fresh dir == empty state
	CHECK(cams.empty());

	Camera a;
	a.id = "sn:123";
	a.name = "Front";
	a.xaddr = "http://10.0.0.1/onvif/device_service";
	a.scopeMac = "aa:bb:cc:dd:ee:ff";
	a.online = true;
	a.manual = true;
	a.lastSeen = 1234;
	a.lastKnownRTSP["profile1"] = "rtsp://10.0.0.1:554/s1";
	a.username = "admin";
	a.password = "pw";

	CHECK(store.SaveCameras({a}));

	CHECK(store.LoadCameras(cams));
	CHECK_EQ(cams.size(), size_t(1));
	CHECK_EQ(cams[0].id, std::string("sn:123"));
	CHECK_EQ(cams[0].name, std::string("Front"));
	CHECK_EQ(cams[0].xaddr, a.xaddr);
	CHECK_EQ(cams[0].scopeMac, a.scopeMac);
	CHECK_EQ(cams[0].online, true);
	CHECK_EQ(cams[0].manual, true);
	CHECK_EQ(cams[0].lastSeen, uint64_t(1234));
	CHECK_EQ(cams[0].lastKnownRTSP.at("profile1"),
		 std::string("rtsp://10.0.0.1:554/s1"));
	// Secrets never go into the JSON.
	CHECK_EQ(cams[0].username, std::string());
	CHECK_EQ(cams[0].password, std::string());

	fs::remove_all(dir);
}

static void TestCollectionRoundTrip()
{
	const std::string dir = UniqueDir();
	Store store(dir);

	CollectionState cs;
	cs.uuid = "col-1";
	cs.display_name = "Default";
	cs.mappings.push_back(
		{"col-1", "CAM-101", "sn:123", "profile1", true});
	cs.mappings.push_back(
		{"col-1", "CAM-102", "sn:456", "profile2", false});
	cs.scene_presets.push_back(
		{"col-1", "Main", "sn:123", "preset1"});

	CHECK(store.SaveCollection(cs));

	CollectionState out;
	CHECK(store.LoadCollection("col-1", out));
	CHECK_EQ(out.display_name, std::string("Default"));
	CHECK_EQ(out.mappings.size(), size_t(2));
	CHECK_EQ(out.mappings[0].source_name, std::string("CAM-101"));
	CHECK_EQ(out.mappings[0].camera_id, std::string("sn:123"));
	CHECK_EQ(out.mappings[0].profileToken, std::string("profile1"));
	CHECK_EQ(out.mappings[0].auto_apply, true);
	CHECK_EQ(out.mappings[1].auto_apply, false);
	CHECK_EQ(out.scene_presets.size(), size_t(1));
	CHECK_EQ(out.scene_presets[0].scene_name, std::string("Main"));
	CHECK_EQ(out.scene_presets[0].camera_id, std::string("sn:123"));
	CHECK_EQ(out.scene_presets[0].preset_token, std::string("preset1"));

	CHECK(store.RemoveCollection("col-1"));
	CHECK(!store.LoadCollection("col-1", out)); // gone

	fs::remove_all(dir);
}

static void TestAppConfigRoundTrip()
{
	const std::string dir = UniqueDir();
	Store store(dir);

	AppConfig cfg;
	CHECK(!store.LoadAppConfig(cfg)); // missing -> defaults kept

	cfg.discovery_interval_s = 120;
	cfg.hello_listener_enabled = false;
	cfg.soap_timeout_s = 7;
	cfg.apply_policy = ApplyPolicyChoice::Always;
	cfg.default_stream = StreamChoice::Low;
	cfg.prompt_timeout_s = 45;
	CHECK(store.SaveAppConfig(cfg));

	AppConfig loaded;
	CHECK(store.LoadAppConfig(loaded));
	CHECK_EQ(loaded.discovery_interval_s, 120);
	CHECK_EQ(loaded.hello_listener_enabled, false);
	CHECK_EQ(loaded.soap_timeout_s, 7);
	CHECK(loaded.apply_policy == ApplyPolicyChoice::Always);
	CHECK(loaded.default_stream == StreamChoice::Low);
	CHECK_EQ(loaded.prompt_timeout_s, 45);
	CHECK_EQ(loaded.soap_retry_media, true); // untouched default

	fs::remove_all(dir);
}

static void TestPoliciesRoundTrip()
{
	const std::string dir = UniqueDir();
	Store store(dir);

	std::map<std::string, ApplyPolicyChoice> policies;
	policies["sn:1"] = ApplyPolicyChoice::Ignore;
	policies["sn:2"] = ApplyPolicyChoice::Always;
	CHECK(store.SaveCameraPolicies(policies));

	std::map<std::string, ApplyPolicyChoice> loaded;
	CHECK(store.LoadCameraPolicies(loaded));
	CHECK_EQ(loaded.size(), size_t(2));
	CHECK(loaded["sn:1"] == ApplyPolicyChoice::Ignore);
	CHECK(loaded["sn:2"] == ApplyPolicyChoice::Always);

	fs::remove_all(dir);
}

static void TestWinCred()
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	const std::string target = "obs-onvif-test/store-" +
				   std::to_string(now.count());
	const std::string secret = "s3cr3t-value";

	// Write -> read back.
	CHECK(Store::WriteCredential(target, secret));
	std::string read;
	bool found = false;
	CHECK(Store::ReadCredential(target, read, found));
	CHECK(found);
	CHECK_EQ(read, secret);

	// Delete -> gone.
	CHECK(Store::DeleteCredential(target));
	read.clear();
	CHECK(Store::ReadCredential(target, read, found));
	CHECK(!found);

	// Reading an unknown target reports absent (not an error).
	CHECK(Store::ReadCredential("obs-onvif-test/does-not-exist", read,
				    found));
	CHECK(!found);

	CHECK_EQ(Store::CameraCredTarget("sn:1"),
		 std::string("obs-onvif/sn:1"));
	CHECK_EQ(Store::DefaultCredTarget(), std::string("obs-onvif/default"));
}

static void TestPolicySerialization()
{
	ApplyPolicyChoice c = ApplyPolicyChoice::Ask;
	CHECK_EQ(Store::PolicyToString(ApplyPolicyChoice::Ask),
		 std::string("ask"));
	CHECK_EQ(Store::PolicyToString(ApplyPolicyChoice::Always),
		 std::string("always"));
	CHECK_EQ(Store::PolicyToString(ApplyPolicyChoice::Ignore),
		 std::string("ignore"));
	CHECK(Store::ParsePolicy("ask", c));
	CHECK(Store::ParsePolicy("always", c));
	CHECK(Store::ParsePolicy("ignore", c));
	CHECK(!Store::ParsePolicy("banana", c));
}

int main()
{
	TestCamerasRoundTrip();
	TestCollectionRoundTrip();
	TestAppConfigRoundTrip();
	TestPoliciesRoundTrip();
	TestWinCred();
	TestPolicySerialization();
	RUN_TESTS("store");
}