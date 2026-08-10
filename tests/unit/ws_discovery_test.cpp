#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "ws_discovery.h"
#include "xml.h"

using obs_onvif::BuildProbe;
using obs_onvif::CloseUdpSocket;
using obs_onvif::DiscoveredDevice;
using obs_onvif::kDiscoveryPort;
using obs_onvif::OpenUdpSocket;
using obs_onvif::ParseDiscoveryResponse;
using obs_onvif::RecvUdp;
using obs_onvif::SendUdp;
using obs_onvif::xml::Child;
using obs_onvif::xml::ChildText;
using obs_onvif::xml::Parse;
using tinyxml2::XMLDocument;

// Realistic WS-Discovery v1 ProbeMatch response (Hikvision-style).
static const char *kProbeMatch =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
               xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <soap:Header>
    <wsa:MessageID>urn:uuid:0d8c9f97-8b2f-4a1b-8b5f-6f9b2d0a1e3c</wsa:MessageID>
    <wsa:RelatesTo>urn:uuid:3b0cf5d2-7dfe-4c6a-8d3b-3f2b1c0a9e8d</wsa:RelatesTo>
    <wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>
  </soap:Header>
  <soap:Body>
    <d:ProbeMatches>
      <d:ProbeMatch>
        <wsa:Address>urn:uuid:bd43994a-1e5f-4e8a-9fc5-2e8b1c3d5f7a</wsa:Address>
        <d:Types>dn:NetworkVideoTransmitter d:Device</d:Types>
        <d:Scopes>onvif://www.onvif.org/name/DS-2CD2032-I onvif://www.onvif.org/hardware/DS-2CD2032-I onvif://www.onvif.org/mac/40:d8:2e:12:34:56 onvif://www.onvif.org/type/video_encoder</d:Scopes>
        <d:XAddrs>http://192.168.1.64:80/onvif/device_service http://192.168.1.64:8899/onvif/device_service</d:XAddrs>
        <d:MetadataVersion>1</d:MetadataVersion>
      </d:ProbeMatch>
    </d:ProbeMatches>
  </soap:Body>
</soap:Envelope>)";

// WS-Discovery Hello (announcement), same payload shape.
static const char *kHello =
    R"(<?xml version="1.0"?>
<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope"
            xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing"
            xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
            xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <e:Header>
    <w:MessageID>urn:uuid:d1d2d3d4-d5d6-4890-abcd-ef0123456789</w:MessageID>
    <w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>
    <w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello</w:Action>
  </e:Header>
  <e:Body>
    <d:Hello>
      <w:Address>urn:uuid:7a7b7c7d-7e7f-4a1b-9c2d-c0ffee123456</w:Address>
      <d:Types>dn:NetworkVideoTransmitter</d:Types>
      <d:Scopes>onvif://www.onvif.org/name/Cam01 onvif://www.onvif.org/mac/aa:bb:cc:dd:ee:ff</d:Scopes>
      <d:XAddrs>http://192.168.1.77:8000/onvif/device_service</d:XAddrs>
    </d:Hello>
  </e:Body>
</e:Envelope>)";

static void TestBuildProbe()
{
	const std::string xml = BuildProbe("urn:uuid:1-2-3-4-5");
	CHECK(!xml.empty());

	XMLDocument doc;
	CHECK(Parse(xml, doc));
	const tinyxml2::XMLElement *env = doc.RootElement();
	CHECK(env != nullptr);
	const tinyxml2::XMLElement *header = Child(env, "Header");
	CHECK(header != nullptr);
	CHECK_EQ(ChildText(header, "MessageID"), std::string("urn:uuid:1-2-3-4-5"));
	CHECK(ChildText(header, "Action")
		      .find("discovery/Probe") != std::string::npos);

	const tinyxml2::XMLElement *body = Child(env, "Body");
	CHECK(body != nullptr);
	const tinyxml2::XMLElement *probe = Child(body, "Probe");
	CHECK(probe != nullptr);
	const std::string types = ChildText(probe, "Types");
	CHECK(types.find("dn:NetworkVideoTransmitter") != std::string::npos);
	CHECK(types.find("d:Device") != std::string::npos);
}

