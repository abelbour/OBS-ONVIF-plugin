#pragma once

#include <string>

struct obs_source;

namespace obs_onvif::glue {

/* Reads the Media Source's current "input" string (may contain user:pass@). */
std::string SourceInputUrl(struct obs_source *src);

/* Rewrites a Media Source's rtsp "input" to `new_url`, preserving embedded
 * credentials from the source's current URL (registry::RewriteSourceUrl),
 * then pushes the settings to the source. Must run on the OBS main thread. */
void RewriteSourceUrl(struct obs_source *src, const std::string &new_url);

} // namespace obs_onvif::glue