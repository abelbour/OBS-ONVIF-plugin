#include "obs_mapping.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include "obs_apply.h"

namespace obs_onvif::glue {

namespace {

bool IsMediaSource(obs_source_t *src)
{
	return obs_source_get_type(src) == OBS_SOURCE_TYPE_INPUT &&
	       std::string(obs_source_get_id(src)) == "ffmpeg_source";
}

bool CollectItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *out = static_cast<std::vector<RtspSource> *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (IsRtspMediaSource(src))
		out->push_back(
			{obs_source_get_name(src), SourceInputUrl(src)});
	return true;
}

} // namespace

bool IsRtspMediaSource(obs_source_t *src)
{
	if (!src || !IsMediaSource(src))
		return false;
	const std::string url = SourceInputUrl(src);
	return url.compare(0, 7, "rtsp://") == 0;
}

std::vector<RtspSource> DiscoverRtspSources()
{
	std::vector<RtspSource> out;
	struct obs_frontend_source_list list {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_scene_t *scene = obs_scene_from_source(list.sources.array[i]);
		if (scene)
			obs_scene_enum_items(scene, CollectItem, &out);
	}
	obs_frontend_source_list_free(&list);
	return out;
}

} // namespace obs_onvif::glue