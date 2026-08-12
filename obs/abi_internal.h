/*
OBS ONVIF Plugin — internal test bridge for the public ABI.

Not installed and not part of the shipped zip; exists so tests/aclink (a C
consumer) can configure the ABI singleton against a seeded store + running
mock ONVIF camera before driving the exported function table.
*/

#ifndef OBS_ONVIF_ABI_INTERNAL_H
#define OBS_ONVIF_ABI_INTERNAL_H

#include "obs-onvif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obs_onvif_test_config_s {
	const char *config_dir;      /* writable temp dir with cameras.json */
	const char *default_user;    /* fallback creds when wincred is absent */
	const char *default_pass;
	const char *collection_uuid; /* scene-binding namespace (may be NULL) */
} obs_onvif_test_config_t;

OBS_ONVIF_API void obs_onvif_abi_test_configure(
	const obs_onvif_test_config_t *cfg);
OBS_ONVIF_API void obs_onvif_abi_test_shutdown(void);
/* Blocks until the PTZ controller queue is drained (deterministic async
 * move/stop sequencing in live tests). */
OBS_ONVIF_API void obs_onvif_abi_test_flush(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OBS_ONVIF_ABI_INTERNAL_H */