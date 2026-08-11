#include "worker.h"

#include <exception>
#include <utility>

#include "onvif_client.h"

namespace obs_onvif::registry {

namespace {

// Runs `fn` against a fresh client for `cam`; converts exceptions into a
// visible error string. Returns an error message when the operation threw.
template <typename Fn>
bool RunWithClient(obs_onvif::OnvifClient &client, Fn &&fn,
		   std::string &err)
{
	try {
		fn(client);
		return true;
	} catch (const std::exception &e) {
		err = e.what();
		return false;
	}
}

// Resolves the camera's first MediaProfile (video-source + encoder tokens).
bool ResolveProfile(obs_onvif::OnvifClient &client,
		    obs_onvif::MediaProfile &out, std::string &err)
{
	try {
		client.GetCapabilities();
		const auto profiles = client.GetProfiles();
		if (profiles.empty()) {
			err = "camera exposes no media profiles";
			return false;
		}
		out = profiles.front();
		return true;
	} catch (const std::exception &e) {
		err = e.what();
		return false;
	}
}

} // namespace

Worker::Worker(std::function<CameraCreds(const std::string &)> creds)
	: creds_(std::move(creds))
{
}

void Worker::SetTimeout(unsigned timeoutMs)
{
	timeoutMs_ = timeoutMs;
}

void Worker::SetAllowBasicFallback(bool allow)
{
	allowBasicFallback_ = allow;
}

obs_onvif::OnvifClient Worker::BuildClient(const Camera &cam) const
{
	CameraCreds creds;
	if (creds_)
		creds = creds_(cam.id);
	return obs_onvif::OnvifClient(cam.xaddr, creds.username, creds.password,
				     allowBasicFallback_,
				     /*validateCert=*/false, timeoutMs_);
}

bool Worker::FirstProfileToken(obs_onvif::OnvifClient &client,
			       std::string &token, std::string &err)
{
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	token = profile.token;
	return true;
}

bool Worker::FirstProfile(const Camera &cam, obs_onvif::MediaProfile &out,
			  std::string &err)
{
	auto client = BuildClient(cam);
	return ResolveProfile(client, out, err);
}

bool Worker::Move(const Camera &cam, double pan, double tilt, double zoom,
		  std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.ContinuousMove(profile, pan, tilt, zoom, /*timeoutSeconds=*/0.5);
	}, err);
}

bool Worker::Stop(const Camera &cam, std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.Stop(profile);
	}, err);
}

bool Worker::GotoPreset(const Camera &cam, const std::string &presetToken,
			std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	if (!RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		    c.GotoPreset(profile, presetToken);
	    }, err))
		return false;

	std::lock_guard<std::mutex> lock(mu_);
	if (!presetToken.empty())
		lastPreset_[cam.id] = presetToken;
	return true;
}

bool Worker::SavePreset(const Camera &cam, const std::string &name,
			std::string &tokenOut, std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	if (!RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		    tokenOut = c.SetPreset(profile, name);
	    }, err))
		return false;

	std::lock_guard<std::mutex> lock(mu_);
	if (!tokenOut.empty())
		lastPreset_[cam.id] = tokenOut;
	return true;
}

bool Worker::ListPresets(const Camera &cam, std::vector<PresetInfo> &out,
			 std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		const auto presets = c.GetPresets(profile);
		out.clear();
		out.reserve(presets.size());
		for (const auto &p : presets)
			out.push_back({p.token, p.name});
	}, err);
}

bool Worker::RenamePreset(const Camera &cam, const std::string &presetToken,
			  const std::string &newName, std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.RenamePreset(profile, presetToken, newName);
	}, err);
}

bool Worker::DeletePreset(const Camera &cam, const std::string &presetToken,
			  std::string &err)
{
	auto client = BuildClient(cam);
	std::string profile;
	if (!FirstProfileToken(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.DeletePreset(profile, presetToken);
	}, err);
}

bool Worker::CurrentPresetToken(const std::string &cameraId,
				std::string &tokenOut) const
{
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = lastPreset_.find(cameraId);
	if (it == lastPreset_.end())
		return false;
	tokenOut = it->second;
	return true;
}

bool Worker::EncoderConfig(const Camera &cam,
			   obs_onvif::VideoEncoderConfig &out,
			   std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	try {
		const auto configs = client.GetVideoEncoderConfigurations();
		for (const auto &cfg : configs) {
			if (cfg.token == profile.videoEncoderToken) {
				out = cfg;
				return true;
			}
		}
		err = "profile's encoder configuration not found";
		return false;
	} catch (const std::exception &e) {
		err = e.what();
		return false;
	}
}

bool Worker::EncoderOptions(const Camera &cam,
			    obs_onvif::VideoEncoderOptions &out,
			    std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetVideoEncoderConfigurationOptions(
			profile.videoEncoderToken);
	}, err);
}

bool Worker::SetEncoderConfig(const Camera &cam,
			      const obs_onvif::VideoEncoderConfig &cfg,
			      std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	obs_onvif::VideoEncoderConfig toSet = cfg;
	toSet.token = profile.videoEncoderToken; // always target the profile's
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetVideoEncoderConfiguration(toSet);
	}, err);
}

bool Worker::ImagingSettings(const Camera &cam,
			     obs_onvif::ImagingSettings &out,
			     std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetImagingSettings(profile.videoSourceToken);
	}, err);
}

bool Worker::ImagingOptions(const Camera &cam,
			    obs_onvif::ImagingOptions &out,
			    std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetImagingOptions(profile.videoSourceToken);
	}, err);
}

bool Worker::SetImagingSettings(const Camera &cam,
				const obs_onvif::ImagingSettings &s,
				std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetImagingSettings(profile.videoSourceToken, s);
	}, err);
}

bool Worker::NetworkInterfaces(const Camera &cam,
			       std::vector<obs_onvif::NetworkInterfaceInfo> &out,
			       std::string &err)
{
	auto client = BuildClient(cam);
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetNetworkInterfaces();
	}, err);
}

bool Worker::SetNetworkInterface(const Camera &cam,
				 const obs_onvif::NetworkInterfaceInfo &ni,
				 std::string &err)
{
	auto client = BuildClient(cam);
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetNetworkInterface(ni);
	}, err);
}

bool Worker::OSDs(const Camera &cam,
		  std::vector<obs_onvif::OSDConfig> &out, std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetOSDs(profile.videoSourceToken);
	}, err);
}

bool Worker::SetOSD(const Camera &cam, const obs_onvif::OSDConfig &cfg,
		    std::string &err)
{
	auto client = BuildClient(cam);
	obs_onvif::OSDConfig toSet = cfg;
	if (toSet.token.empty())
		toSet.token = "osd1"; // device may reassign; kept stable here
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetOSD(toSet);
	}, err);
}

bool Worker::DeleteOSD(const Camera &cam, const std::string &osdToken,
		       std::string &err)
{
	auto client = BuildClient(cam);
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.DeleteOSD(osdToken);
	}, err);
}

} // namespace obs_onvif::registry
