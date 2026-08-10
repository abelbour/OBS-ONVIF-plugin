#include "onvif_client.h"

#include <stdexcept>

#include "soap_client.h"
#include "ws_security.h"
#include "xml.h"

namespace obs_onvif {

namespace {

const char *kTdsNs = "http://www.onvif.org/ver10/device/wsdl";
const char *kTrtNs = "http://www.onvif.org/ver10/media/wsdl";
const char *kPtzNs = "http://www.onvif.org/ver20/ptz/wsdl";
const char *kTtNs = "http://www.onvif.org/ver10/schema";

// Scheme://host[:port] of a URL (everything before the first '/' after the
// authority), empty when the URL is malformed.
std::string UrlAuthority(const std::string &url)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return {};
	const size_t pathStart = url.find('/', scheme + 3);
	if (pathStart == std::string::npos)
		return url;
	return url.substr(0, pathStart);
}

// Returns `data` with its authority replaced by the base URL's authority
// (path preserved), so media/PTZ service URLs always reach the endpoint the
// client was configured with.
std::string SwapToBaseAuthority(const std::string &data,
				const std::string &baseAuthority)
{
	const size_t scheme = data.find("://");
	if (scheme == std::string::npos)
		return data;
	const size_t pathStart = data.find('/', scheme + 3);
	const std::string path =
		pathStart == std::string::npos ? "/" : data.substr(pathStart);
	return baseAuthority + path;
}

// Never returns nullptr (empty string for a missing element/attribute), so it
// is safe to assign directly into std::string.
const char *Attr(const tinyxml2::XMLElement *el, const char *name)
{
	const char *value = el ? el->Attribute(name) : nullptr;
	return value ? value : "";
}

} // namespace

OnvifClient::OnvifClient(std::string baseUrl, std::string username,
			 std::string password)
	: OnvifClient(std::move(baseUrl), std::move(username),
		      std::move(password), /*allowBasicFallback=*/true,
		      /*validateCert=*/false, /*timeoutMs=*/3000)
{
}

OnvifClient::OnvifClient(std::string baseUrl, std::string username,
			 std::string password, bool allowBasicFallback,
			 bool validateCert, unsigned timeoutMs)
	: baseUrl_(std::move(baseUrl)),
	  authority_(UrlAuthority(baseUrl_)),
	  username_(std::move(username)),
	  password_(std::move(password)),
	  allowBasicFallback_(allowBasicFallback),
	  validateCert_(validateCert),
	  timeoutMs_(timeoutMs),
	  deviceService_(baseUrl_)
{
}

std::string OnvifClient::ServiceFor(const std::string &primary,
				    const std::string &capsXAddr)
{
	if (!capsXAddr.empty())
		return SwapToBaseAuthority(capsXAddr, authority_);
	return primary;
}

std::string OnvifClient::PostOperation(const std::string &serviceUrl,
				       const std::string &soapAction,
				       const char *opPrefix,
				       const char *wsdlNs,
				       const std::string &body)
{
	std::vector<std::pair<std::string, std::string>> ns = {
		{opPrefix, wsdlNs}, {"tt", kTtNs}};

	SoapRequest digest;
	digest.url = serviceUrl;
	const auto token = BuildUsernameToken(username_, password_);
	digest.body = xml::Envelope(SecurityHeader(token), body, ns);
	digest.soapAction = soapAction;
	digest.validateCert = validateCert_;
	digest.timeoutMs = timeoutMs_;

	SoapResult out;
	if (allowBasicFallback_) {
		SoapRequest basic;
		basic.url = serviceUrl;
		basic.body = xml::Envelope("", body, ns);
		basic.soapAction = soapAction;
		basic.basicUser = username_;
		basic.basicPass = password_;
		basic.validateCert = validateCert_;
		basic.timeoutMs = timeoutMs_;
		SoapClient().SendWithAuthFallback(digest, basic, out);
	} else {
		SoapClient().Send(digest, out);
	}

	if (!out.transportOk)
		throw std::runtime_error(
			"SOAP transport failed: " +
			(out.error.empty() ? "no response" : out.error));
	if (out.fault.present)
		throw std::runtime_error("SOAP fault " + out.fault.code + ": " +
					 out.fault.reason);
	if (out.httpStatus != 200)
		throw std::runtime_error("HTTP " + std::to_string(out.httpStatus));
	return out.body;
}

