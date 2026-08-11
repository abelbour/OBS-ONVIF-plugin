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
	try {
		client.GetCapabilities();
		const auto profiles = client.GetProfiles();
		if (profiles.empty()) {
			err = "camera exposes no media profiles";
			return false;
		}
		token = profiles.front().token;
		return true;
	} catch (const std::exception &e) {
		err = e.what();
		return false;
	}
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

} // namespace obs_onvif::registry