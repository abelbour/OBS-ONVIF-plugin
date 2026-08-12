#include "worker.h"

#include <exception>
#include <utility>

namespace obs_onvif::registry {

namespace {

// Profile/service cache TTL. Fresh entries answer FirstProfileCached with zero
// network; entries older than this are re-resolved on the next hot-path use.
constexpr std::chrono::seconds kProfileCacheTtl(60);

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
// GetCapabilities is skipped when the client already carries capabilities
// (injected from the profile/service cache), saving one round trip.
bool ResolveProfile(obs_onvif::OnvifClient &client,
		    obs_onvif::MediaProfile &out, std::string &err)
{
	try {
		if (client.capabilities().deviceXAddr.empty())
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

void Worker::SetPtzTransport(bool keepalive, bool authCache)
{
	{
		std::lock_guard<std::mutex> lock(cacheMu_);
		keepalive_ = keepalive;
		authCache_ = authCache;
		for (const auto &kv : pools_) {
			kv.second->SetKeepalive(keepalive);
			kv.second->SetAuthCache(authCache);
		}
	}
}

std::shared_ptr<obs_onvif::SoapPool> Worker::PoolFor(
	const std::string &cameraId) const
{
	std::lock_guard<std::mutex> lock(cacheMu_);
	auto it = pools_.find(cameraId);
	if (it != pools_.end())
		return it->second;
	auto pool = std::make_shared<obs_onvif::SoapPool>();
	pool->SetKeepalive(keepalive_);
	pool->SetAuthCache(authCache_);
	pools_[cameraId] = pool;
	return pool;
}

obs_onvif::OnvifClient Worker::BuildClient(
	const Camera &cam, const obs_onvif::Capabilities *caps,
	const std::shared_ptr<obs_onvif::SoapPool> &pool) const
{
	CameraCreds creds;
	if (creds_)
		creds = creds_(cam.id);
	return obs_onvif::OnvifClient(cam.xaddr, creds.username, creds.password,
				     allowBasicFallback_, /*validateCert=*/false,
				     timeoutMs_, caps, pool);
}

bool Worker::ClientFor(const Camera &cam, obs_onvif::OnvifClient &out,
		       std::string &err) const
{
	const std::shared_ptr<obs_onvif::SoapPool> pool = PoolFor(cam.id);
	obs_onvif::Capabilities caps;
	bool haveCaps = false;
	{
		std::lock_guard<std::mutex> lock(cacheMu_);
		const auto it = profileCache_.find(cam.id);
		if (it != profileCache_.end() &&
		    (std::chrono::steady_clock::now() - it->second.at) <
			    kProfileCacheTtl) {
			caps = it->second.caps;
			haveCaps = true;
		}
	}
	out = BuildClient(cam, haveCaps ? &caps : nullptr, pool);
	(void)err;
	return true;
}

void Worker::StoreCache(const std::string &cameraId,
			const obs_onvif::Capabilities &caps,
			const obs_onvif::MediaProfile &profile) const
{
	std::lock_guard<std::mutex> lock(cacheMu_);
	profileCache_[cameraId] = {caps, profile,
				   std::chrono::steady_clock::now()};
}

bool Worker::FirstProfileCached(const Camera &cam,
				obs_onvif::OnvifClient &client,
				obs_onvif::MediaProfile &out,
				std::string &err)
{
	// Fast path: a fresh cache entry answers with zero network.
	CacheEntry cached;
	bool haveCached = false;
	{
		std::lock_guard<std::mutex> lock(cacheMu_);
		const auto it = profileCache_.find(cam.id);
		if (it != profileCache_.end() &&
		    (std::chrono::steady_clock::now() - it->second.at) <
			    kProfileCacheTtl) {
			cached = it->second;
			haveCached = true;
		}
	}
	if (haveCached) {
		if (client.capabilities().deviceXAddr.empty())
			client = BuildClient(cam, &cached.caps,
					     PoolFor(cam.id));
		out = cached.profile;
		return true;
	}

	// Miss: resolve with the client as built (cached caps skip the
	// GetCapabilities round trip); on a SOAP error re-resolve fully.
	if (client.capabilities().deviceXAddr.empty())
		client = BuildClient(cam, nullptr, PoolFor(cam.id));
	if (ResolveProfile(client, out, err)) {
		StoreCache(cam.id, client.capabilities(), out);
		return true;
	}
	if (!client.capabilities().deviceXAddr.empty()) {
		const auto pool = PoolFor(cam.id);
		{
			std::lock_guard<std::mutex> lock(cacheMu_);
			profileCache_.erase(cam.id);
		}
		client = BuildClient(cam, nullptr, pool);
		if (ResolveProfile(client, out, err)) {
			StoreCache(cam.id, client.capabilities(), out);
			return true;
		}
	}
	return false;
}

bool Worker::FirstProfile(const Camera &cam, obs_onvif::MediaProfile &out,
			  std::string &err)
{
	obs_onvif::OnvifClient client;
	return FirstProfileCached(cam, client, out, err);
}

bool Worker::Move(const Camera &cam, double pan, double tilt, double zoom,
		  std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.ContinuousMove(profile.token, pan, tilt, zoom,
				 /*timeoutSeconds=*/0.0);
	}, err);
}

bool Worker::Stop(const Camera &cam, std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.Stop(profile.token);
	}, err);
}

