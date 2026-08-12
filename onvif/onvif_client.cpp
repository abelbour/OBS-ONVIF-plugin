#include "onvif_client.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "soap_client.h"
#include "ws_security.h"
#include "xml.h"

namespace obs_onvif {

namespace {

const char *kTdsNs = "http://www.onvif.org/ver10/device/wsdl";
const char *kTrtNs = "http://www.onvif.org/ver10/media/wsdl";
const char *kPtzNs = "http://www.onvif.org/ver20/ptz/wsdl";
const char *kTtNs = "http://www.onvif.org/ver10/schema";
const char *kTimgNs = "http://www.onvif.org/ver20/imaging/wsdl";
const char *kTdispNs = "http://www.onvif.org/ver20/display/wsdl";

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

// Minimal XML-escape for user-supplied text inside envelope bodies.
std::string EscapeXml(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

double ParseDouble(const std::string &s)
{
	if (s.empty())
		return 0.0;
	return std::strtod(s.c_str(), nullptr);
}

int ParseInt(const std::string &s)
{
	if (s.empty())
		return 0;
	return std::atoi(s.c_str());
}

bool ParseBool(const std::string &s)
{
	return s == "true" || s == "1";
}

std::string FormatInt(int v)
{
	return std::to_string(v);
}

std::string FormatDouble(double v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.2f", v);
	return buf;
}

// PTZ service URL authority rewrite uses the same media/PTZ pattern.
const char *kVelocityPanTiltSpace =
	"http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace";
const char *kVelocityZoomSpace =
	"http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace";

} // namespace

OnvifClient::OnvifClient()
	: OnvifClient("", "", "", /*allowBasicFallback=*/true,
		      /*validateCert=*/false, /*timeoutMs=*/3000, nullptr,
		      nullptr)
{
}

OnvifClient::OnvifClient(std::string baseUrl, std::string username,
			 std::string password)
	: OnvifClient(std::move(baseUrl), std::move(username),
		      std::move(password), /*allowBasicFallback=*/true,
		      /*validateCert=*/false, /*timeoutMs=*/3000, nullptr,
		      nullptr)
{
}

OnvifClient::OnvifClient(std::string baseUrl, std::string username,
			 std::string password, bool allowBasicFallback,
			 bool validateCert, unsigned timeoutMs,
			 const Capabilities *caps,
			 std::shared_ptr<SoapPool> pool)
	: baseUrl_(std::move(baseUrl)),
	  authority_(UrlAuthority(baseUrl_)),
	  username_(std::move(username)),
	  password_(std::move(password)),
	  allowBasicFallback_(allowBasicFallback),
	  validateCert_(validateCert),
	  timeoutMs_(timeoutMs),
	  deviceService_(baseUrl_),
	  pool_(std::move(pool)),
	  soap_(pool_)
{
	if (caps) {
		// Pre-resolved capabilities: adopt the service XAddrs so a
		// fresh client skips the GetCapabilities round trip.
		caps_ = *caps;
		mediaService_ = ServiceFor(deviceService_, caps->mediaXAddr);
		ptzService_ = ServiceFor(deviceService_, caps->ptzXAddr);
		imagingService_ = ServiceFor(deviceService_, caps->imagingXAddr);
		displayService_ = ServiceFor(deviceService_, caps->displayXAddr);
	}
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
				       const std::string &body,
				       AbortHandle *abort)
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

	SoapRequest basic;
	basic.url = serviceUrl;
	basic.body = xml::Envelope("", body, ns);
	basic.soapAction = soapAction;
	basic.basicUser = username_;
	basic.basicPass = password_;
	basic.validateCert = validateCert_;
	basic.timeoutMs = timeoutMs_;

