#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "camera.h"

namespace obs_onvif::registry {

// Outcome of an IP-change event / prompt resolution.
enum class ApplyDecision { AppliedNow, Prompted, Deferred, Ignored };

// One concrete source rewrite produced when an incident is applied.
struct SourceRewrite {
	SourceMapping mapping;
	std::string old_url; // current Media-Source input value
	std::string new_url; // rewritten input value (credentials spliced)
	std::string camera_id;
};

// Builds the final RTSP URL to write into a Media Source input:
//   1. when `new_url` already embeds userinfo, use it as-is;
//   2. else when `credentials` ("user:pass", URL-encoded) is non-empty,
//      inject it after the scheme;
//   3. else preserve the old URL's embedded credentials when it had them;
//   4. else return `new_url` unchanged.
std::string RewriteSourceUrl(const std::string &old_url,
			     const std::string &new_url,
			     const std::string &credentials);

// Percent-encodes text for use inside a URL userinfo component
// (reserved chars @, :, /, ?, #, % and spaces are escaped).
std::string UrlEncodeUserinfo(const std::string &s);

// Live-output apply policy state machine (deterministic, OBS-free). OBS side
// wiring -- main-thread dispatch and the Apply/Defer/Ignore dialog -- lives in
// obs/obs_apply (M3). This class only decides; the caller applies.
//
// Transitions (PLAN.md §Live-output policy):
//   idle --OnIpChange--> AppliedNow | Prompted | Ignored
//   Prompted --ApplyPending--> AppliedNow
//   Prompted --IgnorePending(remember)--> Ignored   (policy[cam] = ignore)
//   Prompted --OnPromptTimeout(=Defer)--> Deferred   (30 s staleness)
//   Deferred --OnOutputsIdle--> AppliedNow           (re-offer, auto-apply)
class ApplyPolicy {
public:
	ApplyPolicyChoice DefaultPolicy() const;
	void SetDefaultPolicy(ApplyPolicyChoice p);

	ApplyPolicyChoice PolicyFor(const std::string &camera_id) const;
	// Persisted per-camera override; `remember` marks it for SaveCameraPolicies.
	void SetCameraPolicy(const std::string &camera_id, ApplyPolicyChoice p,
			     bool remember);

	// URL bookkeeping used when composing rewrites (mirrors the mapping that
	// M3 reads from each OBS Media Source).
	void TrackSourceUrl(const std::string &source_name,
			    const std::string &current_url);
	void TrackMappings(const std::vector<SourceMapping> &mappings);
	void ForgetSource(const std::string &source_name);

	// Core transition. Returns the deterministic outcome when no prompt is
	// required, otherwise Prompted (incident captured for the caller/UI).
	// `rewrites` receives the concrete source rewrites for auto-applied
	// outcomes and stays empty for Prompted/Deferred/Ignored.
	ApplyDecision OnIpChange(const std::string &camera_id,
				 const std::string &new_stream_uri,
				 bool output_active,
				 const std::string &credentials,
				 std::vector<SourceRewrite> &rewrites);

	// Prompt resolution (async, e.g. within the 30 s window in the UI):
	ApplyDecision ApplyPending(std::vector<SourceRewrite> &rewrites);
	ApplyDecision IgnorePending(bool remember);
	ApplyDecision DeferPending();
	ApplyDecision OnPromptTimeout(); // == DeferPending()

	// Outputs became inactive: a deferred incident is re-offered and
	// auto-applied. Returns true when an incident was applied.
	bool OnOutputsIdle(std::vector<SourceRewrite> &rewrites);

	bool HasPending() const;
	bool HasDeferred() const;
	const std::string &PendingCamera() const;
	const std::string &DeferredCamera() const;

	// Source names that a pending incident would rewrite (empty when there
	// is no pending incident or nothing is mapped). Read-only helper for the
	// Apply/Defer/Ignore prompt UI.
	std::vector<std::string> PendingSources() const;

private:
	struct Incident {
		std::string camera_id;
		std::string new_stream_uri;
		std::string credentials;
	};

	// Fills `rewrites` for `inc` from the current mapping table. Returns the
	// number of rewrites produced.
	size_t BuildRewrites(const Incident &inc,
			     std::vector<SourceRewrite> &rewrites) const;

	ApplyPolicyChoice default_policy_ = ApplyPolicyChoice::Ask;
	std::map<std::string, ApplyPolicyChoice> camera_policy_; // remembered only
	std::map<std::string, std::string> source_urls_; // source_name -> current
	std::vector<SourceMapping> mappings_;
	std::optional<Incident> pending_;
	std::optional<Incident> deferred_;
};

} // namespace obs_onvif::registry