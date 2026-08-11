/*
OBS ONVIF Plugin — ABI consumer smoke test (tests/aclink)

A tiny C consumer of obs-onvif.h that mirrors what an external module
(Advanced Scene Switcher & co) does: resolve obs_onvif_get_abi, then drive the
function-pointer table. The driver (run_aclink_test.py) seeds a store with one
camera pointing at the running mock, starts the mock, and asserts the battery
below: camera list, preset lifecycle, moves, and scene bindings.

usage: aclink <config_dir> <mock_http_port>
*/
#include "abi_internal.h"
#include "obs-onvif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define PHASE(cond, msg)                                                        \
	do {                                                                   \
		if (cond) {                                                    \
			printf("ok: %s\n", msg);                              \
		} else {                                                       \
			printf("FAIL: %s\n", msg);                            \
			g_failures++;                                          \
		}                                                              \
	} while (0)

static int find_preset(const char **names, const char **tokens, int count,
		       const char *name, const char **token_out)
{
	for (int i = 0; i < count; ++i) {
		if (strcmp(names[i], name) == 0) {
			*token_out = tokens[i];
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: aclink <config_dir> <mock_http_port>\n");
		return 2;
	}

	obs_onvif_test_config_t cfg;
	cfg.config_dir = argv[1];
	cfg.default_user = "admin";
	cfg.default_pass = "pass";
	cfg.collection_uuid = "col-aclink";
	obs_onvif_abi_test_configure(&cfg);

	obs_cast_abi_t *abi = obs_onvif_get_abi();
	PHASE(abi != NULL, "obs_onvif_get_abi resolves");
	if (!abi) {
		obs_onvif_abi_test_shutdown();
		return 3;
	}
	PHASE(abi->api_version == 1, "api_version == 1");

	/* Camera list ----------------------------------------------------- */
	obs_cast_camera_info_t *cams = NULL;
	int count = 0;
	int rc = abi->get_camera_list(&cams, &count);
	PHASE(rc == 0 && count == 1, "get_camera_list returns the seeded camera");
	if (rc != 0 || count != 1) {
		obs_onvif_abi_test_shutdown();
		return 4;
	}
	PHASE(strcmp(cams[0].name, "ACLINK-CAM") == 0,
	      "camera name matches seed");
	PHASE(cams[0].online == 1, "camera reported online");
	if (strstr(cams[0].xaddr, argv[2]) == NULL)
		printf("note: xaddr=%s port=%s\n", cams[0].xaddr, argv[2]);
	char *camid = strdup(cams[0].camera_id);
	abi->release_camera_list(cams);
	const char *cam = camid;

	/* Unknown camera -> -1 ------------------------------------------------- */
	PHASE(abi->goto_preset("does-not-exist", "x") == -1,
	      "unknown camera returns -1 (not found)");

	/* Preset lifecycle --------------------------------------------------- */
	char token[128] = {0};
	rc = abi->save_preset(cam, "WideAngle", token, sizeof(token));
	PHASE(rc == 0 && token[0] != '\0', "save_preset captures a preset");
	const char *saved_token = NULL;

	{
		const char **names = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->list_presets(cam, &names, &tokens, &n);
		PHASE(rc == 0 && n >= 2, "list_presets shows Home + saved");
		PHASE(find_preset(names, tokens, n, "Home", &saved_token) &&
		      saved_token != NULL,
		      "initial Home preset present");
		if (saved_token)
			saved_token = strdup(saved_token);
		if (rc == 0)
			abi->release_presets(names, tokens, n);
	}

	rc = abi->goto_preset(cam, saved_token ? saved_token : "preset1");
	PHASE(rc == 0, "goto_preset succeeds");

	{
		char cur[128] = {0};
		rc = abi->get_current_preset(cam, cur, sizeof(cur));
		PHASE(rc == 0 && cur[0] != '\0', "get_current_preset tracks last recall");
		if (rc == 0 && saved_token)
			PHASE(strcmp(cur, saved_token) == 0,
			      "current preset == recalled token");
	}

	rc = abi->rename_preset(cam, token, "Narrow");
	PHASE(rc == 0, "rename_preset succeeds");

	{
		const char **names = NULL;
		const char **tokens = NULL;
		int n = 0;
		const char *renamed = NULL;
		rc = abi->list_presets(cam, &names, &tokens, &n);
		PHASE(find_preset(names, tokens, n, "Narrow", &renamed) &&
		      renamed != NULL,
		      "renamed preset visible through list_presets");
		if (rc == 0)
			abi->release_presets(names, tokens, n);
	}

	rc = abi->delete_preset(cam, token);
	PHASE(rc == 0, "delete_preset succeeds");

	{
		const char **names = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->list_presets(cam, &names, &tokens, &n);
		PHASE(rc == 0 && n == 1, "deleted preset gone (one remains)");
		if (rc == 0)
			abi->release_presets(names, tokens, n);
	}

	/* Moves --------------------------------------------------------------- */
	PHASE(abi->move(cam, 0.1, -0.2, 0.05) == 0, "move round-trips");
	PHASE(abi->stop(cam) == 0, "stop round-trips");

	/* Scene bindings ---------------------------------------------------- */
	{
		const char **scenes = NULL;
		const char **cams2 = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->get_bindings(&scenes, &cams2, &tokens, &n);
		PHASE(rc == 0 && n == 0, "bindings start empty");
		if (rc == 0)
			abi->release_bindings(scenes, cams2, tokens, n);
	}

	PHASE(abi->set_binding("Scene A", cam, "preset1") == 0,
	      "set_binding adds a binding");
	{
		const char **scenes = NULL;
		const char **cams2 = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->get_bindings(&scenes, &cams2, &tokens, &n);
		PHASE(rc == 0 && n == 1 && strcmp(scenes[0], "Scene A") == 0,
		      "binding visible via get_bindings");
		if (rc == 0)
			abi->release_bindings(scenes, cams2, tokens, n);
	}
	PHASE(abi->set_binding("Scene A", cam, "preset2") == 0,
	      "set_binding overwrites the same scene");
	{
		const char **scenes = NULL;
		const char **cams2 = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->get_bindings(&scenes, &cams2, &tokens, &n);
		PHASE(rc == 0 && n == 1 &&
		      strcmp(tokens[0], "preset2") == 0,
		      "binding updated in place");
		if (rc == 0)
			abi->release_bindings(scenes, cams2, tokens, n);
	}
	PHASE(abi->clear_binding("Scene A") == 0, "clear_binding succeeds");
	{
		const char **scenes = NULL;
		const char **cams2 = NULL;
		const char **tokens = NULL;
		int n = 0;
		rc = abi->get_bindings(&scenes, &cams2, &tokens, &n);
		PHASE(rc == 0 && n == 0, "bindings empty after clear");
		if (rc == 0)
			abi->release_bindings(scenes, cams2, tokens, n);
	}

	obs_onvif_abi_test_shutdown();
	free(camid);
	free((void *)saved_token);
	printf(g_failures == 0 ? "ACLINK PASS\n" : "ACLINK FAIL\n");
	return g_failures == 0 ? 0 : 1;
}