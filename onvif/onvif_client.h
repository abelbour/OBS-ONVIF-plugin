#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "soap_client.h"

namespace obs_onvif {

struct Capabilities {
	std::string deviceXAddr;
	std::string mediaXAddr;
	std::string media2XAddr;
	std::string ptzXAddr;
	std::string eventsXAddr;
	std::string imagingXAddr;
	std::string displayXAddr;
};

struct DeviceInfo {
	std::string manufacturer;
	std::string model;
	std::string firmwareVersion;
	std::string serialNumber;
	std::string hardwareId;
};

struct MediaProfile {
	std::string token;
	std::string name;
	std::string videoSourceToken;
	std::string videoEncoderToken;
	std::string ptzConfigToken;
	// True when the profile came from the Media2 service. Stream URIs and
	// encoder configuration for a profile are always taken from the service
	// that owns its token (PLAN.md §Profile-selection rule).
	bool media2 = false;
};

struct StreamUriResult {
	std::string uri;
	std::string timeout; // ISO-8601 duration, e.g. "PT30S"
};

struct Preset {
	std::string token;
	std::string name;
};

struct Resolution {
	int width = 0;
	int height = 0;
};

// One VideoEncoderConfiguration (the first profile's encoder is what the
// config panel edits).
struct VideoEncoderConfig {
	std::string token;
	std::string name;
	std::string encoding; // e.g. "H264"
	Resolution resolution;
	double frameRate = 0.0;
	int bitrate = 0;
};

// Allowed ranges / choices for a VideoEncoderConfiguration (Get...Options).
struct VideoEncoderOptions {
	double minFrameRate = 0.0;
	double maxFrameRate = 0.0;
	int minBitrate = 0;
	int maxBitrate = 0;
	std::vector<Resolution> resolutions;
};

// Imaging settings (brightness/saturation/contrast/sharpness). `present` is
// false when the device's imaging service did not return settings.
struct ImagingSettings {
	bool present = false;
	double brightness = 0.0;
	double colorSaturation = 0.0;
	double contrast = 0.0;
	double sharpness = 0.0;
};

struct ImagingOptions {
	bool present = false;
	double minBrightness = 0.0;
	double maxBrightness = 100.0;
	double minColorSaturation = 0.0;
	double maxColorSaturation = 100.0;
	double minContrast = 0.0;
	double maxContrast = 100.0;
	double minSharpness = 0.0;
	double maxSharpness = 100.0;
};

// One network interface (IPv4 view used by the config panel's Network tab).
struct NetworkInterfaceInfo {
	std::string token;
	std::string name;
	bool enabled = false;
	bool dhcp = false;
	std::string address;
	int prefixLength = 0;
};

// One OSD text overlay configuration.
struct OSDConfig {
	std::string token;
	std::string text;
	bool enabled = false;
};

// Typed ONVIF device-service client on top of SoapClient. All operations are
// WS-Security (UsernameToken digest) by default; when `allowBasicFallback` is
// set (default) a rejected request is retried once with HTTP Basic auth.
//
// Service URLs from GetCapabilities are adopted for media/PTZ calls, with the
// authority (scheme://host:port) rewritten to the base URL's so calls reach
// the same endpoint the client was configured with.
//
// A shared SoapPool may be supplied (M4 §6.8): the connection pool + keep-alive
// reuse and the per-service auth-mode cache live in it, so hot PTZ paths avoid
// a connection handshake and a 401 round-trip on every command. Pre-resolved
// `caps` skip the GetCapabilities round trip entirely.
//
// Transport failures and SOAP faults surface as std::runtime_error.
class OnvifClient {
public:
	// Blank client for use as an out-parameter that gets replaced by the
	// worker's client factory before any operation.
	OnvifClient();
	OnvifClient(std::string baseUrl, std::string username,
		    std::string password);
	OnvifClient(std::string baseUrl, std::string username,
		    std::string password, bool allowBasicFallback,
		    bool validateCert = false, unsigned timeoutMs = 3000,
		    const Capabilities *caps = nullptr,
		    std::shared_ptr<SoapPool> pool = nullptr);

	// Capabilities captured by the last GetCapabilities (or injected via
	// the constructor). Empty XAddrs mean they were never resolved.
	const Capabilities &capabilities() const { return caps_; }

	// Device service (http://www.onvif.org/ver10/device/wsdl).
	Capabilities GetCapabilities();
	DeviceInfo GetDeviceInformation();

