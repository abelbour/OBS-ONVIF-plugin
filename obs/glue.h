#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* OBS-side glue entry points, called from src/plugin-main.c. Registered here:
 * frontend event dispatch (scene->preset, output activity), ABI config, and
 * preset/move hotkeys. */
void obs_onvif_glue_load(void);
void obs_onvif_glue_unload(void);

#ifdef __cplusplus
} /* extern "C" */
#endif