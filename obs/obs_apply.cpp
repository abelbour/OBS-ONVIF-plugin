#include "obs_apply.h"

#include <obs-module.h>

#include "obs_mapping.h"

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

} // namespace

obs_onvif::registry::ApplyPolicy &ApplyPolicyInstance()
{
	return Policy();
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

} // namespace obs_onvif::glue