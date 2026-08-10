#include <string>

#include "check.h"
#include "xml.h"

using obs_onvif::xml::Child;
using obs_onvif::xml::ChildText;
using obs_onvif::xml::Children;
using obs_onvif::xml::Descendant;
using obs_onvif::xml::DescendantText;
using obs_onvif::xml::Envelope;
using obs_onvif::xml::LocalName;
using obs_onvif::xml::Parse;
using obs_onvif::xml::TextOf;
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

// SOAP 1.1 fault (typical of legacy-device responses).
static const char *kFault =
    R"(<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"
            xmlns:ter="http://www.onvif.org/ver10/error">
  <s:Header/>
  <s:Body>
    <s:Fault>
      <faultcode>s:Server</faultcode>
      <faultstring>Invalid token</faultstring>
      <detail><ter:Fault><ter:Code>ter:Sender</ter:Code></ter:Fault></detail>
    </s:Fault>
  </s:Body>
</s:Envelope>)";

static void TestMalformed()
{
	XMLDocument doc;
	CHECK(!Parse("<a><b></a>", doc));
	CHECK(!Parse("", doc));
}

static void TestProbeMatch()
{
	XMLDocument doc;
	CHECK(Parse(kProbeMatch, doc));

	const tinyxml2::XMLElement *envelope = doc.RootElement();
	CHECK(envelope != nullptr);
	CHECK_EQ(std::string(LocalName(envelope)), "Envelope");

	// Namespace prefixes are structurally irrelevant to our lookups.
	const tinyxml2::XMLElement *body =
		Descendant(envelope, {"Body"});
	CHECK(body != nullptr);
	const tinyxml2::XMLElement *matches =
		Child(body, "ProbeMatches");
	CHECK(matches != nullptr);

	const auto probeMatches = Children(matches, "ProbeMatch");
	CHECK_EQ(probeMatches.size(), size_t(1));

	const tinyxml2::XMLElement *pm = probeMatches[0];
	CHECK_EQ(DescendantText(pm, {"Address"}),
		 "urn:uuid:bd43994a-1e5f-4e8a-9fc5-2e8b1c3d5f7a");
	CHECK(DescendantText(pm, {"Types"})
		      .find("NetworkVideoTransmitter") != std::string::npos);
	CHECK(DescendantText(pm, {"Scopes"})
		      .find("40:d8:2e:12:34:56") != std::string::npos);
	CHECK_EQ(ChildText(pm, "MetadataVersion"), "1");
	CHECK_EQ(ChildText(pm, "Missing"), "");

	const auto addrs = Tokens(DescendantText(pm, {"XAddrs"}));
	CHECK_EQ(addrs.size(), size_t(2));
	CHECK_EQ(addrs[0], "http://192.168.1.64:80/onvif/device_service");
	CHECK_EQ(addrs[1], "http://192.168.1.64:8899/onvif/device_service");
}

// Splits whitespace-separated tokens (used to walk XAddrs/Scopes).
static std::vector<std::string> Tokens(const std::string &s)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t next = s.find_first_of(" \t\r\n", pos);
		if (next == std::string::npos)
			next = s.size();
		if (next > pos)
			out.push_back(s.substr(pos, next - pos));
		pos = next + 1;
	}
	return out;
}

static void TestFault()
{
	XMLDocument doc;
	CHECK(Parse(kFault, doc));

	const tinyxml2::XMLElement *envelope = doc.RootElement();
	CHECK(envelope != nullptr);

	const std::vector<const tinyxml2::XMLElement *> bodies =
		Children(envelope, "Body");
	CHECK_EQ(bodies.size(), size_t(1));

	const tinyxml2::XMLElement *fault = Child(bodies[0], "Fault");
	CHECK(fault != nullptr);
	CHECK_EQ(TextOf(Child(fault, "faultcode")), "s:Server");
	CHECK_EQ(TextOf(Child(fault, "faultstring")), "Invalid token");
	CHECK_EQ(DescendantText(fault, {"detail", "Fault", "Code"}),
		 "ter:Sender");
}

static void TestEnvelopeBuild()
{
	const std::string env = Envelope(
		"<wsse:Security/>",
		"<trt:GetProfiles/>",
		{{"wsse", "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"},
		 {"wsu",
		  "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"},
		 {"trt", "http://www.onvif.org/ver10/media/wsdl"}});

	XMLDocument doc;
	CHECK(Parse(env, doc));
	const tinyxml2::XMLElement *envelope = doc.RootElement();
	CHECK(envelope != nullptr);
	CHECK_EQ(std::string(LocalName(envelope)), "Envelope");
	CHECK(Child(envelope, "Header") != nullptr);
	CHECK(Child(envelope, "Body") != nullptr);
	CHECK(Child(envelope, "Body")->FirstChildElement() != nullptr);
}

int main()
{
	TestMalformed();
	TestProbeMatch();
	TestFault();
	TestEnvelopeBuild();
	RUN_TESTS("xml");
}