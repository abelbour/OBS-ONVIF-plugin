#include "registry.h"

#include <algorithm>
#include <utility>

namespace obs_onvif::registry {

Registry::Registry() = default;

Camera *Registry::FindCamera(const std::string &id)
{
	const auto it = cameras_.find(id);
	return it != cameras_.end() ? &it->second : nullptr;
}

const Camera *Registry::FindCamera(const std::string &id) const
{
	const auto it = cameras_.find(id);
	return it != cameras_.end() ? &it->second : nullptr;
}

const std::map<std::string, Camera> &Registry::Cameras() const
{
	return cameras_;
}

size_t Registry::CameraCount() const
{
	return cameras_.size();
}

void Registry::RemoveCamera(const std::string &id)
{
	cameras_.erase(id);
}

Camera *Registry::UpsertCamera(const Camera &cam)
{
	cameras_[cam.id] = cam;
	return &cameras_[cam.id];
}

void Registry::SetMappings(const std::vector<SourceMapping> &mappings)
{
	mappings_ = mappings;
	apply_.TrackMappings(mappings);
}

const std::vector<SourceMapping> &Registry::Mappings() const
{
	return mappings_;
}

std::vector<SourceMapping> Registry::MappingsForCamera(
	const std::string &camera_id) const
{
	std::vector<SourceMapping> out;
	for (const auto &m : mappings_) {
		if (m.camera_id == camera_id)
			out.push_back(m);
	}
	return out;
}

void Registry::AddSourceMapping(const SourceMapping &m)
{
	for (auto &existing : mappings_) {
		if (existing.source_name == m.source_name) {
			existing = m;
			apply_.TrackMappings(mappings_);
			return;
		}
	}
	mappings_.push_back(m);
	apply_.TrackMappings(mappings_);
}

void Registry::RemoveSourceMapping(const std::string &source_name)
{
	mappings_.erase(
		std::remove_if(mappings_.begin(), mappings_.end(),
			       [&](const SourceMapping &m) {
				       return m.source_name == source_name;
			       }),
		mappings_.end());
	apply_.TrackMappings(mappings_);
	apply_.ForgetSource(source_name);
}

bool Registry::Restore(Store &store)
{
	std::vector<Camera> loaded;
	if (!store.LoadCameras(loaded))
		return false;
	cameras_.clear();
	for (auto &c : loaded)
		cameras_[c.id] = std::move(c);
	return true;
}

bool Registry::Persist(Store &store) const
{
	std::vector<Camera> cams;
	cams.reserve(cameras_.size());
	for (const auto &kv : cameras_)
		cams.push_back(kv.second);
	return store.SaveCameras(cams);
}

DeviceUpdate Registry::SeenDevice(const DeviceIdentity &identity,
				  const std::string &display_name,
				  const std::string &new_xaddr,
				  const std::string &profile_token,
				  const std::string &new_stream_uri,
				  uint64_t now_ms, bool output_active,
				  const std::string &credentials,
				  std::vector<SourceRewrite> &rewrites)
{
	rewrites.clear();

	const std::string fingerprint = BuildFingerprint(identity);
	DeviceUpdate up;
	if (fingerprint.empty()) {
		up.action = ApplyDecision::Ignored;
		return up; // cannot identify the device
	}

	Camera *existing = FindCamera(fingerprint);
	if (!existing) {
		Camera cam;
		cam.id = fingerprint;
		cam.name = display_name;
		cam.xaddr = new_xaddr;
		cam.scopeMac = ParseScopeMac(identity.scopes);
		cam.online = true;
		cam.lastSeen = now_ms;
		if (!new_stream_uri.empty())
			cam.lastKnownRTSP[profile_token] = new_stream_uri;
		UpsertCamera(cam);
		up.first_seen = true;
		up.action = ApplyDecision::Ignored; // recorded, nothing to rewrite
		up.stream_uri = new_stream_uri;
		return up;
	}

	Camera &cam = *existing;
	cam.online = true;
	cam.lastSeen = now_ms;
	if (!new_stream_uri.empty())
		cam.lastKnownRTSP[profile_token] = new_stream_uri;
	up.stream_uri = new_stream_uri;

	if (cam.xaddr != new_xaddr) {
		cam.xaddr = new_xaddr;
		up.address_changed = true;
		up.action =
			apply_.OnIpChange(fingerprint, new_stream_uri,
					  output_active, credentials, rewrites);
	} else {
		up.action = ApplyDecision::Ignored; // nothing changed
	}
	return up;
}

ApplyPolicy &Registry::Apply()
{
	return apply_;
}

const ApplyPolicy &Registry::Apply() const
{
	return apply_;
}

} // namespace obs_onvif::registry