bool Worker::AbsoluteMove(const Camera &cam, double pan, double tilt,
			  double zoom, std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.AbsoluteMove(profile.token, pan, tilt, zoom);
	}, err);
}

bool Worker::RelativeMove(const Camera &cam, double pan, double tilt,
			  double zoom, std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.RelativeMove(profile.token, pan, tilt, zoom);
	}, err);
}

bool Worker::MoveAbortable(const Camera &cam, double pan, double tilt,
			   double zoom, unsigned timeoutSeconds,
			   obs_onvif::AbortHandle &abort, std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.ContinuousMove(profile.token, pan, tilt, zoom,
				 (double)timeoutSeconds, &abort);
	}, err);
}

bool Worker::StopAbortable(const Camera &cam, obs_onvif::AbortHandle &abort,
			   std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.Stop(profile.token, &abort);
	}, err);
}

bool Worker::GotoPreset(const Camera &cam, const std::string &presetToken,
			std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	if (!RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		    c.GotoPreset(profile.token, presetToken);
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
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	if (!RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		    tokenOut = c.SetPreset(profile.token, name);
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
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		const auto presets = c.GetPresets(profile.token);
		out.clear();
		out.reserve(presets.size());
		for (const auto &p : presets)
			out.push_back({p.token, p.name});
	}, err);
}

bool Worker::RenamePreset(const Camera &cam, const std::string &presetToken,
			  const std::string &newName, std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.RenamePreset(profile.token, presetToken, newName);
	}, err);
}

bool Worker::DeletePreset(const Camera &cam, const std::string &presetToken,
			  std::string &err)
{
	obs_onvif::OnvifClient client;
	obs_onvif::MediaProfile profile;
	if (!FirstProfileCached(cam, client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.DeletePreset(profile.token, presetToken);
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	try {
		// Media2 profiles are configured through the Media2 service
		// (PLAN.md §Profile-selection rule).
		const auto configs =
			profile.media2
				? client.GetVideoEncoderConfigurations2()
				: client.GetVideoEncoderConfigurations();
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		if (profile.media2)
			out = c.GetVideoEncoderConfigurationOptions2(
				profile.videoEncoderToken);
		else
			out = c.GetVideoEncoderConfigurationOptions(
				profile.videoEncoderToken);
	}, err);
}

bool Worker::SetEncoderConfig(const Camera &cam,
			      const obs_onvif::VideoEncoderConfig &cfg,
			      std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false;
	obs_onvif::VideoEncoderConfig toSet = cfg;
	toSet.token = profile.videoEncoderToken; // always target the profile's
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		if (profile.media2)
			c.SetVideoEncoderConfiguration2(toSet);
		else
			c.SetVideoEncoderConfiguration(toSet);
	}, err);
}

bool Worker::ImagingSettings(const Camera &cam,
			     obs_onvif::ImagingSettings &out,
			     std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetNetworkInterfaces();
	}, err);
}

bool Worker::SetNetworkInterface(const Camera &cam,
				 const obs_onvif::NetworkInterfaceInfo &ni,
				 std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetNetworkInterface(ni);
	}, err);
}

bool Worker::GetHostname(const Camera &cam, std::string &out, std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		out = c.GetHostname();
	}, err);
}

bool Worker::SetHostname(const Camera &cam, const std::string &name,
			 std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetHostname(name);
	}, err);
}

bool Worker::SetNTP(const Camera &cam, const std::vector<std::string> &servers,
		    bool dhcp, std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.SetNTP(servers, dhcp);
	}, err);
}

bool Worker::OSDs(const Camera &cam,
		  std::vector<obs_onvif::OSDConfig> &out, std::string &err)
{
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false; // also populates the display service URL
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
	obs_onvif::OnvifClient client;
	if (!ClientFor(cam, client, err))
		return false;
	obs_onvif::MediaProfile profile;
	if (!ResolveProfile(client, profile, err))
		return false; // also populates the display service URL
	return RunWithClient(client, [&](obs_onvif::OnvifClient &c) {
		c.DeleteOSD(osdToken);
	}, err);
}

} // namespace obs_onvif::registry
