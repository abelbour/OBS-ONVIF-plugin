#include <string>

#include "check.h"
#include "identity.h"

using obs_onvif::BuildFingerprint;
using obs_onvif::DeviceIdentity;
using obs_onvif::ParseScopeMac;

static const char *kScopesWithMac =
    "onvif://www.onvif.org/name/Cam01 "
    "onvif://www.onvif.org/hardware/IPC "
    "onvif://www.onvif.org/mac/40:d8:2e:12:34:56 "
    "onvif://www.onvif.org/type/video_encoder";

static const char *kScopesNoMac =
    "onvif://www.onvif.org/name/Cam02 "
    "onvif://www.onvif.org/type/network_video_transmitter";

static void TestParseScopeMac()
{
	CHECK_EQ(ParseScopeMac(kScopesWithMac),
		 std::string("40:d8:2e:12:34:56"));
	CHECK_EQ(ParseScopeMac(kScopesNoMac), std::string());
	CHECK_EQ(ParseScopeMac(""), std::string());
	CHECK_EQ(ParseScopeMac("onvif://www.onvif.org/name/X"), std::string());
}

static void TestFingerprintPriority()
{
	DeviceIdentity id;
	CHECK_EQ(BuildFingerprint(id), std::string());

	id.uuid = "urn:uuid:abc";
	CHECK_EQ(BuildFingerprint(id), std::string("uuid:abc"));

	id.hardwareId = "HW-1";
	CHECK_EQ(BuildFingerprint(id), std::string("hw:HW-1"));

	id.scopes = kScopesWithMac;
	CHECK_EQ(BuildFingerprint(id), std::string("mac:40:d8:2e:12:34:56"));

	id.serialNumber = "SN-2026";
	CHECK_EQ(BuildFingerprint(id), std::string("sn:SN-2026"));
}

int main()
{
	TestParseScopeMac();
	TestFingerprintPriority();
	RUN_TESTS("identity");
}