	SoapResult out;
	const bool hasPool = pool_ != nullptr;
	if (!allowBasicFallback_) {
		soap_.Send(digest, out, abort);
	} else {
		const SoapPool::AuthMode cached =
			hasPool ? pool_->AuthFor(serviceUrl)
				: SoapPool::AuthMode::Unknown;
		if (cached == SoapPool::AuthMode::Basic) {
			// Accepted mode already known: no 401 round-trip.
			soap_.Send(basic, out, abort);
		} else if (cached == SoapPool::AuthMode::Wsse) {
			soap_.Send(digest, out, abort);
		} else {
			// First contact: digest first, Basic on rejection.
			soap_.Send(digest, out, abort);
			if (out.transportOk && out.httpStatus != 401 &&
			    !out.fault.present) {
				if (hasPool)
					pool_->RememberAuth(
						serviceUrl,
						SoapPool::AuthMode::Wsse);
			} else if (!(abort && abort->Signaled())) {
				SoapResult second;
				soap_.Send(basic, second, abort);
				out = second;
				if (hasPool && out.transportOk)
					pool_->RememberAuth(
						serviceUrl,
						SoapPool::AuthMode::Basic);
			}
		}
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
	c.displayXAddr = xml::DescendantText(resp, {"Capabilities", "Display", "XAddr"});

	mediaService_ = ServiceFor(deviceService_, c.mediaXAddr);
	ptzService_ = ServiceFor(deviceService_, c.ptzXAddr);
	imagingService_ = ServiceFor(deviceService_, c.imagingXAddr);
	displayService_ = ServiceFor(deviceService_, c.displayXAddr);
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

	// Void op: the envelope body carries nothing this caller needs.
	(void)PostOperation(ptzService_,
			    "http://www.onvif.org/ver20/ptz/wsdl/GotoPreset",
			    "trt", kPtzNs, reqBody);
}

std::string OnvifClient::SetPreset(const std::string &profileToken,
				   const std::string &presetName)
{
	const std::string reqBody =
		"<trt:SetPreset><trt:ProfileToken>" + profileToken +
		"</trt:ProfileToken><trt:PresetName>" +
		EscapeXml(presetName) + "</trt:PresetName></trt:SetPreset>";

	const std::string body = PostOperation(
		ptzService_, "http://www.onvif.org/ver20/ptz/wsdl/SetPreset",
		"trt", kPtzNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("SetPreset: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"), "SetPresetResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error("SetPreset: missing response element");
	return xml::ChildText(resp, "PresetToken");
}

std::vector<Preset> OnvifClient::GetPresets(const std::string &profileToken)
{
	const std::string reqBody = "<trt:GetPresets><trt:ProfileToken>" +
				    profileToken +
				    "</trt:ProfileToken></trt:GetPresets>";

	const std::string body = PostOperation(
		ptzService_, "http://www.onvif.org/ver20/ptz/wsdl/GetPresets",
		"trt", kPtzNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetPresets: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"), "GetPresetsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error("GetPresets: missing response element");

	std::vector<Preset> presets;
	for (const tinyxml2::XMLElement *p : xml::Children(resp, "PTZPreset")) {
		Preset pr;
		pr.token = Attr(p, "token");
		pr.name = xml::ChildText(p, "Name");
		presets.push_back(std::move(pr));
	}
	return presets;
}

void OnvifClient::RenamePreset(const std::string &profileToken,
			       const std::string &presetToken,
			       const std::string &newName)
{
	const std::string reqBody =
		"<trt:RenamePreset><trt:ProfileToken>" + profileToken +
		"</trt:ProfileToken><trt:PresetToken>" + presetToken +
		"</trt:PresetToken><trt:NewName>" + EscapeXml(newName) +
		"</trt:NewName></trt:RenamePreset>";

	(void)PostOperation(ptzService_,
			    "http://www.onvif.org/ver20/ptz/wsdl/RenamePreset",
			    "trt", kPtzNs, reqBody);
}

void OnvifClient::DeletePreset(const std::string &profileToken,
			       const std::string &presetToken)
{
	const std::string reqBody =
		"<trt:DeletePreset><trt:ProfileToken>" + profileToken +
		"</trt:ProfileToken><trt:PresetToken>" + presetToken +
		"</trt:PresetToken></trt:DeletePreset>";

	(void)PostOperation(ptzService_,
			    "http://www.onvif.org/ver20/ptz/wsdl/DeletePreset",
			    "trt", kPtzNs, reqBody);
}

void OnvifClient::ContinuousMove(const std::string &profileToken, double pan,
				 double tilt, double zoom,
				 double timeoutSeconds)
{
	ContinuousMove(profileToken, pan, tilt, zoom, timeoutSeconds, nullptr);
}

void OnvifClient::ContinuousMove(const std::string &profileToken, double pan,
				 double tilt, double zoom,
				 double timeoutSeconds, AbortHandle *abort)
{
	// Fixed-buffer velocity injection (M4 §6.8): the body is built with
	// snprintf so the hot path never pays for string formatting churn.
	char velocity[192];
	std::snprintf(velocity, sizeof velocity,
		      "<trt:Velocity><tt:PanTilt x=\"%.3f\" y=\"%.3f\" "
		      "space=\"%s\"/><tt:Zoom x=\"%.3f\" space=\"%s\"/>"
		      "</trt:Velocity>",
		      pan, tilt, kVelocityPanTiltSpace, zoom,
		      kVelocityZoomSpace);
	std::string reqBody = "<trt:ContinuousMove><trt:ProfileToken>" +
			      profileToken + "</trt:ProfileToken>" +
			      velocity;
	if (timeoutSeconds > 0.0) {
		const int totalSec = (int)timeoutSeconds;
		const int hours = totalSec / 3600;
		const int minutes = (totalSec % 3600) / 60;
		const double secs = timeoutSeconds -
				    (double)(hours * 3600 + minutes * 60);
		char timeout[64];
		std::snprintf(timeout, sizeof timeout,
			      "<trt:Timeout>PT%dH%dM%.3fS</trt:Timeout>",
			      hours, minutes, secs);
		reqBody += timeout;
	}
	reqBody += "</trt:ContinuousMove>";

	// Void op: the empty response body is not parsed.
	(void)PostOperation(ptzService_,
			    "http://www.onvif.org/ver20/ptz/wsdl/ContinuousMove",
			    "trt", kPtzNs, reqBody, abort);
}

void OnvifClient::Stop(const std::string &profileToken)
{
	Stop(profileToken, nullptr);
}

void OnvifClient::Stop(const std::string &profileToken, AbortHandle *abort)
{
	const std::string reqBody =
		"<trt:Stop><trt:ProfileToken>" + profileToken +
		"</trt:ProfileToken><trt:PanTilt>true</trt:PanTilt>"
		"<trt:Zoom>true</trt:Zoom></trt:Stop>";

	// Void op: the empty response body is not parsed.
	(void)PostOperation(ptzService_,
			    "http://www.onvif.org/ver20/ptz/wsdl/Stop", "trt",
			    kPtzNs, reqBody, abort);
}

// -- Encoder configuration ---------------------------------------------------

std::vector<VideoEncoderConfig>
OnvifClient::GetVideoEncoderConfigurations()
{
	const std::string body = PostOperation(
		mediaService_,
		"http://www.onvif.org/ver10/media/wsdl/GetVideoEncoderConfigurations",
		"trt", kTrtNs, "<trt:GetVideoEncoderConfigurations/>");

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error(
			"GetVideoEncoderConfigurations: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"),
				 "GetVideoEncoderConfigurationsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error(
			"GetVideoEncoderConfigurations: missing response element");

	std::vector<VideoEncoderConfig> out;
	for (const tinyxml2::XMLElement *c :
	     xml::Children(resp, "Configurations")) {
		VideoEncoderConfig v;
		v.token = Attr(c, "token");
		v.name = xml::ChildText(c, "Name");
		v.encoding = xml::ChildText(c, "Encoding");
		v.resolution.width =
			ParseInt(xml::DescendantText(c, {"Resolution", "Width"}));
		v.resolution.height =
			ParseInt(xml::DescendantText(c, {"Resolution", "Height"}));
		v.frameRate = ParseDouble(
			xml::DescendantText(c, {"RateControl", "FrameRateLimit"}));
		v.bitrate = ParseInt(
			xml::DescendantText(c, {"RateControl", "BitrateLimit"}));
		out.push_back(std::move(v));
	}
	return out;
}

VideoEncoderOptions OnvifClient::GetVideoEncoderConfigurationOptions(
	const std::string &encoderToken)
{
	const std::string reqBody =
		"<trt:GetVideoEncoderConfigurationOptions>"
		"<trt:ConfigurationToken>" +
		encoderToken +
		"</trt:ConfigurationToken>"
		"</trt:GetVideoEncoderConfigurationOptions>";
	const std::string body = PostOperation(
		mediaService_,
		"http://www.onvif.org/ver10/media/wsdl/"
		"GetVideoEncoderConfigurationOptions",
		"trt", kTrtNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error(
			"GetVideoEncoderConfigurationOptions: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"),
				 "GetVideoEncoderConfigurationOptionsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error(
			"GetVideoEncoderConfigurationOptions: missing response");

	VideoEncoderOptions o;
	const tinyxml2::XMLElement *options =
		xml::Child(resp, "Options");
	if (!options)
		return o;
	o.minFrameRate = ParseDouble(
		xml::DescendantText(options, {"FrameRateRange", "Min"}));
	o.maxFrameRate = ParseDouble(
		xml::DescendantText(options, {"FrameRateRange", "Max"}));
	o.minBitrate = ParseInt(
		xml::DescendantText(options, {"BitrateRange", "Min"}));
	o.maxBitrate = ParseInt(
		xml::DescendantText(options, {"BitrateRange", "Max"}));
	/* Resolutions are listed under the encoding element (H264/H265/MPEG4/
	 * JPEG); scan them all. */
	for (const char *enc : {"H264", "H265", "MPEG4", "JPEG"}) {
		const tinyxml2::XMLElement *e = xml::Child(options, enc);
		if (!e)
			continue;
		for (const tinyxml2::XMLElement *r :
		     xml::Children(e, "ResolutionAvailable")) {
			Resolution res;
			res.width = ParseInt(xml::ChildText(r, "Width"));
			res.height = ParseInt(xml::ChildText(r, "Height"));
			o.resolutions.push_back(res);
		}
	}
	return o;
}

void OnvifClient::SetVideoEncoderConfiguration(const VideoEncoderConfig &cfg)
{
	const std::string reqBody =
		"<trt:SetVideoEncoderConfiguration>"
		"<trt:Configuration token=\"" +
		EscapeXml(cfg.token) + "\"><tt:Name>" + EscapeXml(cfg.name) +
		"</tt:Name><tt:Encoding>" + EscapeXml(cfg.encoding) +
		"</tt:Encoding><tt:Resolution><tt:Width>" +
		FormatInt(cfg.resolution.width) + "</tt:Width><tt:Height>" +
		FormatInt(cfg.resolution.height) +
		"</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality>"
		"<tt:RateControl><tt:FrameRateLimit>" +
		FormatDouble(cfg.frameRate) +
		"</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>"
		"<tt:BitrateLimit>" +
		FormatInt(cfg.bitrate) +
		"</tt:BitrateLimit></tt:RateControl>"
		"<tt:SessionTimeout>PT10S</tt:SessionTimeout>"
		"</trt:Configuration>"
		"</trt:SetVideoEncoderConfiguration>";

	const std::string body = PostOperation(
		mediaService_,
		"http://www.onvif.org/ver10/media/wsdl/SetVideoEncoderConfiguration",
		"trt", kTrtNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error(
			"SetVideoEncoderConfiguration: malformed response");
	(void)doc;
}

// -- Imaging ----------------------------------------------------------------

ImagingSettings OnvifClient::GetImagingSettings(
	const std::string &videoSourceToken)
{
	const std::string reqBody =
		"<timg:GetImagingSettings><timg:VideoSourceToken>" +
		videoSourceToken +
		"</timg:VideoSourceToken></timg:GetImagingSettings>";
	const std::string body = PostOperation(
		imagingService_,
		"http://www.onvif.org/ver20/imaging/wsdl/GetImagingSettings",
		"timg", kTimgNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetImagingSettings: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"),
				 "GetImagingSettingsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error(
			"GetImagingSettings: missing response element");

	ImagingSettings s;
	const tinyxml2::XMLElement *settings =
		xml::Child(resp, "ImagingSettings");
	if (!settings)
		return s;
	s.present = true;
	s.brightness = ParseDouble(xml::ChildText(settings, "Brightness"));
	s.colorSaturation =
		ParseDouble(xml::ChildText(settings, "ColorSaturation"));
	s.contrast = ParseDouble(xml::ChildText(settings, "Contrast"));
	s.sharpness = ParseDouble(xml::ChildText(settings, "Sharpness"));
	return s;
}

ImagingOptions OnvifClient::GetImagingOptions(
	const std::string &videoSourceToken)
{
	const std::string reqBody =
		"<timg:GetImagingOptions><timg:VideoSourceToken>" +
		videoSourceToken + "</timg:VideoSourceToken></timg:GetImagingOptions>";
	const std::string body = PostOperation(
		imagingService_,
		"http://www.onvif.org/ver20/imaging/wsdl/GetImagingOptions",
		"timg", kTimgNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetImagingOptions: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"),
				 "GetImagingOptionsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error(
			"GetImagingOptions: missing response element");

	ImagingOptions o;
	const tinyxml2::XMLElement *options =
		xml::Child(resp, "ImagingOptions");
	if (!options)
		return o;
	o.present = true;
	o.minBrightness =
		ParseDouble(xml::DescendantText(options, {"Brightness", "Min"}));
	o.maxBrightness =
		ParseDouble(xml::DescendantText(options, {"Brightness", "Max"}));
	o.minColorSaturation = ParseDouble(
		xml::DescendantText(options, {"ColorSaturation", "Min"}));
	o.maxColorSaturation = ParseDouble(
		xml::DescendantText(options, {"ColorSaturation", "Max"}));
	o.minContrast =
		ParseDouble(xml::DescendantText(options, {"Contrast", "Min"}));
	o.maxContrast =
		ParseDouble(xml::DescendantText(options, {"Contrast", "Max"}));
	o.minSharpness =
		ParseDouble(xml::DescendantText(options, {"Sharpness", "Min"}));
	o.maxSharpness =
		ParseDouble(xml::DescendantText(options, {"Sharpness", "Max"}));
	return o;
}

void OnvifClient::SetImagingSettings(const std::string &videoSourceToken,
				     const ImagingSettings &s)
{
	const std::string reqBody =
		"<timg:SetImagingSettings><timg:VideoSourceToken>" +
		videoSourceToken + "</timg:VideoSourceToken>"
		"<timg:ImagingSettings><tt:Brightness>" +
		FormatDouble(s.brightness) +
		"</tt:Brightness><tt:ColorSaturation>" +
		FormatDouble(s.colorSaturation) +
		"</tt:ColorSaturation><tt:Contrast>" +
		FormatDouble(s.contrast) +
		"</tt:Contrast><tt:Sharpness>" +
		FormatDouble(s.sharpness) +
		"</tt:Sharpness></timg:ImagingSettings>"
		"</timg:SetImagingSettings>";

	const std::string body = PostOperation(
		imagingService_,
		"http://www.onvif.org/ver20/imaging/wsdl/SetImagingSettings",
		"timg", kTimgNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("SetImagingSettings: malformed response");
	(void)doc;
}

// -- Network interfaces -----------------------------------------------------

std::vector<NetworkInterfaceInfo> OnvifClient::GetNetworkInterfaces()
{
	const std::string body = PostOperation(
		deviceService_,
		"http://www.onvif.org/ver10/device/wsdl/GetNetworkInterfaces",
		"tds", kTdsNs, "<tds:GetNetworkInterfaces/>");

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetNetworkInterfaces: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"),
				 "GetNetworkInterfacesResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error(
			"GetNetworkInterfaces: missing response element");

	std::vector<NetworkInterfaceInfo> out;
	for (const tinyxml2::XMLElement *ni :
	     xml::Children(resp, "NetworkInterfaces")) {
		NetworkInterfaceInfo n;
		n.token = Attr(ni, "token");
		n.name = xml::DescendantText(ni, {"Info", "Name"});
		n.enabled = ParseBool(xml::ChildText(ni, "Enabled"));
		const tinyxml2::XMLElement *ipv4 = xml::Child(ni, "IPv4");
		if (ipv4) {
			n.dhcp = ParseBool(xml::ChildText(ipv4, "DHCP"));
			n.address =
				xml::DescendantText(ipv4, {"Manual", "Address"});
			n.prefixLength = ParseInt(
				xml::DescendantText(ipv4, {"Manual", "PrefixLength"}));
		}
		out.push_back(std::move(n));
	}
	return out;
}

void OnvifClient::SetNetworkInterface(const NetworkInterfaceInfo &ni)
{
	std::string ipv4Body;
	if (ni.dhcp) {
		ipv4Body = "<tt:DHCP>true</tt:DHCP>";
	} else {
		ipv4Body = "<tt:Manual><tt:Address>" + EscapeXml(ni.address) +
			   "</tt:Address><tt:PrefixLength>" +
			   FormatInt(ni.prefixLength) +
			   "</tt:PrefixLength></tt:Manual>";
	}
	const std::string reqBody =
		"<tds:SetNetworkInterfaces><tds:InterfaceToken>" +
		EscapeXml(ni.token) +
		"</tds:InterfaceToken><tds:NetworkInterface><tt:Enabled>" +
		(ni.enabled ? std::string("true") : std::string("false")) +
		"</tt:Enabled><tt:IPv4><tt:Enabled>true</tt:Enabled>" +
		ipv4Body +
		"</tt:IPv4></tds:NetworkInterface></tds:SetNetworkInterfaces>";

	const std::string body = PostOperation(
		deviceService_,
		"http://www.onvif.org/ver10/device/wsdl/SetNetworkInterfaces",
		"tds", kTdsNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("SetNetworkInterfaces: malformed response");
	(void)doc;
}

// -- OSD (display service) --------------------------------------------------

std::vector<OSDConfig> OnvifClient::GetOSDs(
	const std::string &videoSourceToken)
{
	const std::string reqBody =
		"<tdisp:GetOSDs>"
		"<tdisp:VideoSourceConfigurationToken>" +
		videoSourceToken +
		"</tdisp:VideoSourceConfigurationToken></tdisp:GetOSDs>";
	const std::string body = PostOperation(
		displayService_,
		"http://www.onvif.org/ver20/display/wsdl/GetOSDs", "tdisp",
		kTdispNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("GetOSDs: malformed response");
	const tinyxml2::XMLElement *env = doc.RootElement();
	const tinyxml2::XMLElement *resp =
		env ? xml::Child(xml::Child(env, "Body"), "GetOSDsResponse")
		    : nullptr;
	if (!resp)
		throw std::runtime_error("GetOSDs: missing response element");

	std::vector<OSDConfig> out;
	for (const tinyxml2::XMLElement *osd : xml::Children(resp, "OSDs")) {
		OSDConfig c;
		c.token = Attr(osd, "token");
		c.text =
			xml::DescendantText(osd, {"TextString", "PlainText"});
		c.enabled = true; // present OSDs are enabled
		out.push_back(std::move(c));
	}
	return out;
}

void OnvifClient::SetOSD(const OSDConfig &cfg)
{
	const std::string reqBody =
		"<tdisp:SetOSD><tdisp:OSD token=\"" + EscapeXml(cfg.token) +
		"\"><tt:Type>Text</tt:Type><tt:Position><tt:X>0</tt:X>"
		"<tt:Y>0</tt:Y></tt:Position><tt:TextString type=\"Plain\">"
		"<tt:PlainText>" +
		EscapeXml(cfg.text) +
		"</tt:PlainText></tt:TextString></tdisp:OSD></tdisp:SetOSD>";

	const std::string body = PostOperation(
		displayService_,
		"http://www.onvif.org/ver20/display/wsdl/SetOSD", "tdisp",
		kTdispNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("SetOSD: malformed response");
	(void)doc;
}

void OnvifClient::DeleteOSD(const std::string &osdToken)
{
	const std::string reqBody = "<tdisp:DeleteOSD><tdisp:OSDToken>" +
				    osdToken + "</tdisp:OSDToken></tdisp:DeleteOSD>";

	const std::string body = PostOperation(
		displayService_,
		"http://www.onvif.org/ver20/display/wsdl/DeleteOSD", "tdisp",
		kTdispNs, reqBody);

	tinyxml2::XMLDocument doc;
	if (!xml::Parse(body, doc))
		throw std::runtime_error("DeleteOSD: malformed response");
	(void)doc;
}

} // namespace obs_onvif