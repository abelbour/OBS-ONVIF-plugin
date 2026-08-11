#include "obs_apply.h"

#include <obs-module.h>

#include "apply.h"

namespace obs_onvif::glue {

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

} // namespace obs_onvif::glue