static void TestParseProbeMatches()
{
	std::vector<DiscoveredDevice> devs;
	CHECK(ParseDiscoveryResponse(kProbeMatch, devs));
	CHECK_EQ(devs.size(), size_t(1));

	const DiscoveredDevice &d = devs[0];
	CHECK_EQ(d.uuid, std::string("urn:uuid:bd43994a-1e5f-4e8a-9fc5-2e8b1c3d5f7a"));
	CHECK_EQ(d.relatesTo,
		 std::string("urn:uuid:3b0cf5d2-7dfe-4c6a-8d3b-3f2b1c0a9e8d"));
	CHECK_EQ(d.xaddrs.size(), size_t(2));
	CHECK_EQ(d.xaddrs[0],
		 std::string("http://192.168.1.64:80/onvif/device_service"));
	CHECK(d.scopes.find("mac/40:d8:2e:12:34:56") != std::string::npos);

	bool hasNvt = false, hasDevice = false;
	for (const std::string &t : d.types) {
		if (t == "NetworkVideoTransmitter")
			hasNvt = true;
		if (t == "Device")
			hasDevice = true;
	}
	CHECK(hasNvt);
	CHECK(hasDevice);
}

static void TestParseHello()
{
	std::vector<DiscoveredDevice> devs;
	CHECK(ParseDiscoveryResponse(kHello, devs));
	CHECK_EQ(devs.size(), size_t(1));
	CHECK_EQ(devs[0].uuid,
		 std::string("urn:uuid:7a7b7c7d-7e7f-4a1b-9c2d-c0ffee123456"));
	CHECK_EQ(devs[0].xaddrs.size(), size_t(1));
	CHECK(!devs[0].scopes.empty());
}

static void TestParseGarbage()
{
	std::vector<DiscoveredDevice> devs;
	CHECK(!ParseDiscoveryResponse("", devs));
	CHECK(!ParseDiscoveryResponse("not xml at all <", devs));
	CHECK(!ParseDiscoveryResponse(
		"<a><b>mismatched</a></b>", devs));
	CHECK_EQ(devs.size(), size_t(0));
}

// Live loopback round-trip against tests/mock_onvif_server.py's UDP responder.
// Invoked via ctest: ws_discovery_test <host> <port>
static void TestLiveUdpRoundTrip(int argc, char **argv)
{
	if (argc != 3) {
		CHECK(true); // skipped when not launched as the live test
		return;
	}

	intptr_t sock = OpenUdpSocket(0, /*joinMulticast=*/false,
				      /*reuseAddr=*/false);
	CHECK(sock != -1);

	const long sent = SendUdp(sock, BuildProbe("urn:uuid:1-2-3-4-5"),
				  argv[1], (uint16_t)std::atoi(argv[2]));
	CHECK(sent > 0);

	std::string reply;
	const long n = RecvUdp(sock, reply, 5000);
	CHECK(n > 0);
	CHECK(reply.find("ProbeMatch") != std::string::npos);

	std::vector<DiscoveredDevice> devs;
	CHECK(ParseDiscoveryResponse(reply, devs));
	CHECK_EQ(devs.size(), size_t(1));
	CHECK(devs[0].uuid.find("urn:uuid:") != std::string::npos);
	CHECK(!devs[0].xaddrs.empty());
	CHECK(devs[0].xaddrs[0].find("127.0.0.1") != std::string::npos);

	CloseUdpSocket(sock);
}

int main(int argc, char **argv)
{
	TestBuildProbe();
	TestParseProbeMatches();
	TestParseHello();
	TestParseGarbage();
	TestLiveUdpRoundTrip(argc, argv);
	(void)kDiscoveryPort;
	RUN_TESTS("ws_discovery");
}