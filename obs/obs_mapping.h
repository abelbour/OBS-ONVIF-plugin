#pragma once

#include <string>
#include <vector>

struct obs_source;

namespace obs_onvif::glue {

/* One RTSP camera source found in the scene tree. */
struct RtspSource {
	std::string name;
	std::string url;
};

/* True when `src` is a Media Source whose input is a rtsp:// URL. */
bool IsRtspMediaSource(struct obs_source *src);

/* Enumerates the scene tree and returns every camera (RTSP Media Source). */
std::vector<RtspSource> DiscoverRtspSources();

} // namespace obs_onvif::glue