	// Media service (http://www.onvif.org/ver10/media/wsdl).
	// Media2-first when the camera exposes a Media2 endpoint: `GetProfiles`
	// tries GetProfiles2 and falls back to classic on a fault or empty reply.
	// Profiles carry `MediaProfile.media2` so the caller routes stream URIs /
	// encoder config to the owning service.
	std::vector<MediaProfile> GetProfiles();
	std::vector<MediaProfile> GetProfiles2();
	StreamUriResult GetStreamUri(const std::string &profileToken);
	StreamUriResult GetStreamUri(const MediaProfile &profile);
	StreamUriResult GetStreamUri2(const std::string &profileToken);

	// PTZ service (http://www.onvif.org/ver20/ptz/wsdl).
	void GotoPreset(const std::string &profileToken,
			const std::string &presetToken);
	// Captures the current position as a preset and returns its token
	// (empty when the device returns none).
	std::string SetPreset(const std::string &profileToken,
			      const std::string &presetName);
	std::vector<Preset> GetPresets(const std::string &profileToken);
	void RenamePreset(const std::string &profileToken,
			  const std::string &presetToken,
			  const std::string &newName);
	void DeletePreset(const std::string &profileToken,
			  const std::string &presetToken);
	// Velocity move (normalized -1..1 per axis). `timeoutSeconds <= 0`
	// omits the ContinuousMove Timeout (the camera moves until Stop);
	// a positive value bounds the move and the caller re-fires to hold it.
	void ContinuousMove(const std::string &profileToken, double pan,
			    double tilt, double zoom, double timeoutSeconds);
	void ContinuousMove(const std::string &profileToken, double pan,
			    double tilt, double zoom, double timeoutSeconds,
			    AbortHandle *abort);
	void Stop(const std::string &profileToken);
	void Stop(const std::string &profileToken, AbortHandle *abort);
	// Absolute/relative position moves (M5c). Velocities/positions are
	// normalized -1..1; absolute positions use the absolute position spaces.
	void AbsoluteMove(const std::string &profileToken, double pan,
			  double tilt, double zoom);
	void RelativeMove(const std::string &profileToken, double pan,
			  double tilt, double zoom);

	// Media: encoder configuration ------------------------------------------
	std::vector<VideoEncoderConfig> GetVideoEncoderConfigurations();
	VideoEncoderOptions GetVideoEncoderConfigurationOptions(
		const std::string &encoderToken);
	void SetVideoEncoderConfiguration(const VideoEncoderConfig &cfg);
	// Media2 variants (ver20/media/wsdl). Call these only for Media2 profiles.
	std::vector<VideoEncoderConfig> GetVideoEncoderConfigurations2();
	VideoEncoderOptions GetVideoEncoderConfigurationOptions2(
		const std::string &encoderToken);
	void SetVideoEncoderConfiguration2(const VideoEncoderConfig &cfg);

	// Imaging service -------------------------------------------------------
	ImagingSettings GetImagingSettings(const std::string &videoSourceToken);
	ImagingOptions GetImagingOptions(const std::string &videoSourceToken);
	void SetImagingSettings(const std::string &videoSourceToken,
				const ImagingSettings &s);

	// Device: network interfaces --------------------------------------------
	std::vector<NetworkInterfaceInfo> GetNetworkInterfaces();
	void SetNetworkInterface(const NetworkInterfaceInfo &ni);
	// Device: hostname + NTP (M5b).
	std::string GetHostname();
	void SetHostname(const std::string &name);
	void SetNTP(const std::vector<std::string> &servers, bool dhcp);

	// Display (ver20): OSD text overlays ------------------------------------
	std::vector<OSDConfig> GetOSDs(const std::string &videoSourceToken);
	void SetOSD(const OSDConfig &cfg);
	void DeleteOSD(const std::string &osdToken);

private:
	// Sends one operation; throws std::runtime_error on transport failure or
	// SOAP fault. Returns the raw response body. `abort`, when non-null,
	// is passed to the transport so an immediate stop can cancel in flight.
	std::string PostOperation(const std::string &serviceUrl,
				  const std::string &soapAction,
				  const char *opPrefix,
				  const char *wsdlNs,
				  const std::string &body,
				  AbortHandle *abort = nullptr);

	std::string ServiceFor(const std::string &primary,
			       const std::string &capsXAddr);

	std::string baseUrl_;
	std::string authority_; // scheme://host[:port] of baseUrl_
	std::string username_;
	std::string password_;
	bool allowBasicFallback_;
	bool validateCert_;
	unsigned timeoutMs_;
	std::string deviceService_;
	std::string mediaService_;
	std::string media2Service_;
	std::string ptzService_;
	std::string imagingService_;
	std::string displayService_;
	Capabilities caps_;
	bool hasMedia2_ = false; // caps advertised a Media2 endpoint
	std::shared_ptr<SoapPool> pool_; // shared transport (pool + auth cache)
	SoapClient soap_;
};

} // namespace obs_onvif