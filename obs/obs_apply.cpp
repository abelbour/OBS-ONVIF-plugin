#include "obs_apply.h"

#include <utility>

#include <obs-module.h>

#include "apply_prompt.h"
#include "obs_mapping.h"
#include "store.h"

namespace obs_onvif::glue {

namespace {

obs_onvif::registry::ApplyPolicy &Policy()
{
	static obs_onvif::registry::ApplyPolicy p;
	return p;
}

int &OutputCount()
{
	static int n = 0;
	return n;
}

std::string &StoreConfigDir()
{
	static std::string dir;
	return dir;
}

std::string &StoreCollection()
{
	static std::string uuid;
	return uuid;
}

} // namespace

obs_onvif::registry::ApplyPolicy &ApplyPolicyInstance()
{
	return Policy();
}

void SetStoreContext(const std::string &config_dir,
		     const std::string &collection)
{
	StoreConfigDir() = config_dir;
	StoreCollection() = collection;
	ReseedApplyState();
}

const std::string &ConfigDir()
{
	return StoreConfigDir();
}

const std::string &SceneCollection()
{
	return StoreCollection();
}

void ReseedApplyState()
{
	std::vector<obs_onvif::registry::SourceMapping> mappings;
	if (!StoreCollection().empty()) {
		obs_onvif::registry::Store store(ConfigDir());
		obs_onvif::registry::CollectionState cs;
		if (store.LoadCollection(StoreCollection(), cs))
			mappings = cs.mappings;
	}
	Policy().TrackMappings(mappings);
	SyncApplyPolicySources();
}

std::string SourceInputUrl(obs_source_t *src)
{
	obs_data_t *settings = obs_source_get_settings(src);
	if (!settings)
		return "";
	const char *input = obs_data_get_string(settings, "input");
	const std::string result = input ? input : "";
	obs_data_release(settings);
	return result;
}

void RewriteSourceUrl(obs_source_t *src, const std::string &new_url)
{
	obs_data_t *settings = obs_source_get_settings(src);
	if (!settings)
		return;
	const std::string old_url = SourceInputUrl(src);
	const std::string final_url =
		obs_onvif::registry::RewriteSourceUrl(old_url, new_url, "");
	obs_data_set_string(settings, "input", final_url.c_str());
	obs_source_update(src, settings);
	obs_data_release(settings);
}

void RestartMediaSource(obs_source_t *src)
{
	proc_handler_t *ph = obs_source_get_proc_handler(src);
	if (!ph)
		return;
	proc_handler_call(ph, "restart", nullptr);
}

void ApplySourceRewrites(
	const std::vector<obs_onvif::registry::SourceRewrite> &rewrites)
{
	for (const auto &rw : rewrites) {
		if (rw.mapping.source_name.empty())
			continue;
		obs_source_t *src =
			obs_get_source_by_name(rw.mapping.source_name.c_str());
		if (!src)
			continue;
		RewriteSourceUrl(src, rw.new_url);
		RestartMediaSource(src);
		obs_source_release(src);
	}
}

void SetOutputActive(bool active)
{
	int &n = OutputCount();
	if (active)
		++n;
	else if (n > 0)
		--n;
	if (n != 0)
		return;
	/* Last output went idle: a deferred incident is re-offered and, per the
	 * plan, auto-applied now. */
	std::vector<obs_onvif::registry::SourceRewrite> rewrites;
	if (Policy().OnOutputsIdle(rewrites))
		ApplySourceRewrites(rewrites);
}

void SyncApplyPolicySources()
{
	for (const auto &r : DiscoverRtspSources())
		Policy().TrackSourceUrl(r.name, r.url);
}

void OnCameraMoved(const std::string &camera_id,
		   const std::string &new_stream_uri,
		   const std::string &credentials)
{
	std::vector<obs_onvif::registry::SourceRewrite> rewrites;
	const bool active = OutputCount() != 0;
	const obs_onvif::registry::ApplyDecision decision = Policy().OnIpChange(
		camera_id, new_stream_uri, active, credentials, rewrites);
	switch (decision) {
	case obs_onvif::registry::ApplyDecision::AppliedNow:
		if (!rewrites.empty()) {
			auto *payload = new std::vector<
				obs_onvif::registry::SourceRewrite>(
				std::move(rewrites));
			obs_queue_task(OBS_TASK_UI,
				       [](void *param) {
					       auto *r = static_cast<
						       std::vector<obs_onvif::
								       registry::
								       SourceRewrite>
							       *>(param);
					       ApplySourceRewrites(*r);
					       delete r;
				       },
				       payload, false);
		}
		break;
	case obs_onvif::registry::ApplyDecision::Prompted:
#ifdef ENABLE_QT
		obs_queue_task(OBS_TASK_UI,
			       [](void *) { ShowApplyPrompt(); }, nullptr,
			       false);
#endif
		break;
	default:
		break;
	}
}

} // namespace obs_onvif::glue