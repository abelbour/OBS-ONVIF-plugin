#include "obs_apply.h"

#include <obs-module.h>

#include "apply.h"

namespace obs_onvif::glue {

std::string SourceInputUrl(obs_source_t *src)
{
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	const char *input = obs_data_get_string(settings, "input");
	return input ? input : "";
}

void RewriteSourceUrl(obs_source_t *src, const std::string &new_url)
{
	OBSDataAutoRelease settings = obs_source_get_settings(src);
	const std::string old_url = SourceInputUrl(src);
	const std::string final_url =
		obs_onvif::registry::RewriteSourceUrl(old_url, new_url, "");
	obs_data_set_string(settings, "input", final_url.c_str());
	obs_source_update(src, settings);
}

} // namespace obs_onvif::glue