Capabilities OnvifClient::GetCapabilities()
{
	const std::string body = PostOperation(
		deviceService_, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
		"tds", kTdsNs, "<tds:GetCapabilities/>");

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetCapabilities: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		throw std::runtime_error("GetCapabilities: empty response");
	const tinyxml2::XMLElement *resp =
		xml::Child(xml::Child(env, "Body"), "GetCapabilitiesResponse");
	if (!resp)
		throw std::runtime_error("GetCapabilities: missing response element");

	Capabilities c;
	c.deviceXAddr = xml::DescendantText(resp, {"Capabilities", "Device", "XAddr"});
	c.mediaXAddr = xml::DescendantText(resp, {"Capabilities", "Media", "XAddr"});
	c.ptzXAddr = xml::DescendantText(resp, {"Capabilities", "PTZ", "XAddr"});
	c.eventsXAddr = xml::DescendantText(resp, {"Capabilities", "Events", "XAddr"});
	c.imagingXAddr = xml::DescendantText(resp, {"Capabilities", "Imaging", "XAddr"});

	mediaService_ = ServiceFor(deviceService_, c.mediaXAddr);
	ptzService_ = ServiceFor(deviceService_, c.ptzXAddr);
	caps_ = c;
	return c;
}

DeviceInfo OnvifClient::GetDeviceInformation()
{
	const std::string body = PostOperation(
		deviceService_,
		"http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
		"tds", kTdsNs, "<tds:GetDeviceInformation/>");

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetDeviceInformation: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		throw std::runtime_error("GetDeviceInformation: empty response");
	const tinyxml2::XMLElement *resp =
		xml::Child(xml::Child(env, "Body"), "GetDeviceInformationResponse");
	if (!resp)
		throw std::runtime_error(
			"GetDeviceInformation: missing response element");

	DeviceInfo info;
	info.manufacturer = xml::ChildText(resp, "Manufacturer");
	info.model = xml::ChildText(resp, "Model");
	info.firmwareVersion = xml::ChildText(resp, "FirmwareVersion");
	info.serialNumber = xml::ChildText(resp, "SerialNumber");
	info.hardwareId = xml::ChildText(resp, "HardwareId");
	return info;
}

std::vector<MediaProfile> OnvifClient::GetProfiles()
{
	const std::string body = PostOperation(
		mediaService_, "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
		"trt", kTrtNs, "<trt:GetProfiles/>");

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetProfiles: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		throw std::runtime_error("GetProfiles: empty response");
	const tinyxml2::XMLElement *resp =
		xml::Child(xml::Child(env, "Body"), "GetProfilesResponse");
	if (!resp)
		throw std::runtime_error("GetProfiles: missing response element");

	std::vector<MediaProfile> profiles;
	for (const tinyxml2::XMLElement *p : xml::Children(resp, "Profiles")) {
		MediaProfile mp;
		mp.token = Attr(p, "token");
		mp.name = xml::ChildText(p, "Name");
		const tinyxml2::XMLElement *vsc =
			xml::Child(p, "VideoSourceConfiguration");
		if (vsc)
			mp.videoSourceToken = Attr(vsc, "token");
		const tinyxml2::XMLElement *vec =
			xml::Child(p, "VideoEncoderConfiguration");
		if (vec)
			mp.videoEncoderToken = Attr(vec, "token");
		const tinyxml2::XMLElement *ptz =
			xml::Child(p, "PTZConfiguration");
		if (ptz)
			mp.ptzConfigToken = Attr(ptz, "token");
		profiles.push_back(std::move(mp));
	}
	return profiles;
}

StreamUriResult OnvifClient::GetStreamUri(const std::string &profileToken)
{
	const std::string reqBody =
		"<trt:GetStreamUri><trt:StreamSetup><trt:Stream>"
		"RTP-Unicast</trt:Stream>"
		"<trt:Transport><tt:Protocol>RtspUnicast</tt:Protocol>"
		"<tt:Tunnel/></trt:Transport></trt:StreamSetup>"
		"<trt:ProfileToken>" +
		profileToken + "</trt:ProfileToken></trt:GetStreamUri>";

	const std::string body = PostOperation(
		mediaService_, "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
		"trt", kTrtNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetStreamUri: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		throw std::runtime_error("GetStreamUri: empty response");
	const tinyxml2::XMLElement *resp =
		xml::Child(xml::Child(env, "Body"), "GetStreamUriResponse");
	if (!resp)
		throw std::runtime_error("GetStreamUri: missing response element");

	StreamUriResult out;
	out.uri = xml::DescendantText(resp, {"MediaUri", "Uri"});
	out.timeout = xml::DescendantText(resp, {"MediaUri", "Timeout"});
	return out;
}

void OnvifClient::GotoPreset(const std::string &profileToken,
			     const std::string &presetToken)
{
	const std::string reqBody = "<trt:GotoPreset><trt:ProfileToken>" +
				    profileToken +
				    "</trt:ProfileToken><trt:PresetToken>" +
				    presetToken +
				    "</trt:PresetToken></trt:GotoPreset>";

	const std::string body = PostOperation(
		ptzService_, "http://www.onvif.org/ver20/ptz/wsdl/GotoPreset",
		"trt", kPtzNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GotoPreset: malformed response");
	(void)doc;
}

} // namespace obs_onvif