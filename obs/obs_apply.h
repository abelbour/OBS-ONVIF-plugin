#pragma once

#include <string>
#include <vector>

#include "apply.h"

struct obs_source;

namespace obs_onvif::glue {

/* Reads the Media Source's current "input" string (may contain user:pass@). */
std::string SourceInputUrl(struct obs_source *src);

/* Rewrites a Media Source's rtsp "input" to `new_url`, preserving embedded
 * credentials from the source's current URL (registry::RewriteSourceUrl),
 * then pushes the settings to the source. Must run on the OBS main thread. */
void RewriteSourceUrl(struct obs_source *src, const std::string &new_url);

/* Fires the Media Source's "restart" proc so it reconnects using the current
 * "input" value. No-op for sources without that proc. Main thread. */
void RestartMediaSource(struct obs_source *src);

/* Main-thread dispatch for the apply policy: applies every produced rewrite
 * (URL rewrite + restart). */
void ApplySourceRewrites(
	const std::vector<obs_onvif::registry::SourceRewrite> &rewrites);

/* Output-activity accounting driving the apply policy. Frontend STREAMING/
 * RECORDING STARTED/STOPPED events call SetOutputActive(true/false); when the
 * last active output goes idle the policy re-offers a deferred incident. */
void SetOutputActive(bool active);

/* The OBS-side apply policy instance (tracked source URLs are maintained
 * here; camera mappings arrive with the M3c settings UI). */
obs_onvif::registry::ApplyPolicy &ApplyPolicyInstance();

/* Rebuilds the policy's source-URL mirror from the OBS session tree. */
void SyncApplyPolicySources();

} // namespace obs_onvif::glue